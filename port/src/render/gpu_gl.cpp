// The OpenGL 3.3 core device (docs/renderer/vulkan.md §2): every call goes straight to GL, so the picture is exactly
// what the renderer drew when it called GL itself.
#include <algorithm>
#include <memory>
#include <vector>

#include "platform/window.h"
#include "render/gl.h"
#include "render/gpu.h"

namespace regs::render::gpu {

namespace {

std::unique_ptr<Device> g_device;

gl::GLenum glFormat(Format f) {
  switch (f) {
    case Format::RGBA8: return gl::RGBA8;
    case Format::RGB8: return gl::RGB8;
    case Format::DXT1: return gl::COMPRESSED_RGBA_S3TC_DXT1;
    case Format::DXT3: return gl::COMPRESSED_RGBA_S3TC_DXT3;
    case Format::DXT5: return gl::COMPRESSED_RGBA_S3TC_DXT5;
    case Format::RGBA16F: return gl::RGBA16F;
    case Format::RGBA32F: return 0x8814;  // GL_RGBA32F
    case Format::R8: return 0x8229;       // GL_R8
    case Format::RG8: return 0x822B;      // GL_RG8
    case Format::RG16: return 0x822C;     // GL_RG16
    case Format::Depth24: return gl::DEPTH_COMPONENT24;
    case Format::None: break;
  }
  return gl::RGBA8;
}

// client layout and type of an upload; with no data, a layout that suits the storage format
void glPixels(Pixels p, Format f, gl::GLenum& layout, gl::GLenum& type) {
  switch (p) {
    case Pixels::BGRA8: layout = gl::BGRA, type = gl::UNSIGNED_BYTE; return;
    case Pixels::BGR8: layout = gl::BGR, type = gl::UNSIGNED_BYTE; return;
    case Pixels::RGBA32F: layout = gl::RGBA, type = gl::FLOAT; return;
    case Pixels::R8: layout = gl::RED, type = gl::UNSIGNED_BYTE; return;
    case Pixels::RG8: layout = 0x8227 /* GL_RG */, type = gl::UNSIGNED_BYTE; return;
    case Pixels::RG16: layout = 0x8227 /* GL_RG */, type = 0x1403 /* GL_UNSIGNED_SHORT */; return;
    case Pixels::Compressed:
    case Pixels::None: break;
  }
  switch (f) {
    case Format::RGBA16F: layout = gl::RGBA, type = gl::HALF_FLOAT; return;
    case Format::RGBA32F: layout = gl::RGBA, type = gl::FLOAT; return;
    case Format::Depth24: layout = gl::DEPTH_COMPONENT, type = gl::UNSIGNED_INT; return;
    case Format::R8: layout = gl::RED, type = gl::UNSIGNED_BYTE; return;
    case Format::RG8: layout = 0x8227, type = gl::UNSIGNED_BYTE; return;
    case Format::RG16: layout = 0x8227, type = 0x1403; return;
    case Format::RGB8: layout = gl::BGR, type = gl::UNSIGNED_BYTE; return;
    default: layout = gl::BGRA, type = gl::UNSIGNED_BYTE; return;
  }
}

gl::GLint glFilter(Filter f) {
  switch (f) {
    case Filter::Nearest: return gl::NEAREST;
    case Filter::Linear: return gl::LINEAR;
    case Filter::LinearMipLinear: return gl::LINEAR_MIPMAP_LINEAR;
    case Filter::NearestMipNearest: return gl::NEAREST_MIPMAP_NEAREST;
  }
  return gl::LINEAR;
}

gl::GLint glWrap(Wrap w) {
  switch (w) {
    case Wrap::Repeat: return gl::REPEAT;
    case Wrap::ClampToEdge: return gl::CLAMP_TO_EDGE;
    case Wrap::ClampToBorder: return gl::CLAMP_TO_BORDER;
  }
  return gl::REPEAT;
}

gl::GLenum glBlend(Blend b) {
  switch (b) {
    case Blend::Zero: return gl::ZERO;
    case Blend::One: return gl::ONE;
    case Blend::SrcAlpha: return gl::SRC_ALPHA;
    case Blend::OneMinusSrcAlpha: return gl::ONE_MINUS_SRC_ALPHA;
    case Blend::DstColor: return 0x0306;  // GL_DST_COLOR
    case Blend::SrcColor: return 0x0300;  // GL_SRC_COLOR
  }
  return gl::ONE;
}

gl::GLenum glPrim(Prim p) { return p == Prim::Lines ? gl::LINES : gl::TRIANGLES; }

class GlDevice final : public Device {
 public:
  explicit GlDevice(platform::Window& w) : window_(w) {}

