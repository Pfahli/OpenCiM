#include "render/renderer2d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "render/gpu.h"

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

}  // namespace

Renderer2D::~Renderer2D() = default;  // GPU objects die with the device

bool Renderer2D::init(std::string* error) {
  gpu::Device& d = gpu::dev();
  program_ = d.createProgram(kVertex, kFragment, {}, error);
  if (!program_) return false;
  uMatrix_ = d.uniformLocation(program_, "uMatrix");
  uTextured_ = d.uniformLocation(program_, "uTextured");
  uTex_ = d.uniformLocation(program_, "uTex");
  uSharp_ = d.uniformLocation(program_, "uSharp");
  vbo_ = d.createBuffer();
  const gpu::VertexAttrib attrs[3] = {
      {0, 2, gpu::AttribType::Float, vbo_, int(sizeof(GuiVertex)), offsetof(GuiVertex, x)},
      {1, 2, gpu::AttribType::Float, vbo_, int(sizeof(GuiVertex)), offsetof(GuiVertex, u)},
      {2, 4, gpu::AttribType::UByteNorm, vbo_, int(sizeof(GuiVertex)), offsetof(GuiVertex, rgba)}};
  vao_ = d.createVertexInput(attrs, 3, 0);
  info = d.info();
  return true;
}

uint32_t Renderer2D::createTexture(const Image& img, uint32_t reuse) {
  flush();  // binding a texture below must not disturb a pending batch
  gpu::Device& d = gpu::dev();
  int faces = img.cube ? 6 : 1, mips = img.mipCount;
  uint32_t t = reuse;
  if (!t) {
    t = d.createTexture(img.cube ? gpu::TexKind::Cube : gpu::TexKind::Tex2D, mips);
  } else {  // the same handle with new contents (texture upscaling swaps loaded textures live)
    std::erase(mipTextures_, t);  // listed again below when it still has mipmaps
    d.redefineTexture(t, mips);
  }
  for (int f = 0; f < faces; ++f) {
    // DDS cube faces come in GL order: +X, -X, +Y, -Y, +Z, -Z
    for (int m = 0; m < mips; ++m) {
      size_t li = size_t(f * mips + m);
      if (li >= img.levels.size()) break;
      int w = std::max(1, img.width >> m), h = std::max(1, img.height >> m);
      const auto& lv = img.levels[li];
      switch (img.format) {
        case Image::Format::DXT1: d.texImage(t, f, m, gpu::Format::DXT1, w, h, gpu::Pixels::Compressed, lv.data(), lv.size()); break;
        case Image::Format::DXT3: d.texImage(t, f, m, gpu::Format::DXT3, w, h, gpu::Pixels::Compressed, lv.data(), lv.size()); break;
        case Image::Format::DXT5: d.texImage(t, f, m, gpu::Format::DXT5, w, h, gpu::Pixels::Compressed, lv.data(), lv.size()); break;
        case Image::Format::BGR8: d.texImage(t, f, m, gpu::Format::RGB8, w, h, gpu::Pixels::BGR8, lv.data()); break;
        default: d.texImage(t, f, m, gpu::Format::RGBA8, w, h, gpu::Pixels::BGRA8, lv.data()); break;
      }
    }
  }
  gpu::SamplerDesc s;
  s.maxLevel = mips - 1;
  s.minFilter = mips > 1 ? gpu::Filter::LinearMipLinear : gpu::Filter::Linear;
  s.magFilter = gpu::Filter::Linear;
  s.wrapS = s.wrapT = s.wrapR = img.cube ? gpu::Wrap::ClampToEdge : gpu::Wrap::Repeat;
  d.setSampler(t, s);
  if (!img.cube && mips > 1) {
    mipTextures_.push_back(t);
    if (anisotropy > 1) applyAnisotropy(t);
  }
  return t;
}

int Renderer2D::videoMemoryInUseMB() { return gpu::dev().videoMemoryInUseMB(); }

