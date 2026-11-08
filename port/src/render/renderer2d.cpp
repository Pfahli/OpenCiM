#include "render/renderer2d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "render/gl.h"

namespace regs::render {

namespace {

const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform mat4 uMatrix;
out vec2 vUv;
out vec4 vColor;
void main() {
  vUv = aUv;
  vColor = aColor;
  gl_Position = uMatrix * vec4(aPos, 0.0, 1.0);
}
)";

const char* kFragment = R"(#version 330 core
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTex;
uniform int uTextured;
uniform float uSharp;
out vec4 oColor;
// UI scale > 1 (uSharp = the scale): "sharp bilinear" for texels the scale magnified. Each texel keeps a flat core
// and blends with its neighbour over about one window pixel, so at 2x an image drawn 1:1 by the scripts becomes an
// exact 2x2 copy of each texel (no blur), and at 1.5x only the texel seams are blended. Texels magnified far more
// than the scale (stretched backgrounds, gradients) and minified ones (mipmapped fonts) keep plain bilinear.
vec4 sampleGui() {
  if (uSharp <= 0.0) return texture(uTex, vUv);
  vec2 size = vec2(textureSize(uTex, 0));
  vec2 t = vUv * size;
  vec2 dx = dFdx(t), dy = dFdy(t);
  vec2 mag = 1.0 / max(vec2(length(vec2(dx.x, dy.x)), length(vec2(dx.y, dy.y))), vec2(1e-6));  // pixels per texel
  vec2 k = mix(max(mag, vec2(1.0)), vec2(1.0), step(vec2(uSharp * 1.25), mag));
  vec2 region = 0.5 - 0.5 / k;
  vec2 cell = floor(t);
  vec2 off = fract(t) - 0.5;
  vec2 f = (off - clamp(off, -region, region)) * k + 0.5;
  return textureGrad(uTex, (cell + f) / size, dFdx(vUv), dFdy(vUv));
}
void main() {
  oColor = uTextured != 0 ? sampleGui() * vColor : vColor;
}
)";

uint32_t compile(gl::GLenum type, const char* src, std::string* error) {
  uint32_t s = gl::CreateShader(type);
  gl::ShaderSource(s, 1, &src, nullptr);
  gl::CompileShader(s);
  int ok = 0;
  gl::GetShaderiv(s, gl::COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    gl::GetShaderInfoLog(s, sizeof log, nullptr, log);
    if (error) *error = log;
    return 0;
  }
  return s;
}

}  // namespace

Renderer2D::~Renderer2D() = default;  // GL objects die with the context

bool Renderer2D::init(std::string* error) {
  uint32_t vs = compile(gl::VERTEX_SHADER, kVertex, error), fs = compile(gl::FRAGMENT_SHADER, kFragment, error);
  if (!vs || !fs) return false;
  program_ = gl::CreateProgram();
  gl::AttachShader(program_, vs);
  gl::AttachShader(program_, fs);
  gl::LinkProgram(program_);
  int ok = 0;
  gl::GetProgramiv(program_, gl::LINK_STATUS, &ok);
  gl::DeleteShader(vs);
  gl::DeleteShader(fs);
  if (!ok) {
    char log[2048];
    gl::GetProgramInfoLog(program_, sizeof log, nullptr, log);
    if (error) *error = log;
    return false;
  }
  uMatrix_ = gl::GetUniformLocation(program_, "uMatrix");
  uTextured_ = gl::GetUniformLocation(program_, "uTextured");
  uTex_ = gl::GetUniformLocation(program_, "uTex");
  uSharp_ = gl::GetUniformLocation(program_, "uSharp");
  gl::GenVertexArrays(1, &vao_);
  gl::BindVertexArray(vao_);
  gl::GenBuffers(1, &vbo_);
  gl::BindBuffer(gl::ARRAY_BUFFER, vbo_);
  gl::EnableVertexAttribArray(0);
  gl::EnableVertexAttribArray(1);
  gl::EnableVertexAttribArray(2);
  gl::VertexAttribPointer(0, 2, gl::FLOAT, 0, sizeof(GuiVertex), reinterpret_cast<void*>(offsetof(GuiVertex, x)));
  gl::VertexAttribPointer(1, 2, gl::FLOAT, 0, sizeof(GuiVertex), reinterpret_cast<void*>(offsetof(GuiVertex, u)));
  gl::VertexAttribPointer(2, 4, gl::UNSIGNED_BYTE, 1, sizeof(GuiVertex), reinterpret_cast<void*>(offsetof(GuiVertex, rgba)));
  const auto* ver = reinterpret_cast<const char*>(gl::GetString(gl::VERSION));
  const auto* ren = reinterpret_cast<const char*>(gl::GetString(gl::RENDERER));
  gl::GLint units = 0;
  gl::GetIntegerv(0x8872 /* GL_MAX_TEXTURE_IMAGE_UNITS */, &units);
  info = std::string("OpenGL ") + (ver ? ver : "?") + " / " + (ren ? ren : "?") + " / " + std::to_string(units) +
         " texture units" + (units < 16 ? " (fewer than the 16 the port needs)" : "");
  return true;
}

