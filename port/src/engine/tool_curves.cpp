// Port: the map editor's curved-road tool (owner's request 2026-11-07; engine/curve_roads.h). It works like the road
// tool of Cities: Skylines, which the owner named as the model:
//   straight  click the start, click the end;
//   curved    click the start, click the bend point, click the end (a quadratic curve through the bend's pull);
//   freeform  click the start, click the end: the road keeps the direction it starts with and bends to the end.
// After each road the tool draws on from its end (in that end's direction) until the right button stops it; a road
// ending on an existing road stops it too. Starts and ends snap to the closed side of a small road's dead end (the
// block becomes a straight piece the curve continues), to a grass tram track's dead end (the same, and the curve gets
// tracks, its turning circle lies over the track) and to the free ends of other curves; elsewhere an end is free
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
#include "core/text.h"
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
  // a junction (owner's request 2026-11-07: crossings at any angle): the point is fixed, the new road's direction
  // there is free. 1 where curve ends meet already, 2 the middle of a curve (cut there), 3 the middle of a straight
  // road block (it and its straight neighbours along the road become two straight curves meeting there)
  int junction = 0;
  std::vector<Vec3> arms;         // the roads already there: their directions away from the point
  std::vector<uint32_t> blocks;   // 3: the blocks that go
  Vec3 far[2];                    // 3: the outer ends of the two straight curves (block edges)
  // 3: the straight blocks along the road on each side (outward, with their far edges), of which fitJunction takes
  // as many as the angle of the new road needs; the block under the point is blocks[0]
  std::vector<uint32_t> chain[2];
  std::vector<Vec3> chainFar[2];
  Vec3 axis, centreEdge[2];
  float guard = 0;                // along the new road from the point: what the junction covers (not in the way)
  float radius = 0;               // where the junction will cut its arms (fitJunction)
  float splitS = 0, splitL = 0;   // 2: where on the curve, and its length
  float otherHalfWidth = 0;       // the widest road already there (road kinds mix at junctions)
  float armRoom = 1e9f;           // 1, 2: the shortest of the roads there, less what junctions at their other ends take
  bool fits = true;
};

