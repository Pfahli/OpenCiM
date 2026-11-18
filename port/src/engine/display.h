#pragma once
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
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
  // the window and its graphics device: Vulkan when asked and available, else OpenGL 3.3
  bool open(const platform::WindowDesc& desc, bool vulkan, std::string* error);
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
  float pickUiScale(script::VM& vm, int w, int h) const;  // the same choice without setting uiScale
  // The graphics page's display options, applied while the game runs (window.regsApplyDisplay): fullscreen
  // (borderless over the monitor) or a w × h window, and the UI scale setting. Returns 0 when nothing changed, 1
  // when the window or scale changed but the scripts' window size stayed, 2 when the scripts' size changed (then
  // window.width/height hold the new size and the GUI must be laid out again). Without relayout a change of the
  // scripts' size is not made (returns 2, nothing changed).
  int applyDisplay(script::VM& vm, bool fullscreen, int w, int h, bool relayout);
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
  // texture() for many at once (a map's materials): the files are read and decoded on worker threads, the uploads
  // stay on this thread. Already uploaded ones are skipped.
  void uploadTextures(script::VM& vm, const std::vector<TextureObj*>& textures);

  // Upscaled textures (setting regsTextureUpscale, docs/renderer/texture-upscaling.md): while on, textures come from
  // the cache below the OpenCiM folder when it holds them; missing ones are made by a background thread and swapped
  // in as they appear. Off: every texture is uploaded again from its original file. Percent of the background run,
  // or -1 when none runs.
  int textureUpscaleProgress() const;
  ~Display();

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
  // the next drawn frame's pixels (RGBA, row 0 the top, width, height) to a callback, once (template previews)
  std::function<void(const std::vector<uint8_t>&, int, int)> captureNext;
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
  void benchFrame(double renderCpuMs);  // REGS_BENCH frame-time measurement (display.cpp)
  void syncRenderSettings(script::VM& vm);  // antialiasing, anisotropy and vsync follow the settings (live)
  bool vsync_ = false, vsyncKnown_ = false;
  struct UpscaleJob;  // display.cpp
  void syncTextureUpscale(script::VM& vm);
  bool loadTextureImage(script::VM& vm, TextureObj* t, render::Image& img, std::string* err);
  struct DecodedTexture;  // display.cpp: read and decoded ahead by uploadTextures, taken by loadTextureImage
  std::map<TextureObj*, DecodedTexture*> decoded_;
  std::unique_ptr<UpscaleJob> upscaleJob_;
  bool upscaleOn_ = false;
  std::chrono::steady_clock::time_point upscalePoll_;
  std::map<uint32_t, size_t> textureBytes_;  // bytes of each uploaded file texture (REGS_FRAME_STATS)
  void dropGuiTargets();  // render-target textures are made again at the new UI scale
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

// The GUI laid out again for a new size of the scripts' window, ow × oh → nw × nh (engine/gui_relayout.cpp): the
// root viewports filling the window and every control in them keep their edge, centre or span relation. Returns the
// number of controls moved or resized.
int relayoutGui(script::VM& vm, script::Object* video, int ow, int oh, int nw, int nh);
// One control to a new rect: a move alone is set quietly; a new size lays its children out again (the same
// anchoring) and then fires its onChangeRect. window.regsSetRect(control, l, t, w, h) for scripts. Returns the
// number of controls changed.
class ControlObj;
int placeControl(script::VM& vm, ControlObj* c, float l, float t, float w, float h);

// The display of the running game, or null when headless.
Display* display();
// The graphics device keeps its linked programs in <user folder>cachegl (OpenGL program binaries; REGS_NO_PROGRAM_CACHE=1
// turns it off). Called when the device opens and when the scripts set the user folder, whichever comes last.
void useProgramCache();

void registerDisplayNatives(script::VM& vm);  // gsWindow_OGL: init, tick (update, render, present), frame stats

}  // namespace regs::engine