uint32_t Renderer2D::createTexture(const Image& img) {
  flush();  // binding a texture below must not disturb a pending batch
  uint32_t t = 0;
  gl::GenTextures(1, &t);
  gl::GLenum target = img.cube ? gl::TEXTURE_CUBE_MAP : gl::TEXTURE_2D;
  gl::BindTexture(target, t);
  gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
  int faces = img.cube ? 6 : 1, mips = img.mipCount;
  for (int f = 0; f < faces; ++f) {
    // DDS cube faces come in GL order: +X, -X, +Y, -Y, +Z, -Z
    gl::GLenum face = img.cube ? gl::GLenum(gl::TEXTURE_CUBE_MAP_POSITIVE_X + f) : gl::TEXTURE_2D;
    for (int m = 0; m < mips; ++m) {
      size_t li = size_t(f * mips + m);
      if (li >= img.levels.size()) break;
      int w = std::max(1, img.width >> m), h = std::max(1, img.height >> m);
      const auto& lv = img.levels[li];
      switch (img.format) {
        case Image::Format::DXT1: gl::CompressedTexImage2D(face, m, gl::COMPRESSED_RGBA_S3TC_DXT1, w, h, 0, int(lv.size()), lv.data()); break;
        case Image::Format::DXT3: gl::CompressedTexImage2D(face, m, gl::COMPRESSED_RGBA_S3TC_DXT3, w, h, 0, int(lv.size()), lv.data()); break;
        case Image::Format::DXT5: gl::CompressedTexImage2D(face, m, gl::COMPRESSED_RGBA_S3TC_DXT5, w, h, 0, int(lv.size()), lv.data()); break;
        case Image::Format::BGR8: gl::TexImage2D(face, m, gl::RGB8, w, h, 0, gl::BGR, gl::UNSIGNED_BYTE, lv.data()); break;
        default: gl::TexImage2D(face, m, gl::RGBA8, w, h, 0, gl::BGRA, gl::UNSIGNED_BYTE, lv.data()); break;
      }
    }
  }
  gl::TexParameteri(target, gl::TEXTURE_MAX_LEVEL, mips - 1);
  gl::TexParameteri(target, gl::TEXTURE_MIN_FILTER, mips > 1 ? gl::LINEAR_MIPMAP_LINEAR : gl::LINEAR);
  gl::TexParameteri(target, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
  if (!img.cube && mips > 1) {
    mipTextures_.push_back(t);
    if (anisotropy > 1) applyAnisotropy(t);
  }
  gl::GLint wrap = img.cube ? gl::CLAMP_TO_EDGE : gl::REPEAT;
  gl::TexParameteri(target, gl::TEXTURE_WRAP_S, wrap);
  gl::TexParameteri(target, gl::TEXTURE_WRAP_T, wrap);
  if (img.cube) {
    gl::TexParameteri(target, gl::TEXTURE_WRAP_R, wrap);
    gl::Enable(gl::TEXTURE_CUBE_MAP_SEAMLESS);
  }
  return t;
}

uint32_t Renderer2D::createTextureBGRA(const uint8_t* bgra, int w, int h) {
  flush();
  uint32_t t = 0;
  gl::GenTextures(1, &t);
  gl::BindTexture(gl::TEXTURE_2D, t);
  gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
  gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, w, h, 0, gl::BGRA, gl::UNSIGNED_BYTE, bgra);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAX_LEVEL, 0);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::REPEAT);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::REPEAT);
  return t;
}

