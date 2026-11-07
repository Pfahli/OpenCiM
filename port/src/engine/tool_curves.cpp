// Port: the map editor's curved-road tool (owner's request 2026-11-07; engine/curve_roads.h). It works like the road
// tool of Cities: Skylines, which the owner named as the model:
//   straight  click the start, click the end;
//   curved    click the start, click the bend point, click the end (a quadratic curve through the bend's pull);
//   freeform  click the start, click the end: the road keeps the direction it starts with and bends to the end.
// After each road the tool draws on from its end (in that end's direction) until the right button stops it; a road
// ending on an existing road stops it too. Starts and ends snap to the closed side of a small road's dead end (the
// block becomes a straight piece the curve continues), to a grass tram track's dead end (the same, and the curve gets
// tracks; the road stops flat there) and to the free ends of other curves; elsewhere an end is free
// and gets a turning circle. From a snapped end the road leaves straight along it (lanes must meet). The preview is
// green when the road fits, red when it is shorter than 6 units, bends tighter than radius 6 or runs over buildings
// (trees under it are cleared). The right button only stops drawing; the fourth mode, remove, takes away the curve
// under the cursor with a click (shown red; its blocks become dead ends again), as the bulldozer does in Cities:
// Skylines. Every change goes on the editor's timeline, so undo works.
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
#include "engine/sim_state.h"
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
  int mode = 1;  // 0 straight, 1 curved, 2 freeform, 3 remove
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
  bool endBlock = sn.ok && sn.block && st.step > 0;
  c.ends = uint8_t((st.startBlock ? 1 : 0) | (endBlock ? 2 : 0));
  // an end on a grass tram track: the road stops flat at its block and gets tracks that run on into it
  if (st.startBlock && st.startEnd.tram) c.ends |= 8 | 4;
  if (endBlock && sn.end.tram) c.ends |= 16 | 4;
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
  if (st.mode == 3) {  // remove: the curve under the cursor shown red
    st.hoverCurve = curveUnder(g, p);
    st.length = st.angle = 0;
    if (st.hoverCurve < 0) {
      clearPreview(grid);
      return;
    }
    grid->curvePreview = g.curves[size_t(st.hoverCurve)];
    grid->hasCurvePreview = true;
    grid->curvePreviewBad = true;
    ++grid->curvesVersion;
    st.previewOk = false;
    return;
  }
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

int openTramJoins(VM& vm, ControlObj* player, GridObj* grid, Object* tl);

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
  if (c.ends & 4) openTramJoins(vm, player, grid, tl);  // tracks from a grass track: street tram dead ends at its other end
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
  if (std::getenv("REGS_DEBUG_CURVES"))
    LOG_INFO("curve tool: click, step %d, mode %d, snap %d (block %d), preview %s", st.step, st.mode, st.endSnap.ok ? 1 : 0,
             st.endSnap.block ? 1 : 0, st.previewOk ? "fits" : "none or red");
  if (st.mode == 3) {
    int k = st.hoverCurve;
    clearPreview(grid);
    removeCurve(vm, player, grid, k);
    move(vm, player, grid, x, y);
    return;
  }
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
  (void)st;
  reset(vm, grid);  // only stops (removing is the Remove mode, as the bulldozer is in Cities: Skylines)
  move(vm, player, grid, x, y);
}

// ---- the game's tram track and stop tools over curves ----
// Tram track tool (GRIDMODE_LAY_TRAM_TRACK): pointing at a curve without tracks shows it with tracks; a click lays them
// on the whole curve. Its price is the street tram pieces' for the curve's length: as many invisible tram_small_basic
// previews (GridData::curveAnchors, not drawn) for the hover script to price, dropped when the tracks are laid. A drag
// of the original tool whose preview runs over a curve is red (it would cut across it).
// Stop tool (GRIDMODE_CREATE_STOP): pointing at a curve shows the chosen stop (its _small variant) on the side under the
// cursor, bent along the curve; the tool's preview is the stop's anchor item (invisible), so price, name and building
// go the game's way, and the click adds the curve's stop record with it. Tram stops need tracks on the curve; stops
// keep 7 units from the curve's ends and 14 from another stop on the same side.

struct Wrap {
  ToolMode orig;
  int mode = 0;
  int curve = -1;       // the curve pointed at, −1 for none (the original tool runs)
  bool armed = false;   // pressed on it
  bool ok = false;
  CurveRoad after;      // the curve as the click would leave it
  int stopAnchor = -1;  // the stop tool's preview anchor
  float lastS = -1;     // the stop preview's place (rebuilt only when it moves)
  int lastSide = 0;
};
Wrap g_tram, g_stop;

const char16_t kHover[] = u"onHoverConstruct";

