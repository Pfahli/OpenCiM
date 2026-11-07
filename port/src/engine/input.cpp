// Input: OS events → window events → input target (gsDesktop) → controls. Follows docs/ui/input.md; the mouse is
// fed in absolute mode once per tick (mousePos), as in the official Linux build.
#include "engine/input.h"

#include <algorithm>
#include <string>
#include <string_view>

#include "engine/api.h"
#include "engine/display.h"
#include "engine/font.h"
#include "engine/engine.h"
#include "engine/gui.h"
#include "engine/host.h"
#include "engine/interaction.h"

namespace regs::engine {

using namespace regs::script;

namespace {

// gsKeyName table (W:0x100828f0, names from L:0x080c73a0)
const char16_t* const kKeyNames[kKeyCount] = {
    u"Undefined", u"Left", u"Right", u"Up", u"Down", u"Backspace", u"Enter", u"Space", u"Tab", u"Control", u"Alt",
    u"Shift", u"Esc", u"Pause", u"Clear", u"Select", u"Print", u"Execute", u"PrintScreen", u"Help", u"Multiply",
    u"Add", u"Separator", u"Subtract", u"Decimal", u"Divide", u"CapsLock", u"NumLock", u"ScrollLock", u"LeftWin",
    u"RightWin", u"Applications", u"LeftShift", u"RightShift", u"LeftControl", u"RightControl", u"LeftAlt",
    u"RightAlt", u"Play", u"Zoom", u"Sleep", u"Cancel", u"Convert", u"NonConvert", u"Accept", u"ModeChange", u"Kana",
    u"Junja", u"Final", u"Hanja", u"Plus", u"Comma", u"Minus", u"Period", u"Oem1", u"Oem2", u"Oem3", u"Oem4", u"Oem5",
    u"Oem6", u"Oem7", u"Oem8", u"Ax", u"Oem102", u"IcoHelp", u"Ico00", u"NumpadEqual", u"Insert", u"Delete", u"Home",
    u"End", u"PageUp", u"PageDown", u"A", u"B", u"C", u"D", u"E", u"F", u"G", u"H", u"I", u"J", u"K", u"L", u"M", u"N",
    u"O", u"P", u"Q", u"R", u"S", u"T", u"U", u"V", u"W", u"X", u"Y", u"Z", u"0", u"1", u"2", u"3", u"4", u"5", u"6",
    u"7", u"8", u"9", u"Numpad0", u"Numpad1", u"Numpad2", u"Numpad3", u"Numpad4", u"Numpad5", u"Numpad6", u"Numpad7",
    u"Numpad8", u"Numpad9", u"F1", u"F2", u"F3", u"F4", u"F5", u"F6", u"F7", u"F8", u"F9", u"F10", u"F11", u"F12",
    u"F13", u"F14", u"F15", u"F16", u"F17", u"F18", u"F19", u"F20", u"F21", u"F22", u"F23", u"F24", u"MouseLeft",
    u"MouseMiddle", u"MouseRight", u"MouseWheelUp", u"MouseWheelDown", u"AxisX", u"AxisY", u"AxisZ", u"RotationX",
    u"RotationY", u"RotationZ", u"Button0", u"Button1", u"Button2", u"Button3", u"Button4", u"Button5", u"Button6",
    u"Button7", u"Button8", u"Button9", u"Button10", u"Button11", u"Button12", u"Button13", u"Button14", u"Button15",
    u"Button16", u"Button17", u"Button18", u"Button19", u"Button20", u"Button21", u"Button22", u"Button23", u"Button24",
    u"Button25", u"Button26", u"Button27", u"Button28", u"Button29", u"Button30", u"Button31"};

// Windows virtual key → gsKey (FUN_100562c0); 0 = dropped
int vkToKey(int vk) {
  if (vk >= '0' && vk <= '9') return 99 + vk - '0';
  if (vk >= 'A' && vk <= 'Z') return 73 + vk - 'A';
  if (vk >= 0x60 && vk <= 0x69) return 109 + vk - 0x60;
  if (vk >= 0x70 && vk <= 0x87) return 119 + vk - 0x70;
  if (vk >= 0x1C && vk <= 0x1F) return 42 + vk - 0x1C;
  if (vk >= 0xA0 && vk <= 0xA5) return 32 + vk - 0xA0;
  switch (vk) {
    case 0x03: return 41; case 0x08: return 5; case 0x09: return 8; case 0x0C: return 14; case 0x0D: return 6;
    case 0x10: return 11; case 0x11: return 9; case 0x12: return 10; case 0x13: return 13; case 0x14: return 26;
    case 0x15: return 46; case 0x17: return 47; case 0x18: return 48; case 0x19: return 49; case 0x1B: return 12;
    case 0x20: return 7; case 0x21: return 71; case 0x22: return 72; case 0x23: return 70; case 0x24: return 69;
    case 0x25: return 1; case 0x26: return 3; case 0x27: return 2; case 0x28: return 4; case 0x29: return 15;
    case 0x2A: return 16; case 0x2B: return 17; case 0x2C: return 18; case 0x2D: return 67; case 0x2E: return 68;
    case 0x2F: return 19; case 0x5B: return 29; case 0x5C: return 30; case 0x5D: return 31; case 0x5F: return 40;
    case 0x6A: return 20; case 0x6B: return 21; case 0x6C: return 22; case 0x6D: return 23; case 0x6E: return 24;
    case 0x6F: return 25; case 0x90: return 27; case 0x91: return 28; case 0x92: return 66;
    case 0xBA: return 54; case 0xBB: return 50; case 0xBC: return 51; case 0xBD: return 52; case 0xBE: return 53;
    case 0xBF: return 55; case 0xC0: return 56; case 0xDB: return 57; case 0xDC: return 58; case 0xDD: return 59;
    case 0xDE: return 60; case 0xDF: return 61; case 0xE1: return 62; case 0xE2: return 63; case 0xE3: return 64;
    case 0xE4: return 65; case 0xFA: return 38; case 0xFB: return 39;
    default: return 0;
  }
}

Value obj(Object* o) { return Value::heap(VT::Object, o); }
Value keyValue(int key) { return Value::string(keyName(key)); }
bool isMouseKey(int k) { return k == kMouseLeft || k == kMouseMiddle || k == kMouseRight; }
int maskBit(int k) {
  switch (k) {
    case kMouseLeft: return 1;
    case kMouseMiddle: return 2;
    case kMouseRight: return 4;
    case kKeyShift: return 0x100;
    case kKeyControl: return 0x200;
    case kKeyAlt: return 0x400;
    default: return 0;
  }
}
ControlObj* asControl(const Ref<Object>& r) { return r && !r->destroyed ? dynamic_cast<ControlObj*>(r.get()) : nullptr; }

WindowObj* gameWindow() {
  Display* d = display();
  return d ? dynamic_cast<WindowObj*>(d->windowObject()) : nullptr;
}

// Default gsInputObject behaviour for targets other than the desktop: owner keyDown/keyRepeat/keyUp[name].
void targetKey(VM& vm, Object* t, const char16_t* ev, int key) {
  if (auto* d = dynamic_cast<DesktopObj*>(t)) {
    if (ev[3] == u'D') d->inputKeyDown(vm, key);
    else if (ev[3] == u'U') d->inputKeyUp(vm, key);
    else d->keyRepeat(vm, key, d->mask);
    return;
  }
  Value k = keyValue(key);
  vm.fireEvent(t, ev, {}, &k);
}

}  // namespace

const char16_t* keyName(int key) { return key >= 0 && key < kKeyCount ? kKeyNames[key] : kKeyNames[0]; }

// ---------------------------------------------------------------------------------------------------------------
// gsControl input behaviour (§3.4-3.10)

ControlObj* ControlObj::parentControl() const { return dynamic_cast<ControlObj*>(parent); }

bool ControlObj::shown() const {
  for (const ControlObj* c = this; c; c = c->parentControl())
    if ((c->flags & 3) != 3) return false;
  return true;
}

bool ControlObj::firesMouseEvents() const { return !isTextSel(); }

bool ControlObj::isTextSel() const {
  for (const ClassInfo* k = cls; k; k = k->parent)
    if (k->name == "gsTextSelCtrl") return true;
  return false;
}

bool ControlObj::isEdit() const {
  for (const ClassInfo* k = cls; k; k = k->parent)
    if (k->name == "gsEditCtrl") return true;
  return false;
}

// ---------------------------------------------------------------------------------------------------------------
// gsTextSelCtrl / gsEditCtrl text entry (§7; controls.md §11): selStart is the caret, selLength the selection

namespace {

int fieldInt(VM& vm, const ControlObj* c, const char16_t* name, int def) {
  auto it = c->fieldValues.find(name);
  return it == c->fieldValues.end() || it->second.isNone() ? def : vm.toInt(it->second);
}

void editSelection(VM& vm, const ControlObj* c, int& start, int& length) {
  int n = int(c->raw.size());
  start = std::clamp(fieldInt(vm, c, u"selStart", 0), 0, n);
  length = std::clamp(fieldInt(vm, c, u"selLength", 0), 0, n - start);
}

void editSetText(VM& vm, ControlObj* c, const std::u16string& s, int caret) {
  c->setText(vm, s);
  c->fieldValues[u"selStart"] = Value::integer(caret);
  c->fieldValues[u"selLength"] = Value::integer(0);
  vm.fireEvent(c, u"onChange", {});
}

// gsEditCtrl::keyChar (L:0x081d5500:26-69): onCheckInput may reject, the selection goes, the char is inserted
// while maxLength allows
void editChar(VM& vm, ControlObj* c, char32_t ch) {
  if (ch <= 0x1F || ch > 0xFFFF) return;
  c->fieldValues[u"rejectInput"] = Value::boolean(false);
  vm.fireEvent(c, u"onCheckInput", {Value::integer(int32_t(ch))});
  auto r = c->fieldValues.find(u"rejectInput");
  if (r != c->fieldValues.end() && !r->second.isNone() && vm.truthy(r->second)) return;
  int start, length;
  editSelection(vm, c, start, length);
  std::u16string s = c->raw;
  s.erase(size_t(start), size_t(length));
  int maxLength = fieldInt(vm, c, u"maxLength", -1);
  if (maxLength == -1 || int(s.size()) < maxLength) s.insert(s.begin() + start++, char16_t(ch));
  editSetText(vm, c, s, start);
}

// gsEditCtrl::keyPress (L:0x081d62f0:28-65): Left/Right move the caret, Backspace/Delete remove; Enter and Esc are
// the scripts' (keyDown events)
void editKey(VM& vm, ControlObj* c, int key) {
  std::u16string_view name = keyName(key);
  int start, length;
  editSelection(vm, c, start, length);
  std::u16string s = c->raw;
  if (name == u"Left" || name == u"Right") {
    c->fieldValues[u"selStart"] = Value::integer(std::clamp(start + (name == u"Left" ? -1 : 1), 0, int(s.size())));
    c->fieldValues[u"selLength"] = Value::integer(0);
  } else if (name == u"Backspace" || name == u"Delete") {
    if (length > 0) s.erase(size_t(start), size_t(length));
    else if (name == u"Backspace" && start > 0) s.erase(size_t(--start), 1);
    else if (name == u"Delete" && start < int(s.size())) s.erase(size_t(start), 1);
    else return;
    editSetText(vm, c, s, start);
  }
}

// a press places the caret at the nearest character boundary of the (single-line) text; no mouse events
void textSelPress(VM& vm, ControlObj* c, float x) {
  int pos = int(c->display.size());
  if (Font* f = controlFont(vm, c); f && !f->glyphs.empty()) {
    TextParams p;
    float tx, ty;
    controlTextPos(c, *f, p, tx, ty);
    float pen = tx, at = c->desktopLeft() + x;
    for (size_t i = 0; i < c->display.size(); ++i) {
      float adv = charAdvance(*f, c->display[i], p);
      if (at < pen + adv * 0.5f) {
        pos = int(i);
        break;
      }
      pen += adv;
    }
  }
  c->fieldValues[u"selStart"] = Value::integer(std::min(pos, int(c->raw.size())));
  c->fieldValues[u"selLength"] = Value::integer(0);
}

}  // namespace

ControlObj* ControlObj::controlAt(float px, float py) {
  float lx = px - marginL, ly = py - marginT;
  for (size_t i = children.size(); i-- > 0;) {
    auto* c = dynamic_cast<ControlObj*>(children[i].get());
    if (!c || c->destroyed || c->clickThrough || !c->shown()) continue;
    if (lx < c->left + c->marginL || lx > c->left + c->width - c->marginR) continue;
    if (ly < c->top + c->marginT || ly > c->top + c->height - c->marginB) continue;
    if (ControlObj* r = c->controlAt(lx - c->left, ly - c->top)) return r;
  }
  return clickThroughSelf ? nullptr : this;
}

void ControlObj::mouseOver(VM& vm) {
  if (ControlObj* p = parentControl()) p->mouseOver(vm);
  hot = true;
  vm.fireEvent(this, u"onMouseOver", {});
}

void ControlObj::mouseOut(VM& vm) {
  if (ControlObj* p = parentControl()) p->mouseOut(vm);
  hot = false;
  vm.fireEvent(this, u"onMouseOut", {});
}

bool ControlObj::mouseMove(VM& vm, float x, float y, float dx, float dy, int buttons) {
  if (!firesMouseEvents()) return true;
  gridPlayerMouseMove(vm, this, x, y, buttons);  // gsGridPlayer's override: hover picking (selection.md §3)
  std::vector<Value> a{Value::real(x), Value::real(y), Value::real(dx), Value::real(dy)};
  if (dragging) {
    if (ControlObj* t = asControl(dragTarget)) t->setRect(vm, t->left + x - dragX, t->top + y - dragY, t->width, t->height);
    vm.fireEvent(this, u"onMouseDrag", a);
  }
  vm.fireEvent(this, u"onMouseMove", a);
  return true;
}

void ControlObj::mouseDown(VM& vm, float x, float y, int key, int buttons) {
  if (!firesMouseEvents()) {
    if (isTextSel()) textSelPress(vm, this, x);
    return;
  }
  gridPlayerMouseDown(vm, this, x, y, key);  // the select click (selection.md §4.2)
  if (key == kMouseLeft) {
    dragX = x;
    dragY = y;
    dragging = true;
  }
  Value k = keyValue(key);
  vm.fireEvent(this, u"mouseDown", {Value::real(x), Value::real(y)}, &k);
}

void ControlObj::mouseUp(VM& vm, float x, float y, int key, int buttons) {
  if (!firesMouseEvents()) return;
  gridPlayerMouseUp(vm, this, x, y, key);
  if (key == kMouseLeft) dragging = false;
  Value k = keyValue(key);
  vm.fireEvent(this, u"mouseUp", {Value::real(x), Value::real(y)}, &k);
}

void ControlObj::keyDown(VM& vm, int key, int mask) {
  Ref<Object> keep(this);
  if (isEdit()) editKey(vm, this, key);
  Value k = keyValue(key);
  vm.fireEvent(this, u"keyDown", {}, &k);
  vm.fireEvent(this, u"onKeyDown", {k});
  if (ControlObj* c = asControl(activeChild)) c->keyDown(vm, key, mask);
}

void ControlObj::keyUp(VM& vm, int key, int mask) {
  Ref<Object> keep(this);
  Value k = keyValue(key);
  vm.fireEvent(this, u"keyUp", {}, &k);
  vm.fireEvent(this, u"onKeyUp", {k});
  if (ControlObj* c = asControl(activeChild)) c->keyUp(vm, key, mask);
}

void ControlObj::keyRepeat(VM& vm, int key, int mask) {
  Ref<Object> keep(this);
  if (isEdit()) editKey(vm, this, key);
  Value k = keyValue(key);
  vm.fireEvent(this, u"keyRepeat", {}, &k);
  if (ControlObj* c = asControl(activeChild)) c->keyRepeat(vm, key, mask);
}

void ControlObj::keyChar(VM& vm, char32_t ch, int mask) {
  Ref<Object> keep(this);
  if (isEdit()) editChar(vm, this, ch);
  if (ControlObj* c = asControl(activeChild)) c->keyChar(vm, ch, mask);
}

bool ControlObj::shortCut(VM& vm, int key) {
  ControlObj* c = asControl(activeChild);
  return c && c->shortCut(vm, key);
}

void ControlObj::bringToFront() {
  if (!parent) return;
  auto& k = parent->children;
  auto it = std::find_if(k.begin(), k.end(), [this](const Ref<Object>& r) { return r.get() == this; });
  if (it == k.end()) return;
  Ref<Object> keep = *it;
  k.erase(it);
  k.push_back(keep);
}

void ControlObj::activate(VM& vm) {
  Ref<Object> keep(this);
  setActive(vm, true);
  if (ControlObj* p = parentControl()) p->activate(vm);
}

void ControlObj::setActive(VM& vm, bool on) {
  if (!canActivate || active == on) return;
  Ref<Object> keep(this);
  active = on;
  ControlObj* p = parentControl();
  if (!on) {
    if (isEdit()) {  // the caret restarts dark, the selection goes
      caretPhase = 0;
      fieldValues[u"selLength"] = Value::integer(0);
    }
    vm.fireEvent(this, u"onDeactivate", {});
    if (p && p->activeChild.get() == this) p->activeChild = nullptr;
    if (ControlObj* c = asControl(activeChild)) c->setActive(vm, false);
    return;
  }
  vm.fireEvent(this, u"onActivate", {});
  if (p) {  // setActiveChild: the previous active sibling deactivates
    ControlObj* old = asControl(p->activeChild);
    p->activeChild = Ref<Object>(this);
    if (old && old != this) old->setActive(vm, false);
  }
  bringToFront();
  flags |= 2;  // raiseStatus(2): an activated control is shown
  if (ControlObj* c = asControl(activeChild)) {
    if (c->visible()) c->setActive(vm, true);
    else activeChild = nullptr;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// gsDesktop (§3.2-3.8)

void DesktopObj::inputMouseMove(VM& vm, float dx, float dy) {
  Ref<Object> keep(this);
  float H = height, W = width, ox = mouseX, oy = mouseY;
  float nx = std::clamp(ox + dx * H, 0.0f, W), ny = std::clamp(oy + dy * H, 0.0f, H);
  if (ControlObj* m = asControl(pointer)) m->setRect(vm, ox, oy, m->width, m->height);  // pointer at the OLD position
  ControlObj* cap = asControl(capture);
  if (!cap || cap == this) {
    ControlObj* hit = controlAt(ox, oy);
    if (hit != asControl(hover)) {
      if (ControlObj* h = asControl(hover)) h->mouseOut(vm);
      hover = Ref<Object>(hit);
      if (hit) hit->mouseOver(vm);
    }
    if (hit && !hit->mouseMove(vm, nx - hit->desktopLeft(), ny - hit->desktopTop(), nx - ox, ny - oy, mask)) {
      nx = ox;
      ny = oy;
    }
  } else if (!cap->mouseMove(vm, nx - cap->desktopLeft(), ny - cap->desktopTop(), nx - ox, ny - oy, mask)) {
    nx = ox;
    ny = oy;
  }
  mouseX = nx;
  mouseY = ny;
}

void DesktopObj::inputMousePos(VM& vm, float x, float y) {
  if (height != 0) inputMouseMove(vm, (x * width - mouseX) / height, (y * height - mouseY) / height);
}

void DesktopObj::inputKeyDown(VM& vm, int k) {
  Ref<Object> keep(this);
  mask |= maskBit(k);
  if (isMouseKey(k)) {
    lastDownKey = k;
    ControlObj* c = controlAt(mouseX, mouseY);
    capture = Ref<Object>(c);
    if (c != asControl(hover)) {
      if (ControlObj* h = asControl(hover)) h->mouseOut(vm);
      hover = Ref<Object>(c);
      if (c) c->mouseOver(vm);
    }
    if (ControlObj* e = asControl(loneEdit); e && e != c) {  // a click elsewhere: the lone edit lets the keyboard go
      loneEdit = nullptr;
      if (!e->destroyed) e->setActive(vm, false);
    }
    if (c) {
      Ref<Object> keepC(c);
      c->activate(vm);
      if (c->isEdit() && c->active)  // the chain up to the desktop broken by a control that cannot be activated?
        for (ControlObj* p = c->parentControl(); p && p != this; p = p->parentControl())
          if (!p->canActivate) {
            loneEdit = Ref<Object>(c);
            break;
          }
      c->mouseDown(vm, mouseX - c->desktopLeft(), mouseY - c->desktopTop(), k, mask);
    }
  } else if (ControlObj* e = asControl(loneEdit); e && !e->destroyed && e->active && e->visible()) {
    if (k == kKeyEscape || k == kKeyEnter) {  // done typing: the edit lets go, the key is swallowed
      loneEdit = nullptr;
      e->setActive(vm, false);
    } else {
      e->keyDown(vm, k, mask);  // Backspace, arrows; not the game's hotkeys (the camera stays put while typing)
    }
    return;
  } else if (k == kKeyControl || k == kKeyAlt) {
    shortcutMode = true;
  } else if (shortcutMode && shortCut(vm, k)) {
    return;  // a handled shortcut fires no keyDown events
  }
  keyDown(vm, k, mask);
}

void DesktopObj::inputKeyUp(VM& vm, int k) {
  Ref<Object> keep(this);
  mask &= ~maskBit(k);
  if (isMouseKey(k)) {
    if (ControlObj* c = asControl(capture)) {
      Ref<Object> keepC(c);
      if (lastDownKey == k) capture = nullptr;
      c->mouseUp(vm, mouseX - c->desktopLeft(), mouseY - c->desktopTop(), k, mask);
    }
    if (lastDownKey == k) {
      ControlObj* hit = controlAt(mouseX, mouseY);
      if (hit != asControl(hover)) {
        if (ControlObj* h = asControl(hover)) h->mouseOut(vm);
        hover = Ref<Object>(hit);
        if (hit) hit->mouseOver(vm);
      }
    }
  }
  if (k == kKeyControl || k == kKeyAlt) shortcutMode = false;
  keyUp(vm, k, mask);
}

void DesktopObj::inputFocus(VM& vm, bool on) {
  Ref<Object> keep(this);
  if (on) {
    inputMouseMove(vm, 0, 0);
    return;
  }
  if (ControlObj* c = asControl(capture)) {
    Ref<Object> keepC(c);
    capture = nullptr;
    c->mouseUp(vm, mouseX - c->desktopLeft(), mouseY - c->desktopTop(), lastDownKey, mask);
  }
  if (ControlObj* h = asControl(hover)) {
    Ref<Object> keepH(h);
    hover = nullptr;
    h->mouseOut(vm);
  }
}

void DesktopObj::resetMouseDown(VM& vm) {
  mask = 0;
  if (ControlObj* c = asControl(capture)) {
    Ref<Object> keepC(c);
    capture = nullptr;
    c->mouseUp(vm, mouseX - c->desktopLeft(), mouseY - c->desktopTop(), lastDownKey, 0);
  }
}

void DesktopObj::setPointer(VM&, Object* o) {
  pointer = Ref<Object>(o);
  if (auto* c = dynamic_cast<ControlObj*>(o)) {
    c->clickThrough = true;
    mouseX = c->left;
    mouseY = c->top;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// window: input targets (§2.2), per-tick mouse (§1.3), per-message keys (§1.1)

void WindowObj::updateInputTarget(VM& vm) {
  Ref<Object> best;
  int prio = 0;
  for (auto& [p, o] : targets)
    if (prio <= p) {
      best = o;
      prio = p;
    }
  if (best.get() == inputTarget.get()) return;
  Ref<Object> old = inputTarget;
  inputTarget = best;
  if (auto* d = dynamic_cast<DesktopObj*>(old.get())) d->inputFocus(vm, false);
  if (auto* d = dynamic_cast<DesktopObj*>(best.get())) d->inputFocus(vm, true);
}

void pollMouse(VM& vm, WindowObj* w) {
  w->snapshot = w->inputTarget;
  Display* d = display();
  if (!d || !w->appActive) return;
  Value cap = vm.getMember(obj(w), u"mouseCapture");
  if (!vm.truthy(cap)) return;
  auto* desk = dynamic_cast<DesktopObj*>(w->inputTarget.get());
  if (!desk || desk->destroyed) return;
  // until the OS reports a position the pointer stays where the scripts put it (hover is still re-evaluated)
  if (!d->mouseKnown()) {
    desk->inputMouseMove(vm, 0, 0);
    return;
  }
  float ww = float(std::max(1, d->window().width())), wh = float(std::max(1, d->window().height()));
  desk->inputMousePos(vm, float(d->mouseX()) / ww, float(d->mouseY()) / wh);
}

void dispatchInput(Engine& eng) {
  WindowObj* w = gameWindow();
  Display* d = display();
  VM& vm = eng.vm();
  using T = platform::InputEvent::Type;
  for (const auto& e : host().input) {
    if (e.type == T::Close) {
      host().quit = true;  // WM_CLOSE → PostQuitMessage
      continue;
    }
    if (!w) continue;
    Ref<Object> keepW(w);
    bool events = !vm.truthy(vm.getMember(obj(w), u"disableInputEvents"));
    auto key = [&](const char16_t* ev, int k) {
      w->fieldValues[u"key"] = Value::integer(k);
      Value name = keyValue(k);
      if (events) vm.fireEvent(w, ev, {}, &name);
      if (Object* t = w->snapshot.get(); t && !t->destroyed) targetKey(vm, t, ev, k);
    };
    switch (e.type) {
      case T::MouseMove:
        if (d) d->setMouse(e.x, e.y);
        break;
      case T::MouseDown:
      case T::MouseUp:
        if (d) d->setMouse(e.x, e.y);
        if (e.button <= 2) key(e.type == T::MouseDown ? u"keyDown" : u"keyUp", e.button == 0 ? kMouseLeft : e.button == 1 ? kMouseRight : kMouseMiddle);
        break;
      case T::MouseWheel:
        w->wheel += e.wheel;
        for (; w->wheel >= 1; w->wheel -= 1) key(u"keyDown", kWheelUp);
        for (; w->wheel <= -1; w->wheel += 1) key(u"keyDown", kWheelDown);
        break;
      case T::KeyDown:
      case T::KeyUp: {
        if (e.system) break;  // WM_SYSKEY* never reach the engine (Alt, F10)
        int k = vkToKey(e.key);
        if (!k) break;
        key(e.type == T::KeyUp ? u"keyUp" : e.repeat ? u"keyRepeat" : u"keyDown", k);
        break;
      }
      case T::Char:
        if (auto* desk = dynamic_cast<DesktopObj*>(w->snapshot.get()); desk && !desk->destroyed) {
          ControlObj* lone = asControl(desk->loneEdit);  // a lone edit (gui.h) takes the characters itself
          if (lone && lone->active && lone->visible()) lone->keyChar(vm, e.ch, desk->mask);
          else desk->keyChar(vm, e.ch, desk->mask);
        }
        break;
      case T::Activate:
      case T::Deactivate:
        w->appActive = e.type == T::Activate;
        vm.fireEvent(w, w->appActive ? u"onActivate" : u"onDeactivate", {});
        break;
      default: break;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------

void registerInputNatives(VM& vm) {
  if (ClassInfo* c = vm.findClass("gsDesktop")) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new DesktopObj(ci)); };
  implement(vm, "gsWindow", u"addInputTarget", [](VM& v, Object* s, int argc, const Value* a) {
    auto* w = dynamic_cast<WindowObj*>(s);
    Object* t = v.resolve(a[0]).object();
    if (!w || !dynamic_cast<DesktopObj*>(t)) return Value();  // must be a gsInputObject
    w->targets.emplace_back(argc > 1 ? v.toInt(a[1]) : 0, Ref<Object>(t));
    w->updateInputTarget(v);
    return Value();
  });
  implement(vm, "gsWindow", u"removeInputTarget", [](VM& v, Object* s, int, const Value* a) {
    auto* w = dynamic_cast<WindowObj*>(s);
    Object* t = v.resolve(a[0]).object();
    if (!w) return Value();
    for (size_t i = w->targets.size(); i-- > 0;)
      if (w->targets[i].second.get() == t) {
        w->targets.erase(w->targets.begin() + std::ptrdiff_t(i));
        break;
      }
    w->updateInputTarget(v);
    return Value();
  });
  if (ClassInfo* c = vm.findClass("gsWindow")) {
    FieldInfo& f = c->fields[u"inputTarget"];
    f.name = u"inputTarget";
    f.readOnly = true;
    f.get = [](VM&, Object* o) {
      auto* w = dynamic_cast<WindowObj*>(o);
      return w && w->inputTarget ? obj(w->inputTarget.get()) : Value();
    };
  }
  if (ClassInfo* c = vm.findClass("gsDesktop")) {
    for (const char16_t* n : {u"mouseX", u"mouseY"}) {
      FieldInfo& f = c->fields[n];
      f.name = n;
      f.readOnly = true;
      bool x = n[5] == u'X';
      f.get = [x](VM&, Object* o) { auto* d = static_cast<DesktopObj*>(o); return Value::real(x ? d->mouseX : d->mouseY); };
    }
    FieldInfo& m = c->fields[u"mouse"];
    m.name = u"mouse";
    m.get = [](VM&, Object* o) { auto* d = static_cast<DesktopObj*>(o); return d->pointer ? obj(d->pointer.get()) : Value(); };
    m.set = [](VM& v, Object* o, const Value& val) { static_cast<DesktopObj*>(o)->setPointer(v, v.resolve(val).object()); };
  }
  implement(vm, "gsDesktop", u"resetMouseDown", [](VM& v, Object* s, int, const Value*) {
    static_cast<DesktopObj*>(s)->resetMouseDown(v);
    return Value();
  });
  implement(vm, "gsControl", u"mouseMove", [](VM& v, Object* s, int argc, const Value* a) {
    auto f = [&](int i) { return i < argc ? v.toFloat(a[i]) : 0.0f; };
    bool r = static_cast<ControlObj*>(s)->mouseMove(v, f(0), f(1), f(2), f(3), argc > 4 ? v.toInt(a[4]) : 0);
    return Value::boolean(r);
  });
  // input flags of every control (gsControl +0x186..+0x1C4)
  if (ClassInfo* c = vm.findClass("gsControl")) {
    auto boolField = [c](const char16_t* n, bool ControlObj::*m) {
      FieldInfo& f = c->fields[n];
      f.name = n;
      f.type = 2;
      f.get = [m](VM&, Object* o) { return Value::boolean(static_cast<ControlObj*>(o)->*m); };
      f.set = [m](VM& v, Object* o, const Value& val) { static_cast<ControlObj*>(o)->*m = v.truthy(val); };
    };
    boolField(u"canActivate", &ControlObj::canActivate);
    boolField(u"clickThrough", &ControlObj::clickThrough);
    boolField(u"clickThroughSelf", &ControlObj::clickThroughSelf);
    FieldInfo& act = c->fields[u"active"];
    act.name = u"active";
    act.type = 2;
    act.get = [](VM&, Object* o) { return Value::boolean(static_cast<ControlObj*>(o)->active); };
    act.set = [](VM& v, Object* o, const Value& val) { static_cast<ControlObj*>(o)->setActive(v, v.truthy(val)); };
    FieldInfo& dt = c->fields[u"dragTarget"];
    dt.name = u"dragTarget";
    dt.get = [](VM&, Object* o) { auto* k = static_cast<ControlObj*>(o); return k->dragTarget ? obj(k->dragTarget.get()) : Value(); };
    dt.set = [](VM& v, Object* o, const Value& val) { static_cast<ControlObj*>(o)->dragTarget = Ref<Object>(v.resolve(val).object()); };
  }
}

}  // namespace regs::engine