void Renderer2D::updateTextureBGRA(uint32_t tex, const uint8_t* bgra, int w, int h) {
  flush();
  gl::BindTexture(gl::TEXTURE_2D, tex);
  gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
  gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, w, h, 0, gl::BGRA, gl::UNSIGNED_BYTE, bgra);
}

void Renderer2D::applyAnisotropy(uint32_t tex) {
  // gsVideo_OGL clamps it to GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT; 1 = plain trilinear
  if (!maxAnisotropy_) {
    gl::GLint maxAniso = 1;
    gl::GetIntegerv(0x84FF, &maxAniso);
    maxAnisotropy_ = std::max(1, maxAniso);
  }
  gl::BindTexture(gl::TEXTURE_2D, tex);
  gl::TexParameterf(gl::TEXTURE_2D, gl::TEXTURE_MAX_ANISOTROPY, std::clamp(anisotropy, 1.0f, float(maxAnisotropy_)));
}

void Renderer2D::setAnisotropy(float a) {
  if (a == anisotropy) return;
  flush();
  anisotropy = a;
  for (uint32_t t : mipTextures_) applyAnisotropy(t);
  gl::BindTexture(gl::TEXTURE_2D, 0);
}

void Renderer2D::destroyTexture(uint32_t tex) {
  if (!tex) return;
  if (tex == batchTex_) flush();
  std::erase(mipTextures_, tex);
  gl::DeleteTextures(1, &tex);
}

void Renderer2D::beginFrame(int width, int height, uint32_t clearArgb) {
  fw_ = width;
  fh_ = height;
  batch_.clear();
  gl::Viewport(0, 0, width, height);
  gl::Disable(gl::SCISSOR_TEST);
  gl::ClearColor(float(clearArgb >> 16 & 0xFF) / 255, float(clearArgb >> 8 & 0xFF) / 255, float(clearArgb & 0xFF) / 255, 1);
  gl::Clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);
  gl::Disable(gl::DEPTH_TEST);
  gl::Disable(gl::CULL_FACE);
  gl::Enable(gl::BLEND);
  gl::BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
  gl::UseProgram(program_);
  gl::Uniform1i(uTex_, 0);
  gl::Uniform1f(uSharp_, sharpScale > 1 ? sharpScale : 0.0f);
  gl::ActiveTexture(gl::TEXTURE0);
  gl::BindVertexArray(vao_);
  gl::BindBuffer(gl::ARRAY_BUFFER, vbo_);
  setProjection(0, 0, float(width), float(height), float(width), float(height));
}

void Renderer2D::endFrame() { flush(); }

void Renderer2D::resume() {
  gl::Disable(gl::DEPTH_TEST);
  gl::Disable(gl::CULL_FACE);
  gl::Enable(gl::BLEND);
  gl::BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
  gl::UseProgram(program_);
  gl::Uniform1i(uTex_, 0);
  gl::ActiveTexture(gl::TEXTURE0);
  gl::BindVertexArray(vao_);
  gl::BindBuffer(gl::ARRAY_BUFFER, vbo_);
  setProjection(proj_[0], proj_[1], proj_[2], proj_[3], proj_[4], proj_[5]);
  setScissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
}