// the tool's whole preview (the game tool's own too, when the cursor comes onto a curve from the road or the grass)
// with its highlighted cells, which toolClearPreview leaves behind (owner's report 2026-11-07: a green marker beside
// the street that stayed after laying tracks on a curve)
void clearToolPreview(VM& vm, ControlObj* player, GridObj* grid) {
  GridData& g = *grid->data;
  for (uint32_t i : toolState(player).newItems) {
    g.curveAnchors.erase(i);
    if (i >= g.items.size() || g.items[i].removed) continue;
    int x, z, w, l;
    gridItemRect(vm, grid, i, x, z, w, l);
    gridUnselectArea(vm, grid, x, z, w, l);
  }
  for (uint32_t r : toolState(player).replaced) gridSetSelected(vm, grid, r, false, false);
  toolClearPreview(vm, player, grid);
}

void wrapClear(VM& vm, ControlObj* player, GridObj* grid, Wrap& w) {
  clearToolPreview(vm, player, grid);
  if (w.stopAnchor >= 0) grid->data->curveAnchors.erase(uint32_t(w.stopAnchor));
  w.curve = -1;
  w.stopAnchor = -1;
  w.ok = false;
  clearPreview(grid);
}

// a point over the middle of a curve (not beyond its ends, where its road blocks are): a track piece there would cut
// across it
bool overCurve(const GridData& g, float x, float z) {
  for (const CurveRoad& c : g.curves) {
    float side, s;
    curveNearest(curveShape(c), x, z, side, s);
    if (std::fabs(side) < kCurveHalfWidth && s > 1.0f && s < curveLength(c) - 1.0f) return true;
  }
  return false;
}

// a preview item the hover script prices but nobody sees
int invisiblePreview(VM& vm, ControlObj* player, GridObj* grid, const std::string& name, int x, int y, int z, int orient,
                     bool bad) {
  int i = toolAddPreview(vm, player, grid, name, x, y, z, orient, 0);
  if (i < 0) return -1;
  grid->data->curveAnchors.insert(uint32_t(i));
  if (bad) grid->data->items[size_t(i)].flags |= kItemNegative;
  return i;
}

void showAfter(GridObj* grid, const CurveRoad& c, bool bad) {
  grid->curvePreview = c;
  grid->hasCurvePreview = true;
  grid->curvePreviewBad = bad;
  ++grid->curvesVersion;
}

// the stop tool's stop (its toolMode: 0 bus, 1 tram; toolType: the design, as stopPreview reads them)
bool stopToolTemplate(VM& vm, ControlObj* player, GridObj* grid, CurveRoad::Stop& st) {
  (void)grid;
  auto field = [&](const char16_t* name) {
    auto f = player->fieldValues.find(name);
    return f == player->fieldValues.end() || f->second.isNone() ? 0 : vm.toInt(f->second);
  };
  int mode = field(u"toolMode");
  if (mode != 0 && mode != 1) return false;
  st.kind = uint8_t(mode);
  st.design = uint8_t(std::clamp(field(u"toolType") + 1, 1, 3));
  return true;
}

