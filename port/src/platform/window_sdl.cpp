// SDL3 windows (Linux and other non-Windows systems, docs/linux.md): the same behaviour the engine gets from the Win32
// window (window_win32.cpp), with SDL doing the OS work. Keys go out as Windows virtual-key codes, because the input
// layer (engine/input.cpp vkToKey) and the scripts' key names are built on them. Window sizes are in pixels: the
// window asks for high pixel density, and mouse positions are scaled from SDL's window points to pixels.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "core/text.h"
#include "platform/window.h"

namespace regs::platform {

namespace {

bool videoUp() {
  static int state = 0;  // 0 not tried, 1 up, -1 failed
  if (state == 0) {
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_APP_ID, "opencim");
    SDL_SetHint(SDL_HINT_APP_NAME, "OpenCiM");
    state = SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) ? 1 : -1;
  }
  return state > 0;
}

// SDL key → Windows virtual-key code (0: none). Letters and digits follow the keyboard layout (the key event's
// keycode), as Windows' VK codes do; the rest go by position.
int toVk(const SDL_KeyboardEvent& k) {
  const SDL_Keycode c = k.key;
  if (c >= SDLK_A && c <= SDLK_Z) return 'A' + int(c - SDLK_A);
  if (c >= SDLK_0 && c <= SDLK_9) return '0' + int(c - SDLK_0);
  const SDL_Scancode s = k.scancode;
  if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + int(s - SDL_SCANCODE_F1);
  if (s >= SDL_SCANCODE_F13 && s <= SDL_SCANCODE_F24) return 0x7C + int(s - SDL_SCANCODE_F13);
  if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return 0x61 + int(s - SDL_SCANCODE_KP_1);
  switch (s) {
    case SDL_SCANCODE_A ... SDL_SCANCODE_Z: return 'A' + int(s - SDL_SCANCODE_A);  // keycode was not a letter
    case SDL_SCANCODE_1 ... SDL_SCANCODE_9: return '1' + int(s - SDL_SCANCODE_1);
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_CLEAR: return 0x0C;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_SELECT: return 0x29;
    case SDL_SCANCODE_PRINTSCREEN: return 0x2C;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_HELP: return 0x2F;
    case SDL_SCANCODE_LGUI: return 0x5B;
    case SDL_SCANCODE_RGUI: return 0x5C;
    case SDL_SCANCODE_APPLICATION: return 0x5D;
    case SDL_SCANCODE_SLEEP: return 0x5F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PERIOD: case SDL_SCANCODE_KP_COMMA: return 0x6E;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_SCROLLLOCK: return 0x91;
    // the punctuation keys by their US position (OEM codes, as Windows reports them for a US layout)
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_NONUSBACKSLASH: return 0xE2;
    default: return 0;
  }
}

class SdlWindow;
// never destroyed: the display (and its window) is a static that outlives a static map made after it
std::map<SDL_WindowID, SdlWindow*>& windows() {
  static auto* m = new std::map<SDL_WindowID, SdlWindow*>;
  return *m;
}
bool appActive = false;  // one of the game's windows has the keyboard focus (WM_ACTIVATEAPP)

class SdlWindow final : public Window {
 public:
  ~SdlWindow() override {
    if (win_) windows().erase(SDL_GetWindowID(win_));
    if (gl_) SDL_GL_DestroyContext(gl_);
    if (win_) SDL_DestroyWindow(win_);
  }