void Renderer2D::setProjection(float px, float py, float pw, float ph, float unitsW, float unitsH) {
  flush();
  const float saved[6] = {px, py, pw, ph, unitsW, unitsH};
  std::copy(saved, saved + 6, proj_);
  gl::Viewport(int(px), int(float(fh_) - py - ph), int(pw), int(ph));
  // ortho(0, unitsW, unitsH, 0): column-major
  const float m[16] = {2 / unitsW, 0, 0, 0, 0, -2 / unitsH, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
  std::copy(m, m + 16, mvp_);
  gl::UniformMatrix4fv(uMatrix_, 1, 0, m);
}

uint32_t Renderer2D::buildProgram(const std::string& vs, const std::string& fs, std::string* error) {
  auto compileStage = [&](gl::GLenum type, const std::string& src) -> uint32_t {
    uint32_t s = gl::CreateShader(type);
    const char* p = src.c_str();
    gl::ShaderSource(s, 1, &p, nullptr);
    gl::CompileShader(s);
    int ok = 0;
    gl::GetShaderiv(s, gl::COMPILE_STATUS, &ok);
    if (!ok) {
      char log[2048];
      gl::GetShaderInfoLog(s, sizeof log, nullptr, log);
      if (error) *error = log;
      gl::DeleteShader(s);
      return 0;
    }
    return s;
  };
  uint32_t v = compileStage(gl::VERTEX_SHADER, vs), f = v ? compileStage(gl::FRAGMENT_SHADER, fs) : 0;
  if (!v || !f) return 0;
  uint32_t prog = gl::CreateProgram();
  gl::AttachShader(prog, v);
  gl::AttachShader(prog, f);
  // GUI vertices: position, uv, colour at the slots of the 2D vertex array
  gl::BindAttribLocation(prog, 0, "regs_Vertex");
  gl::BindAttribLocation(prog, 1, "regs_MultiTexCoord0");
  gl::BindAttribLocation(prog, 2, "regs_Color");
  gl::LinkProgram(prog);
  gl::DeleteShader(v);
  gl::DeleteShader(f);
  int ok = 0;
  gl::GetProgramiv(prog, gl::LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    gl::GetProgramInfoLog(prog, sizeof log, nullptr, log);
    if (error) *error = log;
    return 0;
  }
  return prog;
}

void Renderer2D::drawWithProgram(uint32_t prog, const uint32_t* textures, int textureCount, Prim prim, const GuiVertex* v,
                                 size_t n, const float* custom0) {
  flush();
  gl::UseProgram(prog);
  gl::UniformMatrix4fv(gl::GetUniformLocation(prog, "regs_ModelViewProjectionMatrix"), 1, 0, mvp_);
  if (custom0) gl::Uniform4fv(gl::GetUniformLocation(prog, "custom0"), 1, custom0);
  if (int loc = gl::GetUniformLocation(prog, "timer"); loc >= 0) gl::Uniform1f(loc, timer);
  for (int i = 0; i < textureCount; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "texture%d", i);
    gl::Uniform1i(gl::GetUniformLocation(prog, name), i);
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(i));
    gl::BindTexture(gl::TEXTURE_2D, textures[i]);
  }
  gl::ActiveTexture(gl::TEXTURE0);
  uint32_t keepTex = batchTex_;
  draw(textures && textureCount ? textures[0] : 0, prim, v, n);
  flushWith(false);
  batchTex_ = keepTex;
  gl::UseProgram(program_);
}

void Renderer2D::setScissor(int x, int y, int w, int h) {
  flush();
  scissor_[0] = x;
  scissor_[1] = y;
  scissor_[2] = w;
  scissor_[3] = h;
  if (w < 0 || h < 0) {
    gl::Disable(gl::SCISSOR_TEST);
    return;
  }
  gl::Enable(gl::SCISSOR_TEST);
  gl::Scissor(x, fh_ - y - h, std::max(0, w), std::max(0, h));
}

