// Smooth camera input (owner's request 2026-11-17): the OS input that turns and zooms the game camera without steps.
// The game's own camera (metro/scripts/ui.script) turns in 45-degree steps on the rotate keys and zooms by 1.2 per
// wheel notch through key events. With the smooth camera on (the gameUI extension sets regsSmoothCamera(true)):
//   * the wheel over the world (the grid player or the editor under the pointer, not a panel) zooms in fractions of a
//     notch, so touchpads (two-finger scroll, pinch = Ctrl + wheel) zoom steplessly; Shift + wheel stays the place
//     tool's turn (tools.h toolWheel);
//   * the sideways wheel (two fingers swiped sideways on a touchpad, a tilting wheel) turns the camera;
//   * Ctrl + middle mouse drag turns it with the mouse;
//   * two fingers on a touchscreen zoom (pinch), turn (twist) and move (pan) it; a twist on a touchpad turns it.
// The amounts collect here and the gameUI extension takes them once a frame (regsCameraInput), eases or follows them
// and snaps to the eight main directions (docs: port/docs/camera.md).
#include <cmath>
#include <string>

#include "engine/api.h"
#include "engine/gui.h"
#include "engine/input.h"
#include "platform/window.h"

namespace regs::engine {

using namespace regs::script;

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDragTurn = 0.25f * kPi / 180.0f;  // Ctrl + middle drag: a quarter of a degree per pixel
constexpr float kSideNotch = kPi / 16.0f;          // sideways wheel: 11.25 degrees per notch

struct CameraInput {
  bool smooth = false;
  float zoomSmooth = 0, zoomDirect = 0;  // ln of the distance factor: eased (wheel) / followed at once (pinch)
  float turnSmooth = 0, turnDirect = 0;  // radians, the map turning counterclockwise on screen positive
  float panX = 0, panY = 0;              // pixels the two-finger centre moved
  float pivotX = -1, pivotY = -1;        // the last gesture's centre (client pixels)
  int released = 0;                      // free turns that ended (fingers lifted, drag released)
  bool dragging = false;
  int dragX = 0;
  bool held[kKeyCount] = {};
} g_cam;

// the pointer is over the world: the desktop's hover control is the player (MAIN.$player) or inside it
bool overWorld(VM& vm, WindowObj* w) {
  auto* desk = dynamic_cast<DesktopObj*>(w->snapshot.get());
  if (!desk || desk->destroyed) return false;
  Value* main = vm.findGlobal(u"MAIN");
  if (!main) return false;
  Object* player = vm.getMember(*main, u"$player").object();
  if (!player) return false;
  for (Object* o = desk->hover.get(); o && !o->destroyed;) {
    if (o == player) return true;
    auto* c = dynamic_cast<ControlObj*>(o);
    o = c ? c->parentControl() : nullptr;
  }
  return false;
}

}  // namespace

bool cameraInputEvent(VM& vm, WindowObj* w, const platform::InputEvent& e) {
  using T = platform::InputEvent::Type;
  switch (e.type) {
    case T::KeyDown:
    case T::KeyUp:
      if (!e.system)
        if (int k = engineKeyOf(e.key); k > 0 && k < kKeyCount) g_cam.held[k] = e.type == T::KeyDown;
      return false;
    case T::MouseDown:
    case T::MouseUp: {
      const int k = e.button == 0 ? kMouseLeft : e.button == 1 ? kMouseRight : kMouseMiddle;
      g_cam.held[k] = e.type == T::MouseDown;
      if (e.button != 2) return false;
      if (e.type == T::MouseDown && g_cam.smooth && (inputModifiers() & 0x200) && overWorld(vm, w)) {
        g_cam.dragging = true;  // Ctrl + middle: turning, not the game's panning
        g_cam.dragX = e.x;
        return true;
      }
      if (e.type == T::MouseUp && g_cam.dragging) {
        g_cam.dragging = false;
        ++g_cam.released;
        return true;
      }
      return false;
    }
    case T::MouseMove:
      if (g_cam.dragging) {
        g_cam.turnDirect += float(e.x - g_cam.dragX) * kDragTurn;
        g_cam.dragX = e.x;
      }
      return false;
    case T::MouseWheel:
      if (!g_cam.smooth || (inputModifiers() & 0x100) || !overWorld(vm, w)) return false;
      g_cam.zoomSmooth -= e.wheel * std::log(1.2f);  // a notch: the game's factor 1.2, nearer away from the user
      return true;
    case T::MouseHWheel:
      if (g_cam.smooth && overWorld(vm, w)) g_cam.turnSmooth += e.wheel * kSideNotch;
      return true;
    case T::Gesture:
      if (!g_cam.smooth) return true;
      if (e.gesture == 1 && e.value > 0) g_cam.zoomDirect -= std::log(e.value);  // fingers apart: nearer
      if (e.gesture == 2) g_cam.turnDirect += e.value;
      if (e.gesture == 3) {
        g_cam.panX += e.dx;
        g_cam.panY += e.dy;
      }
      if (e.gesture == 4) ++g_cam.released;
      if (e.gesture != 4) {
        g_cam.pivotX = float(e.x);
        g_cam.pivotY = float(e.y);
      }
      return true;
    default:
      return false;
  }
}

void registerCameraInputNatives(VM& vm) {
  // regsSmoothCamera([on]): the wheel and gestures over the world go to the smooth camera (true) or the game's keys
  implementGlobal(vm, u"regsSmoothCamera", [](VM& v, Object*, int argc, const Value* a) {
    if (argc > 0) {
      g_cam.smooth = v.truthy(a[0]);
      if (!g_cam.smooth) g_cam.dragging = false;
    }
    return Value::boolean(g_cam.smooth);
  }, 0, 1);
  // regsCameraInput(): [zoom eased, zoom direct (ln of the distance factor), turn eased, turn direct (radians, map
  // counterclockwise), pan x, pan y (pixels), pivot x, pivot y, free turns ended, turning by mouse now (0/1)]; the
  // amounts start again from zero
  implementGlobal(vm, u"regsCameraInput", [](VM&, Object*, int, const Value*) {
    auto arr = make<ArrayObj>();
    arr->items = {Value::real(g_cam.zoomSmooth), Value::real(g_cam.zoomDirect), Value::real(g_cam.turnSmooth),
                  Value::real(g_cam.turnDirect), Value::real(g_cam.panX),       Value::real(g_cam.panY),
                  Value::real(g_cam.pivotX),     Value::real(g_cam.pivotY),     Value::integer(g_cam.released),
                  Value::integer(g_cam.dragging ? 1 : 0)};
    g_cam.zoomSmooth = g_cam.zoomDirect = g_cam.turnSmooth = g_cam.turnDirect = g_cam.panX = g_cam.panY = 0;
    g_cam.released = 0;
    return Value::heap(VT::Array, arr.get());
  }, 0, 0);
  // regsKeyHeld(name): the key (a controls.keys name such as "Left" or "MouseMiddle") is held down now
  implementGlobal(vm, u"regsKeyHeld", [](VM& v, Object*, int, const Value* a) {
    Value n = v.resolve(a[0]);
    if (n.type() != VT::String) return Value::boolean(false);
    for (int k = 1; k < kKeyCount; ++k)
      if (n.str() == keyName(k)) return Value::boolean(g_cam.held[k]);
    return Value::boolean(false);
  }, 1, 1);
}

}  // namespace regs::engine
