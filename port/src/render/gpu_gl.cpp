// The OpenGL 3.3 core device (docs/renderer/vulkan.md §2): every call goes straight to GL, so the picture is exactly
// what the renderer drew when it called GL itself.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <unordered_map>
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
  static constexpr gl::GLenum kBoth[2] = {gl::COLOR_ATTACHMENT0, gl::COLOR_ATTACHMENT1};
  std::vector<uint32_t> color1Fbs_;  // framebuffers with a second colour attachment (attachColor1)
  bool hasAttachment1(uint32_t fb) const { return fb && std::find(color1Fbs_.begin(), color1Fbs_.end(), fb) != color1Fbs_.end(); }

 public:
  explicit GlDevice(platform::Window& w) : window_(w) {}

  bool init(std::string* error) {
    auto proc = [](const char* n, void* u) { return static_cast<platform::Window*>(u)->glProc(n); };
    if (const char* missing = gl::load(proc, &window_)) {
      if (error) *error = std::string("OpenGL function missing: ") + missing;
      return false;
    }
    gl::Enable(gl::TEXTURE_CUBE_MAP_SEAMLESS);
    // parallel shader compiles (a batch is queued before its status is asked, queueProgram): the driver picks the
    // thread count (0xFFFFFFFF, GL_KHR_parallel_shader_compile §"MaxShaderCompilerThreadsKHR")
    gl::GLint n = 0;
    gl::GetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &n);
    bool khr = false, arb = false;
    for (gl::GLint i = 0; i < n; ++i) {
      const char* e = reinterpret_cast<const char*>(gl::GetStringi(0x1F03 /* GL_EXTENSIONS */, gl::GLuint(i)));
      if (!e) continue;
      khr = khr || std::strcmp(e, "GL_KHR_parallel_shader_compile") == 0;
      arb = arb || std::strcmp(e, "GL_ARB_parallel_shader_compile") == 0;
    }
    using MaxThreads = void(REGS_GLAPI*)(gl::GLuint);
    if (auto f = reinterpret_cast<MaxThreads>(khr ? window_.glProc("glMaxShaderCompilerThreadsKHR")
                                             : arb ? window_.glProc("glMaxShaderCompilerThreadsARB") : nullptr)) {
      f(0xFFFFFFFFu);
      parallelCompile_ = true;
    }
    programBinary_ = reinterpret_cast<ProgramBinaryFn>(window_.glProc("glProgramBinary"));
    getProgramBinary_ = reinterpret_cast<GetProgramBinaryFn>(window_.glProc("glGetProgramBinary"));
    programParameteri_ = reinterpret_cast<ProgramParameteriFn>(window_.glProc("glProgramParameteri"));
    return true;
  }

  const char* name() const override { return "OpenGL"; }
  std::string info() const override {
    const auto* ver = reinterpret_cast<const char*>(gl::GetString(gl::VERSION));
    const auto* ren = reinterpret_cast<const char*>(gl::GetString(gl::RENDERER));
    gl::GLint units = 0;
    gl::GetIntegerv(0x8872 /* GL_MAX_TEXTURE_IMAGE_UNITS */, &units);
    return std::string("OpenGL ") + (ver ? ver : "?") + " / " + (ren ? ren : "?") + " / " + std::to_string(units) +
           " texture units" + (units < 16 ? " (fewer than the 16 the port needs)" : "") +
           (parallelCompile_ ? " / parallel shader compile" : "");
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
  // a ring of pixel pack buffers per texture, each with a fence: glGetTexImage into a free one returns at once, the
  // copy is read when its fence has passed (polled with a zero timeout). When every buffer is still in flight (the GPU
  // is kRedReads frames behind) this frame's copy is left out rather than waited for.
  void queueReadRed(uint32_t tex, int w, int h) override {
    if (!tex || w <= 0 || h <= 0) return;
    auto& ring = redReads_[tex];
    RedRead* slot = nullptr;
    for (RedRead& r : ring)
      if (!r.fence) { slot = &r; break; }
    if (!slot) return;
    gl::GLsizeiptr bytes = gl::GLsizeiptr(w) * h * gl::GLsizeiptr(sizeof(float));
    if (!slot->pbo) gl::GenBuffers(1, &slot->pbo);
    gl::BindBuffer(gl::PIXEL_PACK_BUFFER, slot->pbo);
    if (slot->bytes != bytes) gl::BufferData(gl::PIXEL_PACK_BUFFER, slot->bytes = bytes, nullptr, gl::STREAM_READ);
    bindForEdit(tex);
    gl::PixelStorei(gl::PACK_ALIGNMENT, 4);
    gl::GetTexImage(gl::TEXTURE_2D, 0, gl::RED, gl::FLOAT, nullptr);  // offset 0 in the bound pack buffer
    gl::BindTexture(gl::TEXTURE_2D, 0);
    gl::BindBuffer(gl::PIXEL_PACK_BUFFER, 0);  // glReadPixels elsewhere writes to client memory again
    slot->fence = gl::FenceSync(gl::SYNC_GPU_COMMANDS_COMPLETE, 0);
    slot->serial = ++redSerial_;
  }
  bool takeReadRed(uint32_t tex, std::vector<float>& out) override {
    auto it = redReads_.find(tex);
    if (it == redReads_.end()) return false;
    RedRead* newest = nullptr;
    for (RedRead& r : it->second) {
      if (!r.fence) continue;
      gl::GLenum s = gl::ClientWaitSync(r.fence, gl::SYNC_FLUSH_COMMANDS_BIT, 0);
      if (s != gl::ALREADY_SIGNALED && s != gl::CONDITION_SATISFIED && s != gl::WAIT_FAILED) continue;
      gl::DeleteSync(r.fence);
      r.fence = nullptr;
      if (s != gl::WAIT_FAILED && (!newest || r.serial > newest->serial)) newest = &r;  // older finished ones: dropped
    }
    if (!newest) return false;
    out.resize(size_t(newest->bytes) / sizeof(float));
    gl::BindBuffer(gl::PIXEL_PACK_BUFFER, newest->pbo);
    gl::GetBufferSubData(gl::PIXEL_PACK_BUFFER, 0, newest->bytes, out.data());
    gl::BindBuffer(gl::PIXEL_PACK_BUFFER, 0);
    return true;
  }
  void destroyTexture(uint32_t tex) override {
    if (auto it = redReads_.find(tex); it != redReads_.end()) {
      for (RedRead& r : it->second) {
        if (r.fence) gl::DeleteSync(r.fence);
        if (r.pbo) gl::DeleteBuffers(1, &r.pbo);
      }
      redReads_.erase(it);
    }
    if (tex) gl::DeleteTextures(1, &tex);
  }
  int videoMemoryInUseMB() override {
    gl::GLint total = 0, avail = 0;
    gl::GetIntegerv(0x9048, &total);  // GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX (KB)
    gl::GetIntegerv(0x9049, &avail);  // GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX (KB)
    for (int i = 0; i < 8 && gl::GetError() != 0; ++i) {}  // no such enums without the extension
    return total > 0 ? int((total - avail) / 1024) : 0;
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
    std::erase(color1Fbs_, fb);
  }
  void bindFramebuffer(uint32_t fb) override { gl::BindFramebuffer(gl::FRAMEBUFFER, fb); }
  void blit(uint32_t src, uint32_t dst, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
            bool color, bool depth) override {
    gl::BindFramebuffer(gl::READ_FRAMEBUFFER, src);
    gl::BindFramebuffer(gl::DRAW_FRAMEBUFFER, dst);
    const bool two = color && hasAttachment1(dst);
    static const gl::GLenum first = gl::COLOR_ATTACHMENT0;
    if (two) gl::DrawBuffers(1, &first);  // the colour into attachment 0 only
    gl::BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                        (color ? gl::COLOR_BUFFER_BIT : 0) | (depth ? gl::DEPTH_BUFFER_BIT : 0), gl::NEAREST);
    if (two) gl::DrawBuffers(2, kBoth);
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
  }
  bool hasColor1() const override { return true; }
  bool attachColor1(uint32_t fb, uint32_t color) override {
    if (!fb || !color) return false;
    gl::BindFramebuffer(gl::FRAMEBUFFER, fb);
    if (color & kRenderbufferBit)
      gl::FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT1, gl::RENDERBUFFER, color & ~kRenderbufferBit);
    else
      gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT1, gl::TEXTURE_2D, color, 0);
    gl::DrawBuffers(2, kBoth);
    bool ok = gl::CheckFramebufferStatus(gl::FRAMEBUFFER) == gl::FRAMEBUFFER_COMPLETE;
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    if (ok && !hasAttachment1(fb)) color1Fbs_.push_back(fb);
    return ok;
  }
  void blitColor1(uint32_t src, uint32_t dst, int w, int h) override {
    static const gl::GLenum second[2] = {gl::NONE, gl::COLOR_ATTACHMENT1};
    gl::BindFramebuffer(gl::READ_FRAMEBUFFER, src);
    gl::BindFramebuffer(gl::DRAW_FRAMEBUFFER, dst);
    gl::ReadBuffer(gl::COLOR_ATTACHMENT1);
    gl::DrawBuffers(2, second);
    gl::BlitFramebuffer(0, 0, w, h, 0, 0, w, h, gl::COLOR_BUFFER_BIT, gl::NEAREST);
    gl::ReadBuffer(gl::COLOR_ATTACHMENT0);
    gl::DrawBuffers(2, kBoth);
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
  }
  void clearColor1() override {
    static const float zero[4] = {0, 0, 0, 0};
    gl::ClearBufferfv(gl::COLOR, 1, zero);
  }
  void colorMask1(bool write) override { gl::ColorMaski(1, write, write, write, write); }
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
    return finishProgram(queueProgram(vs, fs, attribs), error);
  }
  // compile and link issued, no status asked (that would wait for the driver): the ticket is the program. With the
  // program cache, a binary saved before is given to the driver instead (no compile at all).
  uint32_t queueProgram(const std::string& vs, const std::string& fs,
                        const std::vector<std::pair<int, const char*>>& attribs) override {
    Queued q;
    if (!cacheDir_.empty()) {
      q.key = programKey(vs, fs, attribs);
      std::vector<uint8_t> bin;
      if (readFile(cacheDir_ / (q.key + L".bin"), bin) && bin.size() > 4) {
        uint32_t prog = gl::CreateProgram();
        uint32_t format = uint32_t(bin[0]) | uint32_t(bin[1]) << 8 | uint32_t(bin[2]) << 16 | uint32_t(bin[3]) << 24;
        programBinary_(prog, format, bin.data() + 4, gl::GLsizei(bin.size() - 4));
        q.fromCache = true;
        q.vs = vs, q.fs = fs;  // compiled after all when the driver refuses the binary
        for (const auto& [slot, name] : attribs) q.attribs.emplace_back(slot, name);
        queuedPrograms_[prog] = std::move(q);
        return prog;
      }
    }
    uint32_t prog = issueProgram(vs, fs, attribs, q);
    queuedPrograms_[prog] = std::move(q);
    return prog;
  }
  // the first error as createProgram always reported it: the vertex stage's compile log, the fragment stage's, the link
  uint32_t finishProgram(uint32_t prog, std::string* error) override {
    auto it = queuedPrograms_.find(prog);
    if (it == queuedPrograms_.end()) return 0;
    Queued q = std::move(it->second);
    queuedPrograms_.erase(it);
    if (q.fromCache) {
      int linked = 0;
      gl::GetProgramiv(prog, gl::LINK_STATUS, &linked);
      if (linked) return prog;
      gl::DeleteProgram(prog);  // another driver build or a damaged file: compile it
      std::vector<std::pair<int, const char*>> attribs;
      for (const auto& [slot, name] : q.attribs) attribs.emplace_back(slot, name.c_str());
      Queued again;
      again.key = q.key;
      prog = issueProgram(q.vs, q.fs, attribs, again);
      q = std::move(again);
    }
    auto [v, f] = std::pair(q.shaderV, q.shaderF);
    char log[4096];
    int ok = 0;
    for (uint32_t s : {v, f}) {
      gl::GetShaderiv(s, gl::COMPILE_STATUS, &ok);
      if (!ok) {
        gl::GetShaderInfoLog(s, sizeof log, nullptr, log);
        break;
      }
    }
    if (ok) {
      gl::GetProgramiv(prog, gl::LINK_STATUS, &ok);
      if (!ok) gl::GetProgramInfoLog(prog, sizeof log, nullptr, log);
    }
    gl::DeleteShader(v);  // attached: freed with the program
    gl::DeleteShader(f);
    if (!ok) {
      if (error) *error = log;
      gl::DeleteProgram(prog);
      return 0;
    }
    if (!q.key.empty()) saveBinary(prog, q.key);
    return prog;
  }
  // The program cache (optional; <user folder>\cache\gl, engine/display.cpp): linked programs as the driver's
  // binaries (GL_ARB_get_program_binary), one file per program named by a hash of its sources and attribute slots,
  // in a folder per driver (a hash of the vendor, renderer and version strings), so another GPU or driver never
  // gets a binary it did not make. A binary the driver refuses anyway is compiled as usual and saved again.
  void setProgramCache(const std::filesystem::path& dir) override {
    cacheDir_.clear();
    if (dir.empty() || !programBinary_ || !getProgramBinary_ || !programParameteri_) return;
    gl::GLint formats = 0;
    gl::GetIntegerv(0x87FE /* GL_NUM_PROGRAM_BINARY_FORMATS */, &formats);
    if (formats <= 0) return;
    std::string id;
    for (gl::GLenum e : {gl::VENDOR, gl::RENDERER, gl::VERSION}) {
      const char* s = reinterpret_cast<const char*>(gl::GetString(e));
      id += std::string(s ? s : "?") + "\n";
    }
    std::filesystem::path d = dir / hex(fnv(id));
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    if (ec) return;
    cacheDir_ = d;
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
  // GPU frame time (GL 3.3 timer queries): a timestamp pair per frame in a ring of 4, read when the slot comes round
  void beginFrame(int, int) override {
    Timing& t = timing_[timingSlot_];
    if (!t.q[0]) gl::GenQueries(2, t.q);
    if (t.pending) {
      unsigned long long a = 0, b = 0;
      gl::GetQueryObjectui64v(t.q[0], 0x8866 /* GL_QUERY_RESULT */, &a);
      gl::GetQueryObjectui64v(t.q[1], 0x8866, &b);
      if (b > a) gpuMs_ = double(b - a) / 1e6;
      t.pending = false;
    }
    gl::QueryCounter(t.q[0], 0x8E28 /* GL_TIMESTAMP */);
    t.begun = true;
  }
  void present() override {
    Timing& t = timing_[timingSlot_];
    if (t.begun) {
      gl::QueryCounter(t.q[1], 0x8E28);
      t.pending = true;
      t.begun = false;
      timingSlot_ = (timingSlot_ + 1) % 4;
    }
    window_.swap();
  }
  double gpuFrameMs() const override { return gpuMs_; }
  void setVsync(bool on) override { window_.setVsync(on); }

 private:
  static uint32_t issueShader(gl::GLenum type, const std::string& src) {
    uint32_t s = gl::CreateShader(type);
    const char* p = src.c_str();
    gl::ShaderSource(s, 1, &p, nullptr);
    gl::CompileShader(s);
    return s;
  }
  // a program queued: its shaders while they compile, or the sources of one loaded from the cache
  struct Queued {
    uint32_t shaderV = 0, shaderF = 0;
    std::wstring key;  // the cache file's name ("" without the cache)
    bool fromCache = false;
    std::string vs, fs;
    std::vector<std::pair<int, std::string>> attribs;
  };
  uint32_t issueProgram(const std::string& vs, const std::string& fs, const std::vector<std::pair<int, const char*>>& attribs,
                        Queued& q) {
    q.shaderV = issueShader(gl::VERTEX_SHADER, vs);
    q.shaderF = issueShader(gl::FRAGMENT_SHADER, fs);
    uint32_t prog = gl::CreateProgram();
    gl::AttachShader(prog, q.shaderV);
    gl::AttachShader(prog, q.shaderF);
    for (const auto& [slot, name] : attribs) gl::BindAttribLocation(prog, gl::GLuint(slot), name);
    if (!q.key.empty()) programParameteri_(prog, 0x8257 /* GL_PROGRAM_BINARY_RETRIEVABLE_HINT */, 1);
    gl::LinkProgram(prog);
    return prog;
  }
  static uint64_t fnv(const std::string& s, uint64_t h = 0xcbf29ce484222325ull) {
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
  }
  static std::wstring hex(uint64_t v) {
    wchar_t b[17];
    std::swprintf(b, 17, L"%016llx", static_cast<unsigned long long>(v));
    return b;
  }
  static std::wstring programKey(const std::string& vs, const std::string& fs,
                                 const std::vector<std::pair<int, const char*>>& attribs) {
    std::string a;
    for (const auto& [slot, name] : attribs) a += std::to_string(slot) + "=" + name + ";";
    // two hashes of the whole input (sources, slots): 128 bits, so two programs never share a file
    std::string all = vs + '\0' + fs + '\0' + a;
    return hex(fnv(all)) + hex(fnv(all, 0x84222325cbf29ce4ull));
  }
  static bool readFile(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(p.wstring().c_str(), L"rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
  }
  void saveBinary(uint32_t prog, const std::wstring& key) {
    gl::GLint len = 0;
    gl::GetProgramiv(prog, 0x8741 /* GL_PROGRAM_BINARY_LENGTH */, &len);
    if (len <= 0) return;
    std::vector<uint8_t> bin(size_t(len) + 4);
    gl::GLenum format = 0;
    gl::GLsizei got = 0;
    getProgramBinary_(prog, len, &got, &format, bin.data() + 4);
    if (got <= 0) return;
    for (int i = 0; i < 4; ++i) bin[size_t(i)] = uint8_t(format >> (8 * i));
    std::filesystem::path file = cacheDir_ / (key + L".bin"), tmp = file;
    tmp += L".tmp";
    if (FILE* f = _wfopen(tmp.wstring().c_str(), L"wb")) {
      bool ok = std::fwrite(bin.data(), 1, size_t(got) + 4, f) == size_t(got) + 4;
      ok = std::fclose(f) == 0 && ok;
      std::error_code ec;
      if (ok) std::filesystem::rename(tmp, file, ec);
      if (!ok || ec) std::filesystem::remove(tmp, ec);
    }
  }
  bool isCube(uint32_t tex) const { return tex < cube_.size() && cube_[tex]; }
  // texture edits bind on unit 0, as the renderer did before the device existed
  void bindForEdit(uint32_t tex) {
    if (active_ != 0) gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(active_ = 0));
    gl::BindTexture(isCube(tex) ? gl::TEXTURE_CUBE_MAP : gl::TEXTURE_2D, tex);
  }

  platform::Window& window_;
  struct Timing {
    uint32_t q[2] = {0, 0};
    bool begun = false, pending = false;
  };
  Timing timing_[4];
  int timingSlot_ = 0;
  static constexpr int kRedReads = 4;  // queueReadRed's buffers per texture
  struct RedRead {
    uint32_t pbo = 0;
    gl::GLsync fence = nullptr;  // set while the copy is in flight or finished but not taken
    gl::GLsizeiptr bytes = 0;
    uint64_t serial = 0;
  };
  std::unordered_map<uint32_t, std::array<RedRead, kRedReads>> redReads_;  // by texture
  uint64_t redSerial_ = 0;
  double gpuMs_ = -1;
  std::vector<uint8_t> cube_;  // per texture name: a cube map
  std::unordered_map<uint32_t, Queued> queuedPrograms_;  // by program
  bool parallelCompile_ = false;  // GL_KHR/ARB_parallel_shader_compile, switched to the driver's thread count
  std::filesystem::path cacheDir_;  // the program cache's folder for this driver; empty: off
  using ProgramBinaryFn = void(REGS_GLAPI*)(gl::GLuint, gl::GLenum, const void*, gl::GLsizei);
  using GetProgramBinaryFn = void(REGS_GLAPI*)(gl::GLuint, gl::GLsizei, gl::GLsizei*, gl::GLenum*, void*);
  using ProgramParameteriFn = void(REGS_GLAPI*)(gl::GLuint, gl::GLenum, gl::GLint);
  ProgramBinaryFn programBinary_ = nullptr;  // GL 4.1 / GL_ARB_get_program_binary
  GetProgramBinaryFn getProgramBinary_ = nullptr;
  ProgramParameteriFn programParameteri_ = nullptr;
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
