// Port: the GUI laid out again for a new window size while the game runs (graphics page: resolution, fullscreen, UI
// scale; docs/ui/scaling.md §6). The scripts build the desktop, HUD and panels once from the window size, each
// control at a formula of the size: flush with an edge (minimap at height − 300, line panel at width − 357), centred
// (dialogs at int((width − w) · 0.5), the toolbar), or spanning it (desktop, overlays, the HUD layers, the edge
// strips). Each control keeps that relation when the size changes, so game state and open panels stay. Layouts that
// follow the size in other ways are placed again by the scripts: the menus are loaded again (main.script
// extension), and HUD scripts re-place their controls on the desktop's onChangeRect (levelbar, moneybar,
// petitionbar extensions, with window.regsSetRect).
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "engine/display.h"
#include "engine/gui.h"

namespace regs::engine {

using namespace regs::script;

namespace {

// One axis of a control in a container whose client extent went from o to n: a span grows with it, a centred
// control stays centred (with the scripts' int() rounding), a control nearer the far edge than the near one moves
// with the far edge, others stay.
void anchorAxis(float o, float n, float& pos, float& size) {
  float d = n - o;
  float a = pos, b = o - (pos + size), c = pos + size * 0.5f - o * 0.5f;
  bool span = (std::fabs(a) <= 1 && std::fabs(b) <= 1) ||
              (a >= 0 && b >= 0 && a <= 64 && std::fabs(a - b) <= 1 && size >= o * 0.5f);  // margins both sides
  if (span) size = std::max(0.0f, size + d);
  else if (std::fabs(c) <= 1.0f) pos = std::floor((n - size) * 0.5f) + (pos - std::floor((o - size) * 0.5f));
  else if (b < a) pos += d;
}

int relayoutChildren(VM& vm, ControlObj* parent, float ow, float oh, float nw, float nh) {
  int moved = 0;
  // a copy: onChangeRect handlers may add or remove controls
  std::vector<Ref<Object>> kids(parent->children.begin(), parent->children.end());
  for (auto& k : kids) {
    auto* c = dynamic_cast<ControlObj*>(k.get());
    if (!c || c->destroyed) continue;
    float l = c->left, t = c->top, w = c->width, h = c->height;
    anchorAxis(ow, nw, l, w);
    anchorAxis(oh, nh, t, h);
    moved += placeControl(vm, c, l, t, w, h);
  }
  return moved;
}

}  // namespace

int placeControl(VM& vm, ControlObj* c, float l, float t, float w, float h) {
  if (l == c->left && t == c->top && w == c->width && h == c->height) return 0;
  if (std::getenv("REGS_DEBUG_RELAYOUT"))
    LOG_INFO("relayout %p (%s) in %p: %.0f,%.0f %.0fx%.0f -> %.0f,%.0f %.0fx%.0f", (void*)c, c->cls->name.c_str(),
             (void*)c->parentControl(), c->left, c->top, c->width, c->height, l, t, w, h);
  bool resized = w != c->width || h != c->height;
  // a move alone fires no onChangeRect: ui.script's createHudElement handler would put a customised $content back
  // to its default rect (the minimap's icon column)
  c->left = l;
  c->top = t;
  if (!resized) return 1;
  float ow = c->width - c->marginL - c->marginR, oh = c->height - c->marginT - c->marginB;
  c->width = w;
  c->height = h;
  // the children first, then the control's own handlers (9-slice edges, scroll bars, the desktop's listeners) see
  // the finished layout
  int moved = 1 + relayoutChildren(vm, c, ow, oh, w - c->marginL - c->marginR, h - c->marginT - c->marginB);
  vm.fireEvent(c, u"onChangeRect", {});
  return moved;
}

int relayoutGui(VM& vm, Object* video, int ow, int oh, int nw, int nh) {
  if (!video) return 0;
  int moved = 0;
  // the root viewports in pixel coordinates that fill the scripts' window: window.script's GUI viewport and
  // viewport.script's 3D map view
  std::vector<Ref<Object>> roots(video->children.begin(), video->children.end());
  for (auto& r : roots) {
    auto* vp = dynamic_cast<ControlObj*>(r.get());
    if (!vp || vp->destroyed) continue;
    if (vp->left != 0 || vp->top != 0 || std::fabs(vp->width - float(ow)) > 1 || std::fabs(vp->height - float(oh)) > 1) continue;
    moved += placeControl(vm, vp, 0, 0, float(nw), float(nh));
  }
  LOG_INFO("gui: %d controls laid out for %dx%d (was %dx%d)", moved, nw, nh, ow, oh);
  return moved;
}

}  // namespace regs::engine
