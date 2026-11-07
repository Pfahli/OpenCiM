// Port: the map editor's curved-road tool (owner's request 2026-11-07; engine/curve_roads.h). It works like the road
// tool of Cities: Skylines, which the owner named as the model:
//   straight  click the start, click the end;
//   curved    click the start, click the bend point, click the end (a quadratic curve through the bend's pull);
//   freeform  click the start, click the end: the road keeps the direction it starts with and bends to the end.
// After each road the tool draws on from its end (in that end's direction) until the right button stops it; a road
// ending on an existing road stops it too. Starts and ends snap to the closed side of a small road's dead end (the
// block becomes a straight piece the curve continues) and to the free ends of other curves; elsewhere an end is free
// and gets a turning circle. From a snapped end the road leaves straight along it (lanes must meet). The preview is
// green when the road fits, red when it is shorter than 6 units, bends tighter than radius 6 or runs over buildings
// (trees under it are cleared). The right button cancels the road being drawn; without one it removes the curve under
// the cursor (its blocks become dead ends again). Every change goes on the editor's timeline, so undo works.
// Script side: REGS_GRIDMODE_CURVE_ROAD, regsCurveToolMode(mode) / regsCurveToolMode() and regsCurveToolInfo() for the
// panel (data/regs/extend/metro/scripts/editor.script).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>

#include "core/log.h"
#include "engine/api.h"
#include "engine/curve_roads.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/gui.h"
#include "engine/timeline.h"
#include "engine/tool_tracks.h"
#include "engine/tools.h"

