#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace regs::platform {

// OS window with an OpenGL 3.3 core context. The engine talks only to this interface, so other backends
// (SDL, Wayland, Cocoa, a Vulkan swapchain) can replace the Win32 one without touching the engine.

struct WindowDesc {
  int width = 1024, height = 768;
  std::u16string title = u"Cities in Motion";
  // borderless at desktop size; the game's exclusive mode change is not reproduced (the desktop's resolution and
  // refresh rate stay; the resolution setting sizes the window when not fullscreen)
  bool fullscreen = false;
  bool vsync = true;
  bool visible = true;  // false: a hidden window that only provides a context (tools)
  int samples = 0;      // MSAA samples of the window surface (window.antialiasing without postProcess)
};

// Raw input as the OS reports it; the engine's input layer turns it into the game's events.
struct InputEvent {
  enum class Type : uint8_t { MouseMove, MouseDown, MouseUp, MouseWheel, KeyDown, KeyUp, Char, Activate, Deactivate, Close, Resize };
  Type type = Type::MouseMove;
  int x = 0, y = 0;     // client pixels, top-left origin (mouse events); new client size (Resize)
  int button = 0;       // 0 left, 1 right, 2 middle
  int key = 0;          // Windows virtual-key code (the input layer maps it to the engine's key codes)
  bool repeat = false;  // KeyDown from auto-repeat
  bool system = false;  // WM_SYSKEYDOWN/UP (Alt held, F10): the game never sees these on Windows
  char32_t ch = 0;      // Char
  float wheel = 0;      // notches, positive away from the user
};

class Window {
 public:
  virtual ~Window() = default;
  virtual bool create(const WindowDesc& desc, std::string* error) = 0;
  // Collects pending OS events (non-blocking).
  virtual void pump(std::vector<InputEvent>& out) = 0;
  virtual void swap() = 0;
  virtual void setTitle(const std::u16string& title) = 0;
  virtual void setVsync(bool on) = 0;
  virtual void showCursor(bool on) = 0;
  virtual int width() const = 0;   // client size in pixels
  virtual int height() const = 0;
  // Switches between a captioned window with a w × h client area and fullscreen (borderless over the monitor the
  // window is on), keeping the OpenGL context; afterwards width()/height() give the new client size.
  virtual void setMode(bool fullscreen, int w, int h) = 0;
  virtual bool fullscreen() const = 0;
  // the size of the monitor the window is on (the client size fullscreen would give)
  virtual void monitorSize(int& w, int& h) const = 0;
  // The monitor's DPI as the window sees it (96 = 100 %); 0 when the process is not DPI aware (the OS would then
  // stretch the window's pixels). The game sizes its interface itself (UI scale), never through the OS.
  virtual int dpi() const { return 96; }
  // OpenGL entry point lookup for the current context.
  virtual void* glProc(const char* name) const = 0;
};

std::unique_ptr<Window> createWindow();

}  // namespace regs::platform
