#pragma once
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
