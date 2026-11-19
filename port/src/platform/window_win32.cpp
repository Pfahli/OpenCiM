// Win32 window and WGL context. Window class, styles and message handling follow gsWindow_OGL::start (0x10059200)
// and gsWindow_OGL::windowProc (0x10056970) where it matters for the game; the context is a modern 3.3 core one
// (the original used a legacy context). Fullscreen is a borderless window over the monitor instead of a display
// mode change.
#include <windows.h>
#include <windowsx.h>

#include <algorithm>

#include "platform/gamepad.h"
#include "platform/touchpad_win32.h"
#include "platform/window.h"

namespace regs::platform {

namespace {

// WGL_ARB_create_context / WGL_ARB_pixel_format constants
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091, WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092,
              WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126, WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001;
constexpr int WGL_DRAW_TO_WINDOW_ARB = 0x2001, WGL_SUPPORT_OPENGL_ARB = 0x2010, WGL_DOUBLE_BUFFER_ARB = 0x2011,
              WGL_PIXEL_TYPE_ARB = 0x2013, WGL_TYPE_RGBA_ARB = 0x202B, WGL_COLOR_BITS_ARB = 0x2014,
              WGL_ALPHA_BITS_ARB = 0x201B, WGL_DEPTH_BITS_ARB = 0x2022, WGL_STENCIL_BITS_ARB = 0x2023,
              WGL_ACCELERATION_ARB = 0x2003, WGL_FULL_ACCELERATION_ARB = 0x2027, WGL_SAMPLE_BUFFERS_ARB = 0x2041,
              WGL_SAMPLES_ARB = 0x2042, WGL_SWAP_METHOD_ARB = 0x2007, WGL_SWAP_COPY_ARB = 0x2029;
using CreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using ChoosePixelFormat = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using SwapInterval = BOOL(WINAPI*)(int);

const wchar_t* kClass = L"ReGSWindowOGL";

class Win32Window final : public Window {
 public:
  ~Win32Window() override {
    if (rc_) {
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(rc_);
    } else if (sharedRc_ && wglGetCurrentDC() == dc_) {
      wglMakeCurrent(nullptr, nullptr);  // the owner makes its own current again
    }
    if (dc_) ReleaseDC(hwnd_, dc_);
    if (hwnd_) DestroyWindow(hwnd_);
  }