  bool init(std::string* error) {
    auto proc = [](const char* n, void* u) { return static_cast<platform::Window*>(u)->glProc(n); };
    if (const char* missing = gl::load(proc, &window_)) {
      if (error) *error = std::string("OpenGL function missing: ") + missing;
      return false;
    }
    gl::Enable(gl::TEXTURE_CUBE_MAP_SEAMLESS);
    return true;
  }

  const char* name() const override { return "OpenGL"; }
  std::string info() const override {
    const auto* ver = reinterpret_cast<const char*>(gl::GetString(gl::VERSION));
    const auto* ren = reinterpret_cast<const char*>(gl::GetString(gl::RENDERER));
    gl::GLint units = 0;
    gl::GetIntegerv(0x8872 /* GL_MAX_TEXTURE_IMAGE_UNITS */, &units);
    return std::string("OpenGL ") + (ver ? ver : "?") + " / " + (ren ? ren : "?") + " / " + std::to_string(units) +
           " texture units" + (units < 16 ? " (fewer than the 16 the port needs)" : "");
  }
  float maxAnisotropy() override {
    if (!maxAniso_) {
      gl::GLint a = 1;
      gl::GetIntegerv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &a);
      maxAniso_ = std::max(1, a);
    }
    return float(maxAniso_);
  }

  // ---- textures ----
  uint32_t createTexture(TexKind kind, int) override {
    uint32_t t = 0;
    gl::GenTextures(1, &t);
    if (t >= cube_.size()) cube_.resize(size_t(t) + 1, 0);
    cube_[t] = kind == TexKind::Cube;
    return t;
  }
  void texImage(uint32_t tex, int face, int level, Format f, int w, int h, Pixels p, const void* data,
                size_t bytes) override {
    bool cube = isCube(tex);
    bindForEdit(tex);
    gl::GLenum target = cube ? gl::GLenum(gl::TEXTURE_CUBE_MAP_POSITIVE_X + face) : gl::TEXTURE_2D;
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
    if (p == Pixels::Compressed) {
      gl::CompressedTexImage2D(target, level, glFormat(f), w, h, 0, gl::GLsizei(bytes), data);
      return;
    }
    gl::GLenum layout, type;
    glPixels(p, f, layout, type);
    gl::TexImage2D(target, level, gl::GLint(glFormat(f)), w, h, 0, layout, type, data);
  }
  void texSubImage(uint32_t tex, int level, int x, int y, int w, int h, Pixels p, const void* data) override {
    bindForEdit(tex);
    gl::GLenum layout, type;
    glPixels(p, Format::None, layout, type);
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
    gl::TexSubImage2D(gl::TEXTURE_2D, level, x, y, w, h, layout, type, data);
  }
  void setSampler(uint32_t tex, const SamplerDesc& s) override {
    bool cube = isCube(tex);
    bindForEdit(tex);
    gl::GLenum t = cube ? gl::TEXTURE_CUBE_MAP : gl::TEXTURE_2D;
    if (s.maxLevel >= 0) gl::TexParameteri(t, gl::TEXTURE_MAX_LEVEL, s.maxLevel);
    gl::TexParameteri(t, gl::TEXTURE_MIN_FILTER, glFilter(s.minFilter));
    gl::TexParameteri(t, gl::TEXTURE_MAG_FILTER, glFilter(s.magFilter));
    gl::TexParameteri(t, gl::TEXTURE_WRAP_S, glWrap(s.wrapS));
    gl::TexParameteri(t, gl::TEXTURE_WRAP_T, glWrap(s.wrapT));
    if (cube) gl::TexParameteri(t, gl::TEXTURE_WRAP_R, glWrap(s.wrapR));
    if (s.borderWhite) {
      static const float border[4] = {1, 1, 1, 1};
      gl::TexParameterfv(t, gl::TEXTURE_BORDER_COLOR, border);
    }
    if (s.compareLEqual) {
      gl::TexParameteri(t, gl::TEXTURE_COMPARE_MODE, gl::COMPARE_REF_TO_TEXTURE);
      gl::TexParameteri(t, gl::TEXTURE_COMPARE_FUNC, gl::LEQUAL);
    }
  }
  void setAnisotropy(uint32_t tex, float a) override {
    bindForEdit(tex);
    gl::TexParameterf(gl::TEXTURE_2D, gl::TEXTURE_MAX_ANISOTROPY, a);
  }
  void readTextureRed(uint32_t tex, int, int, float* out) override {
    bindForEdit(tex);
    gl::PixelStorei(gl::PACK_ALIGNMENT, 4);
    gl::GetTexImage(gl::TEXTURE_2D, 0, gl::RED, gl::FLOAT, out);
    gl::BindTexture(gl::TEXTURE_2D, 0);
  }
  void destroyTexture(uint32_t tex) override {
    if (tex) gl::DeleteTextures(1, &tex);
  }

