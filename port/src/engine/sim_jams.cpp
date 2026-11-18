// Jam relief for buses and trams: a port QoL change (docs/simulation/transport.md §10, port/docs/ideas.md). The
// original only fires onJammed after 180 s of standing (0x101152a0.c:192-197) and leaves the jam as it is, so two line
// vehicles that lock each other at a junction, or a bus or tram behind a car that cannot go on, stand for good and the
// traffic behind them backs up. The port resolves it in three steps:
//   1. detection: a bus or tram (line types 0 and 1) whose head has not advanced a unit for 120 s of game time,
//      not counting its dwell at a stop, the stand of a breakdown or a crash (the clock starts anew after them); one
//      directly behind a line vehicle already in relief waits for that one;
//   2. clearing: every 3 s one road vehicle of the jam around it turns round onto the opposite lane of its node with
//      a new route, or leaves the map like a finished trip where no turn is possible. Direct blockers (and what blocks
//      them) come first, then the queues waiting for them or for the bus or tram, nearest first, within 60 units;
//      other line vehicles are never turned (step 3 handles them, they are detected on their own). The node it waits
//      for and its crossings are kept for it meanwhile (NodeRt::keep), so a stream of cars cannot starve it;
//   3. respawn: still standing 30 s after step 2 began (the 30 s restart whenever it advances a unit), it fires the
//      original's onJammed if that has not fired yet, its riders get off where it stood and plan again, it leaves the
//      map (onUnspawn, as when a line is paused) and spawns two stops on as soon as that platform is free (after 120 s
//      of waiting for it, the usual search from the first stop), firing the port's onRegsJamMoved for the ticker.
// Always on; REGS_NO_JAM_RELIEF=1 turns it off (detections are still logged then, with how the jam ended by itself).
// Metro trains are only watched (detections logged), water bus and helicopter lines left alone (§10.4). Nothing of it
// is saved: a reload starts the clocks anew. A summary line every 10 game minutes counts what happened.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "core/log.h"
#include "core/env.h"
#include "engine/sim.h"
#include "engine/sim_state.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::engine::sim;
using render::Vec3;