uint32_t Renderer2D::createTextureMipmapped(const uint8_t* px, int w, int h, bool rgba, bool clamp, uint32_t reuse) {
  Image img;
  img.width = w, img.height = h;
  std::vector<uint8_t> level(px, px + size_t(w) * size_t(h) * 4);
  if (rgba)
    for (size_t i = 0; i < level.size(); i += 4) std::swap(level[i], level[i + 2]);
  img.levels.push_back(std::move(level));
  while (w > 1 || h > 1) {  // box filter, edges repeated
    int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    const std::vector<uint8_t>& src = img.levels.back();
    std::vector<uint8_t> dst(size_t(nw) * size_t(nh) * 4);
    for (int y = 0; y < nh; ++y)
      for (int x = 0; x < nw; ++x)
        for (int c = 0; c < 4; ++c) {
          int sum = 0;
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
              sum += src[(size_t(std::min(h - 1, 2 * y + dy)) * size_t(w) + size_t(std::min(w - 1, 2 * x + dx))) * 4 + size_t(c)];
          dst[(size_t(y) * size_t(nw) + size_t(x)) * 4 + size_t(c)] = uint8_t((sum + 2) / 4);
        }
    img.levels.push_back(std::move(dst));
    w = nw, h = nh;
  }
  img.mipCount = int(img.levels.size());
  uint32_t t = createTexture(img, reuse);
  if (clamp) {
    gpu::SamplerDesc s;
    s.maxLevel = img.mipCount - 1;
    s.minFilter = img.mipCount > 1 ? gpu::Filter::LinearMipLinear : gpu::Filter::Linear;
    s.magFilter = gpu::Filter::Linear;
    s.wrapS = s.wrapT = s.wrapR = gpu::Wrap::ClampToEdge;
    gpu::dev().setSampler(t, s);
    if (anisotropy > 1 && img.mipCount > 1) applyAnisotropy(t);
  }
  return t;
}

uint32_t Renderer2D::createTextureBGRA(const uint8_t* bgra, int w, int h) {
  flush();
  gpu::Device& d = gpu::dev();
  uint32_t t = d.createTexture(gpu::TexKind::Tex2D, 1);
  d.texImage(t, 0, 0, gpu::Format::RGBA8, w, h, gpu::Pixels::BGRA8, bgra);
  gpu::SamplerDesc s;
  s.maxLevel = 0;
  d.setSampler(t, s);
  return t;
}

void Renderer2D::updateTextureBGRA(uint32_t tex, const uint8_t* bgra, int w, int h) {
  flush();
  gpu::dev().texSubImage(tex, 0, 0, 0, w, h, gpu::Pixels::BGRA8, bgra);
}

void Renderer2D::applyAnisotropy(uint32_t tex) {
  // gsVideo_OGL clamps it to GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT; 1 = plain trilinear
  if (!maxAnisotropy_) maxAnisotropy_ = std::max(1, int(gpu::dev().maxAnisotropy()));
  gpu::dev().setAnisotropy(tex, std::clamp(anisotropy, 1.0f, float(maxAnisotropy_)));
}

void Renderer2D::setAnisotropy(float a) {
  if (a == anisotropy) return;
  flush();
  anisotropy = a;
  for (uint32_t t : mipTextures_) applyAnisotropy(t);
}

void Renderer2D::destroyTexture(uint32_t tex) {
  if (!tex) return;
  if (tex == batchTex_) flush();
  std::erase(mipTextures_, tex);
  gpu::dev().destroyTexture(tex);
}

void Renderer2D::beginFrame(int width, int height, uint32_t clearArgb) {
  gpu::Device& d = gpu::dev();
  fw_ = width;
  fh_ = height;
  batch_.clear();
  d.beginFrame(width, height);
  d.viewport(0, 0, width, height);
  d.scissorTest(false);
  d.clear(true, true, float(clearArgb >> 16 & 0xFF) / 255, float(clearArgb >> 8 & 0xFF) / 255, float(clearArgb & 0xFF) / 255, 1);
  d.depthTest(false);
  d.cull(false);
  d.blend(true);
  d.blendFunc(gpu::Blend::SrcAlpha, gpu::Blend::OneMinusSrcAlpha);
  d.useProgram(program_);
  d.uniform1i(uTex_, 0);
  d.uniform1f(uSharp_, sharpScale > 1 ? sharpScale : 0.0f);
  d.bindVertexInput(vao_);
  setProjection(0, 0, float(width), float(height), float(width), float(height));
}

void Renderer2D::endFrame() { flush(); }

