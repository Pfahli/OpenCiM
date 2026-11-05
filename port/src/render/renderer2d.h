#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "render/image.h"

namespace regs::render {

// GUI vertex as gsGuiPrimitive_OGL stores it (docs/renderer/interfaces-2d-output.md §2.4): position, uv and a colour
// in R,G,B,A byte order.
struct GuiVertex {
  float x = 0, y = 0;
  float u = 0, v = 0;
  uint32_t rgba = 0xFFFFFFFF;
};

// Converts the engine's 0xAARRGGBB colours to the vertex byte order.
inline uint32_t argbToRgba(uint32_t argb) {
  return (argb & 0xFF00FF00u) | (argb >> 16 & 0xFFu) | (argb & 0xFFu) << 16;
}

// Primitive kinds of gsGuiPrimitive_OGL (flags 4/8/0x10/0x20, otherwise triangles).
enum class Prim : uint8_t { Triangles, Quads, QuadStrip, Lines, LineStrip };

// Immediate-style 2D drawing on an OpenGL 3.3 core context, batched by texture, primitive class and scissor.
// Replaces the engine's fixed-function GUI pass (glOrtho, blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA, no depth).
class Renderer2D {
 public:
  ~Renderer2D();
  bool init(std::string* error);

  // Textures (GL names). Untextured drawing uses texture 0.
  uint32_t createTexture(const Image& img);
  void updateTextureBGRA(uint32_t tex, const uint8_t* bgra, int w, int h);  // same size and format as created
  float timer = 0;       // the effect timer for GUI shaders (`timer` uniform), set by the display each frame
  // UI scale (docs/ui/scaling.md), read at beginFrame: sharpScale > 1 samples magnified texels sharp (0 = plain
  // bilinear, as at 1x); lines are lineWidth window pixels wide (drawn as quads when > 1)
  float sharpScale = 0;
  float lineWidth = 1;
  float anisotropy = 0;  // max anisotropy for mipmapped 2D textures (video.anisotropy), clamped to the driver's
  uint32_t createTextureBGRA(const uint8_t* bgra, int w, int h);
  void destroyTexture(uint32_t tex);

  // Frame: the back buffer size in pixels; clears to 'argb' every frame (the engine cleared only on demand).
  void beginFrame(int width, int height, uint32_t clearArgb);
  void endFrame();

  // Coordinate system for following draws: x in [0, unitsW), y in [0, unitsH) top-down, mapped onto the pixel
  // rectangle (top-left origin). A GUI viewport uses its height as one unit unless it is in pixel mode.
  void setProjection(float px, float py, float pw, float ph, float unitsW, float unitsH);
  // Scissor in window pixels (top-left origin); w or h < 0 disables it.
  void setScissor(int x, int y, int w, int h);

  void draw(uint32_t tex, Prim prim, const GuiVertex* v, size_t count);

  // Hand the context to other GL code (the 3D pass) and take it back with the same projection and scissor.
  void flushAll() { flush(); }
  void resume();

  // Material shaders on GUI items: a GLSL 3.30 program with regs_* inputs (render/glsl.h) drawn under the current
  // projection; texture i is bound to unit i and sampler "texture<i>".
  uint32_t buildProgram(const std::string& vs, const std::string& fs, std::string* error);
  void drawWithProgram(uint32_t prog, const uint32_t* textures, int textureCount, Prim prim, const GuiVertex* v, size_t n,
                       const float* custom0 = nullptr);

  // Reads the back buffer as top-down RGBA rows (screenshots, tests).
  std::vector<uint8_t> readPixels() const;
  int frameWidth() const { return fw_; }
  int frameHeight() const { return fh_; }
  std::string info;  // "OpenGL <version> / <renderer>"

 private:
  void flush();
  void flushWith(bool ownProgram);
  bool lines_ = false;
  uint32_t batchTex_ = 0;
  std::vector<GuiVertex> batch_;
  uint32_t program_ = 0, vao_ = 0, vbo_ = 0;
  int uMatrix_ = -1, uTextured_ = -1, uTex_ = -1, uSharp_ = -1;
  void wideLine(const GuiVertex& a, const GuiVertex& b);
  size_t vboSize_ = 0;
  int fw_ = 0, fh_ = 0;
  float proj_[6] = {};
  float mvp_[16] = {};
  int scissor_[4] = {-1, -1, -1, -1};
};

}  // namespace regs::render