namespace sim {
namespace {

constexpr float kDetect = 120.0f;   // seconds standing before step 2
constexpr float kRespawn = 30.0f;   // seconds of step 2 without progress before step 3
constexpr float kTurnEvery = 3.0f;  // seconds between two cars turned
constexpr float kRadius = 60.0f;    // world units around the bus or tram
constexpr float kUnit = 1.0f;       // progress that counts as moving
constexpr float kDone = 20.0f;      // progress that ends step 2
constexpr float kPlatformWait = 120.0f;  // step 3: seconds the respawn waits for its platform

bool reliefOff() {
  static const bool off = [] {
    const char* e = regs::envVar("REGS_NO_JAM_RELIEF");
    return e && *e && *e != '0';
  }();
  return off;
}

const char* kindOf(int type) { return type == 0 ? "bus" : type == 1 ? "tram" : "metro train"; }

// metro trains are only watched (detections logged, nothing done: transport.md §10.4), and everything is while
// REGS_NO_JAM_RELIEF=1
bool watchOnly(const LineState& line) { return reliefOff() || line.type == 2; }

struct Stats {
  float time = 0;  // simulated seconds since the start (log stamps)
  int detected = 0, resolved = 0, turned = 0, left = 0, respawned = 0, byItself = 0;
} g_stats;

// the REGS_TEST_JAM state
struct TestJam {
  bool parsed = false, on = false, hard = false, placed = false, save = false, saved = false;
  int type = 1, blocker = -1, vehicle = -1;
  float tries = 0, sinceRespawn = -1;
  int replannedAt = 0, evacuatedAt = 0;
} g_test;

bool testing() { return g_test.on; }

// `count` screenshots of the main view, every 12th drawn frame (only while REGS_TEST_JAM runs): <name><16 - left>.png
void shot(SimState& s, const std::string& name, int count = 1) {
  if (!testing()) return;
  s.followShots = count;
  s.followFrame = 0;
  s.followShotName = name;
}

Vec3 headPoint(const SimState& s, const Mover& m) {
  if (m.track.nodes.empty() || m.units.empty() || m.units[0].model < 0) return {};
  return unitPose(s, m, m.units[0], m.dist).position;
}

// a cheap position for the radius test: the start of the node under the head
Vec3 nodePoint(const SimState& s, const Mover& m) {
  if (m.track.nodes.empty()) return {};
  return s.graph.nodes[size_t(m.track.nodes[m.track.at(m.dist)])].p0;
}

bool roadVehicle(const Mover& m) { return m.live && !m.transit && !m.train && !m.boat && !m.aircraft; }

bool clearable(const Mover& m) { return roadVehicle(m) && !m.crashing && !m.testPin; }

// The movers in the way of a standing mover: the occupants of the first node ahead it cannot enter (that node and
// its crossings, canEnter of grid-traffic.md §6.3 without the waiting markers, which name nobody) and the leader it
// queues behind. Empty for moving and parked movers.
void blockersOf(const SimState& s, int id, std::vector<int>& out) {
  out.clear();
  const Mover& m = s.movers[size_t(id)];
  if (!m.live || m.parked || m.track.nodes.empty() || m.speed >= 0.1f) return;
  auto add = [&](int o) {
    if (o >= 0 && o != id && s.movers[size_t(o)].live && std::find(out.begin(), out.end(), o) == out.end()) out.push_back(o);
  };
  int last = lastUsable(m), from = std::max(m.holdEnd, 0);
  for (int k = from; k <= last && k < from + m.lengthNodes + 8; ++k) {
    int n = m.track.nodes[size_t(k)];
    if (s.rt[size_t(n)].occupant == id || canEnter(s, n, id, true)) continue;
    add(s.rt[size_t(n)].occupant);
    for (int c : s.graph.nodes[size_t(n)].crossings) add(s.rt[size_t(c)].occupant);
    break;
  }
  if (m.reserved >= 0) add(s.rt[size_t(m.reserved)].occupant);
}

// A line vehicle directly in the way of vehicle vi whose (active) relief runs; with `older`, only one that has stood
// longer (ties: the lower index), which makes the order strict for two vehicles locking each other. −1 = none.
int lineVehicleAhead(const SimState& s, int vi, bool older) {
  const VehicleState& v = s.vehicles[size_t(vi)];
  std::vector<int> ahead;
  blockersOf(s, v.mover, ahead);
  for (int b : ahead) {
    const Mover& bm = s.movers[size_t(b)];
    if (!bm.transit || bm.vehicle < 0 || bm.vehicle == vi) continue;
    const VehicleState& w = s.vehicles[size_t(bm.vehicle)];
    if (!w.relief || w.line < 0 || watchOnly(s.lines[size_t(w.line)])) continue;
    if (!older || w.jamClock > v.jamClock || (w.jamClock == v.jamClock && bm.vehicle < vi)) return bm.vehicle;
  }
  return -1;
}

struct Jam {
  struct Entry {
    int id;
    int tier;   // 0: in the way of the bus or tram (or of what is in its way), 1: waiting in the jam
    int depth;  // hops from the bus or tram
    float d2;   // squared distance to its head
  };
  std::vector<Entry> members;  // without the bus or tram itself
  std::vector<int> direct;     // its own blockers
  int lineVehicles = 0, roadVehicles = 0, clearable = 0;
};

// The jam around the mover `vid`: the stationary movers within kRadius linked to it by "waits for" in either
// direction (its blockers and theirs, the queues behind those, the vehicles waiting for the nodes it holds)
Jam jamAround(const SimState& s, int vid) {
  Jam jam;
  Vec3 c = nodePoint(s, s.movers[size_t(vid)]);
  float r2 = kRadius * kRadius;
  std::unordered_map<int, std::vector<int>> blk;
  std::unordered_map<int, float> dist2;
  std::unordered_map<int, std::vector<int>> waiters;
  for (size_t id = 0; id < s.movers.size(); ++id) {
    const Mover& m = s.movers[id];
    if (!m.live || m.track.nodes.empty() || m.aircraft || m.boat) continue;
    Vec3 d = nodePoint(s, m) - c;
    float dd = d.x * d.x + d.z * d.z;
    if (dd > r2 && int(id) != vid) continue;
    dist2[int(id)] = dd;
    blockersOf(s, int(id), blk[int(id)]);
  }
  for (auto& [id, bs] : blk)
    for (int b : bs) waiters[b].push_back(id);
  jam.direct = blk[vid];
  std::unordered_map<int, int> depth{{vid, 0}}, tier{{vid, 0}};
  std::vector<int> queue{vid};
  for (size_t q = 0; q < queue.size(); ++q) {  // forward: what is in the way
    int x = queue[q];
    for (int b : blk[x])
      if (dist2.count(b) && !depth.count(b)) depth[b] = depth[x] + 1, tier[b] = 0, queue.push_back(b);
  }
  for (size_t q = 0; q < queue.size(); ++q) {  // then everything linked to those, either way
    int x = queue[q];
    auto visit = [&](int y) {
      if (!dist2.count(y) || depth.count(y)) return;
      depth[y] = depth[x] + 1, tier[y] = 1, queue.push_back(y);
    };
    for (int b : blk[x]) visit(b);
    if (auto w = waiters.find(x); w != waiters.end())
      for (int y : w->second) visit(y);
  }
  for (int id : queue) {
    if (id == vid) continue;
    const Mover& m = s.movers[size_t(id)];
    jam.members.push_back({id, tier[id], depth[id], dist2[id]});
    if (m.transit) ++jam.lineVehicles;
    else if (roadVehicle(m)) ++jam.roadVehicles;
    if (clearable(m)) ++jam.clearable;
  }
  std::sort(jam.members.begin(), jam.members.end(), [](const Jam::Entry& a, const Jam::Entry& b) {
    return a.tier != b.tier ? a.tier < b.tier : a.depth != b.depth ? a.depth < b.depth : a.d2 < b.d2;
  });
  return jam;
}

std::string describe(const SimState& s, const std::vector<int>& ids) {
  std::string out;
  for (int id : ids) {
    const Mover& m = s.movers[size_t(id)];
    char b[160];
    if (m.transit) {
      const VehicleState* v = m.vehicle >= 0 ? &s.vehicles[size_t(m.vehicle)] : nullptr;
      int type = v && v->line >= 0 ? s.lines[size_t(v->line)].type : -1;
      std::snprintf(b, sizeof b, " line vehicle %d (line %d, type %d)", m.vehicle, v ? v->line : -1, type);
    } else {
      int nd = m.track.nodes.empty() ? -1 : m.track.nodes[m.track.at(m.dist)];
      std::snprintf(b, sizeof b, " %s %d%s (standing %.0f s, %zu nodes to go, citizen %d, node %d flags %x)",
                    m.train ? "train" : m.parked ? "parked car" : "car", id,
                    m.testPin ? " (test pin)" : m.crashing ? " (crashed)" : "", m.waitTime,
                    m.track.nodes.size() - 1 - m.track.at(m.dist), m.citizen, nd,
                    nd >= 0 ? s.graph.nodes[size_t(nd)].flags : 0u);
    }
    out += b;
  }
  return out.empty() ? " nobody (a marker or the junction rule)" : out;
}

// The lane of the same road the other way round. Tracks have coincident pairs (oppositeNode, FUN_100e2630); a road's
// two directions lie side by side, so there it is the nearest node of the same class within 8 units sideways that runs
// the other way (directions within 25°) along the same stretch at the same height. Junction nodes have none.
int oppositeLane(const LaneGraph& G, int n) {
  if (int o = oppositeNode(G, n); o >= 0) return o;
  const LaneNode& a = G.nodes[size_t(n)];
  if (a.flags & kPathIntersection) return -1;
  auto flat = [](Vec3 v) {
    v.y = 0;
    float l = std::sqrt(dot(v, v));
    return l > 1e-4f ? v * (1.0f / l) : Vec3{};
  };
  Vec3 da = flat(a.p1 - a.p0), ma = (a.p0 + a.p1) * 0.5f;
  float halfLen = a.length * 0.5f, bestSide = 8.0f;
  uint32_t cls = a.flags & kPathClassMask & ~0x600u;
  int best = -1;
  std::vector<int> found;
  G.query(a.cellX, a.cellZ, 0, 0, 0, found);
  for (int c : found) {
    const LaneNode& b = G.nodes[size_t(c)];
    if (c == n || (b.flags & kPathClassMask & ~0x600u) != cls || (b.flags & (kPathPark | kPathDeleted | kPathIntersection))) continue;
    if (dot(da, flat(b.p1 - b.p0)) > -0.9f) continue;
    Vec3 d = (b.p0 + b.p1) * 0.5f - ma;
    if (std::fabs(d.y) > 2.0f) continue;
    d.y = 0;
    float along = dot(d, da), side = std::sqrt(std::max(0.0f, dot(d, d) - along * along));
    if (std::fabs(along) > halfLen + 1.0f || side >= bestSide) continue;
    bestSide = side;
    best = c;
  }
  return best;
}

// a car leaves the map like a finished trip (stepTraffic's arrival despawn); a citizen's parked car goes as when its
// owner gives it up for a walk or a transit trip (startTrip): the next drive takes a new one
void sendOff(VM& vm, SimState& s, int id) {
  Mover& m = s.movers[size_t(id)];
  int ci = m.citizen;
  if (ci >= 0 && !m.parked) {
    carArrived(vm, s, id, true);
    return;
  }
  m.citizen = -1;
  despawn(vm, s, id);
  if (ci >= 0 && size_t(ci) < s.citizens.size() && s.citizens[size_t(ci)].car == id) {
    s.citizens[size_t(ci)].car = -1;
    s.citizens[size_t(ci)].flags &= ~0x100000u;
  }
}

// Turns a road vehicle round: onto the opposite lane of the node under its middle, at the mirrored place, with a route
// to where it was going (or, for a parked car or without a route there, to one of up to eight random road END
// nodes). Consists of more than one car, a missing or taken opposite lane, or no route: it leaves the map. True when
// it turned.
bool turnRound(VM& vm, SimState& s, int id, const char*& why) {
  const LaneGraph& G = s.graph;
  Mover& m = s.movers[size_t(id)];
  int opp = -1;
  float nd = 0;
  std::vector<int> path;
  why = m.parked && m.citizen >= 0 ? "a citizen's parked car" : "a consist of several cars";
  if (m.units.size() == 1 && m.units[0].model >= 0 && !m.track.nodes.empty() && !(m.parked && m.citizen >= 0)) {
    const UnitModel& hm = s.models[size_t(m.units[0].model)];
    float len = hm.rearOffset + hm.frontOffset;
    size_t i = m.track.at(m.dist + len * 0.5f);
    opp = oppositeLane(G, m.track.nodes[i]);
    why = "no opposite lane";
    if (opp >= 0) {
      float L = G.nodes[size_t(opp)].length, o = m.dist - m.track.start[i];
      nd = std::clamp(L - o - len, 0.0f, std::max(0.0f, L - len));
      if (!canEnter(s, opp, id, true)) opp = -1, why = "the opposite lane is taken";
    }
    if (opp >= 0) {
      float cost = 0;
      int dest = m.parked ? -1 : m.track.nodes.back();
      bool ok = dest >= 0 && dest != opp && G.search(opp, dest, false, path, cost) && !path.empty();
      for (int t = 0; !ok && t < 8 && !s.roadEnds.empty(); ++t) {
        int e = s.roadEnds[size_t(rnd(s)) % s.roadEnds.size()];
        ok = e != opp && G.search(opp, e, false, path, cost) && !path.empty();
      }
      if (!ok) opp = -1, why = "no route from the opposite lane";
      // a car longer than the opposite node reaches into the next one: that one must be free too
      if (opp >= 0 && G.nodes[size_t(opp)].length < len && !canEnter(s, path[0], id, true)) opp = -1, why = "the opposite lane is taken";
    }
  }
  if (opp < 0) {
    sendOff(vm, s, id);
    return false;
  }
  releaseAll(s, id);
  if (m.parked) m.parked = false, ++s.roadLive;
  m.track = Track();
  m.track.push(G, opp);
  for (int n : path) m.track.push(G, n);
  m.dist = nd;
  m.speed = 0;
  m.waitTime = 0;
  m.limit = -1;
  m.reserved = -1;
  m.reckless = false;
  claimBody(s, id);
  why = "";
  return true;
}

void releaseKept(SimState& s, VehicleState& v) {
  for (int n : v.kept)
    if (size_t(n) < s.rt.size() && s.rt[size_t(n)].keep == v.mover) s.rt[size_t(n)].keep = -1;
  v.kept.clear();
}

// Step 2 also keeps the node the vehicle waits for, and its crossings, for it: nobody else claims them, so a stream
// of cars through a junction (each car moving, none of them in a jam) lets it in once the cars inside have left.
void keepAhead(SimState& s, VehicleState& v) {
  releaseKept(s, v);
  const Mover& m = s.movers[size_t(v.mover)];
  int last = lastUsable(m), from = std::max(m.holdEnd, 0);
  for (int k = from; k <= last && k < from + m.lengthNodes + 8; ++k) {
    int n = m.track.nodes[size_t(k)];
    if (s.rt[size_t(n)].occupant == v.mover || canEnter(s, n, v.mover, true)) continue;
    auto keep = [&](int x) {
      NodeRt& r = s.rt[size_t(x)];
      if (r.keep < 0) r.keep = v.mover, v.kept.push_back(x);
    };
    keep(n);
    for (int c : s.graph.nodes[size_t(n)].crossings) keep(c);
    break;
  }
}

void endRelief(SimState& s, VehicleState& v, int vi, const char* how) {
  releaseKept(s, v);
  const LineState& line = s.lines[size_t(v.line)];
  if (watchOnly(line)) {
    ++g_stats.byItself;
    LOG_INFO("sim: jam (%s): %s %d (line %d) %s by itself, %.0f s after it stopped [t=%.0f s]",
             line.type == 2 ? "metro, watched only" : "relief off", kindOf(line.type), vi, v.line, how, v.jamClock,
             g_stats.time);
  } else {
    ++g_stats.resolved;
    LOG_INFO("sim: jam relief: %s %d (line %d) %s after %d cars turned or sent off [t=%.0f s; so far %d detected, %d "
             "resolved, %d respawned]",
             kindOf(line.type), vi, v.line, how, v.reliefCleared, g_stats.time, g_stats.detected, g_stats.resolved,
             g_stats.respawned);
  }
  v.relief = false;
  v.reliefCleared = 0;
  v.reliefStill = v.reliefNext = 0;
}

// step 2: the first road vehicle of the jam turns round or leaves; false when there is none
bool clearOne(VM& vm, SimState& s, VehicleState& v, int vi) {
  Jam jam = jamAround(s, v.mover);
  for (const Jam::Entry& e : jam.members) {
    const Mover& m = s.movers[size_t(e.id)];
    if (!clearable(m)) continue;
    Vec3 p = headPoint(s, m);
    std::string who = describe(s, {e.id});
    const char* why = "";
    bool turned = turnRound(vm, s, e.id, why);
    ++v.reliefCleared;
    ++(turned ? g_stats.turned : g_stats.left);
    LOG_INFO("sim: jam relief: %s %d:%s at (%.0f, %.0f) %s%s%s (%s, %d hops, %.0f units away; %d road vehicles and %d "
             "line vehicles left in the jam) [t=%.0f s]",
             kindOf(s.lines[size_t(v.line)].type), vi, who.c_str(), p.x, p.z, turned ? "turns round" : "leaves the map",
             turned ? "" : ": ", why, e.tier == 0 ? "in the way" : "waiting in the jam", e.depth, std::sqrt(e.d2),
             jam.roadVehicles - 1, jam.lineVehicles, g_stats.time);
    shot(s, "jam-" + std::string(turned ? "turn" : "leave") + "-" + std::to_string(v.reliefCleared) + "-", 3);
    return true;
  }
  return false;
}

// step 3: off the map, riders out, two stops on
void respawn(VM& vm, SimState& s, LineState& line, int vi) {
  VehicleState& v = s.vehicles[size_t(vi)];
  Mover& m = s.movers[size_t(v.mover)];
  Vec3 at = headPoint(s, m);
  releaseKept(s, v);
  if (!v.jammedFired && v.object) {  // the original's ticker message comes first
    vm.fireEvent(v.object.get(), u"onJammed", {});
    v.jammedFired = true;
  }
  int riders = evacuateRiders(s, vi, at);
  v.boardingReserved = 0;
  for (auto& st : line.stops)
    if (st.docked == vi) st.docked = -1;
  despawn(vm, s, v.mover);
  v.stopTime = 0;
  v.halted = v.brokenHalted = false;
  v.doorPos = -1;
  if (v.object) vm.fireEvent(v.object.get(), u"onUnspawn", {});
  int n = int(line.stops.size());
  v.respawnAt = n > 0 ? (std::max(v.stopA, 0) + 2) % n : -1;
  v.respawnWait = 0;
  ++g_stats.respawned;
  LOG_INFO("sim: jam relief: %s %d (line %d) still stands 30 s after the relief began (%d cars turned or sent off): it "
           "leaves the map at (%.0f, %.0f), %d riders get off and plan again; it spawns again at stop %d (stops %d -> %d "
           "when it stopped) [t=%.0f s]",
           kindOf(line.type), vi, v.line, v.reliefCleared, at.x, at.z, riders, v.respawnAt, v.stopA, v.stopB, g_stats.time);
  v.relief = false;
  v.reliefCleared = 0;
  shot(s, "jam-respawn-off-");
}

}  // namespace

// a node kept for a line vehicle still in relief (a kept node of one that left by other ways counts as free)
bool keptFor(const SimState& s, const NodeRt& r, int id) {
  if (r.keep < 0 || r.keep == id || size_t(r.keep) >= s.movers.size()) return false;
  const Mover& k = s.movers[size_t(r.keep)];
  return k.live && k.transit && k.vehicle >= 0 && size_t(k.vehicle) < s.vehicles.size() && s.vehicles[size_t(k.vehicle)].relief;
}

bool stepJamRelief(VM& vm, SimState& s, LineState& line, int vi, float dt) {
  VehicleState& v = s.vehicles[size_t(vi)];
  if (line.type > 2 || v.mover < 0 || !s.movers[size_t(v.mover)].live) return false;
  const Mover& m = s.movers[size_t(v.mover)];
  if (m.travelled < v.jamMark) v.jamMark = m.travelled;  // a new agent (a spawn, a bus's platform change)
  if (m.travelled - v.jamMark >= kUnit) {
    v.jamMark = m.travelled;
    v.jammedFired = false;
    if (v.relief) {
      v.reliefStill = 0;
      if (m.travelled - v.reliefFrom >= kDone) endRelief(s, v, vi, "moves again");
    }
    if (!v.relief) v.jamClock = 0;
  }
  // the dwell at a stop, a breakdown and a crash do not count; neither does standing at the end of its path
  if (v.halted || v.brokenHalted || m.crashing || freeAhead(s, m) <= 1.5f) {
    if (v.relief && (v.halted || v.brokenHalted || m.crashing))
      endRelief(s, v, vi, v.halted ? "reached its stop" : v.brokenHalted ? "stands broken down at its stop" : "crashed");
    if (v.halted || v.brokenHalted || m.crashing) v.jamClock = 0, v.jamMark = m.travelled;
    return false;
  }
  v.jamClock += dt;
  if (!v.relief) {
    if (v.jamClock < kDetect) return false;
    // directly behind a line vehicle whose relief already runs: that one is handled first (its respawn frees the way)
    if (!watchOnly(line) && lineVehicleAhead(s, vi, false) >= 0) return false;
    v.relief = true;
    v.reliefStill = v.reliefNext = 0;
    v.reliefFrom = m.travelled;
    v.reliefCleared = 0;
    ++g_stats.detected;
    Jam jam = jamAround(s, v.mover);
    Vec3 p = headPoint(s, m);
    LOG_INFO("sim: jam %s: %s %d (line %d, stops %d -> %d) has stood %.0f s at (%.0f, %.0f); in its way:%s; %zu vehicles "
             "in the jam within %.0f units (%d road vehicles, %d of them to clear; %d line vehicles) [t=%.0f s, detection "
             "%d]",
             watchOnly(line) ? (line.type == 2 ? "(metro, watched only) detected" : "(relief off) detected") : "relief", kindOf(line.type), vi, v.line, v.stopA, v.stopB, v.jamClock,
             p.x, p.z, describe(s, jam.direct).c_str(), jam.members.size(), kRadius, jam.roadVehicles, jam.clearable,
             jam.lineVehicles, g_stats.time, g_stats.detected);
    shot(s, "jam-detect-");
    if (jam.direct.empty()) {  // nobody named: what the track ahead looks like
      std::string ahead;
      int head = int(m.track.at(m.dist));
      for (int k = head; k < int(m.track.nodes.size()) && k <= head + 6; ++k) {
        int nd = m.track.nodes[size_t(k)];
        const NodeRt& r = s.rt[size_t(nd)];
        char b[256];
        bool enter = canEnter(s, nd, v.mover, true);
        std::snprintf(b, sizeof b, " [%d node %d flags %x occ %d mark %d keep %d enter %d", k, nd, s.graph.nodes[size_t(nd)].flags,
                      r.occupant, int(r.marker), r.keep, int(enter));
        ahead += b;
        if (!enter)
          for (int c : s.graph.nodes[size_t(nd)].crossings) {
            const NodeRt& cr = s.rt[size_t(c)];
            if (cr.occupant < 0 && !(s.graph.nodes[size_t(c)].flags & kPathBlocker)) continue;
            const Mover* om = cr.occupant >= 0 ? &s.movers[size_t(cr.occupant)] : nullptr;
            bool onTrack = om && std::find(om->track.nodes.begin(), om->track.nodes.end(), c) != om->track.nodes.end();
            int idx = onTrack ? int(std::find(om->track.nodes.begin(), om->track.nodes.end(), c) - om->track.nodes.begin()) : -1;
            std::snprintf(b, sizeof b, "; crossing %d flags %x occ %d (%s, at %d of its track, holds [%d, %d))", c,
                          s.graph.nodes[size_t(c)].flags, cr.occupant, om ? (om->live ? "live" : "dead") : "-", idx,
                          om ? om->holdBegin : 0, om ? om->holdEnd : 0);
            ahead += b;
          }
        ahead += "]";
      }
      LOG_INFO("sim: jam relief: %s %d: head at %d, holds [%d, %d), usable to %d, %zu nodes, speed %.2f, free ahead %.1f:%s",
               kindOf(line.type), vi, head, m.holdBegin, m.holdEnd, lastUsable(m), m.track.nodes.size(), m.speed,
               freeAhead(s, m), ahead.c_str());
    }
  }
  if (watchOnly(line)) return false;
  v.reliefStill += dt;
  v.reliefNext -= dt;
  keepAhead(s, v);
  if (v.reliefNext <= 0) {
    v.reliefNext = kTurnEvery;
    clearOne(vm, s, v, vi);
  }
  if (v.mover < 0 || !s.movers[size_t(v.mover)].live) return true;
  if (v.reliefStill >= kRespawn) {
    // a line vehicle in its way that stands in a relief of its own since longer goes first: its respawn frees the way
    // (two line vehicles locking each other: the older one leaves)
    if (int w = lineVehicleAhead(s, vi, true); w >= 0) {
      if (v.reliefStill - dt < kRespawn)
        LOG_INFO("sim: jam relief: %s %d (line %d) waits for %s %d (line %d) ahead of it, in relief for longer [t=%.0f s]",
                 kindOf(line.type), vi, v.line, kindOf(s.lines[size_t(s.vehicles[size_t(w)].line)].type), w,
                 s.vehicles[size_t(w)].line, g_stats.time);
      v.reliefStill = kRespawn;
      return false;
    }
    respawn(vm, s, line, vi);
    return true;
  }
  return false;
}

int jamRespawnStop(SimState& s, VehicleState& v, LineState& line, float dt) {
  (void)s;
  if (v.respawnAt < 0 || line.stops.empty()) return -1;
  v.respawnWait += dt;
  if (v.respawnWait > kPlatformWait) return -1;  // the platform stays taken: any stop, as usual
  int n = int(line.stops.size());
  for (int j = 0; j < n; ++j) {
    int k = (v.respawnAt + j) % n;
    if (line.stops[size_t(k)].reachable && line.stops[size_t(k)].legStart >= 0) return k;
  }
  return -1;
}

void jamRespawned(VM& vm, SimState& s, VehicleState& v, int vi) {
  bool moved = v.respawnAt >= 0;
  float waited = v.respawnWait;
  releaseKept(s, v);
  v.jamClock = v.jamMark = 0;
  v.relief = v.jammedFired = false;
  v.reliefStill = v.reliefNext = v.reliefFrom = 0;
  v.reliefCleared = 0;
  v.respawnAt = -1;
  v.respawnWait = 0;
  if (!moved || v.mover < 0) return;
  const Mover& m = s.movers[size_t(v.mover)];
  Vec3 p = headPoint(s, m);
  LOG_INFO("sim: jam relief: %s %d (line %d) spawns again at stop %d (%.0f, %.0f) after %.1f s, on to stop %d [t=%.0f s]",
           kindOf(s.lines[size_t(v.line)].type), vi, v.line, v.stopA, p.x, p.z, waited, v.stopB, g_stats.time);
  if (v.object) vm.fireEvent(v.object.get(), u"onRegsJamMoved", {Value::vector(p.x, p.y, p.z)});
  shot(s, "jam-respawn-on-", 4);
}

// Test switch REGS_TEST_JAM=<line type>[:hard] (0 bus, 1 tram; default 1): from 15 s into the game a car is parked
// across the way of the vehicle REGS_DEBUG_FOLLOW follows (else the first moving one of that type) on a road node that
// crosses one of the next ten nodes of its path, or after 90 s without such a crossing on its own path. The relief
// must clear it. With
// ":hard" the car is pinned (the relief cannot clear it, like a line vehicle in a deadlock), so the relief turns the
// cars around for 30 s and the vehicle respawns; the pinned car goes once the vehicle has left.
void stepJamTest(VM& vm, SimState& s, float dt) {
  if (int(g_stats.time / 600) != int((g_stats.time + dt) / 600))  // a summary every 10 game minutes
    LOG_INFO("sim: jam summary at t=%.0f s: %d detected, %d resolved after clearing, %d respawned, %d cars turned, %d sent "
             "off, %d ended by themselves (watched only); passengers %d boarded, %d alighted",
             g_stats.time + dt, g_stats.detected, g_stats.resolved, g_stats.respawned, g_stats.turned, g_stats.left,
             g_stats.byItself, s.boarded, s.alighted);
  g_stats.time += dt;
  TestJam& t = g_test;
  if (!t.parsed) {
    t.parsed = true;
    if (const char* e = regs::envVar("REGS_TEST_JAM"); e && *e) {
      t.on = true;
      t.type = std::atoi(e) == 0 && *e == '0' ? 0 : 1;
      t.hard = std::strstr(e, "hard") != nullptr;
      t.save = std::strstr(e, "save") != nullptr;
    }
  }
  if (!t.on) return;
  if (t.placed) {
    // ":save": a save in the middle of the relief (after the third car), userdata/saves/jamsave.gs (main.saveGameData)
    if (t.save && !t.saved && s.vehicles[size_t(t.vehicle)].reliefCleared >= 3) {
      t.saved = true;
      LOG_INFO("sim: test jam: saving as jamsave in the middle of the relief of vehicle %d [t=%.0f s]", t.vehicle, g_stats.time);
      if (Value* main = vm.findGlobal(u"MAIN"))
        vm.call(vm.getMember(vm.getMember(*main, u"$main"), u"saveGameData"), {Value::string(u"jamsave")});
    }
    if (t.blocker >= 0 && g_stats.respawned > 0) {
      if (s.movers[size_t(t.blocker)].live && s.movers[size_t(t.blocker)].testPin) {
        LOG_INFO("sim: test jam: the pinned car %d goes now that vehicle %d has left", t.blocker, t.vehicle);
        despawn(vm, s, t.blocker);
      }
      t.blocker = -1;
      t.sinceRespawn = 0;
      t.replannedAt = s.replanned;
      t.evacuatedAt = s.evacuated;
    }
    if (t.sinceRespawn < 0) return;
    float before = t.sinceRespawn;
    t.sinceRespawn += dt;
    if (int(t.sinceRespawn / 8) > int(before / 8))  // the ticker scrolling by (real time: slower than the game)
      shot(s, "jam-ticker-" + std::to_string(int(t.sinceRespawn / 8)) + "-");
    if (before < 30 && t.sinceRespawn >= 30) {  // what became of the riders
      int riding = 0;
      for (const Citizen& c : s.citizens) riding += c.alive && c.vehicle == t.vehicle;
      LOG_INFO("sim: test jam: 30 s after the respawn: %d citizens planned again since (%d evacuated in all so far), %d "
               "ride vehicle %d now [t=%.0f s]",
               s.replanned - t.replannedAt, s.evacuated, riding, t.vehicle, g_stats.time);
    }
    if (t.sinceRespawn >= 160) t.sinceRespawn = -1;
    return;
  }
  if (g_stats.time < 15 || (t.tries += dt) < 1) return;
  t.tries = 0;
  // the model: a saved single-car road consist, else any single-car road vehicle on the map
  std::vector<Unit> units;
  for (const auto& c : s.roadConsists)
    if (c.size() == 1) {
      units = c;
      break;
    }
  for (size_t id = 0; id < s.movers.size() && units.empty(); ++id)
    if (roadVehicle(s.movers[id]) && s.movers[id].units.size() == 1) units = s.movers[id].units;
  if (units.empty()) return;
  for (const LineState& line : s.lines) {
    if (line.type != t.type || line.deleted) continue;
    for (int vi : line.vehicles) {
      if (s.followed >= 0 && vi != s.followed) continue;
      const VehicleState& v = s.vehicles[size_t(vi)];
      if (v.mover < 0 || v.halted || !s.movers[size_t(v.mover)].live) continue;
      const Mover& m = s.movers[size_t(v.mover)];
      if (m.speed < 1) continue;
      int last = lastUsable(m), target = -1, onPath = -1;
      for (int k = m.holdEnd + 1; k <= last && k <= m.holdEnd + 10 && target < 0; ++k) {
        int n = m.track.nodes[size_t(k)];
        if (s.rt[size_t(n)].occupant >= 0) continue;
        if (onPath < 0 && k >= m.holdEnd + 3 && !(s.graph.nodes[size_t(n)].flags & kPathIntersection)) onPath = n;
        for (int c : s.graph.nodes[size_t(n)].crossings) {
          const LaneNode& cn = s.graph.nodes[size_t(c)];
          if ((cn.flags & kPathClassMask & ~0x600u) == kPathRoad && !(cn.flags & kPathPark) && s.rt[size_t(c)].occupant < 0) {
            target = c;
            break;
          }
        }
      }
      if (target < 0 && g_stats.time > 90) target = onPath;  // no road crossing ahead for over a minute
      if (target < 0) continue;
      int id = parkCar(vm, s, units, target);
      if (id < 0) continue;
      s.movers[size_t(id)].testPin = t.hard;
      t.placed = true;
      t.blocker = id;
      t.vehicle = vi;
      Vec3 p = s.graph.nodes[size_t(target)].p0;
      LOG_INFO("sim: test jam: car %d parked on %s node %d at (%.0f, %.0f) across the way of %s %d (line %d)%s [t=%.0f s]", id,
               target == onPath ? "its path's" : "a crossing road", target, p.x, p.z, kindOf(line.type), vi, v.line,
               t.hard ? ", pinned" : "", g_stats.time);
      shot(s, "jam-placed-");
      return;
    }
  }
}

}  // namespace sim
}  // namespace regs::engine