void wrapMove(VM& vm, ControlObj* player, GridObj* grid, float x, float y, Wrap& w) {
  ToolState& t = toolState(player);
  if (!grid->data) return;
  GridData& g = *grid->data;
  if (w.armed) return;  // pressed on a curve: the click applies it
  if (!t.inProgress) {
    ToolPick pick = toolPick(vm, player, x, y, false);
    int k = pick.hit ? curveUnder(g, pick.point) : -1;
    if (std::getenv("REGS_DEBUG_CURVES") && k != w.curve)
      LOG_INFO("curve tool: mode %d hover %d -> %d (hit %d at %.1f %.1f)", w.mode, w.curve, k, pick.hit ? 1 : 0, pick.point.x, pick.point.z);
    if (k >= 0 && w.curve == k) {  // the same target: keep the preview (no new preview items per mouse move)
      if (w.mode == 0x11) return;
      float side, s;
      curveNearest(curveShape(g.curves[size_t(k)]), pick.point.x, pick.point.z, side, s);
      if ((side >= 0 ? 1 : -1) == w.lastSide && std::fabs(s - w.lastS) < 0.75f) return;
    }
    if (k >= 0) {
      wrapClear(vm, player, grid, w);
      const CurveRoad& c = g.curves[size_t(k)];
      w.curve = k;
      w.after = c;
      const float cs = g.cellSize;
      if (w.mode == 0x11) {
        w.ok = !(c.ends & 4);
        w.after.ends |= 4;
        int n = std::max(1, int(std::ceil(curveLength(c) / 12.0f)));
        int cx = int(c.x0 / cs + float(g.width) * 0.5f), cz = int(c.z0 / cs + float(g.length) * 0.5f);
        for (int i = 0; i < n && w.ok; ++i)
          invisiblePreview(vm, player, grid, "tram_small_basic", cx, int(c.y0 * 256.0f / cs), cz, 0, false);
      } else {
        CurveRoad::Stop st;
        w.ok = stopToolTemplate(vm, player, grid, st);
        std::vector<CurveSample> shape = curveShape(c);
        float side, s;
        curveNearest(shape, pick.point.x, pick.point.z, side, s);
        st.s = s;
        st.side = side >= 0 ? 1 : -1;
        w.lastS = s, w.lastSide = st.side;
        float L = curveLength(c);
        if (s < 7.0f || s > L - 7.0f) w.ok = false;
        if (st.kind == 1 && !(c.ends & 4)) w.ok = false;  // a tram stop needs tracks
        for (const CurveRoad::Stop& o : c.stops)
          if (o.side == st.side && std::fabs(o.s - s) < 14.0f) w.ok = false;
        w.after.stops.push_back(st);
        int ax, az, ao;
        stopAnchor(g, w.after, st, ax, az, ao);
        float ay = curveAt(shape, std::clamp(s, 0.0f, L)).p.y;
        w.stopAnchor = invisiblePreview(vm, player, grid, stopTemplateName(st), ax, int(ay * 256.0f / cs), az, ao, !w.ok);
        if (w.stopAnchor < 0) w.ok = false;
        if (std::getenv("REGS_DEBUG_CURVES"))
          LOG_INFO("curve tool: stop target kind %d design %d at s %.1f of %.1f side %d, anchor %d (%s), %s", int(st.kind),
                   int(st.design), s, L, int(st.side), w.stopAnchor, stopTemplateName(st).c_str(), w.ok ? "fits" : "refused");
      }
      showAfter(grid, w.after, !w.ok);
      vm.fireEvent(player, kHover, {});
      return;
    }
  }
  if (w.curve >= 0) wrapClear(vm, player, grid, w);
  if (w.orig.move) w.orig.move(vm, player, grid, x, y);
  // the original track tool's drag: pieces over a curve are errors (they would cut across it)
  if (w.mode == 0x11 && t.inProgress && !g.curves.empty()) {
    bool any = false;
    for (uint32_t i : t.newItems) {
      if (i >= g.items.size()) continue;
      int ix, iz, iw, il;
      gridItemRect(vm, grid, i, ix, iz, iw, il);
      float wx = (float(ix) + float(iw) * 0.5f - float(g.width) * 0.5f) * g.cellSize;
      float wz = (float(iz) + float(il) * 0.5f - float(g.length) * 0.5f) * g.cellSize;
      if (!overCurve(g, wx, wz)) continue;
      g.items[i].flags |= kItemNegative;
      gridSetSelected(vm, grid, i, true, true);
      any = true;
    }
    if (any) vm.fireEvent(player, kHover, {});
  }
}

// the street tram pieces where curves with tracks meet road blocks: dead ends closed toward the curve become straight
// pieces, so the block's tram lanes run on into the curve's (after tracks on a curve and after the track tool's drags)
int openTramJoins(VM& vm, ControlObj* player, GridObj* grid, Object* tl) {
  if (!grid || !grid->data) return 0;
  std::vector<CurveRoad> curves = grid->data->curves;
  int opened = 0;
  gridEditBegin(vm, grid);
  for (const CurveRoad& c : curves) {
    if (!(c.ends & 4)) continue;
    for (int end = 0; end < 2; ++end) {
      if (!(c.ends & (1 << end))) continue;
      Vec3 q = end ? Vec3{c.x1, c.y1, c.z1} : Vec3{c.x0, c.y0, c.z0};
      Vec3 out = end ? Vec3{-c.dx1, 0, -c.dz1} : Vec3{c.dx0, 0, c.dz0};  // the block's way into the curve
      for (const RoadEnd& e : findSmallRoadEnds(vm, player, grid, q.x, q.z, 1.0f))
        if (!e.deadEnd && dot(e.dir, out) > 0.9f) {
          if (!e.tram && openTramBlockEnd(vm, player, grid, e, tl) >= 0) ++opened;
          break;
        }
    }
  }
  gridEditEnd(vm, grid);
  if (opened) LOG_INFO("curve tool: %d street tram dead ends at curves made straight", opened);
  return opened;
}