struct State {
  int mode = 1;  // 0 straight, 1 curved, 2 freeform, 3 remove
  int kind = 0;  // the road kind drawn (CurveProfile): 0 small, 1 large, 2 avenue, 3 alley
  int oneWay = 0;  // small roads: 0 both ways, 1 one-way with one lane, 2 one-way with two lanes (in the drawing direction)
  int bridge = 0;  // new curves are bridges (1) or tunnels (2) (regsCurveToolBridge; REGS_TEST_CURVE_BRIDGE=1 / 2 at start)
  int step = 0;  // 0 nothing picked, 1 the start, 2 (curved) the bend point too
  Vec3 start, bend;
  bool startDir = false;  // the start's direction is fixed (a snapped end, or drawing on)
  Vec3 dir0;
  bool startBlock = false;
  RoadEnd startEnd;
  Snap startSnap;  // a start at a junction point (startSnap.junction)
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

// The preview's cells marked as the game's tools mark theirs (green: it fits, red: it does not or it would be removed):
// the road materials take no tint, so the preview mesh alone does not show its state.
VM* g_litVm = nullptr;
GridObj* g_litGrid = nullptr;
int g_lit[4] = {0, 0, -1, -1};  // the marked cells' box (x0, z0, x1, z1), empty when x1 < x0

void unlitPreview() {
  if (!g_litVm || !g_litGrid || g_lit[2] < g_lit[0]) return;
  gridUnselectArea(*g_litVm, g_litGrid, g_lit[0], g_lit[1], g_lit[2] - g_lit[0] + 1, g_lit[3] - g_lit[1] + 1);
  g_lit[2] = g_lit[0] - 1;
}

void litPreview(VM& vm, GridObj* grid, const std::vector<CurveRoad>& curves, bool red) {
  unlitPreview();
  g_litVm = &vm, g_litGrid = grid;
  const GridData& g = *grid->data;
  const float cs = g.cellSize;
  int bx0 = 1 << 30, bz0 = 1 << 30, bx1 = -1, bz1 = -1;
  for (const CurveRoad& c : curves) {
    int x0, z0, x1, z1;
    curveCells(g, c, x0, z0, x1, z1);
    std::vector<CurveSample> shape = curveShape(c, curveFreeEnds(g, c));
    const float hw = curveProfile(c.kind).halfWidth;
    for (int z = z0; z <= z1; ++z)
      for (int x = x0; x <= x1; ++x) {
        float side, s;
        curveNearest(shape, (float(x) + 0.5f - float(g.width) * 0.5f) * cs, (float(z) + 0.5f - float(g.length) * 0.5f) * cs, side, s);
        if (std::fabs(side) > hw) continue;
        if ((s <= 0.01f && (c.ends & 1)) || (s >= shape.back().s - 0.01f && (c.ends & 2))) continue;  // the road blocks there
        gridSelectArea(vm, grid, x, z, 1, 1, red);
        bx0 = std::min(bx0, x), bz0 = std::min(bz0, z), bx1 = std::max(bx1, x), bz1 = std::max(bz1, z);
      }
  }
  g_lit[0] = bx0, g_lit[1] = bz0, g_lit[2] = bx1, g_lit[3] = bz1;
}

void clearPreview(GridObj* grid) {
  unlitPreview();
  grid->curvePreviewMore.clear();
  if (grid->hasCurvePreview) {
    grid->hasCurvePreview = false;
    ++grid->curvesVersion;
  }
  g_state.previewOk = false;
}

void reset(VM& vm, GridObj* grid) {
  g_state.step = 0;
  g_state.startDir = g_state.startBlock = false;
  g_state.startSnap = Snap();
  if (grid) {
    clearPreview(grid);
    unselectAll(vm, grid);
  }
}

// a free point's height over the ground: the pavement 0.2 over it, elevated tracks two cells (as the grid's elevated
// track tool)
float freeLift() { return curveIsElevated(g_state.kind) || g_state.bridge == 1 ? 6.0f : 0.2f; }

// the snap target near p: a dead end's closed side (8 units) or a curve's free end (6 units), the nearer one
Snap findSnap(VM& vm, ControlObj* player, GridObj* grid, const Vec3& p) {
  Snap best;
  float bestD = 1e30f;
  const CurveProfile& pf = curveProfile(g_state.kind);
  for (const RoadEnd& e : findSmallRoadEnds(vm, player, grid, p.x, p.z, pf.piece * 0.5f + 2.0f, pf.itemType)) {
    if (!e.deadEnd) continue;
    float d = len2(e.point - p);
    if (d < bestD) bestD = d, best = Snap{true, e.point, e.dir, true, e};
  }
  const GridData& g = *grid->data;
  const float reach2 = std::max(36.0f, pf.halfWidth * pf.halfWidth);
  for (const CurveRoad& c : g.curves) {
    if (c.kind != g_state.kind) continue;  // only the same kind of road joins
    CurveEnds free = curveFreeEnds(g, c);
    free.free0 = free.free0 && !(c.ends & 8);  // an end on a grass tram track is taken
    free.free1 = free.free1 && !(c.ends & 16);
    if (free.free0) {
      Vec3 q{c.x0, c.y0, c.z0};
      float d = len2(q - p);
      if (d <= reach2 && d < bestD) bestD = d, best = Snap{true, q, Vec3{-c.dx0, 0, -c.dz0}, false, {}};
    }
    if (free.free1) {
      Vec3 q{c.x1, c.y1, c.z1};
      float d = len2(q - p);
      if (d <= reach2 && d < bestD) bestD = d, best = Snap{true, q, Vec3{c.dx1, 0, c.dz1}, false, {}};
    }
  }
  // the map's edge (roads, express roads, the railway): the end on the border, leaving the map straight out
  // (curveEndOffMap: no dead end, the traffic's way in and out)
  if (!best.ok && curveKindLeavesMap(g_state.kind)) {
    const float hx = float(g.width) * g.cellSize * 0.5f, hz = float(g.length) * g.cellSize * 0.5f;
    const float bx = hx - std::fabs(p.x), bz = hz - std::fabs(p.z), reach = pf.halfWidth + 6.0f;
    if (std::min(bx, bz) <= reach && bx >= -reach && bz >= -reach) {
      Vec3 q = p, out;
      if (bx <= bz) {
        q.x = p.x < 0 ? -hx : hx;
        q.z = std::clamp(p.z, -hz + pf.halfWidth, hz - pf.halfWidth);
        out = Vec3{p.x < 0 ? 1.0f : -1.0f, 0, 0};
      } else {
        q.z = p.z < 0 ? -hz : hz;
        q.x = std::clamp(p.x, -hx + pf.halfWidth, hx - pf.halfWidth);
        out = Vec3{0, 0, p.z < 0 ? 1.0f : -1.0f};
      }
      q.y = p.y + freeLift();
      return Snap{true, q, out, false, {}};
    }
  }
  if (best.ok || curveNoJunctions(g_state.kind)) return best;  // tracks and express roads: no junctions
  // junction points, in this order: where curve ends meet, the middle of a curve, the middle of a straight block
  const float hw = pf.halfWidth;
  for (const CurveRoad& c : g.curves) {
    if ((c.kind == 3) != (g_state.kind == 3)) continue;  // road kinds mix at junctions (footpaths among themselves)
    for (int end = 0; end < 2; ++end) {
      Vec3 q = end ? Vec3{c.x1, c.y1, c.z1} : Vec3{c.x0, c.y0, c.z0};
      float d = len2(q - p);
      if (d > hw * hw || d >= bestD) continue;
      Snap s;
      for (const CurveRoad& k : g.curves) {
        if ((k.kind == 3) != (c.kind == 3)) continue;
        if ((std::fabs(k.x0 - q.x) < 0.2f && std::fabs(k.z0 - q.z) < 0.2f) || (std::fabs(k.x1 - q.x) < 0.2f && std::fabs(k.z1 - q.z) < 0.2f))
          s.otherHalfWidth = std::max(s.otherHalfWidth, curveProfile(k.kind).halfWidth);
        if (std::fabs(k.x0 - q.x) < 0.2f && std::fabs(k.z0 - q.z) < 0.2f) s.arms.push_back(unit(Vec3{k.dx0, 0, k.dz0}));
        if (std::fabs(k.x1 - q.x) < 0.2f && std::fabs(k.z1 - q.z) < 0.2f) s.arms.push_back(unit(Vec3{-k.dx1, 0, -k.dz1}));
      }
      if (s.arms.size() < 2) continue;  // a free end (the snap above) or a single one
      {
        std::vector<CurveJunction> js = findCurveJunctions(g);
        for (size_t ki = 0; ki < g.curves.size(); ++ki) {
          const CurveRoad& k = g.curves[ki];
          bool at0 = std::fabs(k.x0 - q.x) < 0.2f && std::fabs(k.z0 - q.z) < 0.2f, at1 = std::fabs(k.x1 - q.x) < 0.2f && std::fabs(k.z1 - q.z) < 0.2f;
          if (!at0 && !at1) continue;
          float t0, t1;
          curveTrims(js, int(ki), t0, t1);
          s.armRoom = std::min(s.armRoom, curveLength(k) - (at0 ? t1 : t0));
        }
      }
      s.ok = true, s.p = q, s.junction = 1;
      bestD = d, best = s;
    }
  }
  if (best.ok) return best;
  for (const CurveRoad& c : g.curves) {
    if ((c.kind == 3) != (g_state.kind == 3)) continue;
    std::vector<CurveSample> shape = curveShape(c);
    float side, s;
    curveNearest(shape, p.x, p.z, side, s);
    float L = shape.back().s;
    if (std::fabs(side) >= hw || s < hw + 2.0f || s > L - hw - 2.0f || side * side >= bestD) continue;
    CurveSample k = curveAt(shape, s);
    Snap sn;
    sn.ok = true, sn.p = k.p, sn.junction = 2;
    sn.splitS = s, sn.splitL = L;
    sn.otherHalfWidth = curveProfile(c.kind).halfWidth;
    {
      float t0, t1;
      curveTrims(findCurveJunctions(g), int(&c - g.curves.data()), t0, t1);
      sn.armRoom = std::min(s - t0, L - s - t1);
    }
    sn.arms = {unit(k.dir), unit(k.dir) * -1.0f};
    bestD = side * side, best = sn;
  }
  if (best.ok) return best;
  const float half = pf.piece * 0.5f;
  for (const RoadEnd& e : findSmallRoadEnds(vm, player, grid, p.x, p.z, pf.piece + 2.0f, pf.itemType)) {
    if (e.deadEnd || e.tram) continue;
    Vec3 C = e.point - e.dir * half, v = flat(p - C), perp{-e.dir.z, 0, e.dir.x};
    if (std::fabs(dot(v, e.dir)) > half || std::fabs(dot(v, perp)) > hw) continue;
    auto hasChildren = [&](uint32_t item) {
      for (const GridData::Item& it : g.items)
        if (!it.removed && it.parent == item + 1) return true;
      return false;
    };
    if (hasChildren(e.item)) continue;  // tram tracks or a stop on it
    Snap sn;
    sn.ok = true, sn.p = C, sn.junction = 3;
    sn.arms = {e.dir, e.dir * -1.0f};
    sn.blocks = {e.item};
    sn.axis = e.dir;
    for (int side = 0; side < 2; ++side) {  // the straight neighbours along the road (up to 4)
      Vec3 ax = side ? e.dir * -1.0f : e.dir, edge = C + ax * half;
      sn.far[side] = sn.centreEdge[side] = edge;
      uint32_t prev = e.item;
      for (int k = 1; k <= 4; ++k) {
        Vec3 beyond = C + ax * (float(2 * k + 1) * half);
        bool found = false;
        for (const RoadEnd& n : findSmallRoadEnds(vm, player, grid, beyond.x, beyond.z, 1.0f, pf.itemType)) {
          if (n.deadEnd || n.tram || n.item == prev || dot(n.dir, ax) < 0.9f || hasChildren(n.item)) continue;
          sn.chain[side].push_back(n.item);
          sn.chainFar[side].push_back(n.point);
          prev = n.item, found = true;
          break;
        }
        if (!found) break;
      }
    }
    best = sn;
    break;
  }
  return best;
}

// the room a junction point needs for a new road leaving it along `out`: the radius its arms are cut at (as
// findCurveJunctions finds it) and, at a block point, as many straight blocks each way as make the arms that long
void fitJunction(Snap& sn, const Vec3& out) {
  if (!sn.junction) return;
  const CurveProfile& pf = curveProfile(g_state.kind);
  const float hw = std::max(pf.halfWidth, sn.otherHalfWidth);
  float R = hw;
  Vec3 o = unit(out);
  for (const Vec3& a : sn.arms) {
    float th = std::acos(std::clamp(dot(o, a), -1.0f, 1.0f));
    if (th > 0.01f && th < kPi - 0.05f) R = std::max(R, hw / std::tan(th * 0.5f));
  }
  R += 1.0f;  // the margin findCurveJunctions adds
  sn.guard = R + pf.halfWidth;
  sn.radius = R;
  sn.fits = true;
  if (sn.junction == 1 || sn.junction == 2) sn.fits = sn.armRoom >= R + 1.0f;  // the roads there outlast it
  if (sn.junction != 3) return;
  const float half = pf.piece * 0.5f;
  sn.blocks.resize(1);
  sn.fits = true;
  for (int side = 0; side < 2; ++side) {
    sn.far[side] = sn.centreEdge[side];
    float reach = half;
    for (size_t k = 0; reach < R + 1.0f && k < sn.chain[side].size(); ++k) {
      sn.blocks.push_back(sn.chain[side][k]);
      sn.far[side] = sn.chainFar[side][k];
      reach += pf.piece;
    }
    if (reach < R + 1.0f) sn.fits = false;  // the road ends (or turns) too near for this angle
  }
}

// the smallest angle (degrees) between a new road leaving a junction point along `out` and the roads already there
float junctionAngle(const Snap& sn, const Vec3& out) {
  float best = 180.0f;
  for (const Vec3& a : sn.arms)
    best = std::min(best, std::acos(std::clamp(dot(unit(out), a), -1.0f, 1.0f)) * 180.0f / kPi);
  return best;
}

// the curve under p (within its road's half width and the kerb), -1 for none
int curveUnder(const GridData& g, const Vec3& p) {
  int best = -1;
  float bestD = 1.5f;  // how far beyond the outer edge (the kerb edge too)
  for (size_t i = 0; i < g.curves.size(); ++i) {
    const CurveRoad& c = g.curves[i];
    std::vector<CurveSample> shape = curveShape(c, curveFreeEnds(g, c));
    float side, s;
    curveNearest(shape, p.x, p.z, side, s);
    // beyond an end on a road block the point is on the block (the track tool's drag starts there, not on the curve)
    if ((s <= 0.01f && (c.ends & 1)) || (s >= shape.back().s - 0.01f && (c.ends & 2))) continue;
    float d = std::fabs(side) - curveProfile(c.kind).halfWidth;
    if (d < bestD) bestD = d, best = int(i);
  }
  return best;
}

// items a road would run over: everything covering but its own blocks, trees and markers (trees are cleared)
bool overItems(VM& vm, GridObj* grid, const CurveRoad& c, std::vector<uint32_t>& trees, std::vector<uint32_t>& blocking) {
  const GridData& g = *grid->data;
  int x0, z0, x1, z1;
  curveCells(g, c, x0, z0, x1, z1);
  const CurveEnds caps = curveFreeEnds(g, c);
  std::vector<CurveSample> shape = curveShape(c, caps);
  const float cs = g.cellSize;
  for (uint32_t i : gridItemsNear(vm, grid, x0, z0, x1, z1)) {
    if (i >= g.items.size() || g.items[i].removed) continue;
    GridItemShape sh = gridItemShape(vm, grid, i);
    if (sh.flags & kItemTemp) continue;
    if (sh.type == 0x15) continue;  // markers
    if ((g_state.startBlock && i == g_state.startEnd.item) || (g_state.endSnap.block && i == g_state.endSnap.end.item)) continue;
    auto going = [&](const Snap& s) { return std::find(s.blocks.begin(), s.blocks.end(), i) != s.blocks.end(); };
    if (going(g_state.startSnap) || going(g_state.endSnap)) continue;  // blocks a junction replaces
    int ix, iz, iw, il;
    gridItemRect(vm, grid, i, ix, iz, iw, il);
    if (ix > x1 || iz > z1 || ix + iw - 1 < x0 || iz + il - 1 < z0) continue;
    bool hit = false;
    for (int z = iz; z < iz + il && !hit; ++z)
      for (int x = ix; x < ix + iw && !hit; ++x) {
        float wx = (float(x) + 0.5f - float(g.width) * 0.5f) * cs, wz = (float(z) + 0.5f - float(g.length) * 0.5f) * cs;
        float side, s;
        curveNearest(shape, wx, wz, side, s);
        hit = std::fabs(side) < curveProfile(c.kind).halfWidth - 0.5f;
        // a tunnel passes under what stands over its tube
        if (hit && !curveTunnelOpen(g, c, s, caps)) hit = false;
        // elevated tracks pass over what stays a cell under them (their pillars stand between)
        if (hit && (curveIsElevated(c.kind) || c.bridge == 1)) {
          int top = g.items[i].y + (0x200 - int(g.heightBase)) + sh.height;
          hit = top > int(std::lround(curveAt(shape, s).p.y * 256.0f / cs)) - 0x100;
        }
        // near a junction point the junction takes the corner (its neighbours' blocks are not in the way)
        if (hit && ((g_state.startSnap.junction && s < g_state.startSnap.guard) ||
                    (g_state.endSnap.junction && s > shape.back().s - g_state.endSnap.guard)))
          hit = false;
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
  // a junction point fixes only the end's place (forced: a road end fixes its direction too); a straight road from a
  // fixed direction to a junction point bends as a freeform one
  const bool forced = sn.ok && sn.junction == 0;
  const int mode = st.mode == 0 && st.startDir && sn.junction ? 2 : st.mode;
  if (mode == 0 || (mode == 2 && !st.startDir)) {  // straight (and freeform's first road)
    d0 = st.startDir ? st.dir0 : unit(E - S);
    if (st.startDir && !sn.ok) {  // from a fixed end only straight on
      float t = std::max(1.0f, dot(flat(E - S), d0));
      E = S + d0 * t;
      E.y = h.y;
    }
    float L = std::sqrt(len2(E - S));
    d1 = forced ? sn.out * -1.0f : d0;
    a = b = L / 3;
  } else if (mode == 1) {
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
      d1 = forced ? sn.out * -1.0f : unit(E - C);
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
    d1 = forced ? sn.out * -1.0f : unit(E - C);
    a = k * 2.0f / 3.0f;
    b = std::sqrt(len2(E - C)) * 2.0f / 3.0f;
  }
  // an end on an existing road fixes the arrival too: with both directions fixed the smoothest join (a curve or an
  // S) has handles of about half the distance; the bend point only shapes roads with a free end
  if (forced && !(mode == 1 && st.step < 2)) a = b = 0.45f * std::sqrt(len2(E - S));
  c = CurveRoad();
  c.kind = st.kind;
  c.x0 = S.x, c.y0 = S.y, c.z0 = S.z, c.dx0 = d0.x, c.dz0 = d0.z;
  c.x1 = E.x, c.y1 = E.y, c.z1 = E.z, c.dx1 = d1.x, c.dz1 = d1.z;
  c.a = std::max(a, 0.5f), c.b = std::max(b, 0.5f);
  bool endBlock = forced && sn.block && st.step > 0;
  c.ends = uint8_t((st.startBlock ? 1 : 0) | (endBlock ? 2 : 0));
  // an end on a grass tram track: the curve gets tracks that run on into it (its turning circle lies over the track)
  if (c.kind == 0 && st.oneWay) c.ends |= uint8_t(0x20 | (st.oneWay == 1 ? 0x40 : 0));  // one-way, as drawn
  c.bridge = uint8_t(st.bridge);
  if (st.startBlock && st.startEnd.tram) c.ends |= 8 | 4;
  if (endBlock && sn.end.tram) c.ends |= 16 | 4;
  return true;
}

// ---- roundabouts (modes 4: one lane, 5: two lanes; owner's request 2026-11-07) ----
// A ring of four quarter-circle curves (Bézier handles 0.5523·r), one-way counter-clockwise as seen from above (right-hand
// traffic), centre and radius picked with two clicks. Roads join it like any curve: ending on the ring cuts it there
// and makes a junction, whose lanes keep to the ring's direction.
std::vector<CurveRoad> roundaboutArcs(const Vec3& C, float r, bool single) {
  static const float dx[5] = {1, 0, -1, 0, 1}, dz[5] = {0, -1, 0, 1, 0};  // east, north, west, south (north is −z)
  std::vector<CurveRoad> arcs;
  for (int q = 0; q < 4; ++q) {
    CurveRoad c;
    c.kind = 0;
    c.x0 = C.x + dx[q] * r, c.y0 = C.y, c.z0 = C.z + dz[q] * r;
    c.x1 = C.x + dx[q + 1] * r, c.y1 = C.y, c.z1 = C.z + dz[q + 1] * r;
    c.dx0 = dz[q], c.dz0 = -dx[q];  // the tangent turning from east to north
    c.dx1 = dz[q + 1], c.dz1 = -dx[q + 1];
    c.a = c.b = 0.5523f * r;
    c.ends = uint8_t(0x20 | 0x80 | (single ? 0x40 : 0));  // one-way, a ring
    arcs.push_back(c);
  }
  return arcs;
}

void moveRoundabout(VM& vm, GridObj* grid, const Vec3& p) {
  State& st = g_state;
  st.length = st.angle = 0;
  if (st.step == 0) {
    clearPreview(grid);
    return;
  }
  float r = std::clamp(std::sqrt(len2(p - st.start)), 10.0f, 40.0f);
  std::vector<CurveRoad> arcs = roundaboutArcs(st.start, r, st.mode == 4);
  bool bad = false;
  for (const CurveRoad& c : arcs) {
    std::vector<uint32_t> trees, blocking;
    if (overItems(vm, grid, c, trees, blocking)) bad = true;
    for (uint32_t i : blocking) select(vm, grid, i, true);
  }
  grid->curvePreview = arcs[0];
  grid->curvePreviewMore.assign(arcs.begin() + 1, arcs.end());
  grid->hasCurvePreview = true;
  grid->curvePreviewBad = bad;
  ++grid->curvesVersion;
  litPreview(vm, grid, arcs, bad);
  st.previewOk = !bad;
  st.length = r;  // the panel shows the radius
}

void commitRoundabout(VM& vm, ControlObj* player, GridObj* grid) {
  State& st = g_state;
  std::vector<CurveRoad> arcs = roundaboutArcs(st.start, std::max(10.0f, st.length), st.mode == 4);
  Object* tl = playerTimeline(vm, player);
  unselectAll(vm, grid);
  clearPreview(grid);
  gridEditBegin(vm, grid);
  if (tl) timelineBeginGroup(vm, tl, "%8");
  for (const CurveRoad& c : arcs) {
    std::vector<uint32_t> trees, blocking;
    overItems(vm, grid, c, trees, blocking);
    for (uint32_t t : trees) {
      if (t >= grid->data->items.size() || grid->data->items[t].removed) continue;
      timelineRecordDelete(vm, tl, grid, t);
      gridEditRemoveItem(vm, grid, t);
    }
    applyCurveEdit(vm, grid, c, true);
    timelineRecordCurve(vm, tl, c, true);
  }
  if (tl) timelineEndGroup(vm, tl);
  gridEditEnd(vm, grid);
  LOG_INFO("curve tool: roundabout, radius %.1f, %s, %zu curves now", st.length, st.mode == 4 ? "one lane" : "two lanes",
           grid->data->curves.size());
  st.step = 0;
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
  if (st.mode >= 4) {  // roundabouts: the centre, then the size
    moveRoundabout(vm, grid, p);
    return;
  }
  Snap sn = findSnap(vm, player, grid, p);
  Vec3 h = sn.ok ? sn.p : Vec3{p.x, p.y + freeLift(), p.z};  // a free end over the ground
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
    litPreview(vm, grid, {grid->curvePreview}, true);
    st.previewOk = false;
    return;
  }
  if (st.step == 0) {
    clearPreview(grid);
    if (sn.ok && sn.block) select(vm, grid, sn.end.item, false);
    for (uint32_t b : sn.blocks) select(vm, grid, b, false);
    if (!sn.ok) st.hoverCurve = curveUnder(g, p);
    st.length = st.angle = 0;
    return;
  }
  // the curve tool's bend step picks a free point (no snapping)
  if (st.mode == 1 && st.step == 1) sn = Snap(), h = Vec3{p.x, p.y + freeLift(), p.z}, st.endSnap = sn;
  CurveRoad c;
  if (!buildCurve(g, h, sn, c)) {
    clearPreview(grid);
    return;
  }
  const CurveProfile& pf = curveProfile(c.kind);
  bool bad = curveLength(c) < std::min(6.0f, pf.piece) || curveMinRadius(c) < pf.minRadius;
  // junction points: the room they need for this road (blocks to replace, the part of the road they cover)
  if (st.startSnap.junction) fitJunction(st.startSnap, Vec3{c.dx0, 0, c.dz0});
  if (st.endSnap.junction && !(st.mode == 1 && st.step == 1)) fitJunction(st.endSnap, Vec3{-c.dx1, 0, -c.dz1});
  sn = st.endSnap;
  if (!st.startSnap.fits || !sn.fits) bad = true;
  // the new road itself must outlast the junctions at its ends
  if (curveLength(c) < (st.startSnap.junction ? st.startSnap.radius + 1.0f : 0.0f) + (sn.junction ? sn.radius + 1.0f : 0.0f)) bad = true;
  std::vector<uint32_t> trees, blocking;
  if (!bad && overItems(vm, grid, c, trees, blocking)) bad = true;
  if (!bad && c.bridge == 2) {  // a tunnel: the ground over its tube as high as the portal (6 over the road)
    std::vector<CurveSample> shape = curveShape(c);
    float lowest = 1e9f;
    for (float s = 0; s <= shape.back().s; s += 1.5f) {
      if (curveTunnelOpen(g, c, s)) continue;
      CurveSample k = curveAt(shape, s);
      int cx = int(std::lround(k.p.x / g.cellSize + float(g.width) * 0.5f)), cz = int(std::lround(k.p.z / g.cellSize + float(g.length) * 0.5f));
      if (cx < 0 || cz < 0 || cx >= g.width || cz >= g.length) continue;
      float ground = float(g.height[size_t(cz) * size_t(g.width) + size_t(cx)]) * g.cellSize / 256.0f;
      lowest = std::min(lowest, ground - k.p.y);
    }
    if (lowest < 6.0f) bad = true;
    if (std::getenv("REGS_DEBUG_CURVES")) LOG_INFO("curve tool: tunnel, ground over the tube at least %.1f over the road", lowest);
  }
  // at a junction point the new road must keep 30 degrees from the roads there
  float jAngle = 180.0f;
  if (st.startSnap.junction) jAngle = std::min(jAngle, junctionAngle(st.startSnap, Vec3{c.dx0, 0, c.dz0}));
  if (sn.junction && !(st.mode == 1 && st.step == 1)) jAngle = std::min(jAngle, junctionAngle(sn, Vec3{-c.dx1, 0, -c.dz1}));
  if (jAngle < 30.0f) bad = true;
  if (std::getenv("REGS_DEBUG_CURVES") && (st.startSnap.junction || sn.junction))
    LOG_INFO("curve tool: junction start %d end %d, angle %.0f, fits %d/%d, guard %.1f/%.1f, chains %zu+%zu, blocks %zu", st.startSnap.junction,
             sn.junction, jAngle, st.startSnap.fits ? 1 : 0, sn.fits ? 1 : 0, st.startSnap.guard, sn.guard, st.startSnap.chain[0].size(),
             st.startSnap.chain[1].size(), st.startSnap.blocks.size());
  for (uint32_t b : st.startSnap.blocks) select(vm, grid, b, bad);
  if (!(st.mode == 1 && st.step == 1))
    for (uint32_t b : sn.blocks) select(vm, grid, b, bad);
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
  litPreview(vm, grid, {c}, bad);
  st.preview = c;
  st.previewOk = !bad && !(st.mode == 1 && st.step == 1);
  st.length = curveLength(c);
  float cosA = std::clamp(c.dx0 * c.dx1 + c.dz0 * c.dz1, -1.0f, 1.0f);
  st.angle = std::acos(cosA) * 180.0f / kPi;
}

int openTramJoins(VM& vm, ControlObj* player, GridObj* grid, Object* tl);

// a junction point made real (inside the commit's edit batch and timeline group): the curve through it cut in two, or
// the straight blocks there replaced by two straight curves meeting at it. Returns the exact point.
Vec3 makeJunction(VM& vm, GridObj* grid, Object* tl, const Snap& sn, int kind) {
  GridData& g = *grid->data;
  if (sn.junction == 2) {
    int best = -1;
    float bestD = 0.5f, bestS = 0;
    for (size_t i = 0; i < g.curves.size(); ++i) {
      if ((g.curves[i].kind == 3) != (kind == 3)) continue;
      float side, s;
      curveNearest(curveShape(g.curves[i]), sn.p.x, sn.p.z, side, s);
      if (std::fabs(side) < bestD) bestD = std::fabs(side), best = int(i), bestS = s;
    }
    if (best < 0) return sn.p;
    CurveRoad old = g.curves[size_t(best)], first, second;
    Vec3 q = splitCurve(old, bestS, first, second);
    applyCurveEdit(vm, grid, old, false);
    timelineRecordCurve(vm, tl, old, false);
    for (const CurveRoad& h : {first, second}) {
      applyCurveEdit(vm, grid, h, true);
      timelineRecordCurve(vm, tl, h, true);
    }
    return q;
  }
  if (sn.junction == 3) {
    for (uint32_t b : sn.blocks) {
      if (b >= g.items.size() || g.items[b].removed) continue;
      timelineRecordDelete(vm, tl, grid, b);
      gridEditRemoveItem(vm, grid, b);
    }
    for (int side = 0; side < 2; ++side) {  // from the block edge beyond (lanes meet there) to the point
      Vec3 F = sn.far[side], d = unit(sn.p - F);
      float L = std::sqrt(len2(sn.p - F));
      CurveRoad h;
      h.kind = kind;
      h.x0 = F.x, h.y0 = F.y, h.z0 = F.z, h.dx0 = d.x, h.dz0 = d.z;
      h.x1 = sn.p.x, h.y1 = sn.p.y, h.z1 = sn.p.z, h.dx1 = d.x, h.dz1 = d.z;
      h.a = h.b = std::max(0.5f, L / 3.0f);
      h.ends = 1;
      applyCurveEdit(vm, grid, h, true);
      timelineRecordCurve(vm, tl, h, true);
    }
  }
  return sn.p;
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
  // junction points: the road or block there is cut first; the new road ends exactly at the cut
  if (st.startSnap.junction) {
    Vec3 q = makeJunction(vm, grid, tl, st.startSnap, c.kind);
    c.x0 = q.x, c.y0 = q.y, c.z0 = q.z;
  }
  if (st.endSnap.junction) {
    Vec3 q = makeJunction(vm, grid, tl, st.endSnap, c.kind);
    c.x1 = q.x, c.y1 = q.y, c.z1 = q.z;
  }
  applyCurveEdit(vm, grid, c, true);
  timelineRecordCurve(vm, tl, c, true);
  if (c.ends & 4) openTramJoins(vm, player, grid, tl);  // tracks from a grass track: street tram dead ends at its other end
  if (tl) timelineEndGroup(vm, tl);
  gridEditEnd(vm, grid);
  LOG_INFO("curve tool: road %.1f long, %zu trees cleared, %zu curves now", curveLength(c), trees.size(),
           grid->data->curves.size());
  // draw on from its end, unless it ended on a road
  st.startSnap = Snap();
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
    for (const RoadEnd& e : findSmallRoadEnds(vm, player, grid, q.x, q.z, 1.0f, curveProfile(c.kind).itemType))
      if (!e.deadEnd && dot(e.dir, out) > 0.9f) {
        replaceRoadBlockEnd(vm, player, grid, e, false, tl);
        break;
      }
  }
  // its stops' anchor items (the stop items lines use) go with it
  for (const CurveRoad::Stop& st : c.stops) {
    int ax, az, ao;
    stopAnchor(g, c, st, ax, az, ao);
    for (uint32_t i : std::vector<uint32_t>(g.curveAnchors.begin(), g.curveAnchors.end()))
      if (i < g.items.size() && !g.items[i].removed && g.items[i].x == ax && g.items[i].z == az) {
        timelineRecordDelete(vm, tl, grid, i);
        gridEditRemoveItem(vm, grid, i);
        break;
      }
  }
  applyCurveEdit(vm, grid, c, false);
  timelineRecordCurve(vm, tl, c, false);
  if (tl) timelineEndGroup(vm, tl);
  gridEditEnd(vm, grid);
  LOG_INFO("curve tool: road removed, %zu curves now", g.curves.size());
}

// ---- the game's bulldozer (GRIDMODE_REMOVE_OBJECTS) over curves (owner's wish 2026-11-08) ----
// Pointing at a curve shows it red; a click removes it in the editor (its blocks become dead ends again, its stops
// go); in a game curves are refused, as the game refuses roads there (toolMode 1). Elsewhere the bulldozer is the
// game's own.
struct Bulldozer {
  ToolMode orig;
  int curve = -1;
} g_bulldozer;

bool bulldozerInGame(VM& vm, ControlObj* player) {
  auto f = player->fieldValues.find(u"toolMode");
  return f != player->fieldValues.end() && !f->second.isNone() && vm.toInt(f->second) == 1;
}

void bulldozerMove(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  if (!grid || !grid->data) return;
  ToolPick pick = toolPick(vm, player, x, y, false);
  int k = pick.hit ? curveUnder(*grid->data, pick.point) : -1;
  if (k >= 0) {
    if (g_bulldozer.curve != k) {
      if (g_bulldozer.orig.cancel) g_bulldozer.orig.cancel(vm, player, grid);  // the game's own highlight goes
      g_bulldozer.curve = k;
      grid->curvePreview = grid->data->curves[size_t(k)];
      grid->hasCurvePreview = true;
      grid->curvePreviewBad = true;  // red, as the bulldozer shows what it takes
      ++grid->curvesVersion;
      litPreview(vm, grid, {grid->data->curves[size_t(k)]}, true);
      if (std::getenv("REGS_DEBUG_CURVES")) LOG_INFO("bulldozer: curve %d shown", k);
    }
    return;
  }
  if (g_bulldozer.curve >= 0) {
    g_bulldozer.curve = -1;
    clearPreview(grid);
  }
  if (g_bulldozer.orig.move) g_bulldozer.orig.move(vm, player, grid, x, y);
}

bool bulldozerFinish(VM& vm, ControlObj* player, GridObj* grid) {
  if (grid && g_bulldozer.curve >= 0) {
    int k = g_bulldozer.curve;
    g_bulldozer.curve = -1;
    clearPreview(grid);
    if (!bulldozerInGame(vm, player)) removeCurve(vm, player, grid, k);
    return true;
  }
  return g_bulldozer.orig.finish ? g_bulldozer.orig.finish(vm, player, grid) : true;
}

// ---- the game's rename tool (GRIDMODE_RENAME, the map editor) over curves (2026-11-14) ----
// Pointing at a curve marks it and the curves of its kind joined on at its ends (not through a junction), as the game
// marks a road segment; with the right button held the curve alone. The hover event and getReplacedObjects carry a
// stand-in gsGridItemController whose name reads and writes a pending name (<curveName>, interaction.cpp); the game's
// dialog sets it and calls applyTool, which names the curves and records it (timeline 0x070810D0). Elsewhere the
// tool is the game's own.
struct Renamer {
  ToolMode orig;
  std::vector<int> curves;  // the marked curves, the hovered one first
  bool right = false;
  uint64_t version = 0;  // the curves' version they were marked at (an undo replays them: marked afresh)
  Ref<Object> proxy;
} g_renamer;

// the curves of k's kind joined end to end with it (two ends at a point, no junction)
std::vector<int> curveChain(const GridData& g, int k) {
  std::vector<int> out{k};
  std::set<int> seen{k};
  for (size_t i = 0; i < out.size(); ++i) {
    const CurveRoad& c = g.curves[size_t(out[i])];
    for (int end = 0; end < 2; ++end) {
      float x = end ? c.x1 : c.x0, z = end ? c.z1 : c.z0;
      std::vector<int> at;
      for (size_t j = 0; j < g.curves.size(); ++j) {
        if (int(j) == out[i]) continue;
        const CurveRoad& o = g.curves[j];
        if ((std::fabs(o.x0 - x) < 0.2f && std::fabs(o.z0 - z) < 0.2f) || (std::fabs(o.x1 - x) < 0.2f && std::fabs(o.z1 - z) < 0.2f))
          at.push_back(int(j));
      }
      if (at.size() != 1 || seen.count(at[0]) || g.curves[size_t(at[0])].kind != c.kind) continue;
      seen.insert(at[0]);
      out.push_back(at[0]);
    }
  }
  return out;
}

void renameDrop(ControlObj* player, GridObj* grid) {
  if (g_renamer.curves.empty()) return;
  g_renamer.curves.clear();
  toolState(player).extraReplaced.clear();
  g_renamer.proxy = Ref<Object>();
  if (grid) clearPreview(grid);
}

void renameMove(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  if (!grid || !grid->data) return;
  ToolState& t = toolState(player);
  ToolPick pick = toolPick(vm, player, x, y, false);
  int k = pick.hit ? curveUnder(*grid->data, pick.point) : -1;
  if (k >= 0) {
    if (g_renamer.curves.empty() || g_renamer.curves[0] != k || g_renamer.right != t.right ||
        g_renamer.version != grid->curvesVersion) {
      if (g_renamer.curves.empty() && g_renamer.orig.cancel) g_renamer.orig.cancel(vm, player, grid);  // the game's marks go
      const GridData& g = *grid->data;
      g_renamer.curves = t.right ? std::vector<int>{k} : curveChain(g, k);
      g_renamer.right = t.right;
      g_renamer.version = grid->curvesVersion;
      std::vector<CurveRoad> marked;
      std::string name;
      for (int i : g_renamer.curves) {
        marked.push_back(g.curves[size_t(i)]);
        if (name.empty()) name = g.curves[size_t(i)].name;
      }
      litPreview(vm, grid, marked, false);
      g_renamer.proxy = vm.newObject(vm.findClass("gsGridItemController"), nullptr);
      g_renamer.proxy->fieldValues[u"<curveName>"] = Value::string(utf8ToUtf16(name));
      t.extraReplaced = {g_renamer.proxy.get()};
      if (std::getenv("REGS_DEBUG_CURVES"))
        LOG_INFO("rename: curve %d, %zu curves marked, name \"%s\"", k, g_renamer.curves.size(), name.c_str());
      vm.fireEvent(player, u"onHoverRename", {Value::heap(VT::Object, g_renamer.proxy.get())});
    }
    return;
  }
  renameDrop(player, grid);
  if (g_renamer.orig.move) g_renamer.orig.move(vm, player, grid, x, y);
}

bool renameFinish(VM& vm, ControlObj* player, GridObj* grid) {
  if (grid && grid->data && !g_renamer.curves.empty() && g_renamer.proxy) {
    GridData& g = *grid->data;
    std::string name = utf16ToUtf8(vm.toStr(g_renamer.proxy->fieldValues[u"<curveName>"]));
    Object* tl = playerTimeline(vm, player);
    std::vector<int> renamed;
    for (int i : g_renamer.curves)
      if (size_t(i) < g.curves.size() && g.curves[size_t(i)].name != name) renamed.push_back(i);
    if (tl && renamed.size() > 1) timelineBeginGroup(vm, tl, "%2");
    for (int i : renamed) {
      g.curves[size_t(i)].name = name;
      if (tl) timelineRecordCurveName(vm, tl, g.curves[size_t(i)]);
    }
    if (tl && renamed.size() > 1) timelineEndGroup(vm, tl);
    LOG_INFO("rename: %zu curves named \"%s\"", renamed.size(), name.c_str());
    renameDrop(player, grid);
    return true;
  }
  return g_renamer.orig.finish ? g_renamer.orig.finish(vm, player, grid) : true;
}

void renameCancel(VM& vm, ControlObj* player, GridObj* grid) {
  renameDrop(player, grid);
  if (g_renamer.orig.cancel) g_renamer.orig.cancel(vm, player, grid);
}

void bulldozerCancel(VM& vm, ControlObj* player, GridObj* grid) {
  if (std::getenv("REGS_DEBUG_CURVES")) LOG_INFO("bulldozer: cancel (curve %d)", g_bulldozer.curve);
  if (grid && g_bulldozer.curve >= 0) {
    g_bulldozer.curve = -1;
    clearPreview(grid);
  }
  if (g_bulldozer.orig.cancel) g_bulldozer.orig.cancel(vm, player, grid);
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
  if (st.mode >= 4) {
    if (st.step == 0) {
      ToolPick pick = toolPick(vm, player, x, y, false);
      if (!pick.hit) return;
      st.start = Vec3{pick.point.x, pick.point.y + 0.2f, pick.point.z};
      st.step = 1;
    } else if (st.previewOk) {
      commitRoundabout(vm, player, grid);
    }
    move(vm, player, grid, x, y);
    return;
  }
  if (st.step == 0) {
    ToolPick pick = toolPick(vm, player, x, y, false);
    if (!pick.hit) return;
    const Snap& sn = st.endSnap;
    st.start = sn.ok ? sn.p : Vec3{pick.point.x, pick.point.y + freeLift(), pick.point.z};
    st.startDir = sn.ok && !sn.junction;  // from a junction point the direction is free
    st.dir0 = st.startDir ? sn.out : Vec3{1, 0, 0};
    st.startBlock = sn.ok && sn.block;
    st.startEnd = sn.end;
    st.startSnap = sn.junction ? sn : Snap();
    st.step = 1;
    move(vm, player, grid, x, y);
    return;
  }
  if (st.mode == 1 && st.step == 1) {
    ToolPick pick = toolPick(vm, player, x, y, false);
    if (!pick.hit) return;
    Vec3 b{pick.point.x, pick.point.y + freeLift(), pick.point.z};
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

// the other one-way curves of k's ring (a roundabout, cut by its junctions): followed end to start both ways
std::vector<int> ringOf(const GridData& g, int k) {
  std::vector<int> out;
  const CurveRoad& c0 = g.curves[size_t(k)];
  auto near = [](float x0, float z0, float x1, float z1) { return std::fabs(x0 - x1) < 0.2f && std::fabs(z0 - z1) < 0.2f; };
  for (int dir = 0; dir < 2; ++dir) {  // 0: onward from the end, 1: back from the start
    int at = k;
    for (size_t guard = 0; guard < g.curves.size(); ++guard) {
      const CurveRoad& a = g.curves[size_t(at)];
      int next = -1;
      for (size_t i = 0; i < g.curves.size() && next < 0; ++i) {
        const CurveRoad& b = g.curves[i];
        if (int(i) == at || !(b.ends & 0x20) || b.kind != c0.kind) continue;
        if (dir == 0 ? near(a.x1, a.z1, b.x0, b.z0) : near(a.x0, a.z0, b.x1, b.z1)) next = int(i);
      }
      if (next < 0 || next == k || std::find(out.begin(), out.end(), next) != out.end()) break;
      out.push_back(next);
      at = next;
    }
  }
  return out;
}

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
  std::vector<int> ring;  // tram tracks on a roundabout: the ring's other curves (they get tracks with it)
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
    if (std::fabs(side) < curveProfile(c.kind).halfWidth && s > 1.0f && s < curveLength(c) - 1.0f) return true;
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
      if (t.cellX != -1 && w.curve < 0) {  // the original tool's red cell (a spot where no stop fits) goes
        gridEditBegin(vm, grid);
        gridUnselectArea(vm, grid, t.cellX, t.cellZ, 1, 1);
        gridEditEnd(vm, grid);
        t.cellX = t.cellZ = -1;
      }
      wrapClear(vm, player, grid, w);
      const CurveRoad& c = g.curves[size_t(k)];
      w.curve = k;
      w.after = c;
      const float cs = g.cellSize;
      if (w.mode == 0x11) {
        const CurveProfile& pf = curveProfile(c.kind);
        // a roundabout's ring takes tracks all round (trams turn on it): priced for the whole ring
        w.ring = (c.ends & 0x80) ? ringOf(g, k) : std::vector<int>();
        bool missing = !(c.ends & 4);
        float total = curveLength(c);
        for (int r : w.ring) missing = missing || !(g.curves[size_t(r)].ends & 4), total += curveLength(g.curves[size_t(r)]);
        w.ok = missing && pf.tram[0];  // footpaths take none
        w.after.ends |= 4;
        // a one-way road (a roundabout ring too) takes a single track: half the street pieces' price
        if (c.ends & 0x20) total *= 0.5f;
        int n = std::max(1, int(std::ceil(total / pf.piece)));
        int cx = int(c.x0 / cs + float(g.width) * 0.5f), cz = int(c.z0 / cs + float(g.length) * 0.5f);
        for (int i = 0; i < n && w.ok; ++i)
          invisiblePreview(vm, player, grid, pf.tram, cx, int(c.y0 * 256.0f / cs), cz, 0, false);
      } else {
        CurveRoad::Stop st;
        w.ok = stopToolTemplate(vm, player, grid, st) && curveTakesStop(c.kind, st.kind);  // small roads; large and avenues: bus
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
        w.stopAnchor = invisiblePreview(vm, player, grid, stopTemplateName(st, c.kind), ax, int(ay * 256.0f / cs), az, ao, !w.ok);
        if (w.stopAnchor < 0) w.ok = false;
        if (std::getenv("REGS_DEBUG_CURVES"))
          LOG_INFO("curve tool: stop target kind %d design %d at s %.1f of %.1f side %d, anchor %d (%s), %s", int(st.kind),
                   int(st.design), s, L, int(st.side), w.stopAnchor, stopTemplateName(st, c.kind).c_str(), w.ok ? "fits" : "refused");
      }
      showAfter(grid, w.after, !w.ok);
      if (w.mode == 0x11)
        for (int r : w.ring) {
          CurveRoad more = g.curves[size_t(r)];
          more.ends |= 4;
          grid->curvePreviewMore.push_back(more);
        }
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
      for (const RoadEnd& e : findSmallRoadEnds(vm, player, grid, q.x, q.z, 1.0f, curveProfile(c.kind).itemType))
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
    std::vector<CurveRoad> befores{before}, afters{after};
    if (w.mode == 0x11)
      for (int r : w.ring)
        if (size_t(r) < grid->data->curves.size()) {
          befores.push_back(grid->data->curves[size_t(r)]);
          afters.push_back(befores.back());
          afters.back().ends |= 4;
        }
    for (size_t i = 0; i < befores.size(); ++i) {
      applyCurveEdit(vm, grid, befores[i], false);
      timelineRecordCurve(vm, tl, befores[i], false);
      applyCurveEdit(vm, grid, afters[i], true);
      timelineRecordCurve(vm, tl, afters[i], true);
    }
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
  {  // the bulldozer (GRIDMODE_REMOVE_OBJECTS, registered before this) removes curves too
    g_bulldozer.orig = toolModeOf(8);
    ToolMode b = g_bulldozer.orig;
    b.move = bulldozerMove;
    b.finish = bulldozerFinish;
    b.cancel = bulldozerCancel;
    registerToolMode(8, b);
  }
  {  // the rename tool (GRIDMODE_RENAME, registered before this) names curves too
    g_renamer.orig = toolModeOf(kGridModeRename);
    ToolMode r = g_renamer.orig;
    r.move = renameMove;
    r.finish = renameFinish;
    r.cancel = renameCancel;
    registerToolMode(kGridModeRename, r);
  }
  vm.setGlobal(u"REGS_GRIDMODE_CURVE_ROAD", Value::integer(kModeCurveRoad));
  // regsCurveToolMode([mode]): the mode (0 straight, 1 curved, 2 freeform, 3 remove); setting it drops the road being drawn
  implementGlobal(vm, u"regsCurveToolMode", [](VM& v, Object*, int argc, const Value* a) {
    if (argc > 0) {
      g_state.mode = std::clamp(v.toInt(a[0]), 0, 5);
      g_state.step = 0;
      g_state.startDir = g_state.startBlock = false;
      g_state.startSnap = Snap();
    }
    return Value::integer(g_state.mode);
  }, 0, 1);
  // regsCurveToolKind([kind]): the road kind drawn (0 small, 1 large, 2 avenue, 3 alley); setting it drops the road
  // being drawn
  implementGlobal(vm, u"regsCurveToolKind", [](VM& v, Object*, int argc, const Value* a) {
    if (argc > 0) {
      g_state.kind = std::clamp(v.toInt(a[0]), 0, kCurveKinds - 1);
      g_state.step = 0;
      g_state.startDir = g_state.startBlock = false;
      g_state.startSnap = Snap();
    }
    return Value::integer(g_state.kind);
  }, 0, 1);
  // regsCurveToolOneWay([n]): small roads both ways (0), one-way with one lane (1) or two lanes (2)
  implementGlobal(vm, u"regsCurveToolOneWay", [](VM& v, Object*, int argc, const Value* a) {
    if (argc > 0) g_state.oneWay = std::clamp(v.toInt(a[0]), 0, 2);
    return Value::integer(g_state.oneWay);
  }, 0, 1);
  // regsCurveToolBridge([on]): new curves are bridges (true or 1: deck over the ground as it is, the bridge span and
  // pillars), tunnels (2: the tube under the ground, portals at the open ends) or plain (false, 0)
  if (const char* e = std::getenv("REGS_TEST_CURVE_BRIDGE")) g_state.bridge = std::atoi(e) == 2 ? 2 : 1;
  implementGlobal(vm, u"regsCurveToolBridge", [](VM& v, Object*, int argc, const Value* a) {
    if (argc > 0) {
      Value x = v.resolve(a[0]);
      g_state.bridge = x.type() == VT::Int ? std::clamp(v.toInt(x), 0, 2) : v.truthy(x) ? 1 : 0;
    }
    return Value::integer(g_state.bridge);
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
