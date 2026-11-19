#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace regs::platform {

// OS window, with an OpenGL 3.3 core context or (openGL = false) bare for a Vulkan surface. The engine talks only to
// this interface, so other backends (SDL, Wayland, Cocoa) can replace the Win32 one without touching the engine.

struct WindowDesc {
  int width = 1024, height = 768;
  std::u16string title = u"Cities in Motion";
  // borderless at desktop size; the game's exclusive mode change is not reproduced (the desktop's resolution and
  // refresh rate stay; the resolution setting sizes the window when not fullscreen)
  bool fullscreen = false;
  bool vsync = true;
  bool visible = true;  // false: a hidden window that only provides a context (tools)
  int samples = 0;      // MSAA samples of the window surface (window.antialiasing without postProcess)
  bool openGL = true;   // false: no GL pixel format or context (the Vulkan device makes its own surface)
  // a second window (pop-out views, engine/popout.cpp): sizable with a maximise box, no controller or touchpad input
  // of its own, and with openGL the pixel format of `shareGL` and no context of its own (it draws with that window's,
  // made current on it by makeCurrent)
  bool resizable = false;
  bool secondary = false;
  // no Windows title bar or frame (pop-outs, like the setup program): the whole window is the picture; its edges
  // resize it, a band along the top moves it (double-click: maximise), and it has a close button of its own at the top
  // right that the engine draws (chrome / chromeHot)
  bool borderless = false;
  class Window* shareGL = nullptr;
  int x = kDefaultPos, y = kDefaultPos;  // outer top-left on the desktop; kDefaultPos lets the OS choose
  static constexpr int kDefaultPos = -0x7FFFFFFF;
};

// Raw input as the OS reports it; the engine's input layer turns it into the game's events.
struct InputEvent {
  enum class Type : uint8_t { MouseMove, MouseDown, MouseUp, MouseWheel, KeyDown, KeyUp, Char, Activate, Deactivate, Close, Resize,
                              MouseHWheel, Gesture, ToggleFullscreen };  // (the last: a borderless window's own button)
  Type type = Type::MouseMove;
  int x = 0, y = 0;     // client pixels, top-left origin (mouse events); new client size (Resize)
  int button = 0;       // 0 left, 1 right, 2 middle
  int key = 0;          // Windows virtual-key code (the input layer maps it to the engine's key codes)
  bool repeat = false;  // KeyDown from auto-repeat
  bool system = false;  // WM_SYSKEYDOWN/UP (Alt held, F10): the game never sees these on Windows
  char32_t ch = 0;      // Char
  float wheel = 0;      // notches, positive away from the user (MouseHWheel: positive to the right)
  // Gesture (touchscreen two fingers, touchpad twist; x, y = the fingers' centre): 1 zoom (value = the fingers' distance
  // now / before), 2 turn (value = radians since the last one, counterclockwise positive), 3 pan (dx, dy = pixels the
  // centre moved), 4 end (the fingers lifted)
  uint8_t gesture = 0;
  float value = 0, dx = 0, dy = 0;
  // KeyDown/KeyUp made by the engine itself (the controller's buttons): the engine key code, used instead of key
  int engineKey = 0;
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
  // moves the system cursor to a client position (the controller's virtual cursor; no effect without the focus)
  virtual void warpCursor(int, int) {}
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
  // OpenGL: makes the context current on this window (a secondary window: the context it shares); false when it cannot
  virtual bool makeCurrent() { return false; }
  // the window has the keyboard focus (it is the foreground window)
  virtual bool focused() const { return false; }
  virtual bool minimized() const { return false; }
  // outer top-left on the desktop and client size of the window (when fullscreen: as it was before), and placing it
  // there (not fullscreen)
  virtual void placement(int& x, int& y, int& w, int& h) const { x = y = 0, w = width(), h = height(); }
  virtual void place(int x, int y, int w, int h) {}
  virtual void focus() {}
  // borderless windows: the close button (top-left corner and size of its square; the fullscreen button is the same
  // size, `gap` pixels to its left of its left edge) and the height of the band that moves the window, in client
  // pixels; what the pointer is on: 0 nothing, 1 the close button, 2 the close button held down, 3 the band (moving the
  // window is possible there), 4 the fullscreen button, 5 it held down. Releasing the fullscreen button sends a
  // ToggleFullscreen event, the close button a Close event.
  virtual bool borderless() const { return false; }
  virtual void chrome(float& closeX, float& closeY, float& closeSize, float& band) const {
    closeX = closeY = closeSize = band = 0;
  }
  virtual float chromeGap() const { return 0; }
  virtual int chromeHot() const { return 0; }
  virtual bool maximized() const { return false; }
  // tests: an OS message (Win32: message, wParam, lParam) handled as if the system had sent it
  virtual void testMessage(unsigned msg, uintptr_t wparam, intptr_t lparam) {}
  // The OS handles a Vulkan surface is made from (Win32: HWND and HINSTANCE).
  virtual void* nativeWindow() const = 0;
  virtual void* nativeInstance() const = 0;
};

std::unique_ptr<Window> createWindow();
// The work area (desktop minus the task bar) of a monitor other than the one `away` is on, preferring the largest;
// false (and that monitor's work area) when there is only one.
bool otherMonitorWorkArea(const Window* away, int& x, int& y, int& w, int& h);
// Whether an outer window rect lies mostly on some monitor (a remembered place on a monitor since unplugged does not).
bool onSomeMonitor(int x, int y, int w, int h);

}  // namespace regs::platform