  // ---- renderbuffers ----
  uint32_t createRenderbuffer(Format f, int w, int h, int samples) override {
    uint32_t rb = 0;
    gl::GenRenderbuffers(1, &rb);
    gl::BindRenderbuffer(gl::RENDERBUFFER, rb);
    if (samples > 0) gl::RenderbufferStorageMultisample(gl::RENDERBUFFER, samples, glFormat(f), w, h);
    else gl::RenderbufferStorage(gl::RENDERBUFFER, glFormat(f), w, h);
    return rb | kRenderbufferBit;
  }
  void destroyRenderbuffer(uint32_t rb) override {
    rb &= ~kRenderbufferBit;
    if (rb) gl::DeleteRenderbuffers(1, &rb);
  }

  // ---- framebuffers ----
  uint32_t createFramebuffer(uint32_t color, uint32_t depth) override {
    uint32_t fb = 0;
    gl::GenFramebuffers(1, &fb);
    gl::BindFramebuffer(gl::FRAMEBUFFER, fb);
    auto attach = [](gl::GLenum point, uint32_t h) {
      if (h & kRenderbufferBit) gl::FramebufferRenderbuffer(gl::FRAMEBUFFER, point, gl::RENDERBUFFER, h & ~kRenderbufferBit);
      else gl::FramebufferTexture2D(gl::FRAMEBUFFER, point, gl::TEXTURE_2D, h, 0);
    };
    if (color) attach(gl::COLOR_ATTACHMENT0, color);
    if (depth) attach(gl::DEPTH_ATTACHMENT, depth);
    if (!color) {
      gl::DrawBuffer(gl::NONE);
      gl::ReadBuffer(gl::NONE);
    }
    bool ok = gl::CheckFramebufferStatus(gl::FRAMEBUFFER) == gl::FRAMEBUFFER_COMPLETE;
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    if (!ok) {
      gl::DeleteFramebuffers(1, &fb);
      return 0;
    }
    return fb;
  }
  void destroyFramebuffer(uint32_t fb) override {
    if (fb) gl::DeleteFramebuffers(1, &fb);
  }
  void bindFramebuffer(uint32_t fb) override { gl::BindFramebuffer(gl::FRAMEBUFFER, fb); }
  void blit(uint32_t src, uint32_t dst, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
            bool color, bool depth) override {
    gl::BindFramebuffer(gl::READ_FRAMEBUFFER, src);
    gl::BindFramebuffer(gl::DRAW_FRAMEBUFFER, dst);
    gl::BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                        (color ? gl::COLOR_BUFFER_BIT : 0) | (depth ? gl::DEPTH_BUFFER_BIT : 0), gl::NEAREST);
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
  }
  void readPixels(uint32_t fb, int x, int y, int w, int h, uint8_t* rgba) override {
    if (fb) gl::BindFramebuffer(gl::FRAMEBUFFER, fb);
    gl::PixelStorei(gl::PACK_ALIGNMENT, 1);
    gl::ReadPixels(x, y, w, h, gl::RGBA, gl::UNSIGNED_BYTE, rgba);
    if (fb) gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
  }