  bool create(const WindowDesc& desc, std::string* error) override {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    // real pixels on high-DPI screens: per-monitor aware (v2), so Windows never bitmap-stretches the window. regs.exe
    // sets this first thing in main(); hosts that did not get it here, before the first window (a no-op otherwise)
    if (GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) != DPI_AWARENESS_PER_MONITOR_AWARE)
      SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    static bool registered = false;
    if (!registered) {
      WNDCLASSEXW wc{sizeof wc};
      wc.style = CS_OWNDC;
      wc.lpfnWndProc = &Win32Window::proc;
      wc.hInstance = inst;
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
      wc.lpszClassName = kClass;
      RegisterClassExW(&wc);
      registered = true;
    }
    // a throwaway window and legacy context give access to the WGL extension entry points
    CreateContextAttribs createAttribs = nullptr;
    ChoosePixelFormat choose = nullptr;
    // (not for a window sharing another's context: the throwaway context would leave none current)
    const bool ownContext = desc.openGL && !desc.shareGL;
    if (ownContext) {
      HWND tmp = CreateWindowExW(0, kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
      HDC tdc = GetDC(tmp);
      PIXELFORMATDESCRIPTOR pfd = legacyFormat();
      SetPixelFormat(tdc, ::ChoosePixelFormat(tdc, &pfd), &pfd);
      HGLRC trc = wglCreateContext(tdc);
      wglMakeCurrent(tdc, trc);
      createAttribs = reinterpret_cast<CreateContextAttribs>(wglGetProcAddress("wglCreateContextAttribsARB"));
      choose = reinterpret_cast<ChoosePixelFormat>(wglGetProcAddress("wglChoosePixelFormatARB"));
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(trc);
      ReleaseDC(tmp, tdc);
      DestroyWindow(tmp);
    }
    if (ownContext && !createAttribs) {
      if (error) *error = "the graphics driver does not offer OpenGL 3.3 (wglCreateContextAttribsARB missing)";
      return false;
    }

    fullscreen_ = desc.fullscreen;
    resizable_ = desc.resizable;
    secondary_ = desc.secondary;
    DWORD style = windowedStyle();  // the game's caption window, no resize (pop-outs: sizable)
    RECT r{0, 0, desc.width, desc.height};
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    const bool placed = desc.x != WindowDesc::kDefaultPos && desc.y != WindowDesc::kDefaultPos;
    const POINT at = placed ? POINT{desc.x + 40, desc.y + 20} : POINT{0, 0};  // the monitor it opens on
    if (fullscreen_) {
      style = WS_POPUP;
      MONITORINFO mi{sizeof mi};
      GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTOPRIMARY), &mi);
      r = mi.rcMonitor;
      x = r.left;
      y = r.top;
    } else {
      MONITORINFO mi{sizeof mi};
      GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTOPRIMARY), &mi);
      if (!secondary_ &&
          (desc.width >= mi.rcMonitor.right - mi.rcMonitor.left || desc.height >= mi.rcMonitor.bottom - mi.rcMonitor.top)) {
        style = WS_POPUP;  // as large as the monitor or larger (4K screenshots): a plain window at its corner, no frame
        x = mi.rcMonitor.left;
        y = mi.rcMonitor.top;
        oversize_ = true;
      } else {
        AdjustWindowRectEx(&r, style, FALSE, 0);
        if (placed) x = desc.x, y = desc.y;
      }
    }
    hwnd_ = CreateWindowExW(0, kClass, reinterpret_cast<const wchar_t*>(desc.title.c_str()), style, x, y,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, this);
    if (!hwnd_) {
      if (error) *error = "CreateWindowEx failed";
      return false;
    }
    if (!desc.openGL) {  // a bare window for a Vulkan surface
      if (desc.visible) ShowWindow(hwnd_, SW_SHOW);
      UpdateWindow(hwnd_);
      RECT cr;
      GetClientRect(hwnd_, &cr);
      width_ = cr.right;
      height_ = cr.bottom;
      return true;
    }
    dc_ = GetDC(hwnd_);
    if (auto* owner = dynamic_cast<Win32Window*>(desc.shareGL); owner && owner->rc_) {
      // a secondary window drawn with the owner's context: the same pixel format, so the context can be made current
      // on it (wglMakeCurrent wants a device context of the same format)
      PIXELFORMATDESCRIPTOR spfd{};
      DescribePixelFormat(owner->dc_, owner->format_, sizeof spfd, &spfd);
      if (!SetPixelFormat(dc_, owner->format_, &spfd)) {
        if (error) *error = "SetPixelFormat (shared) failed";
        return false;
      }
      format_ = owner->format_;
      sharedRc_ = owner->rc_;
      opengl32_ = owner->opengl32_;
      swapInterval_ = owner->swapInterval_;
      if (desc.visible) ShowWindow(hwnd_, SW_SHOW);
      UpdateWindow(hwnd_);
      RECT cr;
      GetClientRect(hwnd_, &cr);
      width_ = cr.right;
      height_ = cr.bottom;
      return true;
    }
    int format = 0;
    UINT n = 0;
    PIXELFORMATDESCRIPTOR pfd = legacyFormat();
    // MSAA through the pixel format, stepping the sample count down by 2 until a format exists (0x10059200.c:256-309)
    // Swap method: copy (WGL_SWAP_COPY_ARB). With the driver's default (exchange) the window showed single black frames
    // alternating with good ones, most under load (fast forward): an undrawn back buffer reached the screen although
    // every frame was drawn completely (measured: 43 black flashes in 90 s by default, 0 with copy). Formats without
    // copy fall back to the driver's choice.
    for (int swapCopy = 1; swapCopy >= 0 && !format; --swapCopy) {
      for (int samples = std::max(0, desc.samples); choose; samples -= 2) {
        const int attribs[] = {WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1,
                               WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB, WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
                               WGL_COLOR_BITS_ARB, 24, WGL_ALPHA_BITS_ARB, 8, WGL_DEPTH_BITS_ARB, 24,
                               WGL_STENCIL_BITS_ARB, 8, WGL_SAMPLE_BUFFERS_ARB, samples > 0 ? 1 : 0,
                               WGL_SAMPLES_ARB, std::max(samples, 0),
                               swapCopy ? WGL_SWAP_METHOD_ARB : 0, WGL_SWAP_COPY_ARB, 0};
        if (choose(dc_, attribs, nullptr, 1, &format, &n) && n > 0) break;
        format = 0;
        if (samples <= 0) break;
      }
      if (!choose) break;
    }
    if (!format) format = ::ChoosePixelFormat(dc_, &pfd);
    DescribePixelFormat(dc_, format, sizeof pfd, &pfd);
    if (!SetPixelFormat(dc_, format, &pfd)) {
      if (error) *error = "SetPixelFormat failed";
      return false;
    }
    format_ = format;
    const int ctx[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                       WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    rc_ = createAttribs(dc_, nullptr, ctx);
    if (!rc_ || !wglMakeCurrent(dc_, rc_)) {
      if (error) *error = "could not create an OpenGL 3.3 core context";
      return false;
    }
    opengl32_ = GetModuleHandleW(L"opengl32.dll");
    swapInterval_ = reinterpret_cast<SwapInterval>(wglGetProcAddress("wglSwapIntervalEXT"));
    setVsync(desc.vsync);
    if (desc.visible) ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    // two-finger gestures on a touchscreen (zoom, turn, pan); one finger stays a mouse (tools, clicks), and the
    // touchpad's twist comes as raw input (smooth camera, owner's request 2026-11-17)
    GESTURECONFIG gc[] = {{GID_ZOOM, GC_ZOOM, 0},
                          {GID_ROTATE, GC_ROTATE, 0},
                          {GID_PAN, GC_PAN, GC_PAN_WITH_SINGLE_FINGER_VERTICALLY | GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY |
                                                GC_PAN_WITH_INERTIA}};
    SetGestureConfig(hwnd_, 0, UINT(sizeof(gc) / sizeof(gc[0])), gc, sizeof(GESTURECONFIG));
    if (!secondary_) {
      touchpad_.attach(hwnd_);
      gamepadAttach(hwnd_);  // PlayStation pads without XInput (platform/gamepad.h)
    }
    RECT cr;
    GetClientRect(hwnd_, &cr);
    width_ = cr.right;
    height_ = cr.bottom;
    return true;
  }

  void pump(std::vector<InputEvent>& out) override {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) queued_.push_back({InputEvent::Type::Close});
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    // messages can also arrive outside the pump (sent by the system), so everything goes through the queue
    out.insert(out.end(), queued_.begin(), queued_.end());
    queued_.clear();
  }

  void swap() override {
    if (dc_) SwapBuffers(dc_);
  }
  void setTitle(const std::u16string& t) override { SetWindowTextW(hwnd_, reinterpret_cast<const wchar_t*>(t.c_str())); }
  void setVsync(bool on) override {
    if (swapInterval_) swapInterval_(on ? 1 : 0);
  }
  void showCursor(bool on) override { cursor_ = on; }
  // the controller's virtual cursor (engine/gamepad.cpp): the system cursor follows it while the window has the
  // focus, so the mouse goes on from where the cursor is
  void warpCursor(int x, int y) override {
    if (GetForegroundWindow() != hwnd_) return;
    POINT p{x, y};
    ClientToScreen(hwnd_, &p);
    SetCursorPos(p.x, p.y);
  }
  int width() const override { return width_; }
  int height() const override { return height_; }
  bool fullscreen() const override { return fullscreen_; }
  void monitorSize(int& w, int& h) const override {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
    w = mi.rcMonitor.right - mi.rcMonitor.left;
    h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  }
  // the graphics page while the game runs: a new style and size on the same window, so the pixel format and the GL
  // context (every texture, buffer and program) stay
  void setMode(bool fullscreen, int w, int h) override {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
    DWORD visible = DWORD(GetWindowLongPtrW(hwnd_, GWL_STYLE)) & WS_VISIBLE;
    DWORD style = (fullscreen ? WS_POPUP : windowedStyle()) | visible;
    RECT r = mi.rcMonitor;
    if (!fullscreen) {
      RECT frame{0, 0, w, h};
      AdjustWindowRectEx(&frame, style, FALSE, 0);
      int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
      RECT cur;
      GetWindowRect(hwnd_, &cur);
      int x = cur.left, y = cur.top;
      if (fullscreen_ && restore_.right > restore_.left) {  // back where it was before fullscreen
        x = restore_.left;
        y = restore_.top;
      } else if (fullscreen_) {  // from fullscreen: centred in the work area
        x = mi.rcWork.left + std::max(0, int(mi.rcWork.right - mi.rcWork.left - fw) / 2);
        y = mi.rcWork.top + std::max(0, int(mi.rcWork.bottom - mi.rcWork.top - fh) / 2);
      }
      r = RECT{x, y, x + fw, y + fh};
    }
    if (fullscreen && !fullscreen_) {  // remembered for the way back and for placement()
      if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
      GetWindowRect(hwnd_, &restore_);
      RECT cr0;
      GetClientRect(hwnd_, &cr0);
      restoreW_ = cr0.right;
      restoreH_ = cr0.bottom;
    }
    SetWindowLongPtrW(hwnd_, GWL_STYLE, LONG_PTR(style));
    SetWindowPos(hwnd_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    fullscreen_ = fullscreen;
    RECT cr;
    GetClientRect(hwnd_, &cr);
    width_ = cr.right;
    height_ = cr.bottom;
  }
  int dpi() const override {
    DPI_AWARENESS a = GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd_));
    return a == DPI_AWARENESS_PER_MONITOR_AWARE ? int(GetDpiForWindow(hwnd_)) : 0;
  }
  bool makeCurrent() override {
    HGLRC rc = rc_ ? rc_ : sharedRc_;
    return rc && dc_ && wglMakeCurrent(dc_, rc);
  }
  bool focused() const override { return GetForegroundWindow() == hwnd_; }
  bool minimized() const override { return IsIconic(hwnd_) != 0; }
  void placement(int& x, int& y, int& w, int& h) const override {
    if (fullscreen_ && restore_.right > restore_.left) {
      x = restore_.left, y = restore_.top, w = restoreW_, h = restoreH_;
      return;
    }
    WINDOWPLACEMENT wp{sizeof wp};
    GetWindowPlacement(hwnd_, &wp);  // the restored rect also while maximised or minimised
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectEx(&frame, windowedStyle(), FALSE, 0);
    x = wp.rcNormalPosition.left;
    y = wp.rcNormalPosition.top;
    w = (wp.rcNormalPosition.right - wp.rcNormalPosition.left) - (frame.right - frame.left);
    h = (wp.rcNormalPosition.bottom - wp.rcNormalPosition.top) - (frame.bottom - frame.top);
  }
  void place(int x, int y, int w, int h) override {
    if (fullscreen_) setMode(false, w, h);
    RECT r{0, 0, std::max(w, 160), std::max(h, 90)};
    AdjustWindowRectEx(&r, windowedStyle(), FALSE, 0);
    SetWindowPos(hwnd_, nullptr, x, y, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOOWNERZORDER);
    RECT cr;
    GetClientRect(hwnd_, &cr);
    width_ = cr.right;
    height_ = cr.bottom;
  }
  void focus() override { SetForegroundWindow(hwnd_); }
  void* nativeWindow() const override { return hwnd_; }
  void* nativeInstance() const override { return GetModuleHandleW(nullptr); }
  void* glProc(const char* name) const override {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    auto bad = reinterpret_cast<intptr_t>(p);
    if (bad == 0 || bad == 1 || bad == 2 || bad == 3 || bad == -1) p = reinterpret_cast<void*>(GetProcAddress(opengl32_, name));
    return p;
  }

 private:
  static PIXELFORMATDESCRIPTOR legacyFormat() {
    PIXELFORMATDESCRIPTOR pfd{sizeof pfd, 1};
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    return pfd;
  }

  DWORD windowedStyle() const {
    return resizable_ ? WS_OVERLAPPEDWINDOW : WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  }

  void emit(InputEvent e) { queued_.push_back(e); }

  LRESULT handle(UINT m, WPARAM w, LPARAM l) {
    using T = InputEvent::Type;
    auto mouse = [&](T t, int button) {
      InputEvent e{t};
      e.x = GET_X_LPARAM(l);
      e.y = GET_Y_LPARAM(l);
      e.button = button;
      emit(e);
    };
    switch (m) {
      case WM_MOUSEMOVE: mouse(T::MouseMove, 0); return 0;
      case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: SetCapture(hwnd_); mouse(T::MouseDown, 0); return 0;
      case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: SetCapture(hwnd_); mouse(T::MouseDown, 1); return 0;
      case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: SetCapture(hwnd_); mouse(T::MouseDown, 2); return 0;
      case WM_LBUTTONUP: ReleaseCapture(); mouse(T::MouseUp, 0); return 0;
      case WM_RBUTTONUP: ReleaseCapture(); mouse(T::MouseUp, 1); return 0;
      case WM_MBUTTONUP: ReleaseCapture(); mouse(T::MouseUp, 2); return 0;
      case WM_MOUSEWHEEL: {
        InputEvent e{T::MouseWheel};
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(hwnd_, &p);  // wheel positions arrive in screen coordinates
        e.x = p.x;
        e.y = p.y;
        e.wheel = float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA;
        emit(e);
        return 0;
      }
      case WM_MOUSEHWHEEL: {  // a tilting wheel or two fingers swiped sideways on a touchpad
        InputEvent e{T::MouseHWheel};
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(hwnd_, &p);
        e.x = p.x;
        e.y = p.y;
        e.wheel = float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA;
        emit(e);
        return 0;
      }
      case WM_GESTURE: {
        GESTUREINFO gi{};
        gi.cbSize = sizeof(gi);
        if (!GetGestureInfo(reinterpret_cast<HGESTUREINFO>(l), &gi)) break;
        POINT p{gi.ptsLocation.x, gi.ptsLocation.y};
        ScreenToClient(hwnd_, &p);
        const bool begin = (gi.dwFlags & GF_BEGIN) != 0, end = (gi.dwFlags & GF_END) != 0;
        InputEvent e{T::Gesture};
        e.x = p.x;
        e.y = p.y;
        if (gi.dwID == GID_ZOOM) {
          const double dist = double(gi.ullArguments);
          if (!begin && gestureDist_ > 0) {
            e.gesture = 1;
            e.value = float(dist / gestureDist_);
            emit(e);
          }
          gestureDist_ = dist;
        } else if (gi.dwID == GID_ROTATE) {
          const double a = GID_ROTATE_ANGLE_FROM_ARGUMENT(DWORD(gi.ullArguments & 0xFFFFFFFFu));
          if (!begin) {
            e.gesture = 2;
            e.value = float(a - gestureAngle_);
            emit(e);
          }
          gestureAngle_ = begin ? 0.0 : a;
        } else if (gi.dwID == GID_PAN) {
          if (!begin) {
            e.gesture = 3;
            e.dx = float(p.x - gesturePan_.x);
            e.dy = float(p.y - gesturePan_.y);
            emit(e);
          }
          gesturePan_ = p;
        }
        if (end && gi.dwID != GID_BEGIN && gi.dwID != GID_END) {
          InputEvent f{T::Gesture};
          f.gesture = 4;
          f.x = p.x;
          f.y = p.y;
          emit(f);
        }
        CloseGestureInfoHandle(reinterpret_cast<HGESTUREINFO>(l));
        return 0;
      }
      case WM_INPUT: {
        if (gamepadRawInput(reinterpret_cast<void*>(l))) break;  // a PlayStation pad report
        float angle = 0;
        bool ended = false;
        if (touchpad_.input(l, angle, ended)) {
          InputEvent e{T::Gesture};
          POINT p{};
          GetCursorPos(&p);
          ScreenToClient(hwnd_, &p);
          e.x = p.x;
          e.y = p.y;
          e.gesture = ended ? 4 : 2;
          e.value = angle;
          emit(e);
        }
        break;  // DefWindowProc cleans up the raw input
      }
      case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        if (m == WM_KEYDOWN && w < 256) held_[w] = true;
        InputEvent e{T::KeyDown};
        e.key = int(w);
        e.repeat = (l & (1 << 30)) != 0;
        e.system = m == WM_SYSKEYDOWN;
        emit(e);
        if (m == WM_SYSKEYDOWN && w != VK_F4) return 0;  // keep Alt from opening the system menu
        break;
      }
      case WM_KEYUP: case WM_SYSKEYUP: {
        if (w < 256) held_[w] = false;
        InputEvent e{T::KeyUp};
        e.key = int(w);
        e.system = m == WM_SYSKEYUP;
        emit(e);
        if (m == WM_SYSKEYUP) return 0;
        break;
      }
      case WM_CHAR: {
        char16_t u = char16_t(w);
        if (u >= 0xD800 && u < 0xDC00) { highSurrogate_ = u; return 0; }
        InputEvent e{T::Char};
        e.ch = (u >= 0xDC00 && u < 0xE000 && highSurrogate_) ? 0x10000 + ((highSurrogate_ - 0xD800) << 10) + (u - 0xDC00) : u;
        highSurrogate_ = 0;
        emit(e);
        return 0;
      }
      case WM_ACTIVATEAPP: emit({w ? T::Activate : T::Deactivate}); break;
      // the focus going to another window of the game (a pop-out view): the keys held here are let go, as their key-up
      // messages now go to that window (held arrow keys would scroll on for ever)
      case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
          for (int k = 0; k < 256; ++k)
            if (held_[k]) {
              held_[k] = false;
              InputEvent e{T::KeyUp};
              e.key = k;
              emit(e);
            }
        break;
      case WM_SETCURSOR:
        if (!cursor_ && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        break;
      case WM_SIZE:
        width_ = LOWORD(l);
        height_ = HIWORD(l);
        { InputEvent e{T::Resize}; e.x = width_; e.y = height_; emit(e); }
        return 0;
      // moved to a monitor with another DPI: the client keeps its pixel size (the game's resolution and UI scale were
      // chosen at start), no OS rescaling of the window
      case WM_GETDPISCALEDSIZE: {
        RECT r{};
        GetWindowRect(hwnd_, &r);
        auto* s = reinterpret_cast<SIZE*>(l);
        s->cx = r.right - r.left;
        s->cy = r.bottom - r.top;
        return TRUE;
      }
      case WM_DPICHANGED: return 0;
      case WM_ERASEBKGND: return 1;
      case WM_GETMINMAXINFO:  // a window larger than the monitor keeps its size
        if (resizable_) {
          auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
          mmi->ptMinTrackSize.x = 320;
          mmi->ptMinTrackSize.y = 200;
          return 0;
        }
        if (oversize_) {
          auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
          mmi->ptMaxTrackSize.x = mmi->ptMaxSize.x = 16384;
          mmi->ptMaxTrackSize.y = mmi->ptMaxSize.y = 16384;
          return 0;
        }
        break;
      case WM_CLOSE: emit({T::Close}); return 0;  // the game decides (scripts may ask before quitting)
    }
    return DefWindowProcW(hwnd_, m, w, l);
  }

  static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == h) return self->handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
  }

  HWND hwnd_ = nullptr;
  HDC dc_ = nullptr;
  HGLRC rc_ = nullptr;
  HGLRC sharedRc_ = nullptr;  // a secondary window: the owner's context
  int format_ = 0;
  bool resizable_ = false, secondary_ = false;
  bool held_[256] = {};
  RECT restore_{0, 0, 0, 0};  // the window rect before fullscreen (setMode)
  int restoreW_ = 0, restoreH_ = 0;
  HMODULE opengl32_ = nullptr;
  SwapInterval swapInterval_ = nullptr;
  bool fullscreen_ = false, cursor_ = true;
  int width_ = 0, height_ = 0;
  char16_t highSurrogate_ = 0;
  std::vector<InputEvent> queued_;
  bool oversize_ = false;
  double gestureDist_ = 0, gestureAngle_ = 0;  // WM_GESTURE: the last zoom distance and turn angle
  POINT gesturePan_{};
  TouchpadTwist touchpad_;
};

}  // namespace

