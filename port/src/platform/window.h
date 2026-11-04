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
  bool fullscreen = false;  // borderless at desktop size; the game's exclusive mode change is not reproduced
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
  // OpenGL entry point lookup for the current context.
  virtual void* glProc(const char* name) const = 0;
};

std::unique_ptr<Window> createWindow();

}  // namespace regs::platform