void Renderer2D::wideLine(const GuiVertex& a, const GuiVertex& b) {
  // a lineWidth-pixel band around the segment, in the current units (pixels per unit from the projection)
  float kx = proj_[2] / proj_[4], ky = proj_[3] / proj_[5];  // window pixels per unit
  float dx = (b.x - a.x) * kx, dy = (b.y - a.y) * ky, len = std::sqrt(dx * dx + dy * dy);
  if (len <= 0) return;
  float h = lineWidth * 0.5f, nx = -dy / len * h / kx, ny = dx / len * h / ky;
  GuiVertex q[4] = {a, b, b, a};
  q[0].x += nx; q[0].y += ny;
  q[1].x += nx; q[1].y += ny;
  q[2].x -= nx; q[2].y -= ny;
  q[3].x -= nx; q[3].y -= ny;
  const GuiVertex t[6] = {q[0], q[1], q[2], q[0], q[2], q[3]};
  batch_.insert(batch_.end(), t, t + 6);
}

void Renderer2D::draw(uint32_t tex, Prim prim, const GuiVertex* v, size_t n) {
  bool lines = prim == Prim::Lines || prim == Prim::LineStrip;
  if (lines && lineWidth > 1) {  // core profiles only guarantee 1-pixel lines
    if (tex != batchTex_ || lines_) {
      flush();
      batchTex_ = tex;
      lines_ = false;
    }
    if (prim == Prim::Lines)
      for (size_t i = 0; i + 1 < n; i += 2) wideLine(v[i], v[i + 1]);
    else
      for (size_t i = 1; i < n; ++i) wideLine(v[i - 1], v[i]);
    return;
  }
  if (tex != batchTex_ || lines != lines_) {
    flush();
    batchTex_ = tex;
    lines_ = lines;
  }
  switch (prim) {
    case Prim::Triangles: batch_.insert(batch_.end(), v, v + n - n % 3); break;
    case Prim::Lines: batch_.insert(batch_.end(), v, v + n - n % 2); break;
    case Prim::LineStrip:
      for (size_t i = 1; i < n; ++i) { batch_.push_back(v[i - 1]); batch_.push_back(v[i]); }
      break;
    case Prim::Quads:
      for (size_t i = 0; i + 3 < n; i += 4) {
        const GuiVertex q[6] = {v[i], v[i + 1], v[i + 2], v[i], v[i + 2], v[i + 3]};
        batch_.insert(batch_.end(), q, q + 6);
      }
      break;
    case Prim::QuadStrip:
      for (size_t i = 0; i + 3 < n; i += 2) {
        const GuiVertex q[6] = {v[i], v[i + 1], v[i + 3], v[i], v[i + 3], v[i + 2]};
        batch_.insert(batch_.end(), q, q + 6);
      }
      break;
  }
}

void Renderer2D::flush() { flushWith(true); }

void Renderer2D::flushWith(bool ownProgram) {
  if (batch_.empty()) return;
  size_t bytes = batch_.size() * sizeof(GuiVertex);
  if (bytes > vboSize_) {
    vboSize_ = std::max(bytes, size_t(1) << 20);
    gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(vboSize_), nullptr, gl::STREAM_DRAW);
  } else {
    gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(vboSize_), nullptr, gl::STREAM_DRAW);  // orphan
  }
  gl::BufferSubData(gl::ARRAY_BUFFER, 0, gl::GLsizeiptr(bytes), batch_.data());
  if (ownProgram) {  // a material program binds its own textures and has no such uniform
    gl::Uniform1i(uTextured_, batchTex_ != 0);
    if (batchTex_) gl::BindTexture(gl::TEXTURE_2D, batchTex_);
  }
  gl::DrawArrays(lines_ ? gl::LINES : gl::TRIANGLES, 0, gl::GLsizei(batch_.size()));
  batch_.clear();
}

std::vector<uint8_t> Renderer2D::readPixels() const {
  std::vector<uint8_t> px(size_t(fw_) * size_t(fh_) * 4), out(px.size());
  gl::PixelStorei(gl::PACK_ALIGNMENT, 1);
  gl::ReadPixels(0, 0, fw_, fh_, gl::RGBA, gl::UNSIGNED_BYTE, px.data());
  size_t row = size_t(fw_) * 4;
  for (int y = 0; y < fh_; ++y) std::copy_n(px.data() + size_t(fh_ - 1 - y) * row, row, out.data() + size_t(y) * row);
  return out;
}

}  // namespace regs::render