bool wrapFinish(VM& vm, ControlObj* player, GridObj* grid, Wrap& w) {
  if (std::getenv("REGS_DEBUG_CURVES")) LOG_INFO("curve tool: finish mode %d, armed %d, curve %d, ok %d", w.mode, w.armed ? 1 : 0, w.curve, w.ok ? 1 : 0);
  if (!w.armed) {
    bool r = w.orig.finish ? w.orig.finish(vm, player, grid) : toolCommit(vm, player, grid);
    if (w.mode == 0x11) openTramJoins(vm, player, grid, playerTimeline(vm, player));  // a drag up to a curve with tracks
    return r;
  }
  w.armed = false;
  if (!grid || !grid->data || w.curve < 0 || size_t(w.curve) >= grid->data->curves.size() || !w.ok) {
    if (grid) wrapClear(vm, player, grid, w);
    return true;
  }
  CurveRoad before = grid->data->curves[size_t(w.curve)], after = w.after;
  Object* tl = playerTimeline(vm, player);
  gridEditBegin(vm, grid);
  if (tl) timelineBeginGroup(vm, tl, w.mode == 0x11 ? "%6" : "%8");
  bool go = true;
  if (w.mode == 0x11) {
    for (uint32_t i : toolState(player).newItems) grid->data->curveAnchors.erase(i);
    clearToolPreview(vm, player, grid);  // the price carriers go
  } else {
    int mounted = 0;
    toolCommit(vm, player, grid, [&](uint32_t, uint32_t) { ++mounted; });  // the anchor stop item, named by the script
    go = mounted > 0;
  }
  if (go) {
    applyCurveEdit(vm, grid, before, false);
    timelineRecordCurve(vm, tl, before, false);
    applyCurveEdit(vm, grid, after, true);
    timelineRecordCurve(vm, tl, after, true);
    if (w.mode == 0x11) openTramJoins(vm, player, grid, tl);  // the blocks' tram dead ends at its ends
  }
  if (tl) timelineEndGroup(vm, tl);
  gridEditEnd(vm, grid);
  LOG_INFO("curve tool: %s on a curve%s", w.mode == 0x11 ? "tram tracks" : "a stop",
           go ? "" : " refused (its stop item did not fit)");
  w.curve = -1;
  w.stopAnchor = -1;
  clearPreview(grid);
  return true;
}

void wrapTool(int mode, Wrap& w) {
  w.orig = toolModeOf(mode);
  w.mode = mode;
  ToolMode m = w.orig;
  m.move = [&w](VM& v, ControlObj* p, GridObj* g, float x, float y) { wrapMove(v, p, g, x, y, w); };
  m.down = [&w](VM& v, ControlObj* p, GridObj* g) {
    // the press point itself (a click without a mouse move over the curve since the tool was chosen)
    if (w.curve < 0 && g) wrapMove(v, p, g, toolState(p).toolX, toolState(p).toolY, w);
    if (std::getenv("REGS_DEBUG_CURVES") && g) {
      ToolPick pk = toolPick(v, p, toolState(p).toolX, toolState(p).toolY, false);
      LOG_INFO("curve tool: press mode %d on curve %d at %.0f,%.0f: hit %d at %.1f %.1f, under %d, inProgress %d", w.mode, w.curve,
               toolState(p).toolX, toolState(p).toolY, pk.hit ? 1 : 0, pk.point.x, pk.point.z, pk.hit ? curveUnder(*g->data, pk.point) : -2,
               toolState(p).inProgress ? 1 : 0);
    }
    if (w.curve >= 0) w.armed = true;
    else if (w.orig.down) w.orig.down(v, p, g);
  };
  m.finish = [&w](VM& v, ControlObj* p, GridObj* g) { return wrapFinish(v, p, g, w); };
  m.cancel = [&w](VM& v, ControlObj* p, GridObj* g) {
    if (std::getenv("REGS_DEBUG_CURVES")) LOG_INFO("curve tool: cancel mode %d (armed %d)", w.mode, w.armed ? 1 : 0);
    w.armed = false;
    if (g) wrapClear(v, p, g, w);
    if (w.orig.cancel) w.orig.cancel(v, p, g);
    else if (g) toolClearPreview(v, p, g);
  };
  registerToolMode(mode, m);
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
  wrapTool(0x11, g_tram);  // the game's tram track tool and stop tool over curves (registered before this)
  wrapTool(10, g_stop);
  vm.setGlobal(u"REGS_GRIDMODE_CURVE_ROAD", Value::integer(kModeCurveRoad));
  // regsCurveToolMode([mode]): the mode (0 straight, 1 curved, 2 freeform, 3 remove); setting it drops the road being drawn
  implementGlobal(vm, u"regsCurveToolMode", [](VM& v, Object*, int argc, const Value* a) {
    if (argc > 0) {
      g_state.mode = std::clamp(v.toInt(a[0]), 0, 3);
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
