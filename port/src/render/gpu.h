#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace regs::platform {
class Window;
}

// The render backend boundary (docs/renderer/vulkan.md §2): a small state machine with handles, shaped like the subset
// of OpenGL 3.3 the port uses. The renderer (renderer2d/3d) and the engine's data textures talk only to this; the
// OpenGL device (gpu_gl.cpp) passes every call straight to GL, the Vulkan device (gpu_vk.cpp) emulates it.
//
// Conventions are OpenGL's everywhere: viewport and scissor rectangles count rows from the bottom, texture rows are
// stored in the order they are given (row 0 first), clip-space depth runs −1..1, front faces are counter-clockwise.
// Handles are uint32_t, 0 = none. Renderbuffer handles have the top bit set (kRenderbufferBit) so they never collide
// with texture handles.
namespace regs::render::gpu {

enum class TexKind : uint8_t { Tex2D, Cube };
// storage formats; RGBA8 holds what the engine uploads as BGRA bytes, never sRGB (the engine has no sRGB anywhere)
enum class Format : uint8_t { None, RGBA8, RGB8, DXT1, DXT3, DXT5, RGBA16F, RGBA32F, R8, RG8, RG16, Depth24 };
// client pixel layouts of uploads (tightly packed rows)
enum class Pixels : uint8_t { None, BGRA8, BGR8, RGBA32F, R8, RG8, RG16, Compressed };
enum class Filter : uint8_t { Nearest, Linear, LinearMipLinear, NearestMipNearest };
enum class Wrap : uint8_t { Repeat, ClampToEdge, ClampToBorder };
enum class Blend : uint8_t { Zero, One, SrcAlpha, OneMinusSrcAlpha, DstColor, SrcColor };
enum class Compare : uint8_t { Less, LEqual };
enum class Face : uint8_t { Back, Front };
enum class Prim : uint8_t { Triangles, Lines };
enum class AttribType : uint8_t { Float, UByteNorm };

constexpr uint32_t kRenderbufferBit = 0x80000000u;

struct SamplerDesc {
  Filter minFilter = Filter::Linear, magFilter = Filter::Linear;
  Wrap wrapS = Wrap::Repeat, wrapT = Wrap::Repeat, wrapR = Wrap::Repeat;
  int maxLevel = -1;            // highest mip level sampled; −1 leaves it
  bool borderWhite = false;     // ClampToBorder colour (1, 1, 1, 1) (shadow maps: outside counts as lit)
  bool compareLEqual = false;   // depth texture read by sampler2DShadow
};

struct VertexAttrib {
  int slot = 0;                 // attribute location
  int components = 4;
  AttribType type = AttribType::Float;
  uint32_t buffer = 0;
  int stride = 0;
  size_t offset = 0;
};

class Device {
 public:
  virtual ~Device() = default;
  virtual const char* name() const = 0;  // "OpenGL" / "Vulkan"
  virtual std::string info() const = 0;  // "<API> <version> / <GPU> / ..." for the log and window.mode
  virtual float maxAnisotropy() = 0;     // the driver's limit (1 = none)

  // ---- textures (GL: glGenTextures, glTexImage2D, glTexSubImage2D, glTexParameter) ----
  // An empty texture of a kind; levels = the mip levels it will get (a hint for backends that allocate up front).
  virtual uint32_t createTexture(TexKind kind, int levels = 1) = 0;
  // (Re)defines one level (and cube face) with its storage format, size and contents (data may be null: undefined).
  // bytes is the size of compressed data.
  virtual void texImage(uint32_t tex, int face, int level, Format f, int w, int h, Pixels p, const void* data,
                        size_t bytes = 0) = 0;
  // Replaces a rectangle of an uncompressed level (the layout may differ from the storage format: converted).
  virtual void texSubImage(uint32_t tex, int level, int x, int y, int w, int h, Pixels p, const void* data) = 0;
  virtual void setSampler(uint32_t tex, const SamplerDesc& s) = 0;
  virtual void setAnisotropy(uint32_t tex, float a) = 0;  // 1 = off; clamped to maxAnisotropy() by the caller
  // Level 0's red channel as floats (w·h values).
  virtual void readTextureRed(uint32_t tex, int w, int h, float* out) = 0;
  virtual void destroyTexture(uint32_t tex) = 0;

  // ---- renderbuffers (multisampled or plain attachments that are never sampled) ----
  virtual uint32_t createRenderbuffer(Format f, int w, int h, int samples) = 0;
  virtual void destroyRenderbuffer(uint32_t rb) = 0;