  // ---- buffers and vertex layouts ----
  uint32_t createBuffer() override {
    uint32_t b = 0;
    gl::GenBuffers(1, &b);
    return b;
  }
  void bufferData(uint32_t buf, size_t bytes, const void* data, bool stream) override {
    gl::BindBuffer(gl::ARRAY_BUFFER, buf);
    gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(bytes), data, stream ? gl::STREAM_DRAW : gl::STATIC_DRAW);
  }
  void bufferSubData(uint32_t buf, size_t offset, size_t bytes, const void* data) override {
    gl::BindBuffer(gl::ARRAY_BUFFER, buf);
    gl::BufferSubData(gl::ARRAY_BUFFER, gl::GLintptr(offset), gl::GLsizeiptr(bytes), data);
  }
  void destroyBuffer(uint32_t buf) override {
    if (buf) gl::DeleteBuffers(1, &buf);
  }
  uint32_t createVertexInput(const VertexAttrib* attribs, int count, uint32_t indexBuffer) override {
    uint32_t vao = 0;
    gl::GenVertexArrays(1, &vao);
    gl::BindVertexArray(vao);
    for (int i = 0; i < count; ++i) {
      const VertexAttrib& a = attribs[i];
      gl::BindBuffer(gl::ARRAY_BUFFER, a.buffer);
      gl::EnableVertexAttribArray(gl::GLuint(a.slot));
      gl::VertexAttribPointer(gl::GLuint(a.slot), a.components, a.type == AttribType::Float ? gl::FLOAT : gl::UNSIGNED_BYTE,
                              a.type == AttribType::UByteNorm, a.stride, reinterpret_cast<void*>(a.offset));
    }
    if (indexBuffer) gl::BindBuffer(gl::ELEMENT_ARRAY_BUFFER, indexBuffer);
    gl::BindVertexArray(0);
    return vao;
  }
  void destroyVertexInput(uint32_t vi) override {
    if (vi) gl::DeleteVertexArrays(1, &vi);
  }
  void bindVertexInput(uint32_t vi) override { gl::BindVertexArray(vi); }

  // ---- programs ----
  uint32_t createProgram(const std::string& vs, const std::string& fs,
                         const std::vector<std::pair<int, const char*>>& attribs, std::string* error) override {
    uint32_t v = compile(gl::VERTEX_SHADER, vs, error), f = v ? compile(gl::FRAGMENT_SHADER, fs, error) : 0;
    if (!v || !f) {
      if (v) gl::DeleteShader(v);
      return 0;
    }
    uint32_t prog = gl::CreateProgram();
    gl::AttachShader(prog, v);
    gl::AttachShader(prog, f);
    for (const auto& [slot, name] : attribs) gl::BindAttribLocation(prog, gl::GLuint(slot), name);
    gl::LinkProgram(prog);
    gl::DeleteShader(v);
    gl::DeleteShader(f);
    int ok = 0;
    gl::GetProgramiv(prog, gl::LINK_STATUS, &ok);
    if (!ok) {
      char log[4096];
      gl::GetProgramInfoLog(prog, sizeof log, nullptr, log);
      if (error) *error = log;
      return 0;
    }
    return prog;
  }
  void useProgram(uint32_t prog) override {
    gl::UseProgram(prog);
    program_ = prog;
  }
  uint32_t currentProgram() const override { return program_; }
  int uniformLocation(uint32_t prog, const char* name) override { return gl::GetUniformLocation(prog, name); }
  void uniform1i(int loc, int v) override { gl::Uniform1i(loc, v); }
  void uniform1f(int loc, float v) override { gl::Uniform1f(loc, v); }
  void uniform3f(int loc, float x, float y, float z) override { gl::Uniform3f(loc, x, y, z); }
  void uniform4f(int loc, float x, float y, float z, float w) override { gl::Uniform4f(loc, x, y, z, w); }
  void uniform3fv(int loc, int count, const float* v) override { gl::Uniform3fv(loc, count, v); }
  void uniform4fv(int loc, int count, const float* v) override { gl::Uniform4fv(loc, count, v); }
  void uniformMatrix3fv(int loc, int count, const float* m) override { gl::UniformMatrix3fv(loc, count, 0, m); }
  void uniformMatrix4fv(int loc, int count, const float* m) override { gl::UniformMatrix4fv(loc, count, 0, m); }

  // ---- fixed state ----
  void viewport(int x, int y, int w, int h) override { gl::Viewport(x, y, w, h); }
  void scissorTest(bool on) override { on ? gl::Enable(gl::SCISSOR_TEST) : gl::Disable(gl::SCISSOR_TEST); }
  void scissor(int x, int y, int w, int h) override { gl::Scissor(x, y, w, h); }
  void clear(bool color, bool depth, float r, float g, float b, float a, float depthValue) override {
    if (color) gl::ClearColor(r, g, b, a);
    if (depth) gl::ClearDepth(depthValue);
    gl::Clear((color ? gl::COLOR_BUFFER_BIT : 0) | (depth ? gl::DEPTH_BUFFER_BIT : 0));
  }
  void blend(bool on) override { on ? gl::Enable(gl::BLEND) : gl::Disable(gl::BLEND); }
  void blendFunc(Blend src, Blend dst) override { gl::BlendFunc(glBlend(src), glBlend(dst)); }
  void depthTest(bool on) override { on ? gl::Enable(gl::DEPTH_TEST) : gl::Disable(gl::DEPTH_TEST); }
  void depthMask(bool on) override { gl::DepthMask(on ? 1 : 0); }
  void depthFunc(Compare c) override { gl::DepthFunc(c == Compare::Less ? gl::LESS : gl::LEQUAL); }
  void cull(bool on) override { on ? gl::Enable(gl::CULL_FACE) : gl::Disable(gl::CULL_FACE); }
  void cullFace(Face f) override { gl::CullFace(f == Face::Front ? gl::FRONT : gl::BACK); }
  void polygonOffset(bool on, float factor, float units) override {
    if (on) {
      gl::Enable(gl::POLYGON_OFFSET_FILL);
      gl::PolygonOffset(factor, units);
    } else {
      gl::Disable(gl::POLYGON_OFFSET_FILL);
    }
  }
  void bindTexture(int unit, uint32_t tex, bool cube) override {
    if (unit != active_) gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(active_ = unit));
    gl::BindTexture(cube ? gl::TEXTURE_CUBE_MAP : gl::TEXTURE_2D, tex);
  }

  // ---- drawing ----
  void drawArrays(Prim p, int first, int count) override { gl::DrawArrays(glPrim(p), first, count); }
  void drawElements(Prim p, int count, size_t firstIndex) override {
    gl::DrawElements(glPrim(p), count, gl::UNSIGNED_INT, reinterpret_cast<void*>(firstIndex * 4));
  }

  // ---- frame ----
  void beginFrame(int, int) override {}
  void present() override { window_.swap(); }
  void setVsync(bool on) override { window_.setVsync(on); }

 private:
  static uint32_t compile(gl::GLenum type, const std::string& src, std::string* error) {
    uint32_t s = gl::CreateShader(type);
    const char* p = src.c_str();
    gl::ShaderSource(s, 1, &p, nullptr);
    gl::CompileShader(s);
    int ok = 0;
    gl::GetShaderiv(s, gl::COMPILE_STATUS, &ok);
    if (!ok) {
      char log[4096];
      gl::GetShaderInfoLog(s, sizeof log, nullptr, log);
      if (error) *error = log;
      gl::DeleteShader(s);
      return 0;
    }
    return s;
  }
  bool isCube(uint32_t tex) const { return tex < cube_.size() && cube_[tex]; }
  // texture edits bind on unit 0, as the renderer did before the device existed
  void bindForEdit(uint32_t tex) {
    if (active_ != 0) gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(active_ = 0));
    gl::BindTexture(isCube(tex) ? gl::TEXTURE_CUBE_MAP : gl::TEXTURE_2D, tex);
  }

  platform::Window& window_;
  std::vector<uint8_t> cube_;  // per texture name: a cube map
  uint32_t program_ = 0;
  int active_ = 0;
  int maxAniso_ = 0;
};

}  // namespace

Device& dev() { return *g_device; }
void setDevice(std::unique_ptr<Device> d) { g_device = std::move(d); }
bool hasDevice() { return g_device != nullptr; }

std::unique_ptr<Device> createGlDevice(platform::Window& window, std::string* error) {
  auto d = std::make_unique<GlDevice>(window);
  if (!d->init(error)) return nullptr;
  return d;
}

}  // namespace regs::render::gpu