namespace regs::engine {

using namespace regs::script;
using render::Vec3;

namespace {

constexpr int kModeCurveRoad = 0x42;
constexpr float kPi = 3.14159265f;

struct Snap {
  bool ok = false;
  Vec3 p;            // the point (y = the road's item height)
  Vec3 out;          // the way out of the existing road into the new one
  bool block = false;
  RoadEnd end;       // block: its edge
};

struct State {
  int mode = 1;  // 0 straight, 1 curved, 2 freeform
  int step = 0;  // 0 nothing picked, 1 the start, 2 (curved) the bend point too
  Vec3 start, bend;
  bool startDir = false;  // the start's direction is fixed (a snapped end, or drawing on)
  Vec3 dir0;
  bool startBlock = false;
  RoadEnd startEnd;
  // the last preview
  bool previewOk = false;
  CurveRoad preview;
  Snap endSnap;
  int hoverCurve = -1;
  float length = 0, angle = 0;
  std::set<uint32_t> selected;
};
State g_state;

float len2(const Vec3& v) { return v.x * v.x + v.z * v.z; }
Vec3 flat(const Vec3& v) { return Vec3{v.x, 0, v.z}; }
Vec3 unit(const Vec3& v) { return render::normalize(flat(v)); }

void unselectAll(VM& vm, GridObj* grid) {
  for (uint32_t i : g_state.selected) gridSetSelected(vm, grid, i, false, false);
  g_state.selected.clear();
}
void select(VM& vm, GridObj* grid, uint32_t item, bool bad) {
  gridSetSelected(vm, grid, item, true, bad);
  g_state.selected.insert(item);
}

void clearPreview(GridObj* grid) {
  if (grid->hasCurvePreview) {
    grid->hasCurvePreview = false;
    ++grid->curvesVersion;
  }
  g_state.previewOk = false;
}

void reset(VM& vm, GridObj* grid) {
  g_state.step = 0;
  g_state.startDir = g_state.startBlock = false;
  if (grid) {
    clearPreview(grid);
    unselectAll(vm, grid);
  }
}

// the snap target near p: a dead end's closed side (8 units) or a curve's free end (6 units), the nearer one
Snap findSnap(VM& vm, ControlObj* player, GridObj* grid, const Vec3& p) {
  Snap best;
  float bestD = 1e30f;
  for (const RoadEnd& e : findSmallRoadEnds(vm, player, grid, p.x, p.z, 8.0f)) {
    if (!e.deadEnd) continue;
    float d = len2(e.point - p);
    if (d < bestD) bestD = d, best = Snap{true, e.point, e.dir, true, e};
  }
  const GridData& g = *grid->data;
  for (const CurveRoad& c : g.curves) {
    CurveEnds free = curveFreeEnds(g, c);
    if (free.free0) {
      Vec3 q{c.x0, c.y0, c.z0};
      float d = len2(q - p);
      if (d <= 36.0f && d < bestD) bestD = d, best = Snap{true, q, Vec3{-c.dx0, 0, -c.dz0}, false, {}};
    }
    if (free.free1) {
      Vec3 q{c.x1, c.y1, c.z1};
      float d = len2(q - p);
      if (d <= 36.0f && d < bestD) bestD = d, best = Snap{true, q, Vec3{c.dx1, 0, c.dz1}, false, {}};
    }
  }
  return best;
}

// the curve under p (within the road's half width), ???????1 for none
int curveUnder(const GridData& g, const Vec3& p) {
  int best = -1;
  float bestD = kCurveHalfWidth + 1.5f;  // the kerb edge too
  for (size_t i = 0; i < g.curves.size(); ++i) {
    float side, s;
    curveNearest(curveShape(g.curves[i], curveFreeEnds(g, g.curves[i])), p.x, p.z, side, s);
    if (std::fabs(side) < bestD) bestD = std::fabs(side), best = int(i);
  }
  return best;
}

// items a road would run over: everything covering but its own blocks, trees and markers (trees are cleared)
bool overItems(VM& vm, GridObj* grid, const CurveRoad& c, std::vector<uint32_t>& trees, std::vector<uint32_t>& blocking) {
  const GridData& g = *grid->data;
  int x0, z0, x1, z1;
  curveCells(g, c, x0, z0, x1, z1);
  std::vector<CurveSample> shape = curveShape(c, curveFreeEnds(g, c));
  const float cs = g.cellSize;
  for (uint32_t i : gridItemsNear(vm, grid, x0, z0, x1, z1)) {
    if (i >= g.items.size() || g.items[i].removed) continue;
    GridItemShape sh = gridItemShape(vm, grid, i);
    if (sh.flags & kItemTemp) continue;
    if (sh.type == 0x15) continue;  // markers
    if ((g_state.startBlock && i == g_state.startEnd.item) || (g_state.endSnap.block && i == g_state.endSnap.end.item)) continue;
    int ix, iz, iw, il;
    gridItemRect(vm, grid, i, ix, iz, iw, il);
    if (ix > x1 || iz > z1 || ix + iw - 1 < x0 || iz + il - 1 < z0) continue;
    bool hit = false;
    for (int z = iz; z < iz + il && !hit; ++z)
      for (int x = ix; x < ix + iw && !hit; ++x) {
        float wx = (float(x) + 0.5f - float(g.width) * 0.5f) * cs, wz = (float(z) + 0.5f - float(g.length) * 0.5f) * cs;
        float side, s;
        curveNearest(shape, wx, wz, side, s);
        hit = std::fabs(side) < kCurveHalfWidth - 0.5f;
      }
    if (!hit) continue;
    if (sh.type == 0x13) trees.push_back(i);
    else blocking.push_back(i);
  }
  return !blocking.empty();
}

// the road from the state's start to h (snapped by sn): false when there is none to show
bool buildCurve(const GridData& g, const Vec3& h, const Snap& sn, CurveRoad& c) {
  (void)g;
  State& st = g_state;
  Vec3 S = st.start, E = h;
  if (len2(E - S) < 1.0f) return false;
  Vec3 d0, d1;
  float a = 0, b = 0;
  if (st.mode == 0 || (st.mode == 2 && !st.startDir)) {  // straight (and freeform's first road)
    d0 = st.startDir ? st.dir0 : unit(E - S);
    if (st.startDir && !sn.ok) {  // from a fixed end only straight on
      float t = std::max(1.0f, dot(flat(E - S), d0));
      E = S + d0 * t;
      E.y = h.y;
    }
    float L = std::sqrt(len2(E - S));
    d1 = sn.ok ? sn.out * -1.0f : d0;
    a = b = L / 3;
  } else if (st.mode == 1) {
    Vec3 C = st.step >= 2 ? st.bend : h;
    if (st.step < 2) {  // the bend point being chosen: a straight guide to it
      d0 = st.startDir ? st.dir0 : unit(C - S);
      E = st.startDir ? S + d0 * std::max(1.0f, dot(flat(C - S), d0)) : C;
      E.y = h.y;
      float L = std::sqrt(len2(E - S));
      d1 = d0;
      a = b = L / 3;
    } else {
      d0 = st.startDir ? st.dir0 : unit(C - S);
      d1 = sn.ok ? sn.out * -1.0f : unit(E - C);
      a = std::sqrt(len2(C - S)) * 2.0f / 3.0f;
      b = std::sqrt(len2(E - C)) * 2.0f / 3.0f;
    }
  } else {  // freeform from a fixed direction: the bend on the start's line, as far from the start as from the end
    d0 = st.dir0;
    Vec3 v = flat(E - S);
    float along = dot(v, d0);
    if (along < 0.5f) return false;
    float k = len2(v) / (2.0f * along);
    Vec3 C = S + d0 * k;
    d1 = sn.ok ? sn.out * -1.0f : unit(E - C);
    a = k * 2.0f / 3.0f;
    b = std::sqrt(len2(E - C)) * 2.0f / 3.0f;
  }
  // an end on an existing road fixes the arrival too: with both directions fixed the smoothest join (a curve or an
  // S) has handles of about half the distance; the bend point only shapes roads with a free end
  if (sn.ok && !(st.mode == 1 && st.step < 2)) a = b = 0.45f * std::sqrt(len2(E - S));
  c = CurveRoad();
  c.x0 = S.x, c.y0 = S.y, c.z0 = S.z, c.dx0 = d0.x, c.dz0 = d0.z;
  c.x1 = E.x, c.y1 = E.y, c.z1 = E.z, c.dx1 = d1.x, c.dz1 = d1.z;
  c.a = std::max(a, 0.5f), c.b = std::max(b, 0.5f);
  c.ends = uint8_t((st.startBlock ? 1 : 0) | (sn.ok && sn.block && st.step > 0 ? 2 : 0));
  return true;
}

void move(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  if (!grid->data) return;
  State& st = g_state;
  ToolPick pick = toolPick(vm, player, x, y, false);
  unselectAll(vm, grid);
  if (!pick.hit) {
    clearPreview(grid);
    return;
  }
  const GridData& g = *grid->data;
  Vec3 p = pick.point;
  Snap sn = findSnap(vm, player, grid, p);
  Vec3 h = sn.ok ? sn.p : Vec3{p.x, p.y + 0.2f, p.z};  // a free end: the pavement 0.2 over the ground
  st.endSnap = sn;
  st.hoverCurve = -1;
  if (st.step == 0) {
    clearPreview(grid);
    if (sn.ok && sn.block) select(vm, grid, sn.end.item, false);
    if (!sn.ok) st.hoverCurve = curveUnder(g, p);
    st.length = st.angle = 0;
    return;
  }
  // the curve tool's bend step picks a free point (no snapping)
  if (st.mode == 1 && st.step == 1) sn = Snap(), h = Vec3{p.x, p.y + 0.2f, p.z}, st.endSnap = sn;
  CurveRoad c;
  if (!buildCurve(g, h, sn, c)) {
    clearPreview(grid);
    return;
  }
  bool bad = curveLength(c) < 6.0f || curveMinRadius(c) < 6.0f;
  std::vector<uint32_t> trees, blocking;
  if (!bad && overItems(vm, grid, c, trees, blocking)) bad = true;
  for (uint32_t i : blocking) select(vm, grid, i, true);
  if (std::getenv("REGS_DEBUG_CURVES"))
    LOG_INFO("curve tool: preview length %.1f, min radius %.1f, %zu items in the way%s", curveLength(c), curveMinRadius(c),
             blocking.size(), blocking.empty() ? "" : (" (first type " + std::to_string(gridItemShape(vm, grid, blocking[0]).type) + ")").c_str());
  if (st.startBlock) select(vm, grid, st.startEnd.item, bad);
  if (sn.ok && sn.block && !(st.mode == 1 && st.step == 1)) select(vm, grid, sn.end.item, bad);
  grid->curvePreview = c;
  grid->hasCurvePreview = true;
  grid->curvePreviewBad = bad;
  ++grid->curvesVersion;
  st.preview = c;
  st.previewOk = !bad && !(st.mode == 1 && st.step == 1);
  st.length = curveLength(c);
  float cosA = std::clamp(c.dx0 * c.dx1 + c.dz0 * c.dz1, -1.0f, 1.0f);
  st.angle = std::acos(cosA) * 180.0f / kPi;
}

void commit(VM& vm, ControlObj* player, GridObj* grid) {
  State& st = g_state;
  CurveRoad c = st.preview;
  Object* tl = playerTimeline(vm, player);
  std::vector<uint32_t> trees, blocking;
  overItems(vm, grid, c, trees, blocking);
  unselectAll(vm, grid);
  clearPreview(grid);
  gridEditBegin(vm, grid);
  if (tl) timelineBeginGroup(vm, tl, "%8");
  if (c.ends & 1) replaceRoadBlockEnd(vm, player, grid, st.startEnd, true, tl);
  if (c.ends & 2) replaceRoadBlockEnd(vm, player, grid, st.endSnap.end, true, tl);
  for (uint32_t t : trees) {
    timelineRecordDelete(vm, tl, grid, t);
    gridEditRemoveItem(vm, grid, t);
  }
  applyCurveEdit(vm, grid, c, true);
  timelineRecordCurve(vm, tl, c, true);
  if (tl) timelineEndGroup(vm, tl);
  gridEditEnd(vm, grid);
  LOG_INFO("curve tool: road %.1f long, %zu trees cleared, %zu curves now", curveLength(c), trees.size(),
           grid->data->curves.size());
  // draw on from its end, unless it ended on a road
  if (st.endSnap.ok) {
    reset(vm, grid);
    return;
  }
  st.start = Vec3{c.x1, c.y1, c.z1};
  st.dir0 = Vec3{c.dx1, 0, c.dz1};
  st.startDir = true;
  st.startBlock = false;
  st.step = 1;
}

void removeCurve(VM& vm, ControlObj* player, GridObj* grid, int index) {
  GridData& g = *grid->data;
  if (index < 0 || size_t(index) >= g.curves.size()) return;
  CurveRoad c = g.curves[size_t(index)];
  Object* tl = playerTimeline(vm, player);
  gridEditBegin(vm, grid);
  if (tl) timelineBeginGroup(vm, tl, "%!12");
  // its blocks become dead ends again (the straight piece whose edge it started or ended at)
  for (int end = 0; end < 2; ++end) {
    if (!(c.ends & (1 << end))) continue;
    Vec3 q = end ? Vec3{c.x1, c.y1, c.z1} : Vec3{c.x0, c.y0, c.z0};
    Vec3 out = end ? Vec3{-c.dx1, 0, -c.dz1} : Vec3{c.dx0, 0, c.dz0};  // the block's way out into the curve
    for (const RoadEnd& e : findSmallRoadEnds(vm, player, grid, q.x, q.z, 1.0f))
      if (!e.deadEnd && dot(e.dir, out) > 0.9f) {
        replaceRoadBlockEnd(vm, player, grid, e, false, tl);
        break;
      }
  }
  applyCurveEdit(vm, grid, c, false);
  timelineRecordCurve(vm, tl, c, false);
  if (tl) timelineEndGroup(vm, tl);
  gridEditEnd(vm, grid);
  LOG_INFO("curve tool: road removed, %zu curves now", g.curves.size());
}

void up(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  if (!grid->data) return;
  State& st = g_state;
  move(vm, player, grid, x, y);  // the preview at the release point
  LOG_INFO("curve tool: click, step %d, mode %d, snap %d (block %d), preview %s", st.step, st.mode, st.endSnap.ok ? 1 : 0,
           st.endSnap.block ? 1 : 0, st.previewOk ? "fits" : "none or red");
  if (st.step == 0) {
    ToolPick pick = toolPick(vm, player, x, y, false);
    if (!pick.hit) return;
    const Snap& sn = st.endSnap;
    st.start = sn.ok ? sn.p : Vec3{pick.point.x, pick.point.y + 0.2f, pick.point.z};
    st.startDir = sn.ok;
    st.dir0 = sn.ok ? sn.out : Vec3{1, 0, 0};
    st.startBlock = sn.ok && sn.block;
    st.startEnd = sn.end;
    st.step = 1;
    move(vm, player, grid, x, y);
    return;
  }
  if (st.mode == 1 && st.step == 1) {
    ToolPick pick = toolPick(vm, player, x, y, false);
    if (!pick.hit) return;
    Vec3 b{pick.point.x, pick.point.y + 0.2f, pick.point.z};
    if (st.startDir) b = st.start + st.dir0 * std::max(1.0f, dot(flat(b - st.start), st.dir0));
    st.bend = b;
    st.step = 2;
    move(vm, player, grid, x, y);
    return;
  }
  if (st.previewOk) commit(vm, player, grid);
  move(vm, player, grid, x, y);
}

void right(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  if (!grid->data) return;
  State& st = g_state;
  if (st.step > 0) {
    reset(vm, grid);
  } else {
    ToolPick pick = toolPick(vm, player, x, y, false);
    if (std::getenv("REGS_DEBUG_CURVES"))
      LOG_INFO("curve tool: right click, hit %d at %.1f %.1f %.1f, curve under it %d", pick.hit ? 1 : 0, pick.point.x,
               pick.point.y, pick.point.z, pick.hit ? curveUnder(*grid->data, pick.point) : -2);
    if (std::getenv("REGS_DEBUG_CURVES"))
      for (const CurveRoad& k : grid->data->curves) {
        float side, s;
        curveNearest(curveShape(k, curveFreeEnds(*grid->data, k)), pick.point.x, pick.point.z, side, s);
        LOG_INFO("curve tool:   curve %.1f,%.1f -> %.1f,%.1f: distance %.1f at %.1f", k.x0, k.z0, k.x1, k.z1, side, s);
      }
    if (pick.hit) removeCurve(vm, player, grid, curveUnder(*grid->data, pick.point));
  }
  move(vm, player, grid, x, y);
}

}  // namespace

void registerCurveTool(VM& vm) {
  ToolMode m;
  m.move = move;
  m.up = up;
  m.right = right;
  // resetTool (the editor's autosave after each change, saving, other tools) only drops the preview: the road being
  // drawn goes on; choosing the tool or a mode (regsCurveToolMode) starts afresh
  m.cancel = [](VM& v, ControlObj*, GridObj* grid) {
    if (!grid) return;
    clearPreview(grid);
    unselectAll(v, grid);
  };
  m.finish = [](VM&, ControlObj*, GridObj*) { return true; };
  registerToolMode(kModeCurveRoad, m);
  vm.setGlobal(u"REGS_GRIDMODE_CURVE_ROAD", Value::integer(kModeCurveRoad));
  // regsCurveToolMode([mode]): the mode (0 straight, 1 curved, 2 freeform); setting it drops the road being drawn
  implementGlobal(vm, u"regsCurveToolMode", [](VM& v, Object*, int argc, const Value* a) {
    if (argc > 0) {
      g_state.mode = std::clamp(v.toInt(a[0]), 0, 2);
      g_state.step = 0;
      g_state.startDir = g_state.startBlock = false;
    }
    return Value::integer(g_state.mode);
  }, 0, 1);
  // regsCurveToolInfo(): [step, length, angle in degrees, fits (0/1), a curve under the cursor (0/1)] for the panel
  implementGlobal(vm, u"regsCurveToolInfo", [](VM&, Object*, int, const Value*) {
    auto arr = make<ArrayObj>();
    arr->items = {Value::integer(g_state.step), Value::real(g_state.length), Value::real(g_state.angle),
                  Value::integer(g_state.previewOk ? 1 : 0), Value::integer(g_state.hoverCurve >= 0 ? 1 : 0)};
    return Value::heap(VT::Array, arr.get());
  }, 0, 0);
}

}  // namespace regs::engine