  // ---- framebuffers: a colour and/or a depth attachment (textures or renderbuffers); 0 = the back buffer ----
  // Returns 0 when the combination is not complete.
  virtual uint32_t createFramebuffer(uint32_t color, uint32_t depth) = 0;
  virtual void destroyFramebuffer(uint32_t fb) = 0;
  virtual void bindFramebuffer(uint32_t fb) = 0;
  // NEAREST copy between framebuffers (multisampled sources are resolved), rectangles in GL window coordinates
  virtual void blit(uint32_t src, uint32_t dst, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
                    bool color, bool depth) = 0;
  // RGBA8 pixels of a framebuffer, bottom row first
  virtual void readPixels(uint32_t fb, int x, int y, int w, int h, uint8_t* rgba) = 0;

  // ---- buffers and vertex layouts ----
  virtual uint32_t createBuffer() = 0;
  // (Re)allocates with this size and contents (data may be null). stream: rewritten often (GL STREAM_DRAW).
  virtual void bufferData(uint32_t buf, size_t bytes, const void* data, bool stream) = 0;
  virtual void bufferSubData(uint32_t buf, size_t offset, size_t bytes, const void* data) = 0;
  virtual void destroyBuffer(uint32_t buf) = 0;
  // A vertex layout (GL: a vertex array object); indices are 32-bit.
  virtual uint32_t createVertexInput(const VertexAttrib* attribs, int count, uint32_t indexBuffer) = 0;
  virtual void destroyVertexInput(uint32_t vi) = 0;
  virtual void bindVertexInput(uint32_t vi) = 0;

  // ---- programs: GLSL 3.30 core vertex and fragment source; attributes named here get these locations ----
  virtual uint32_t createProgram(const std::string& vs, const std::string& fs,
                                 const std::vector<std::pair<int, const char*>>& attribs, std::string* error) = 0;
  virtual void useProgram(uint32_t prog) = 0;
  virtual uint32_t currentProgram() const = 0;
  virtual int uniformLocation(uint32_t prog, const char* name) = 0;  // −1 when the program has no such uniform
  // set on the current program; −1 locations are ignored
  virtual void uniform1i(int loc, int v) = 0;
  virtual void uniform1f(int loc, float v) = 0;
  virtual void uniform3f(int loc, float x, float y, float z) = 0;
  virtual void uniform4f(int loc, float x, float y, float z, float w) = 0;
  virtual void uniform3fv(int loc, int count, const float* v) = 0;
  virtual void uniform4fv(int loc, int count, const float* v) = 0;
  virtual void uniformMatrix3fv(int loc, int count, const float* m) = 0;  // column-major
  virtual void uniformMatrix4fv(int loc, int count, const float* m) = 0;

  // ---- fixed state ----
  virtual void viewport(int x, int y, int w, int h) = 0;
  virtual void scissorTest(bool on) = 0;
  virtual void scissor(int x, int y, int w, int h) = 0;
  // clears the bound framebuffer inside the scissor rectangle when the test is on
  virtual void clear(bool color, bool depth, float r, float g, float b, float a, float depthValue = 1.0f) = 0;
  virtual void blend(bool on) = 0;
  virtual void blendFunc(Blend src, Blend dst) = 0;
  virtual void depthTest(bool on) = 0;
  virtual void depthMask(bool on) = 0;
  virtual void depthFunc(Compare c) = 0;
  virtual void cull(bool on) = 0;
  virtual void cullFace(Face f) = 0;
  virtual void polygonOffset(bool on, float factor, float units) = 0;
  virtual void bindTexture(int unit, uint32_t tex, bool cube = false) = 0;

  // ---- drawing on the bound vertex layout ----
  virtual void drawArrays(Prim p, int first, int count) = 0;
  virtual void drawElements(Prim p, int count, size_t firstIndex) = 0;

  // ---- frame ----
  virtual void beginFrame(int width, int height) = 0;  // the back buffer's size this frame
  virtual void present() = 0;
  virtual void setVsync(bool on) = 0;
};

// The device of the running game (or tool); set once the window is open.
Device& dev();
void setDevice(std::unique_ptr<Device> d);
bool hasDevice();

// The OpenGL 3.3 device on a window with a current context; null (error set) when GL entry points are missing.
std::unique_ptr<Device> createGlDevice(platform::Window& window, std::string* error);
// The Vulkan device on a window made without a GL context (WindowDesc::openGL = false); null (error set) when Vulkan is
// missing, has no suitable GPU, or the port was built without it (CMake REGS_VULKAN).
std::unique_ptr<Device> createVulkanDevice(platform::Window& window, bool vsync, std::string* error);

}  // namespace regs::render::gpu