void Renderer2D::resume() {
  gpu::Device& d = gpu::dev();
  d.depthTest(false);
  d.cull(false);
  d.blend(true);
  d.blendFunc(gpu::Blend::SrcAlpha, gpu::Blend::OneMinusSrcAlpha);
  d.useProgram(program_);
  d.uniform1i(uTex_, 0);
  d.bindVertexInput(vao_);
  setProjection(proj_[0], proj_[1], proj_[2], proj_[3], proj_[4], proj_[5]);
  setScissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
}

void Renderer2D::setProjection(float px, float py, float pw, float ph, float unitsW, float unitsH) {
  flush();
  const float saved[6] = {px, py, pw, ph, unitsW, unitsH};
  std::copy(saved, saved + 6, proj_);
  gpu::dev().viewport(int(px), int(float(fh_) - py - ph), int(pw), int(ph));
  // ortho(0, unitsW, unitsH, 0): column-major
  const float m[16] = {2 / unitsW, 0, 0, 0, 0, -2 / unitsH, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
  std::copy(m, m + 16, mvp_);
  gpu::dev().uniformMatrix4fv(uMatrix_, 1, m);
}

uint32_t Renderer2D::buildProgram(const std::string& vs, const std::string& fs, std::string* error) {
  // GUI vertices: position, uv, colour at the slots of the 2D vertex array
  return gpu::dev().createProgram(vs, fs, {{0, "regs_Vertex"}, {1, "regs_MultiTexCoord0"}, {2, "regs_Color"}}, error);
}

void Renderer2D::drawWithProgram(uint32_t prog, const uint32_t* textures, int textureCount, Prim prim, const GuiVertex* v,
                                 size_t n, const float* custom0) {
  flush();
  gpu::Device& d = gpu::dev();
  d.useProgram(prog);
  d.uniformMatrix4fv(d.uniformLocation(prog, "regs_ModelViewProjectionMatrix"), 1, mvp_);
  if (custom0) d.uniform4fv(d.uniformLocation(prog, "custom0"), 1, custom0);
  if (int loc = d.uniformLocation(prog, "timer"); loc >= 0) d.uniform1f(loc, timer);
  for (int i = 0; i < textureCount; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "texture%d", i);
    d.uniform1i(d.uniformLocation(prog, name), i);
    d.bindTexture(i, textures[i]);
  }
  uint32_t keepTex = batchTex_;
  draw(textures && textureCount ? textures[0] : 0, prim, v, n);
  flushWith(false);
  batchTex_ = keepTex;
  d.useProgram(program_);
}

void Renderer2D::setScissor(int x, int y, int w, int h) {
  flush();
  scissor_[0] = x;
  scissor_[1] = y;
  scissor_[2] = w;
  scissor_[3] = h;
  if (w < 0 || h < 0) {
    gpu::dev().scissorTest(false);
    return;
  }
  gpu::dev().scissorTest(true);
  gpu::dev().scissor(x, fh_ - y - h, std::max(0, w), std::max(0, h));
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
  gpu::Device& d = gpu::dev();
  size_t bytes = batch_.size() * sizeof(GuiVertex);
  if (bytes > vboSize_) vboSize_ = std::max(bytes, size_t(1) << 20);
  d.bufferData(vbo_, vboSize_, nullptr, true);  // orphan
  d.bufferSubData(vbo_, 0, bytes, batch_.data());
  if (ownProgram) {  // a material program binds its own textures and has no such uniform
    d.uniform1i(uTextured_, batchTex_ != 0);
    if (batchTex_) d.bindTexture(0, batchTex_);
  }
  d.drawArrays(lines_ ? gpu::Prim::Lines : gpu::Prim::Triangles, 0, int(batch_.size()));
  batch_.clear();
}

std::vector<uint8_t> Renderer2D::readPixels() const {
  std::vector<uint8_t> px(size_t(fw_) * size_t(fh_) * 4), out(px.size());
  gpu::dev().readPixels(0, 0, 0, fw_, fh_, px.data());
  size_t row = size_t(fw_) * 4;
  for (int y = 0; y < fh_; ++y) std::copy_n(px.data() + size_t(fh_ - 1 - y) * row, row, out.data() + size_t(y) * row);
  return out;
}

}  // namespace regs::render