std::unique_ptr<Window> createWindow() { return std::make_unique<Win32Window>(); }

bool otherMonitorWorkArea(const Window* away, int& x, int& y, int& w, int& h) {
  HMONITOR own = away ? MonitorFromWindow(static_cast<HWND>(away->nativeWindow()), MONITOR_DEFAULTTOPRIMARY)
                      : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  struct Find {
    HMONITOR own;
    RECT best{0, 0, 0, 0};
    long long area = -1;
  } f{own};
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
    auto* f = reinterpret_cast<Find*>(p);
    if (m == f->own) return TRUE;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(m, &mi);
    long long a = (long long)(mi.rcWork.right - mi.rcWork.left) * (mi.rcWork.bottom - mi.rcWork.top);
    if (a > f->area) f->area = a, f->best = mi.rcWork;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&f));
  RECT r = f.best;
  if (f.area < 0) {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(own, &mi);
    r = mi.rcWork;
  }
  x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
  return f.area >= 0;
}

bool onSomeMonitor(int x, int y, int w, int h) {
  RECT r{x, y, x + std::max(w, 1), y + std::max(h, 1)};
  HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
  if (!m) return false;
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(m, &mi);
  RECT in{};
  IntersectRect(&in, &r, &mi.rcMonitor);
  return (long long)(in.right - in.left) * (in.bottom - in.top) * 2 >= (long long)(r.right - r.left) * (r.bottom - r.top);
}

}  // namespace regs::platform