  bool create(const WindowDesc& desc, std::string* error) override {
    if (!videoUp()) {
      if (error) *error = std::string("SDL video: ") + SDL_GetError();
      return false;
    }
    resizable_ = desc.resizable;
    secondary_ = desc.secondary;
    borderless_ = desc.borderless;
    openGL_ = desc.openGL;
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.openGL) flags |= SDL_WINDOW_OPENGL;
    else flags |= SDL_WINDOW_VULKAN;
    if (!desc.visible) flags |= SDL_WINDOW_HIDDEN;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (desc.borderless) flags |= SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE;
    if (desc.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    owner_ = dynamic_cast<SdlWindow*>(desc.shareGL);
    if (desc.openGL && !owner_) {
      SDL_GL_ResetAttributes();
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
      SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
      SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
      SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, desc.samples > 0 ? 1 : 0);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, std::max(0, desc.samples));
    }
    const std::string title = utf16ToUtf8(desc.title);
    // MSAA through the window's format, stepping the sample count down by 2 until a window is made (as the Win32 one)
    for (int samples = std::max(0, desc.samples);; samples -= 2) {
      if (desc.openGL && !owner_) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, samples > 0 ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, std::max(0, samples));
      }
      win_ = SDL_CreateWindow(title.c_str(), desc.width, desc.height, flags);
      if (win_ || samples <= 0) break;
    }
    if (!win_) {
      if (error) *error = std::string("SDL_CreateWindow: ") + SDL_GetError();
      return false;
    }
    windows()[SDL_GetWindowID(win_)] = this;
    // the asked size is in pixels: on a scaled desktop (Wayland at 200 %) the window is made smaller in points
    if (!desc.fullscreen) {
      const float d = SDL_GetWindowPixelDensity(win_);
      if (d > 1.01f) SDL_SetWindowSize(win_, int(std::lround(desc.width / d)), int(std::lround(desc.height / d)));
      if (desc.x != WindowDesc::kDefaultPos && desc.y != WindowDesc::kDefaultPos) SDL_SetWindowPosition(win_, desc.x, desc.y);
    }
    if (desc.borderless) SDL_SetWindowHitTest(win_, &SdlWindow::hitTest, this);
    if (desc.openGL) {
      if (owner_) {
        if (!owner_->gl_) {
          if (error) *error = "the shared window has no OpenGL context";
          return false;
        }
      } else {
        gl_ = SDL_GL_CreateContext(win_);
        if (!gl_) {
          if (error) *error = std::string("could not create an OpenGL 3.3 core context: ") + SDL_GetError();
          return false;
        }
        SDL_GL_MakeCurrent(win_, gl_);
        setVsync(desc.vsync);
      }
    }
    SDL_StartTextInput(win_);
    SDL_SyncWindow(win_);
    updateSize();
    return true;
  }

  void pump(std::vector<InputEvent>& out) override {
    drainEvents();
    out.insert(out.end(), queued_.begin(), queued_.end());
    queued_.clear();
  }

  void swap() override {
    if (openGL_) SDL_GL_SwapWindow(win_);
  }
  void setTitle(const std::u16string& t) override { SDL_SetWindowTitle(win_, utf16ToUtf8(t).c_str()); }
  void setVsync(bool on) override {
    if (!openGL_) return;
    // adaptive vsync first (late frames tear instead of halving the rate), then plain
    if (!on || !SDL_GL_SetSwapInterval(-1)) SDL_GL_SetSwapInterval(on ? 1 : 0);
  }
  void showCursor(bool on) override {
    cursor_ = on;
    applyCursor();
  }
  void warpCursor(int x, int y) override {
    if (!focused()) return;
    const float d = density();
    SDL_WarpMouseInWindow(win_, float(x) / d, float(y) / d);
  }
  void clipCursor(bool on) override {
    if (on && focused()) {
      int w = 0, h = 0;
      SDL_GetWindowSize(win_, &w, &h);
      const SDL_Rect r{0, 0, w, h};
      SDL_SetWindowMouseRect(win_, &r);
      clipped_ = true;
    } else if (clipped_) {
      SDL_SetWindowMouseRect(win_, nullptr);
      clipped_ = false;
    }
  }
  int width() const override { return width_; }
  int height() const override { return height_; }
  bool fullscreen() const override { return (SDL_GetWindowFlags(win_) & SDL_WINDOW_FULLSCREEN) != 0; }
  void monitorSize(int& w, int& h) const override {
    w = 1920, h = 1080;
    if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(win_))) {
      w = int(std::lround(m->w * m->pixel_density));
      h = int(std::lround(m->h * m->pixel_density));
    }
  }
  void setMode(bool fullscreen, int w, int h) override {
    if (fullscreen) {
      SDL_SetWindowFullscreenMode(win_, nullptr);  // borderless over the desktop, no mode change
      SDL_SetWindowFullscreen(win_, true);
    } else {
      SDL_SetWindowFullscreen(win_, false);
      SDL_SyncWindow(win_);
      const float d = density();
      SDL_SetWindowSize(win_, std::max(1, int(std::lround(w / d))), std::max(1, int(std::lround(h / d))));
    }
    SDL_SyncWindow(win_);
    updateSize();
  }
  int dpi() const override {
    const float s = SDL_GetWindowDisplayScale(win_);
    return s > 0 ? int(std::lround(96.0f * s)) : 96;
  }
  void* glProc(const char* name) const override { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }
  bool makeCurrent() override {
    SDL_GLContext c = gl_ ? gl_ : owner_ ? owner_->gl_ : nullptr;
    return c && SDL_GL_MakeCurrent(win_, c);
  }
  bool focused() const override { return (SDL_GetWindowFlags(win_) & SDL_WINDOW_INPUT_FOCUS) != 0; }
  bool minimized() const override { return (SDL_GetWindowFlags(win_) & SDL_WINDOW_MINIMIZED) != 0; }
  void placement(int& x, int& y, int& w, int& h) const override {
    SDL_GetWindowPosition(win_, &x, &y);
    w = width_, h = height_;
  }
  void place(int x, int y, int w, int h) override {
    if (fullscreen()) setMode(false, w, h);
    const float d = density();
    SDL_SetWindowSize(win_, std::max(1, int(std::lround(std::max(w, 160) / d))), std::max(1, int(std::lround(std::max(h, 90) / d))));
    SDL_SetWindowPosition(win_, x, y);
    SDL_SyncWindow(win_);
    updateSize();
  }
  void focus() override { SDL_RaiseWindow(win_); }
  bool borderless() const override { return borderless_; }
  bool maximized() const override { return (SDL_GetWindowFlags(win_) & SDL_WINDOW_MAXIMIZED) != 0; }
  void chrome(float& closeX, float& closeY, float& closeSize, float& band) const override {
    const float s = scale();
    closeSize = std::round(32 * s);
    closeX = float(width_) - std::round(44 * s);
    closeY = std::round(10 * s);
    band = borderless_ && !fullscreen() ? std::round(40 * s) : 0;
  }
  float chromeGap() const override { return std::round(40 * scale()); }
  int chromeHot() const override { return borderless_ ? hot_ : 0; }
  void* nativeWindow() const override { return win_; }
  void* nativeInstance() const override { return nullptr; }
  std::vector<const char*> vulkanExtensions() const override {
    Uint32 n = 0;
    const char* const* e = SDL_Vulkan_GetInstanceExtensions(&n);
    return e ? std::vector<const char*>(e, e + n) : std::vector<const char*>{};
  }
  bool vulkanSurface(void* instance, void* surface) const override {
    return SDL_Vulkan_CreateSurface(win_, static_cast<VkInstance>(instance), nullptr, static_cast<VkSurfaceKHR*>(surface));
  }

  // SDL's one event queue, shared out to the windows it belongs to
  static void drainEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_EVENT_QUIT) {  // a termination signal: the first window closes (the game asks the scripts)
        if (!windows().empty()) windows().begin()->second->emit({InputEvent::Type::Close});
        continue;
      }
      const SDL_WindowID id = SDL_GetWindowFromEvent(&ev) ? SDL_GetWindowID(SDL_GetWindowFromEvent(&ev)) : 0;
      auto it = windows().find(id);
      if (it != windows().end()) it->second->handle(ev);
    }
    // the application as a whole gained or lost the focus (another program came to the front)
    const bool active = SDL_GetKeyboardFocus() != nullptr;
    if (active != appActive) {
      appActive = active;
      for (auto& [id, w] : windows()) w->emit({active ? InputEvent::Type::Activate : InputEvent::Type::Deactivate});
    }
  }

 private:
  float density() const {
    const float d = SDL_GetWindowPixelDensity(win_);
    return d > 0 ? d : 1.0f;
  }
  float scale() const { return float(dpi()) / 96.0f; }
  void updateSize() {
    SDL_GetWindowSizeInPixels(win_, &width_, &height_);
  }
  void applyCursor() {
    if (cursor_) SDL_ShowCursor();
    else if (SDL_GetMouseFocus() == win_) SDL_HideCursor();
  }
  void emit(InputEvent e) { queued_.push_back(e); }
  int px(float v) const { return int(std::lround(v * density())); }

  int buttonAt(int x, int y) const {
    float cx, cy, cs, band;
    chrome(cx, cy, cs, band);
    const float m = 4 * scale();
    auto in = [&](float bx) {
      return float(x) >= bx - m && float(x) < bx + cs + m && float(y) >= cy - m && float(y) < cy + cs + m;
    };
    return in(cx) ? 1 : in(cx - chromeGap()) ? 4 : 0;
  }
  // borderless windows: SDL asks what a point is on, and moves or sizes the window itself
  static SDL_HitTestResult SDLCALL hitTest(SDL_Window*, const SDL_Point* p, void* data) {
    auto* self = static_cast<SdlWindow*>(data);
    if (self->fullscreen()) return SDL_HITTEST_NORMAL;
    const int x = self->px(float(p->x)), y = self->px(float(p->y));
    const int w = self->width_, h = self->height_;
    if (!self->maximized()) {
      const int e = int(std::round(6 * self->scale()));
      const bool l = x < e, r = x >= w - e, t = y < e, b = y >= h - e;
      if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
      if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
      if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
      if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
      if (t) return SDL_HITTEST_RESIZE_TOP;
      if (b) return SDL_HITTEST_RESIZE_BOTTOM;
      if (l) return SDL_HITTEST_RESIZE_LEFT;
      if (r) return SDL_HITTEST_RESIZE_RIGHT;
    }
    float cx, cy, cs, band;
    self->chrome(cx, cy, cs, band);
    if (!self->buttonAt(x, y) && float(y) < band) return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
  }

  void handle(const SDL_Event& ev) {
    using T = InputEvent::Type;
    switch (ev.type) {
      case SDL_EVENT_MOUSE_MOTION: {
        const int x = px(ev.motion.x), y = px(ev.motion.y);
        if (borderless_) {
          const int b = buttonAt(x, y);
          float cx, cy, cs, band;
          chrome(cx, cy, cs, band);
          hot_ = b == 0 ? (float(y) < band && !fullscreen() ? 3 : 0) : b + (heldButton_ == b ? 1 : 0);
          if (heldButton_) return;
        }
        InputEvent e{T::MouseMove};
        e.x = x, e.y = y;
        emit(e);
        return;
      }
      case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        const bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const int x = px(ev.button.x), y = px(ev.button.y);
        int button = -1;
        if (ev.button.button == SDL_BUTTON_LEFT) button = 0;
        else if (ev.button.button == SDL_BUTTON_RIGHT) button = 1;
        else if (ev.button.button == SDL_BUTTON_MIDDLE) button = 2;
        if (button < 0) return;
        if (borderless_ && button == 0) {  // the close and fullscreen buttons: pressed and let go over the same one acts
          if (down && buttonAt(x, y)) {
            heldButton_ = buttonAt(x, y);
            hot_ = heldButton_ + 1;
            return;
          }
          if (!down && heldButton_) {
            const int was = heldButton_, b = buttonAt(x, y);
            heldButton_ = 0;
            hot_ = b;
            if (b == was) emit({b == 1 ? T::Close : T::ToggleFullscreen});
            return;
          }
        }
        SDL_CaptureMouse(down);
        InputEvent e{down ? T::MouseDown : T::MouseUp};
        e.x = x, e.y = y, e.button = button;
        emit(e);
        return;
      }
      case SDL_EVENT_MOUSE_WHEEL: {
        const float dir = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        const int x = px(ev.wheel.mouse_x), y = px(ev.wheel.mouse_y);
        if (ev.wheel.y != 0) {
          InputEvent e{T::MouseWheel};
          e.x = x, e.y = y, e.wheel = ev.wheel.y * dir;
          emit(e);
        }
        if (ev.wheel.x != 0) {
          InputEvent e{T::MouseHWheel};
          e.x = x, e.y = y, e.wheel = ev.wheel.x * dir;
          emit(e);
        }
        return;
      }
      case SDL_EVENT_PINCH_BEGIN: case SDL_EVENT_PINCH_UPDATE: case SDL_EVENT_PINCH_END: {
        float mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        InputEvent e{T::Gesture};
        e.x = px(mx), e.y = px(my);
        if (ev.type == SDL_EVENT_PINCH_UPDATE) {
          e.gesture = 1;
          e.value = ev.pinch.scale;
          emit(e);
        } else if (ev.type == SDL_EVENT_PINCH_END) {
          e.gesture = 4;
          emit(e);
        }
        return;
      }
      case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        const int vk = toVk(ev.key);
        if (!vk) return;
        const bool down = ev.type == SDL_EVENT_KEY_DOWN;
        if (vk < 256) held_[vk] = down;
        InputEvent e{down ? T::KeyDown : T::KeyUp};
        e.key = vk;
        e.repeat = ev.key.repeat;
        // Windows sends keys with Alt held (and F10) as system keys, which the game never sees
        e.system = (ev.key.mod & SDL_KMOD_ALT) != 0 || vk == 0x79 || vk == 0x12;
        emit(e);
        return;
      }
      case SDL_EVENT_TEXT_INPUT: {
        const std::u16string u = utf8ToUtf16(ev.text.text ? ev.text.text : "");
        for (size_t i = 0; i < u.size(); ++i) {
          char32_t c = u[i];
          if (c >= 0xD800 && c < 0xDC00 && i + 1 < u.size()) c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
          InputEvent e{T::Char};
          e.ch = c;
          emit(e);
        }
        return;
      }
      case SDL_EVENT_WINDOW_FOCUS_LOST:  // the keys held here are let go (their key-up events go elsewhere now)
        for (int k = 0; k < 256; ++k)
          if (held_[k]) {
            held_[k] = false;
            InputEvent e{T::KeyUp};
            e.key = k;
            emit(e);
          }
        return;
      case SDL_EVENT_WINDOW_MOUSE_ENTER: applyCursor(); return;
      case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (!heldButton_) hot_ = 0;
        SDL_ShowCursor();
        return;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
        updateSize();
        InputEvent e{T::Resize};
        e.x = width_, e.y = height_;
        emit(e);
        return;
      }
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED: emit({T::Close}); return;  // the game decides (scripts may ask first)
      default: return;
    }
  }

  SDL_Window* win_ = nullptr;
  SDL_GLContext gl_ = nullptr;
  SdlWindow* owner_ = nullptr;  // a secondary window drawn with this window's context
  bool openGL_ = true, resizable_ = false, secondary_ = false, borderless_ = false;
  bool cursor_ = true, clipped_ = false;
  int width_ = 0, height_ = 0;
  int hot_ = 0, heldButton_ = 0;
  bool held_[256] = {};
  std::vector<InputEvent> queued_;
};

}  // namespace

