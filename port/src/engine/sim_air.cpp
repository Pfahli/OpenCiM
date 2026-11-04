// Aircraft and helicopters (docs/simulation/boats-aircraft.md §5, §6.2): inline flight nodes outside the lane graph,
// the background aircraft that land on and take off from the airport runways, and the Bézier legs of helicopter
// lines.
// Deviations: no gear or door animation; inline nodes take the chord as their end direction where a saved quaternion
// does not point along it; helicopter separation (checkHelicopterPaths) is in engine/sim.cpp without the one-node
// creep towards a helicopter near only the fourth node.
#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "core/text.h"
#include "engine/sim_state.h"

namespace regs::engine {
using namespace regs::script;
using render::Vec3;

namespace sim {

// ---- inline nodes (0x2000000): one owner each, recycled once released ----

int newInlineNode(SimState& s, LaneNode n) {
  LaneGraph& G = s.graph;
  n.flags = (n.flags & ~(0x1000000u | kPathDeleted)) | 0x2000000u;
  n.item = -1;
  n.next.clear(), n.crossings.clear();
  Vec3 d = n.p1 - n.p0;
  n.length = std::sqrt(dot(d, d));
  n.cellX = int((n.p0.x + n.p1.x) / G.cellSize + float(G.width) + 0.5f) >> 1;
  n.cellZ = int((n.p0.z + n.p1.z) / G.cellSize + float(G.length) + 0.5f) >> 1;
  int i;
  if (!s.freeInline.empty()) {
    i = s.freeInline.back();
    s.freeInline.pop_back();
    G.nodes[size_t(i)] = std::move(n);
  } else {
    i = int(G.nodes.size());
    G.nodes.push_back(std::move(n));
  }
  if (s.rt.size() < G.nodes.size()) s.rt.resize(G.nodes.size());
  s.rt[size_t(i)] = NodeRt();
  return i;
}

void freeInlineNode(SimState& s, int i) {
  LaneNode& n = s.graph.nodes[size_t(i)];
  if (!(n.flags & 0x2000000u) || (n.flags & kPathDeleted)) return;
  n.flags |= kPathDeleted;
  s.rt[size_t(i)] = NodeRt();
  s.freeInline.push_back(i);
}

namespace {

Vec3 bez(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, float t) {
  float u = 1 - t;
  return a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + d * (t * t * t);
}
Vec3 bezD1(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, float t) {
  float u = 1 - t;
  return (b - a) * (3 * u * u) + (c - b) * (6 * u * t) + (d - c) * (3 * t * t);
}
float bezLen(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {  // FUN_100861f0: 8 chords
  float len = 0;
  Vec3 prev = a;
  for (int k = 1; k <= 8; ++k) {
    Vec3 p = bez(a, b, c, d, float(k) / 8.0f), e = p - prev;
    len += std::sqrt(dot(e, e));
    prev = p;
  }
  return len;
}
Vec3 unit(const Vec3& v) {
  float l = std::sqrt(dot(v, v));
  return l > 1e-6f ? v * (1.0f / l) : Vec3{0, 0, -1};
}
Vec3 perp(const Vec3& v) { return std::fabs(v.x) > std::fabs(v.y) ? Vec3{-v.z, 0, v.x} : Vec3{0, v.z, -v.y}; }  // FUN_100851b0

// an inline node from the end of prev to p1, heading dir at its end
int airNode(SimState& s, int prev, const Vec3& p1, const Vec3& dir, uint32_t flags, float curveFactor, float speedLimit = 6) {
  const LaneNode& pv = s.graph.nodes[size_t(prev)];
  LaneNode n;
  n.p0 = pv.p1;
  n.f0 = pv.f1;
  n.p1 = p1;
  n.f1 = unit(dir);
  n.flags = flags;
  n.curveFactor = curveFactor;
  n.speedLimit = speedLimit;
  return newInlineNode(s, n);
}

// FUN_100e1600: the successor straight on (largest positive dot of the chords, the first on ties)
int straightOn(const SimState& s, int n) {
  if (n < 0) return -1;
  const LaneNode& a = s.graph.nodes[size_t(n)];
  int best = -1;
  float bestDot = 0;
  for (int k : a.next) {
    const LaneNode& b = s.graph.nodes[size_t(k)];
    float d = dot(b.p1 - b.p0, a.p1 - a.p0);
    if (d > bestDot) best = k, bestDot = d;
  }
  return best;
}
// FUN_100e2e50: the predecessor straight back
int straightBack(const SimState& s, int n) {
  if (n < 0) return -1;
  const LaneGraph& G = s.graph;
  const LaneNode& a = G.nodes[size_t(n)];
  std::vector<int> found;
  G.query(int(a.p0.x / G.cellSize) + G.width / 2, int(a.p0.z / G.cellSize) + G.length / 2, 0, 0, 0, found);
  int best = -1;
  float bestDot = 0;
  for (int k : found) {
    const LaneNode& b = G.nodes[size_t(k)];
    if (std::find(b.next.begin(), b.next.end(), n) == b.next.end()) continue;
    float d = dot(b.p1 - b.p0, a.p1 - a.p0);
    if (d > bestDot) best = k, bestDot = d;
  }
  return best;
}
int forward(const SimState& s, int n, int steps, bool back) {
  for (int k = 0; k < steps && n >= 0; ++k) n = back ? straightBack(s, n) : straightOn(s, n);
  return n;
}

// a plane owns its whole route from the start (occupiedCount = ringCount + 1): every node claimed at once
int spawnPlane(VM& vm, SimState& s, int model, const std::vector<int>& route, float speed) {
  int id = spawnTraffic(vm, s, {Unit{model, false, 0, 0}}, route[0], std::vector<int>(route.begin() + 1, route.end()), false);
  Mover& m = s.movers[size_t(id)];
  --s.roadLive;
  m.aircraft = true;
  ++s.aircraftLive;
  for (size_t k = 0; k < m.track.nodes.size(); ++k) s.rt[size_t(m.track.nodes[k])].occupant = id;
  m.holdBegin = 0;
  m.holdEnd = int(m.track.nodes.size());
  m.speed = speed;
  return id;
}

float mapEdge(const SimState& s) { return -0.8f * float(s.graph.width) * s.graph.cellSize; }  // @0x10167b68

// FUN_1010b430: approach from beyond the opposite map edge, a 15-node glide to the touchdown node R, the landing
// roll R .. R+14, then the taxi path from T to a terminal gate
void spawnLanding(VM& vm, SimState& s, int model) {
  AirPending& p = s.landing;
  if (p.a < 0) {
    if (s.airEntries.empty() || s.airGatesIn.empty()) return;
    p.entry = s.airEntries[size_t(rnd(s)) % s.airEntries.size()];
    p.a = forward(s, p.entry, 15, false);
    p.b = forward(s, p.a, 15, false);
    p.gate = s.airGatesIn[size_t(rnd(s)) % s.airGatesIn.size()];
    float cost = 0;
    if (p.a < 0 || p.b < 0 || !s.graph.search(p.b, p.gate, false, p.path, cost)) {
      p = AirPending();
      return;
    }
  }
  for (int n = p.a, k = 0; k < 15; ++k, n = straightOn(s, n))  // R .. R+14 and T .. gate free now, else next tick
    if (n < 0 || !canEnter(s, n, -2, false)) return;
  if (!canEnter(s, p.b, -2, false)) return;
  for (int n : p.path)
    if (!canEnter(s, n, -2, false)) return;
  const LaneGraph& G = s.graph;
  Vec3 E = G.nodes[size_t(p.entry)].p0 + Vec3{0, 15, 0};
  Vec3 d = unit(Vec3{E.x, 0, E.z});
  d = d + perp(d) * rndFloat(s, -0.5f, 0.5f);
  Vec3 dA = d + perp(d) * rndFloat(s, -0.5f, 0.5f);
  Vec3 R0 = G.nodes[size_t(p.a)].p0, glide = unit(R0 - E);
  Vec3 S{mapEdge(s) * d.x, rndFloat(s, 150, 200), mapEdge(s) * d.z};
  Vec3 se = S - E;
  float len = std::sqrt(dot(se, se));
  Vec3 P1 = S + unit(dA) * (0.333f * len), P2 = E - glide * (0.333f * len);
  int n = int(bezLen(S, P1, P2, E) / 10.0f);
  uint32_t flags = (G.nodes[size_t(p.entry)].flags & ~0x1000000u) | 0x2000000u;
  // a 12-unit lead-in that ends at S
  LaneNode first;
  first.p0 = S + unit(S - P1) * 12.0f;
  first.p1 = S;
  first.f0 = first.f1 = unit(dA);
  first.flags = flags;
  first.curveFactor = 20;
  first.speedLimit = 6;
  std::vector<int> route{newInlineNode(s, first)};
  for (int i = 1; i <= n; ++i) {
    float t = float(i) / float(n);
    route.push_back(airNode(s, route.back(), bez(S, P1, P2, E, t), bezD1(S, P1, P2, E, t), flags, 20));
  }
  for (int k = 1; k <= 15; ++k) route.push_back(airNode(s, route.back(), E + (R0 - E) * (float(k) / 15.0f), glide, flags, 20));
  for (int g = p.a, k = 0; k < 15 && g >= 0; ++k, g = straightOn(s, g)) route.push_back(g);
  route.push_back(p.b);
  route.insert(route.end(), p.path.begin(), p.path.end());
  spawnPlane(vm, s, model, route, 15.0f);
  p = AirPending();
}

// FUN_1010c190: from a terminal gate along the taxi path to T', the take-off roll to L', a 15-node climb to 10 units
// above the runway's end and the flight out beyond the opposite map edge
void spawnDeparting(VM& vm, SimState& s, int model) {
  AirPending& p = s.departing;
  if (p.a < 0) {
    if (s.airGatesOut.empty() || s.airExits.empty()) return;
    p.gate = s.airGatesOut[size_t(rnd(s)) % s.airGatesOut.size()];
    p.entry = s.airExits[size_t(rnd(s)) % s.airExits.size()];
    p.a = forward(s, p.entry, 15, true);  // L'
    p.b = forward(s, p.a, 15, true);      // T'
    float cost = 0;
    if (p.a < 0 || p.b < 0 || !s.graph.search(p.gate, p.b, false, p.path, cost)) {
      p = AirPending();
      return;
    }
  }
  if (!canEnter(s, p.gate, -2, false)) return;
  for (int n : p.path)
    if (!canEnter(s, n, -2, false)) return;
  for (int n = straightOn(s, p.b), k = 0; k < 15; ++k, n = straightOn(s, n))
    if (n < 0 || !canEnter(s, n, -2, false)) return;
  const LaneGraph& G = s.graph;
  const LaneNode& L = G.nodes[size_t(p.a)];
  Vec3 E = G.nodes[size_t(p.entry)].p1 + Vec3{0, 10, 0};
  Vec3 d = unit(Vec3{E.x, 0, E.z});
  d = d + perp(d) * rndFloat(s, -0.5f, 0.5f);
  Vec3 dZ = d * -1.0f + perp(d) * rndFloat(s, -0.5f, 0.5f);
  Vec3 climb = unit((E - L.p1) + Vec3{0, 10, 0});
  Vec3 S{mapEdge(s) * d.x, rndFloat(s, 150, 200), mapEdge(s) * d.z};
  Vec3 se = S - E;
  float len = std::sqrt(dot(se, se));
  Vec3 P1 = E + climb * (0.333f * len), P2 = S - unit(dZ) * (0.333f * len);
  int n = int(bezLen(S, P2, P1, E) / 10.0f);
  Vec3 le = L.p1 - E;
  float c = 0.333f * std::sqrt(dot(le, le));
  Vec3 H1 = L.p0 + unit(L.p1 - L.p0) * c, H2 = E - climb * c;
  uint32_t flags = (G.nodes[size_t(p.entry)].flags & ~0x1000000u) | 0x2000000u;
  std::vector<int> route{p.gate};
  route.insert(route.end(), p.path.begin(), p.path.end());
  int g = p.b;
  for (int k = 0; k < 15 && g >= 0; ++k) {
    g = straightOn(s, g);
    if (g >= 0) route.push_back(g);
  }
  for (int k = 1; k <= 15; ++k) {
    float t = float(k) / 15.0f;
    route.push_back(airNode(s, route.back(), bez(L.p1, H1, H2, E, t), bezD1(L.p1, H1, H2, E, t), flags, 30));
  }
  for (int i = 1; i <= n; ++i) {
    float t = float(i) / float(n);
    route.push_back(airNode(s, route.back(), bez(E, P1, P2, S, t), bezD1(E, P1, P2, S, t), flags, 30));
  }
  spawnPlane(vm, s, model, route, 0.0f);
  p = AirPending();
}

}  // namespace

void stepAircraft(VM& vm, SimState& s) {
  if (!s.vehiclesModule) return;
  auto lists = s.vehiclesModule->fieldValues.find(u"<airplanes>");
  auto* arr = lists == s.vehiclesModule->fieldValues.end() ? nullptr : lists->second.array();
  if (!arr || arr->items.empty()) return;
  int cap = std::min(int(s.airEntries.size()), 2);
  if (s.aircraftLive >= cap) return;
  // a pending spawn goes first; otherwise a coin decides (0x1010f1e0.c:60-82)
  bool landing = s.landing.a >= 0 || (s.departing.a < 0 && (rnd(s) & 1) == 0);
  int model = modelFor(vm, s.grid, s, utf16ToUtf8(vm.toStr(arr->items[size_t(rnd(s)) % arr->items.size()])));
  if (model < 0) return;
  if (landing) spawnLanding(vm, s, model);
  else spawnDeparting(vm, s, model);
}

// 0x1010f1e0.c:217-266: a plane stopped at the end of its route is removed after 6 s, or at once when it stopped in
// the air (a departure reaching the map edge); one stuck for 60 s too
bool aircraftDone(const SimState& s, const Mover& m) {
  if (m.waitTime > 60) return true;
  int left = int(m.track.nodes.size()) - 1 - int(m.track.at(m.dist));
  if (left > 2 + 2 || m.speed >= 0.1f) return false;
  float y = unitPose(s, m, m.units[0], m.dist).position.y;
  return m.waitTime > 6 || y > 20;
}

// FUN_10113280: a helicopter leg, 5 + trunc(d/20) inline Bézier nodes from the departure pad to the arrival pad,
// lifted 100 units and pushed out 50 along the pads, slow at both ends (6 − 5(2t − 1)^4), ending on the pad node
void helicopterLeg(SimState& s, int from, int to, std::vector<int>& out) {
  out.clear();
  if (from < 0 || to < 0) return;
  const LaneNode &A = s.graph.nodes[size_t(from)], &B = s.graph.nodes[size_t(to)];
  Vec3 P0 = A.p1, P3 = B.p0, d3 = P0 - P3;
  Vec3 P1 = P0 + unit(A.p1 - A.p0) * 50.0f + Vec3{0, 100, 0}, P2 = P3 + unit(B.p0 - B.p1) * 50.0f + Vec3{0, 100, 0};
  int n = 5 + int(std::sqrt(dot(d3, d3)) / 20.0f);
  int prev = from;
  for (int i = 1; i <= n; ++i) {
    float t = float(i) / float(n), u = 2 * t - 1;
    prev = airNode(s, prev, bez(P0, P1, P2, P3, t), bezD1(P0, P1, P2, P3, t), 0x2004000u, 30, 6 - 5 * u * u * u * u);
    out.push_back(prev);
  }
  out.push_back(to);
}

}  // namespace sim
}  // namespace regs::engine
