#pragma once
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "platform/window.h"
#include "render/renderer2d.h"
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class TextureObj;

// The game's one window and its renderer (gsWindow_OGL + gsVideo_OGL back end). Created when the script calls
// window.init(), unless the host runs headless.
class Display {
 public:
  bool open(const platform::WindowDesc& desc, std::string* error);
  platform::Window& window() { return *window_; }
  render::Renderer2D& r2d() { return r2d_; }
  render::Renderer3D& r3d() { return r3d_; }
  int antialiasing = 0;  // window.antialiasing: MSAA samples for post-effect scene targets
  // UI scale (setting regsUiScale, docs/ui/scaling.md): window pixels per script pixel. The scripts see a window of
  // (window size / uiScale); the GUI pass, pixel-mode viewports and render targets are drawn at uiScale × their
  // script size, so the interface grows while the 3D views keep the window's full resolution.
  float uiScale = 1.0f;
  // Picks uiScale for a window of w × h pixels from the setting (unset: 2 on windows taller than 1800 px), steps it
  // down while the scripts' window would be smaller than 1024 × 720, and returns it.
  float chooseUiScale(script::VM& vm, int w, int h);
  // the effect timer (gsShaders_OGL +0xdc, L:0x081ea990): real seconds, wrapped at 1024; the `timer` uniform of 3D and
  // GUI shaders and video.getEffectTimer()
  float effectTimer = 0;
  void advanceEffectTimer(float dt) {
    effectTimer += dt;
    if (effectTimer >= 1024.0f) effectTimer -= 1024.0f;
  }
  // The framebuffer behind a render-target texture, at least minW × minH (created or grown on demand).
  render::RenderTarget* renderTarget(TextureObj* t, int minW, int minH);

  // Program for a GUI material shader (.shader path), 0 when it cannot be built yet.
  uint32_t guiProgram(script::VM& vm, const std::u16string& path);

  // GL texture for a script texture, uploaded from its file on first use (0 when it cannot be loaded).
  uint32_t texture(script::VM& vm, TextureObj* t);

  // Draws every viewport of the video object and presents the frame.
  void renderFrame(script::VM& vm, script::Object* window, script::Object* video);
  // The gsWindow_OGL object that opened the display, and the last mouse position in client pixels.
  script::Object* windowObject() const { return windowObj_.get(); }
  void setWindowObject(script::Object* w) { windowObj_ = script::Ref<script::Object>(w); }
  void setMouse(int x, int y) { mx_ = x; my_ = y; mouseKnown_ = true; }
  int mouseX() const { return mx_; }
  int mouseY() const { return my_; }
  bool mouseKnown() const { return mouseKnown_; }  // false until the OS reported a position over the window

  // Saves the next rendered frame as a PNG (read back before the buffers are swapped).
  void requestScreenshot(const std::filesystem::path& file) { shot_ = file; }
  void dumpTarget(const render::RenderTarget& rt, const std::filesystem::path& file);

 private:
  // The port's frame-rate readout (setting regsShowFps, graphics page): measured and drawn over the GUI only while
  // the setting is on. Frame time = interval between two rendered frames (limiter sleeps and vsync included).
  struct FpsReadout {
    bool on = false;
    std::chrono::steady_clock::time_point last;
    std::deque<double> window;  // frame times of the last second, oldest first
    double windowSum = 0;
    int frames = 0;             // since the shown average was taken
    double elapsed = 0;
    std::u16string text;
  };
  void drawFpsReadout(script::VM& vm);
  void syncRenderSettings(script::VM& vm);  // antialiasing, anisotropy and vsync follow the settings (live)
  bool vsync_ = false, vsyncKnown_ = false;
  FpsReadout fps_;
  bool uiFilterSharp_ = true;  // scaled GUI textures: sharp bilinear (default) or plain bilinear (REGS_UI_FILTER=linear)
  bool saveScreenshot(const std::filesystem::path& file);
  std::unique_ptr<platform::Window> window_;
  render::Renderer2D r2d_;
  render::Renderer3D r3d_;
  std::map<TextureObj*, render::RenderTarget> targets_;
  std::map<std::u16string, uint32_t> guiPrograms_;
  std::filesystem::path shot_;
  uint32_t blank_ = 0;
  script::Ref<script::Object> windowObj_;
  int mx_ = 0, my_ = 0;
  bool mouseKnown_ = false;
};

// The display of the running game, or null when headless.
Display* display();

void registerDisplayNatives(script::VM& vm);  // gsWindow_OGL: init, tick (update, render, present), frame stats

}  // namespace regs::engine