std::unique_ptr<Window> createWindow() { return std::make_unique<SdlWindow>(); }

bool otherMonitorWorkArea(const Window* away, int& x, int& y, int& w, int& h) {
  if (!videoUp()) return false;
  SDL_DisplayID own = away && away->nativeWindow() ? SDL_GetDisplayForWindow(static_cast<SDL_Window*>(away->nativeWindow()))
                                                   : SDL_GetPrimaryDisplay();
  int n = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&n);
  SDL_Rect best{0, 0, 0, 0};
  long long area = -1;
  for (int i = 0; i < n; ++i) {
    if (ids[i] == own) continue;
    SDL_Rect r;
    if (!SDL_GetDisplayUsableBounds(ids[i], &r)) continue;
    const long long a = (long long)r.w * r.h;
    if (a > area) area = a, best = r;
  }
  SDL_free(ids);
  if (area < 0) SDL_GetDisplayUsableBounds(own, &best);
  x = best.x, y = best.y, w = best.w, h = best.h;
  return area >= 0;
}

bool onSomeMonitor(int x, int y, int w, int h) {
  if (!videoUp()) return false;
  const SDL_Rect r{x, y, std::max(w, 1), std::max(h, 1)};
  int n = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&n);
  bool on = false;
  for (int i = 0; i < n && !on; ++i) {
    SDL_Rect d, in;
    if (SDL_GetDisplayBounds(ids[i], &d) && SDL_GetRectIntersection(&r, &d, &in))
      on = (long long)in.w * in.h * 2 >= (long long)r.w * r.h;
  }
  SDL_free(ids);
  return on;
}

}  // namespace regs::platform
