// The simulation's vehicles (docs/simulation/grid-traffic.md §6, transport.md): the transport lines of a loaded map
// (docs/save-format/lines-timeline.md §2.1-2.5) and the background traffic (vehicles-movement.md), all driving the lane
// graph as movers that claim lane nodes ahead of them (§6.3).
// Deviations: carriages follow their head along its track instead of holding their own nodes, every vehicle updates
// every sub-step (no far-away round robin), platoons queue on one node instead of the leader's whole chain, a crash
// drifts the whole consist by one offset and leaves no crash marker or emergency vehicle (§6.6 crashSettled).
#include "engine/sim.h"
#include "core/env.h"
#include "engine/night_lights.h"
#include "engine/sign_atlas.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <numeric>
#include <set>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/curve_roads.h"
#include "engine/grid_edit.h"
#include "engine/scene3d.h"
#include "engine/settings.h"
#include "engine/sim_graph.h"
#include "engine/sim_state.h"
#include "engine/sim_sounds.h"
#include "engine/particles.h"
#include "engine/snowplows.h"
#include "engine/traffic_maps.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

bool simIsTrainStation(const std::string& templateName) { return templateName == "station-track" || templateName == "terminal-track"; }
using render::DrawItem;
using render::Mat4;
using render::Vec3;

using namespace sim;

namespace sim {

SimState* simOf(GridObj* g) { return g ? g->sim.get() : nullptr; }

TransportObj* transport(Object* o) { return dynamic_cast<TransportObj*>(o); }

LineState* lineOf(Object* o) {
  auto* t = transport(o);
  auto s = t ? t->sim.lock() : nullptr;
  return s && t->index >= 0 && size_t(t->index) < s->lines.size() ? &s->lines[size_t(t->index)] : nullptr;
}
VehicleState* vehicleOf(Object* o) {
  auto* t = transport(o);
  auto s = t ? t->sim.lock() : nullptr;
  return s && t->index >= 0 && size_t(t->index) < s->vehicles.size() ? &s->vehicles[size_t(t->index)] : nullptr;
}
bool vehicleSpawned(Object* o) {
  auto* t = transport(o);
  auto s = t ? t->sim.lock() : nullptr;
  VehicleState* v = vehicleOf(o);
  return v && s && v->mover >= 0 && s->movers[size_t(v->mover)].live;
}

float fieldFloat(VM& vm, const Object* o, std::u16string_view name, float def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toFloat(it->second);
}
int fieldInt(VM& vm, const Object* o, std::u16string_view name, int def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toInt(it->second);
}

size_t Track::at(float s) const {
  if (nodes.empty()) return 0;
  auto it = std::upper_bound(start.begin(), start.end(), s);
  return it == start.begin() ? 0 : size_t(it - start.begin()) - 1;
}

// MSVC rand(): the engine's random stream
int rnd(SimState& s) {
  s.seed = s.seed * 214013u + 2531011u;
  return int((s.seed >> 16) & 0x7fff);
}
float rndFloat(SimState& s, float a, float b) { return a + (b - a) * (float(rnd(s)) / 32767.0f); }

float densityOf(SimState& s, Object* module, const char16_t* field) {
  // debugging: REGS_POPULATION_DENSITY / REGS_TRAFFIC_DENSITY override the fields
  if (const char* e = regs::envVar(field[4] == u'P' ? "REGS_POPULATION_DENSITY" : "REGS_TRAFFIC_DENSITY"); e && *e)
    return std::clamp(float(std::atof(e)), 0.25f, 8.0f);
  if (!module || !s.vm) return 1.0f;
  auto it = module->fieldValues.find(field);
  float v = it == module->fieldValues.end() || it->second.isNone() ? 1.0f : s.vm->toFloat(it->second);
  return v > 0 ? std::clamp(v, 0.25f, 8.0f) : 1.0f;
}
float populationDensity(SimState& s) { return densityOf(s, s.citizensModule.get(), u"regsPopulationDensity"); }
float trafficDensity(SimState& s) { return densityOf(s, s.vehiclesModule.get(), u"regsTrafficDensity"); }

int lineAddStop(VM& vm, GridObj* grid, SimState& s, int li, int item, bool temp);  // engine/sim_lines.cpp

Vec3 itemCentre(VM& vm, GridObj* grid, int item, int* sxOut, int* szOut) {
  const GridData& g = *grid->data;
  const auto& it = g.items[size_t(item)];
  int w = 1, l = 1, ty = 0, tf = 0;
  if (it.type < g.types.size())
    if (auto t = grid->templates.find(g.types[it.type]); t != grid->templates.end()) {
      w = std::max(1, fieldInt(vm, t->second.get(), u"width", 1));
      l = std::max(1, fieldInt(vm, t->second.get(), u"length", 1));
      ty = fieldInt(vm, t->second.get(), u"type", 0);
      tf = fieldInt(vm, t->second.get(), u"flags", 0);
    }
  int sx = (it.orient & 1) ? l : w, sz = (it.orient & 1) ? w : l;
  if (sxOut) *sxOut = sx;
  if (szOut) *szOut = sz;
  float cs = g.cellSize;
  return {(float(it.x) + float(sx) * 0.5f - float(g.width) * 0.5f) * cs + it.offX, float(itemHeight(g, it, uint32_t(tf), ty)) * cs / 256.0f,
          (float(it.z) + float(sz) * 0.5f - float(g.length) * 0.5f) * cs + it.offZ};
}

int modelFor(VM& vm, GridObj* grid, SimState& s, const std::string& name) {
  auto known = s.modelIndex.find(name);
  if (known != s.modelIndex.end()) return known->second;
  int index = -1;
  auto it = grid->templates.find(name);
  if (auto* o = it == grid->templates.end() ? nullptr : dynamic_cast<ObjectObj*>(it->second.get())) {
    UnitModel m;
    m.object = o;
    m.frontOffset = fieldFloat(vm, o, u"frontOffset", 0);
    m.rearOffset = fieldFloat(vm, o, u"rearOffset", 0);
    m.frontAnchor = fieldFloat(vm, o, u"frontAnchor", 0);
    m.rearAnchor = fieldFloat(vm, o, u"rearAnchor", 0);
    m.acceleration = fieldFloat(vm, o, u"acceleration", 1);
    m.braking = std::max(0.01f, fieldFloat(vm, o, u"braking", 1));
    m.turning = fieldFloat(vm, o, u"turning", 1);
    m.speed = fieldFloat(vm, o, u"speed", 1);
    m.length = std::max(1, fieldInt(vm, o, u"length", 1));
    m.capacity = fieldInt(vm, o, u"capacity", 0);
    if (auto d = o->fieldValues.find(u"displayName"); d != o->fieldValues.end() && !d->second.isNone())
      m.named = !vm.toStr(d->second).empty();
    if (auto c = o->fieldValues.find(u"colorMap"); c != o->fieldValues.end())
      if (auto* cm = dynamic_cast<ColormapObj*>(c->second.object()); cm && !cm->argb.empty()) m.colorMap = cm;
    if (o->data) {
      m.boundsMin = {o->data->mesh.boundsMin[0], o->data->mesh.boundsMin[1], o->data->mesh.boundsMin[2]};
      m.boundsMax = {o->data->mesh.boundsMax[0], o->data->mesh.boundsMax[1], o->data->mesh.boundsMax[2]};
    }
    index = int(s.models.size());
    s.models.push_back(m);
  } else {
    LOG_WARN("vehicle model %s: no template", name.c_str());
  }
  s.modelIndex[name] = index;
  return index;
}

// gsColorMap::getRandomColor (L:0x080ef430): a pixel picked by gsPseudoRandom15(seed)
uint32_t paint(const SimState& s, int model, uint32_t seed) {
  if (model < 0 || !s.models[size_t(model)].colorMap) return 0;
  const auto& px = s.models[size_t(model)].colorMap->argb;
  return px[((seed * 0x687faU + 0x4d3d86U) >> 17) % px.size()];
}

// length of a unit along the track: its body from rear to front (the spawn spacing, transport.md §4.2)
float unitLength(const SimState& s, const Unit& u) {
  if (u.model < 0) return 6;
  const UnitModel& m = s.models[size_t(u.model)];
  return m.frontOffset + m.rearOffset;
}
float trailLength(const SimState& s, const Mover& m) {
  float behind = 0;
  for (size_t k = 1; k < m.units.size(); ++k) behind += unitLength(s, m.units[k]);
  return behind;
}
// lengthNodes of a spawned consist: max(model length, trunc(Σ length / 6 + 0.99)) (0x101089d0, transport.md §4.2)
int lengthNodesOf(const SimState& s, const std::vector<Unit>& units, bool fromLength) {
  float total = 0;
  for (const Unit& u : units) total += unitLength(s, u);
  int L = fromLength ? int(total / 6 + 0.99f) : 0;
  return std::max(units.empty() || units[0].model < 0 ? 1 : s.models[size_t(units[0].model)].length, L);
}

// REGS_DEBUG_SIM=1 logs the stop cycle of the line vehicles and the traffic counts
bool debugSim() {
  static const bool on = [] {
    const char* e = regs::envVar("REGS_DEBUG_SIM");
    return e && *e && *e != '0';
  }();
  return on;
}

// ---- save reading ----

struct Reader {
  const std::vector<uint8_t>& d;
  uint32_t u32(size_t at) const {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  }
  float f32(size_t at) const {
    float v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  }
  uint16_t u16(size_t at) const {
    uint16_t v = 0;
    if (at + 2 <= d.size()) std::memcpy(&v, d.data() + at, 2);
    return v;
  }
  template <typename F>
  void children(size_t from, size_t end, F fn) const {
    end = std::min(end, d.size());
    for (size_t at = from; at + 8 <= end;) {
      uint32_t tag = u32(at), size = u32(at + 4);
      size_t body = at + 8, stop = std::min(end, body + size);
      fn(tag, body, stop);
      at = body + size;
    }
  }
  std::vector<std::string> names(size_t from, size_t end, uint32_t count) const {
    std::vector<std::string> out;
    size_t p = from;
    for (uint32_t i = 0; i < count && p < end; ++i) {
      std::string s;
      while (p < end && d[p]) s += char(d[p++]);
      ++p;
      out.push_back(s);
    }
    return out;
  }
};

// a 0x07040200 vehicle agent (vehicles-movement.md §2.3)
struct SavedAgent {
  int model = -1;
  int lengthNodes = 1;
  uint16_t colorSeed = 0;
  float posOnNode = 0, speed = 0;
  uint32_t flags = 0, curNode = 0, prevNode = 0, nextCar = 0, prevCar = 0;
  render::Vec3 crashOffset{};
  std::vector<LaneNode> inlineNodes;  // 0x07030200 children: the record's flight nodes, refs 0x80000000 | i
  int stepsLeft = -1;
  std::vector<uint32_t> ring;
  // the movement path (0x07030100): packed step codes and the read cursor (vehicles-movement.md §2.5)
  int nibbles = 0;
  uint32_t cursor = 0;
  std::vector<uint32_t> steps;
};

// FUN_100e1810: the nodes of a saved path from its cursor, starting after node n; false when a code names no link
bool decodePath(const LaneGraph& G, const SavedAgent& a, int n, std::vector<int>& out) {
  uint32_t cursor = a.cursor;
  for (int k = 0; k < a.stepsLeft && int(cursor >> 3) < a.nibbles; ++k) {
    uint32_t word = (cursor >> 3) / 8 < a.steps.size() ? a.steps[(cursor >> 3) / 8] : 0;
    uint32_t code = (word >> (4 * ((cursor >> 3) & 7))) & 0xF, prog = cursor & 7;
    size_t link = code < 8 ? code + 1 : 0;
    cursor += code < 8 ? 8 : prog == 0 ? 16 - code : 1;
    const LaneNode& nd = G.nodes[size_t(n)];
    if (link >= nd.next.size()) return false;
    n = nd.next[link];
    out.push_back(n);
  }
  return true;
}

// ---- claims (grid-traffic.md §6.3) ----

bool isMarked(const SimState& s, int n) {
  if (s.rt[size_t(n)].marker) return true;
  for (int c : s.graph.nodes[size_t(n)].crossings)
    if (s.rt[size_t(c)].marker) return true;
  return false;
}

// FUN_100e14b0: free, not marked (junction nodes excepted), not a blocker, and no crossing node held by another
// port: trams on railway tracks: how often a tram or a train had to wait for the other on shared track (REGS_DEBUG_SIM)
int g_railShareWaits = 0;
bool canEnter(const SimState& s, int n, int id, bool ignoreMarks) {
  const NodeRt& r = s.rt[size_t(n)];
  const LaneNode& nd = s.graph.nodes[size_t(n)];
  if (r.occupant >= 0 && r.occupant != id) return false;
  if (r.keep >= 0 && keptFor(s, r, id)) return false;  // port: kept for a line vehicle in jam relief (sim_jams.cpp)
  if (r.marker && !(nd.flags & kPathLaneChange) && !ignoreMarks) return false;
  if (nd.flags & kPathBlocker) return false;
  for (int c : nd.crossings) {
    const NodeRt& cr = s.rt[size_t(c)];
    if ((cr.occupant >= 0 && cr.occupant != id) || (cr.marker && !ignoreMarks) || (s.graph.nodes[size_t(c)].flags & kPathBlocker)) {
      if (cr.occupant >= 0 && (nd.railTwin || s.graph.nodes[size_t(c)].railTwin)) ++g_railShareWaits;
      return false;
    }
  }
  return true;
}

// FUN_10107560: the queued reserver becomes the occupant
void releaseNode(SimState& s, int id, int n) {
  NodeRt& r = s.rt[size_t(n)];
  if (r.reserver == id) r.reserver = -1;
  if (r.occupant == id) {
    r.occupant = r.reserver;
    r.reserver = -1;
    if (r.occupant >= 0 && s.movers[size_t(r.occupant)].reserved == n) s.movers[size_t(r.occupant)].reserved = -1;
  }
  if (s.movers[size_t(id)].transit || (s.graph.nodes[size_t(n)].flags & kPathLaneChange)) r.marker = false;
  if (s.graph.nodes[size_t(n)].flags & 0x2000000u) freeInlineNode(s, n);  // a flight node has no other user
}

int lastUsable(const Mover& m) {
  int last = int(m.track.nodes.size()) - 1;
  return m.limit >= 0 ? std::min(m.limit, last) : last;
}

// claimNodes (0x1010de50.c:99-261): claims reach `look` nodes past the head's node; claims into intersection nodes
// only become real once L ordinary nodes behind them can be claimed too, so nobody stops inside a junction
void claimAhead(SimState& s, int id) {
  Mover& m = s.movers[size_t(id)];
  const UnitModel& hm = s.models[size_t(m.units[0].model)];
  int head = int(m.track.at(m.dist)), last = lastUsable(m);
  if (m.holdEnd <= head) m.holdEnd = head + 1;
  int look = int(m.speed / (6 * hm.braking) + 0.5f) + 1 + m.lengthNodes;
  bool ignore = m.transit;
  for (int k = head; k < m.holdEnd && !ignore; ++k) ignore = isMarked(s, m.track.nodes[size_t(k)]);
  // straightRun: held nodes since the last junction node; the loop runs while the run or the safe claims (counted
  // as queue entries past the head's node) are shorter than the look-ahead, so it always looks through a junction
  int run = 0;
  for (int k = m.holdEnd - 1; k > head && !(s.graph.nodes[size_t(m.track.nodes[size_t(k)])].flags & kPathIntersection); --k) ++run;
  int k = m.holdEnd, safe = m.holdEnd;
  while (k <= last && std::min(run, safe - head - 1) < look) {
    int n = m.track.nodes[size_t(k)];
    NodeRt& r = s.rt[size_t(n)];
    if (r.occupant != id && !canEnter(s, n, id, ignore)) {
      if (m.transit || (s.graph.nodes[size_t(n)].flags & kPathLaneChange)) r.marker = true;  // waiting marker
      // queue behind the leader (claimNodes' platoon, grid-traffic.md §6.3): only on a node the original would accept
      // and commit: not marked (unless marks are ignored), not a junction node and right after the committed range
      // (the original commits a platoon only up to `safe`). Queuing anyway handed a junction's crossing nodes from one
      // car to the next past a waiting tram's marker (transport.md §10.3).
      bool platoon = (ignore || !isMarked(s, n)) && !(s.graph.nodes[size_t(n)].flags & kPathIntersection) && k == safe;
      if (platoon && r.occupant >= 0 && r.occupant != id && r.reserver < 0 && m.reserved < 0) {
        r.reserver = id;
        m.reserved = n;
      }
      break;
    }
    ++k;
    if (s.graph.nodes[size_t(n)].flags & kPathIntersection) run = 0;
    else if (++run >= m.lengthNodes) safe = k;
  }
  int commit = k > last ? k : safe;  // the end of the known path commits everything
  for (int j = m.holdEnd; j < commit; ++j) {
    NodeRt& r = s.rt[size_t(m.track.nodes[size_t(j)])];
    r.occupant = id;
    if (r.reserver == id) r.reserver = -1;
    if (m.reserved == m.track.nodes[size_t(j)]) m.reserved = -1;
  }
  m.holdEnd = std::max(m.holdEnd, commit);
}

void releaseAll(SimState& s, int id) {
  Mover& m = s.movers[size_t(id)];
  for (int j = m.holdBegin; j < m.holdEnd && size_t(j) < m.track.nodes.size(); ++j) releaseNode(s, id, m.track.nodes[size_t(j)]);
  if (m.reserved >= 0) releaseNode(s, id, m.reserved);
  // port fix: a node handed to it as the queued reserver (releaseNode) can lie past the held range, when it queued for
  // a node behind junction nodes it could enter but had not committed yet (claimAhead); such a node was never released
  // and stayed taken by a despawned mover for good, the cause of most lasting tram and bus jams (transport.md §10.5)
  for (size_t j = size_t(std::max(m.holdEnd, 0)); j < m.track.nodes.size(); ++j) {
    int n = m.track.nodes[j];
    if (!(s.graph.nodes[size_t(n)].flags & 0x2000000u) && (s.rt[size_t(n)].occupant == id || s.rt[size_t(n)].reserver == id))
      releaseNode(s, id, n);
  }
  for (size_t j = size_t(std::max(m.holdBegin, m.holdEnd)); j < m.track.nodes.size(); ++j)  // flight nodes not reached
    if (s.graph.nodes[size_t(m.track.nodes[j])].flags & 0x2000000u) freeInlineNode(s, m.track.nodes[j]);
  m.reserved = -1;
  m.holdBegin = m.holdEnd = 0;
}

// claims the nodes under a consist placed from a save or at a spawn
void claimBody(SimState& s, int id) {
  Mover& m = s.movers[size_t(id)];
  const UnitModel& hm = s.models[size_t(m.units[0].model)];
  int tail = int(m.track.at(std::max(0.0f, m.dist - trailLength(s, m)))), head = int(m.track.at(m.dist + hm.rearOffset + hm.frontOffset));
  m.holdBegin = tail;
  m.holdEnd = head + 1;
  for (int j = tail; j <= head; ++j) {
    NodeRt& r = s.rt[size_t(m.track.nodes[size_t(j)])];
    if (r.occupant < 0) r.occupant = id;
  }
}

// ---- driving ----

float nodeCap(const LaneNode& n, const UnitModel& m) {
  float t = m.turning * n.curveFactor;
  return t < n.speedLimit ? std::max(t, 1.5f) : n.speedLimit;
}

// FUN_101075d0: the allowed speed rises linearly with the free distance to the end of the held nodes
float targetSpeed(const SimState& s, const Mover& v, const UnitModel& m) {
  const LaneGraph& G = s.graph;
  const Track& tr = v.track;
  size_t i = tr.at(v.dist);
  const LaneNode& cur = G.nodes[size_t(tr.nodes[i])];
  float d = tr.start[i] + cur.length - v.dist - m.rearOffset - m.frontOffset - 1.0f;
  float vmax = nodeCap(cur, m);
  float reach = 10.0f / m.braking;
  int end = std::min(v.holdEnd - 1, lastUsable(v));
  if (v.reckless) end = std::min(int(i) + int(v.speed / (6 * m.braking) + 0.5f) + 1 + v.lengthNodes, lastUsable(v));
  for (size_t k = i + 1; int(k) <= end; ++k) {
    if (d >= reach) return vmax * m.speed;
    const LaneNode& nd = G.nodes[size_t(tr.nodes[k])];
    vmax = std::min(vmax, nodeCap(nd, m) + std::max(d * m.braking, 0.0f));
    d += nd.length;
  }
  if (d >= reach) return vmax * m.speed;
  float b = d * m.braking;
  return (vmax <= b ? vmax : std::max(b, 0.0f)) * m.speed;
}

// drops released nodes far behind the last unit
void trimTrack(Mover& m) {
  int drop = m.holdBegin - 2;
  if (drop < 32) return;
  float shift = m.track.start[size_t(drop)];
  m.track.nodes.erase(m.track.nodes.begin(), m.track.nodes.begin() + drop);
  m.track.start.erase(m.track.start.begin(), m.track.start.begin() + drop);
  for (float& f : m.track.start) f -= shift;
  m.dist -= shift;
  m.holdBegin -= drop;
  m.holdEnd -= drop;
  if (m.limit >= 0) m.limit -= drop;
}

// the free distance of the head's front to the end of its usable track
float freeAhead(const SimState& s, const Mover& m) {
  int last = lastUsable(m);
  if (last < 0) return 0;
  const UnitModel& hm = s.models[size_t(m.units[0].model)];
  float end = m.track.start[size_t(last)] + s.graph.nodes[size_t(m.track.nodes[size_t(last)])].length;
  return end - (m.dist + hm.rearOffset + hm.frontOffset + 1.0f);
}

// startCrash (FUN_1010a260): the whole consist drifts off in a random direction
void startCrash(SimState& s, int id, float dt) {
  if (id < 0) return;
  Mover& m = s.movers[size_t(id)];
  if (!m.live || m.parked || m.crashing) return;
  m.crashing = true;
  m.crashOffset = render::Vec3{rndFloat(s, -1, 1), 0, rndFloat(s, -1, 1)} * (dt * m.speed * 0.4f);
}

Object* vehicleController(VM& vm, SimState& s, int id);

// crash (0x1010de50.c:322-338): the reckless car, the next node's occupant and those of its crossings
void crash(SimState& s, int id, int nd, float dt) {
  ++s.crashes;
  if (debugSim()) {
    Vec3 p = s.graph.nodes[size_t(nd)].p0, e = s.eye;
    LOG_INFO("sim: crash of mover %d at node %d (%.0f, %.0f; %.0f from the camera)", id, nd, p.x, p.z,
             std::sqrt((p.x - e.x) * (p.x - e.x) + (p.z - e.z) * (p.z - e.z)));
  }
  if (s.vehiclesModule)
    if (Object* ctrl = vehicleController(*s.vm, s, id)) s.vm->fireEvent(s.vehiclesModule.get(), u"onCrashBegin", {Value::heap(VT::Object, ctrl)});
  startCrash(s, id, dt);
  startCrash(s, s.rt[size_t(nd)].occupant, dt);
  for (int c : s.graph.nodes[size_t(nd)].crossings) startCrash(s, s.rt[size_t(c)].occupant, dt);
}

// the mods' road speed (SimState::roadSpeed, engine/snowplows.h) for a mover's head node: road and street-tram lanes
// only (not in tunnels: metro, trains, boats and aircraft keep their speed), the cleared factor where a snowplow
// drove lately (TrafficMaps::lanePlow above 0.5)
float roadWeatherFactor(const SimState& s, const Mover& m) {
  if (s.roadSpeed >= 0.999f && s.roadSpeedCleared >= 0.999f) return 1.0f;
  if (m.boat || m.aircraft || m.train || m.track.nodes.empty()) return 1.0f;
  int node = m.track.nodes[m.track.at(m.dist)];
  uint32_t f = s.graph.nodes[size_t(node)].flags;
  if (!(f & (kPathRoad | kPathTram)) || (f & (kPathTrain | kPathMetro | 0x800000u))) return 1.0f;
  bool cleared = s.trafficMaps && size_t(node) < s.trafficMaps->lanePlow.size() && s.trafficMaps->lanePlow[size_t(node)] > 0.5f;
  return std::clamp(cleared ? s.roadSpeedCleared : s.roadSpeed, 0.3f, 1.0f);
}

// one update of a mover (grid-traffic.md §6.2): claims, speed, distance, release behind the last car
void moveMover(SimState& s, int id, float dt) {
  Mover& m = s.movers[size_t(id)];
  if (!m.live || m.parked || m.track.nodes.empty()) return;
  const UnitModel& hm = s.models[size_t(m.units[0].model)];
  if (!m.crashing) claimAhead(s, id);
  float target = targetSpeed(s, m, hm);
  float behind = 0;
  for (size_t k = 1; k < m.units.size(); ++k) {  // every car's own node caps the consist (§6.2 step 3)
    const Unit& u = m.units[k];
    behind += unitLength(s, u);
    if (u.model < 0) continue;
    size_t at = m.track.at(std::max(0.0f, m.dist - behind));
    const UnitModel& um = s.models[size_t(u.model)];
    target = std::min(target, nodeCap(s.graph.nodes[size_t(m.track.nodes[at])], um) * um.speed);
  }
  target *= roadWeatherFactor(s, m);  // the mods' road speed (snow, heavy rain; full on plowed lanes)
  if (!m.crashing) {
    float cand = m.speed + hm.acceleration * dt;
    m.speed = cand <= target ? cand : std::max(target, 0.0f);
  } else {  // 0x1010de50.c:286-301
    float cand = m.speed - 4 * dt;
    m.speed = cand < target ? std::max(cand, 0.0f) : target;
    float l = std::sqrt(dot(m.crashOffset, m.crashOffset));
    if (l > 1e-6f) m.crashOffset = m.crashOffset + m.crashOffset * (m.speed * dt * 0.4f / l);
  }
  // speed is in cells per second (196608 = 3·65536); the front never passes the end of the held nodes
  int end = std::min(m.holdEnd - 1, lastUsable(m));
  float limitDist = m.track.start[size_t(end)] + s.graph.nodes[size_t(m.track.nodes[size_t(end)])].length - hm.rearOffset - hm.frontOffset;
  float before = m.dist;
  m.dist = std::min(m.dist + 3.0f * m.speed * dt, std::max(m.dist, limitDist));
  m.travelled += m.dist - before;
  if (m.dist <= before + 1e-5f) {
    m.waitTime += dt;
    // a reckless head at the end of what it holds, the next node taken: a crash (0x1010de50.c:314-326)
    if (m.reckless && !m.crashing && m.speed > 0 && end < lastUsable(m) && !canEnter(s, m.track.nodes[size_t(end) + 1], id, true))
      crash(s, id, m.track.nodes[size_t(end) + 1], dt);
    if (m.dist >= limitDist) m.speed = std::min(m.speed, 0.0f);
  } else {
    m.waitTime = 0;
  }
  int tail = int(m.track.at(std::max(0.0f, m.dist - behind)));
  for (; m.holdBegin < tail; ++m.holdBegin) releaseNode(s, id, m.track.nodes[size_t(m.holdBegin)]);
  trimTrack(m);
}

int newMover(SimState& s) {
  if (!s.freeMovers.empty()) {
    int id = s.freeMovers.back();
    s.freeMovers.pop_back();
    s.movers[size_t(id)] = Mover();
    return id;
  }
  s.movers.emplace_back();
  return int(s.movers.size()) - 1;
}

// the gsGridVehicleController of a mover, made on demand (FUN_10107150); a line vehicle answers with its gsTransportVehicle
Object* vehicleController(VM& vm, SimState& s, int id) {
  Mover& m = s.movers[size_t(id)];
  if (m.transit) return m.vehicle >= 0 ? s.vehicles[size_t(m.vehicle)].object.get() : nullptr;
  if (m.controller || m.units.empty() || m.units[0].model < 0) return m.controller.get();
  ClassInfo* c = vm.findClass("gsGridVehicleController");
  if (!c) return nullptr;
  m.controller = vm.newObject(c, s.grid);
  Object* o = m.controller.get();
  o->fieldValues[u"<model>"] = Value::heap(VT::Object, s.models[size_t(m.units[0].model)].object);
  o->fieldValues[u"<instance>"] = Value::boolean(true);
  o->fieldValues[u"isVehicle"] = Value::boolean(true);
  o->fieldValues[u"<id>"] = Value::integer(m.colorSeed);
  o->fieldValues[u"<mover>"] = Value::integer(id);
  if (!m.track.nodes.empty()) setPoseFields(o, unitPose(s, m, m.units[0], m.dist));
  return o;
}

// traffic vehicles whose model has a display name get theirs up front (what hover and selection pick)
void attachController(VM& vm, SimState& s, int id) {
  Mover& m = s.movers[size_t(id)];
  if (m.units.empty() || m.units[0].model < 0 || !s.models[size_t(m.units[0].model)].named) return;
  vehicleController(vm, s, id);
}

void despawn(VM& vm, SimState& s, int id) {
  Mover& m = s.movers[size_t(id)];
  if (!m.live) return;
  if (m.crashing && m.controller && s.vehiclesModule)  // the agent destructor (0x10107dd0.c:18-21)
    vm.fireEvent(s.vehiclesModule.get(), u"onCrashEnd", {Value::heap(VT::Object, m.controller.get())});
  releaseAll(s, id);
  m.live = false;
  if (m.train) --s.trainsLive;
  else if (m.boat) --s.boatsLive, m.boat = false;
  else if (m.aircraft) --s.aircraftLive, m.aircraft = false;
  else if (m.plow) --s.plowsLive, m.plow = false;
  else if (!m.transit && !m.parked) --s.roadLive;
  if (m.extra) --s.extraLive, m.extra = false;
  if (Object* o = m.controller.get()) {
    o->fieldValues[u"<instance>"] = Value::boolean(false);
    if (!o->destroyed) vm.destroy(o);
  }
  m.controller = Ref<Object>();
  ++s.despawned;
  if (!m.transit) s.freeMovers.push_back(id);
}

// a traffic consist on a fresh path from node `from` (FUN_10108b30 + the path handed to it)
int spawnTraffic(VM& vm, SimState& s, const std::vector<Unit>& units, int from, const std::vector<int>& path, bool train) {
  int id = newMover(s);
  Mover& m = s.movers[size_t(id)];
  m.live = true;
  m.train = train;
  m.units = units;
  m.colorSeed = uint16_t(rnd(s) & 0xffff);
  for (Unit& u : m.units) u.seed = uint16_t(rnd(s) & 0xffff), u.tint = paint(s, u.model, u.seed);
  m.lengthNodes = lengthNodesOf(s, m.units, true);
  m.track.push(s.graph, from);
  for (int n : path) m.track.push(s.graph, n);
  m.dist = 0;
  claimBody(s, id);
  attachController(vm, s, id);
  if (train) ++s.trainsLive;
  else ++s.roadLive;
  ++s.spawned;
  return id;
}

std::vector<Unit> consistOf(VM& vm, SimState& s, const Value& list);

// FUN_1010ce70 (boats-aircraft.md §4.3): a free water START lane, an END lane at least 200 units away, a lane search
// and a random consist of setBoats; a ship taller than one cell keeps away from low bridges (any other node between
// −8.5 and 10.5 high within ±3 cells of its route)
void spawnBoat(VM& vm, SimState& s) {
  if (!s.vehiclesModule) return;
  auto lists = s.vehiclesModule->fieldValues.find(u"<boats>");
  auto* arr = lists == s.vehiclesModule->fieldValues.end() ? nullptr : lists->second.array();
  if (!arr || arr->items.empty() || s.waterStarts.empty() || s.waterEnds.empty()) return;
  const LaneGraph& G = s.graph;
  int from = s.waterStarts[size_t(rnd(s)) % s.waterStarts.size()];
  if (!canEnter(s, from, -2, false)) return;
  int to = s.waterEnds[size_t(rnd(s)) % s.waterEnds.size()];
  Vec3 gap = G.nodes[size_t(from)].p0 - G.nodes[size_t(to)].p1;
  if (dot(gap, gap) < 40000.0f) return;
  std::vector<int> path;
  float cost = 0;
  if (!G.search(from, to, true, path, cost) || path.empty()) return;
  std::vector<Unit> units = consistOf(vm, s, arr->items[size_t(rnd(s)) % arr->items.size()]);
  if (units.empty()) return;
  if (Object* o = s.models[size_t(units[0].model)].object; o && fieldInt(vm, o, u"height", 1) > 1) {
    std::vector<int> near;
    auto low = [&](int n) {
      G.query(G.nodes[size_t(n)].cellX, G.nodes[size_t(n)].cellZ, 0, 0, 0, near);
      for (int m : near) {
        const LaneNode& nd = G.nodes[size_t(m)];
        if ((nd.flags & 0x1000f900) != kPathWater && nd.p0.y > -8.5f && nd.p0.y < 10.5f) return true;
      }
      return false;
    };
    if (low(from)) return;
    for (int n : path)
      if (low(n)) return;
  }
  if (s.rt[size_t(from)].occupant >= 0) return;
  int id = spawnTraffic(vm, s, units, from, path, false);
  Mover& m = s.movers[size_t(id)];
  m.boat = true;
  --s.roadLive, ++s.boatsLive;
}

int parkCar(VM& vm, SimState& s, const std::vector<Unit>& units, int node) {
  if (s.rt[size_t(node)].occupant >= 0) return -1;
  int id = newMover(s);
  Mover& m = s.movers[size_t(id)];
  m.live = true;
  m.parked = true;
  m.units = units;
  m.lengthNodes = 1;
  m.track.push(s.graph, node);
  const UnitModel& hm = s.models[size_t(units[0].model)];
  m.dist = std::max(0.0f, (s.graph.nodes[size_t(node)].length - hm.rearOffset - hm.frontOffset) * 0.5f);
  claimBody(s, id);
  (void)vm;
  return id;
}

// ---- traffic sources ----

// stand-in for the citizens' car trips (citizens.md §5.3, §7): a road START node to a road END node chosen with the
// destination falloff (rand() & 0xfff) ≤ 2²⁹ / (dx² + dz² + 2¹⁷), up to 32 tries
int spawnRoadTrip(VM& vm, SimState& s) {
  if (s.roadStarts.empty() || s.roadEnds.empty() || s.roadConsists.empty()) return -1;
  int from = s.roadStarts[size_t(rnd(s)) % s.roadStarts.size()];
  if (!canEnter(s, from, -2, false)) return -1;
  const LaneNode& a = s.graph.nodes[size_t(from)];
  int to = -1;
  for (int t = 0; t < 32 && to < 0; ++t) {
    int e = s.roadEnds[size_t(rnd(s)) % s.roadEnds.size()];
    float dx = (s.graph.nodes[size_t(e)].p0.x - a.p0.x) / s.graph.cellSize, dz = (s.graph.nodes[size_t(e)].p0.z - a.p0.z) / s.graph.cellSize;
    if (float(rnd(s) & 0xfff) <= 536870912.0f / (dx * dx + dz * dz + 131072.0f)) to = e;
  }
  std::vector<int> path;
  float cost = 0;
  if (to < 0 || to == from || !s.graph.search(from, to, false, path, cost) || path.empty()) return -1;
  return spawnTraffic(vm, s, s.roadConsists[size_t(rnd(s)) % s.roadConsists.size()], from, path, false);
}

// a train consist from setPassengerTrains / setCargoTrains: names, a leading '!' turns the car around
std::vector<Unit> consistOf(VM& vm, SimState& s, const Value& list) {
  std::vector<Unit> units;
  auto* arr = list.array();
  if (!arr) return units;
  for (const Value& v : arr->items) {
    std::string name = utf16ToUtf8(vm.toStr(v));
    Unit u;
    u.reversed = !name.empty() && name[0] == '!';
    u.model = modelFor(vm, s.grid, s, u.reversed ? name.substr(1) : name);
    if (u.model >= 0) units.push_back(u);
  }
  return units;
}

// FUN_1010ae20: passenger (rand() even) or cargo, a free entry node and an exit node of that kind, a random consist
void spawnTrain(VM& vm, SimState& s) {
  if (!s.vehiclesModule) return;
  int mode = rnd(s) & 1 ? 0 : 1;
  auto lists = s.vehiclesModule->fieldValues.find(mode ? u"<passengerTrains>" : u"<cargoTrains>");
  auto* arr = lists == s.vehiclesModule->fieldValues.end() ? nullptr : lists->second.array();
  if (!arr || arr->items.empty() || s.trainStarts[mode].empty() || s.trainEnds[mode].empty()) return;
  int from = s.trainStarts[mode][size_t(rnd(s)) % s.trainStarts[mode].size()];
  int to = s.trainEnds[mode][size_t(rnd(s)) % s.trainEnds[mode].size()];
  if (!canEnter(s, from, -2, true)) return;
  std::vector<int> path;
  float cost = 0;
  if (!s.graph.search(from, to, true, path, cost) || path.empty()) return;
  std::vector<Unit> units = consistOf(vm, s, arr->items[size_t(rnd(s)) % arr->items.size()]);
  if (!units.empty()) spawnTraffic(vm, s, units, from, path, true);
}

// the traffic of one sub-step: movers, end of path and jams, sources
void stepTraffic(VM& vm, SimState& s, float dt) {
  for (size_t id = 0; id < s.movers.size(); ++id) {
    Mover& m = s.movers[id];
    if (!m.live || m.transit || m.parked) continue;
    moveMover(s, int(id), dt);
    if (m.aircraft) {
      if (aircraftDone(s, m)) {
        despawn(vm, s, int(id));
        continue;
      }
      // 0x1010f1e0.c:217-266: the gear retracts between 20 and 30 units of altitude (doorOpenStart2/Len2), the doors
      // open 2..3 s after the plane stopped at the end of its route (doorOpenStart/Len)
      Object* o = s.models[size_t(m.units[0].model)].object;
      float y = unitPose(s, m, m.units[0], m.dist).position.y;
      int left = int(m.track.nodes.size()) - 1 - int(m.track.at(m.dist));
      m.animTime = -1;
      if (o && (left > 4 || m.speed >= 0.1f)) {
        float gs = fieldFloat(vm, o, u"doorOpenStart2", -1), gl = fieldFloat(vm, o, u"doorOpenLen2", 0);
        if (y > 20 && gs >= 0) m.animTime = gs + std::min((y - 20) * 0.1f, 1.0f) * gl;
      } else if (o && m.waitTime > 2) {
        m.speed = 0;
        float ds = fieldFloat(vm, o, u"doorOpenStart", -1), dl = fieldFloat(vm, o, u"doorOpenLen", 0);
        if (ds >= 0) m.animTime = ds + std::clamp(m.waitTime - 2, 0.0f, 1.0f) * dl;
      }
      continue;
    }
    if (m.boat) {
      int left = int(m.track.nodes.size()) - 1 - int(m.track.at(m.dist));
      if (left <= m.lengthNodes || m.waitTime > 60) despawn(vm, s, int(id));
      continue;
    }
    // arrival despawns (citizens.md §7.3); a road car stuck for 30 s is given up, a train after 60 s (§6.1, §6.5)
    if ((freeAhead(s, m) < 1.5f && m.speed < 0.1f) || m.waitTime > (m.train ? 60.0f : 30.0f)) {
      bool stuck = m.waitTime > 30;
      if (stuck) ++s.stuck;
      if (m.citizen >= 0) carArrived(vm, s, int(id), stuck);
      else despawn(vm, s, int(id));
    }
  }
  int trainTarget = int(float(int(s.trainStarts[0].size() + s.trainStarts[1].size()) >> 2) * trafficDensity(s) + 0.5f);
  if (s.trainsLive < trainTarget) spawnTrain(vm, s);
  if (s.boatsLive < int(std::sqrt(float(s.waterStarts.size())))) spawnBoat(vm, s);
  stepAircraft(vm, s);
  float d = trafficDensity(s);
  if (!s.citizensLoaded) {  // no home slots, no citizens: the saved traffic, scaled, kept up by stand-in trips
    if (s.roadLive < int(float(s.roadTarget) * d)) spawnRoadTrip(vm, s);
    return;
  }
  // with citizens: below 1x fewer citizens own a car (pickCar); above 1x background trips add (d − 1) × the citizens'
  // running number of moving cars
  int cars = s.roadLive - s.extraLive;
  s.citizenCarsAvg = s.citizenCarsAvg < 0 ? float(cars) : s.citizenCarsAvg + (float(cars) - s.citizenCarsAvg) * std::min(1.0f, dt / 60.0f);
  int target = d > 1 ? int((d - 1) * s.citizenCarsAvg + 0.5f) : 0;
  for (int tries = 0; tries < 4 && s.extraLive < target; ++tries) {
    int id = spawnRoadTrip(vm, s);
    if (id >= 0) s.movers[size_t(id)].extra = true, ++s.extraLive;
  }
}

// ---- line vehicles ----

// The half train length past the platform the track (ending on stop k's leg end) halts at, so that the middle stands
// at the stop (transport.md §4.3): lengthNodes >> 1 nodes of the next leg when it goes on from that platform (counted
// in v.extraNodes, which the next appendLeg skips). Called after every leg, also the first leg after a turn
// (reverseConsist), which used to go without it. Where the next leg turns round on the same track (it starts on the
// opposite of the platform node or of a node or two past it: the metro's end stations) the train drives straight on
// along its own track for the half length (v.turnNodes) and the turned leg starts back over their opposites
// (2026-11-15; the first try of 2026-11-05 locked two shuttling trains head-on, before the jam relief and with the
// leg's start a node past the platform not recognised). Checked on Vienna with REGS_TEST_TERMINUS=3 (8000 frames at
// speed 3): every metro halt's middle within 1 unit of the platform node, 57 turns, no jams. The original pulls the
// nodes from the pre-loaded next leg there (transport.md §9 question 2, not traced).
int oppositeNode(const LaneGraph& G, int n);
void appendHalfLength(SimState& s, VehicleState& v, const LineState& line, int k) {
  const LaneGraph& G = s.graph;
  Mover& m = s.movers[size_t(v.mover)];
  const LineStopState& st = line.stops[size_t(k)];
  const LineStopState& nx = line.stops[size_t((k + 1) % int(line.stops.size()))];
  v.extraNodes = 0;
  if (v.units.empty() || st.legEnd < 0 || m.track.nodes.empty() || m.track.nodes.back() != st.legEnd) return;
  int extra = lengthNodesOf(s, v.units, true) >> 1, before = int(m.track.nodes.size()) - 1;
  v.turnNodes.clear();
  if (nx.legStart == st.legEnd) {
    for (int i = 0; i < extra && size_t(i) < nx.path.size(); ++i) m.track.push(G, nx.path[size_t(i)]);
    v.extraNodes = std::min(extra, int(nx.path.size()));
    m.limit += v.extraNodes;
  } else if (line.type != 3 && line.type != 4 && nx.legStart >= 0) {
    // a turn on the same track (2026-11-15, the metro's end stations): the next leg starts on the opposite of the
    // platform node or of a node or two straight on past it. The train drives straight on along its own track for
    // half its length (every node with its opposite, so the turned leg can run back over them) and turns from there.
    auto straightOn = [&](int cur) {
      const LaneNode& c = G.nodes[size_t(cur)];
      int best = -1;
      float bestDot = 0.9f;
      for (int n : c.next) {
        const LaneNode& nn = G.nodes[size_t(n)];
        if ((nn.flags & kPathClassMask) != (c.flags & kPathClassMask) || (nn.flags & kPathDeleted)) continue;
        float d = dot(c.f1, nn.f0);
        if (d > bestDot && oppositeNode(G, n) >= 0) bestDot = d, best = n;
      }
      return best;
    };
    auto opposes = [&](int a, int b) {  // b runs over a the other way (coincident lanes count)
      const LaneNode &x = G.nodes[size_t(a)], &y = G.nodes[size_t(b)];
      Vec3 d0 = x.p0 - y.p1, d1 = x.p1 - y.p0;
      return dot(d0, d0) < 0.04f && dot(d1, d1) < 0.04f;
    };
    int skip = -1;  // how many nodes past the platform the next leg's start lies (0: the platform's opposite)
    for (int k = 0, cur = st.legEnd; k <= 3 && cur >= 0; ++k, cur = straightOn(cur))
      if (opposes(cur, nx.legStart)) {
        skip = k;
        break;
      }
    if (skip >= 0) {
      int cur = st.legEnd;
      for (int i = 0; i < std::max(extra, skip); ++i) {
        int nn = straightOn(cur);
        if (nn < 0) break;
        m.track.push(G, nn);
        v.turnNodes.push_back(nn);
        cur = nn;
      }
      m.limit += int(v.turnNodes.size());
      v.turnSkip = skip;
    }
  }
  if (static const bool dbg = regs::envVar("REGS_DEBUG_HALT") != nullptr; dbg)
    LOG_INFO("sim: halt length: line type %d, stop %d: %d of %d nodes past the platform (%s)", line.type,
             (k + 1) % int(line.stops.size()), int(m.track.nodes.size()) - 1 - before, extra,
             nx.legStart == st.legEnd ? "next leg" : !v.turnNodes.empty() ? "straight on before the turn" : "none: the next leg starts on another lane");
  if (static const bool dbg = regs::envVar("REGS_DEBUG_HALT") != nullptr; dbg && nx.legStart >= 0 && nx.legStart != st.legEnd) {
    const LaneNode &a = G.nodes[size_t(st.legEnd)], &b = G.nodes[size_t(nx.legStart)];
    LOG_INFO("sim: turn: platform %d %.1f,%.1f->%.1f,%.1f len %.1f; next leg %d %.1f,%.1f->%.1f,%.1f; opposite %d",
             st.legEnd, a.p0.x, a.p0.z, a.p1.x, a.p1.z, a.length, nx.legStart, b.p0.x, b.p0.z, b.p1.x, b.p1.z, oppositeNode(G, st.legEnd));
  }
}

// the leg from stop k's platform to the next stop's, plus `extra` nodes of the leg after: every line vehicle but the
// helicopter halts half its length past the platform node, its middle at the stop (transport.md §4.3;
// 0x101152a0.c:173-174 sets pathStepsLeft = (lengthNodes >> 1) + steps for buses and trams too, lengthNodes from the
// cars' lengths / 6 for every type)
void appendLeg(SimState& s, VehicleState& v, const LineState& line, int k) {
  const LaneGraph& G = s.graph;
  Mover& m = s.movers[size_t(v.mover)];
  const LineStopState& st = line.stops[size_t(k)];
  if (line.type == 4) {  // a helicopter flies a fresh Bézier leg to the next pad (FUN_10113280)
    std::vector<int> leg;
    helicopterLeg(s, st.legStart, st.legEnd, leg);
    for (int n : leg) m.track.push(G, n);
    m.limit = int(m.track.nodes.size()) - 1;
    v.extraNodes = 0;
    return;
  }
  for (size_t i = size_t(v.extraNodes); i < st.path.size(); ++i) m.track.push(G, st.path[i]);
  m.limit = int(m.track.nodes.size()) - 1;
  v.extraNodes = 0;
  appendHalfLength(s, v, line, k);
}

// FUN_100e2630: the node of the same track the other way round
int oppositeNode(const LaneGraph& G, int n) {
  const LaneNode& a = G.nodes[size_t(n)];
  std::vector<int> found;
  G.query(a.cellX, a.cellZ, 0, 0, a.flags & 0x1000f900, found);
  for (int c : found) {
    const LaneNode& b = G.nodes[size_t(c)];
    Vec3 d0 = b.p0 - a.p1, d1 = b.p1 - a.p0;
    if (c != n && dot(d0, d0) < 0.04f && dot(d1, d1) < 0.04f) return c;
  }
  return -1;
}

// FUN_101101e0: the last car becomes the head, every car stands on the opposite nodes of the ones it stood on, and
// the leg of stop k goes on from there (its start is the opposite of the platform the train arrived on)
bool reverseConsist(SimState& s, VehicleState& v, LineState& line, int k, int arrivedOn) {
  (void)arrivedOn;  // the leg start decides, the platform arrived on only explains why
  const LaneGraph& G = s.graph;
  Mover& m = s.movers[size_t(v.mover)];
  const LineStopState& st = line.stops[size_t(k)];
  if (m.units.empty() || m.track.nodes.empty()) return false;
  auto same = [&](int a, int b) {  // coincident lanes of a station (a stop node and its plain twin) count as one
    if (a < 0 || b < 0) return false;
    Vec3 d0 = G.nodes[size_t(a)].p0 - G.nodes[size_t(b)].p0, d1 = G.nodes[size_t(a)].p1 - G.nodes[size_t(b)].p1;
    return a == b || (dot(d0, d0) < 0.04f && dot(d1, d1) < 0.04f);
  };
  const UnitModel& hm = s.models[size_t(m.units[0].model)];
  size_t headIdx = m.track.at(m.dist + hm.rearOffset + hm.frontOffset);
  float tailRear = m.dist - trailLength(s, m);
  float lastLen = unitLength(s, m.units.back());
  float end = m.track.start[headIdx] + G.nodes[size_t(m.track.nodes[headIdx])].length;
  // the leg runs back over the opposites of the nodes the train stands on: it may start on the opposite of the head's
  // node or a node or two ahead of it (a terminus without track past the platform); the train keeps its place
  std::vector<int> nodes;
  // driven on past the platform: back over the nodes beyond the one whose opposite the leg starts on
  for (int i = int(v.turnNodes.size()) - 1; i >= v.turnSkip; --i) {
    int o = oppositeNode(G, v.turnNodes[size_t(i)]);
    if (o < 0) {
      nodes.clear();
      break;
    }
    nodes.push_back(o);
  }
  v.turnNodes.clear();
  nodes.push_back(st.legStart);
  nodes.insert(nodes.end(), st.path.begin(), st.path.end());
  int opp = oppositeNode(G, m.track.nodes[headIdx]);
  size_t jn = 0;
  float before = 0;
  while (jn < nodes.size() && jn < 8 && !same(nodes[jn], opp)) before += G.nodes[size_t(nodes[jn++])].length;
  if (opp < 0 || jn == nodes.size() || jn == 8) {
    if (debugSim())
      LOG_INFO("sim: vehicle %d cannot turn round at stop %d: its leg does not run back under it", int(&v - s.vehicles.data()), k);
    return false;
  }
  float dist = before + end - (std::max(0.0f, tailRear) + lastLen);
  releaseAll(s, v.mover);
  std::vector<Unit> units(m.units.rbegin(), m.units.rend());
  for (Unit& u : units) u.reversed = !u.reversed;
  m.units = units;
  v.units = units;
  m.track = Track();
  for (int nd : nodes) m.track.push(G, nd);
  m.dist = std::max(0.0f, dist);
  m.speed = 0;
  m.limit = int(m.track.nodes.size()) - 1;
  appendHalfLength(s, v, line, k);  // the half length past the next stop, as appendLeg
  claimBody(s, v.mover);
  if (debugSim()) LOG_INFO("sim: vehicle %d turns round at stop %d", int(&v - s.vehicles.data()), k);
  if (s.followed == int(&v - s.vehicles.data())) s.followShots = 16, s.followShotName = "follow-turn-";  // films the turn
  return true;
}

bool spawnLineVehicle(SimState& s, VehicleState& v, LineState& line, int at) {
  if (line.stops.empty() || !line.stops[size_t(at)].reachable || v.mover < 0) return false;
  Mover& m = s.movers[size_t(v.mover)];
  releaseAll(s, v.mover);
  m = Mover();
  m.transit = true;
  m.vehicle = int(&v - s.vehicles.data());
  m.units = v.units;
  m.lengthNodes = lengthNodesOf(s, v.units, line.type >= 2);
  m.track.push(s.graph, line.stops[size_t(at)].legStart);
  v.extraNodes = 0;
  v.stopA = at;
  v.stopB = (at + 1) % int(line.stops.size());
  appendLeg(s, v, line, at);
  m.live = true;
  return true;
}

void stepVehicle(VM& vm, SimState& s, LineState& line, int vi, float dt) {
  VehicleState& v = s.vehicles[size_t(vi)];
  if (v.units.empty() || v.units[0].model < 0 || v.mover < 0) return;
  if (!s.movers[size_t(v.mover)].live) {
    // broken without an agent (taken off the line while broken): repaired after 60 s, no spawn before
    // (0x101152a0.c:32-43)
    if (v.broken) {
      v.stopTime = std::max(0.0f, v.stopTime) + dt;
      if (v.stopTime >= 60) {
        v.broken = false;
        v.stopTime = 0;
        vm.fireEvent(v.object.get(), u"onFixed", {});
      }
      return;
    }
    // spawning (transport.md §4.2): at the first stop of the ring whose platform is free and whose leg has a path
    // (port: after a jam relief respawn only the stop two on, as soon as its platform is free; engine/sim_jams.cpp)
    bool placed = false;
    int only = jamRespawnStop(s, v, line, dt);
    for (int j = 0; j < int(line.stops.size()) && !placed; ++j) {
      int k = only >= 0 ? only : j;
      if (!line.stops[size_t(k)].reachable || line.stops[size_t(k)].legStart < 0) continue;
      placed = spawnLineVehicle(s, v, line, k) && canEnter(s, s.movers[size_t(v.mover)].track.nodes[0], v.mover, true);
      if (!placed) s.movers[size_t(v.mover)].live = false;
      if (only >= 0) break;
    }
    if (!placed) {
      if (debugSim()) {
        static int throttle = 0;
        if (++throttle % 300 == 1) {
          std::string why;
          for (const auto& st : line.stops) {
            int occ = st.legStart >= 0 ? s.rt[size_t(st.legStart)].occupant : -2;
            why += " [reach " + std::to_string(st.reachable) + " start " + std::to_string(st.legStart) + " occupant " + std::to_string(occ) + "]";
          }
          LOG_INFO("sim: vehicle %d cannot spawn:%s", vi, why.c_str());
        }
      }
      return;
    }
    claimBody(s, v.mover);
    vm.fireEvent(v.object.get(), u"onSpawn", {});
    jamRespawned(vm, s, v, vi);
  }
  if (stepJamRelief(vm, s, line, vi, dt)) return;  // port: jam relief (engine/sim_jams.cpp)
  Mover& m = s.movers[size_t(v.mover)];
  int n = int(line.stops.size());
  LineStopState& b = line.stops[size_t(v.stopB)];
  if (m.crashing) {  // crashed: stands, then despawns after 60 s (§4.6, :454), to re-enter at a platform
    moveMover(s, v.mover, dt);
    if (m.waitTime > 60) {
      vm.fireEvent(v.object.get(), u"onUnspawn", {});
      for (auto& st : line.stops)
        if (st.docked == vi) st.docked = -1;
      despawn(vm, s, v.mover);
      v.stopTime = 0;
      v.halted = v.brokenHalted = false;
    }
    return;
  }
  // the stop's announcement (< 32 nodes left) and the vehicle bell (< 8) on the way to stop B (0x101152a0.c:221-248)
  if (!v.halted) {
    int legEnd = m.limit >= 0 ? m.limit : int(m.track.nodes.size()) - 1;
    int left = legEnd - int(m.track.at(m.dist));
    if (left < 32 && !v.announced) v.announced = true, simSoundVehicleEvent(vm, s, vi, SimSoundEvent::Announce);
    if (left < 8 && !v.bellRung) v.bellRung = true, simSoundVehicleEvent(vm, s, vi, SimSoundEvent::Bell);
  }
  // the stop cycle (transport.md §4.3, 0x101152a0.c:300-388)
  bool arrived = freeAhead(s, m) < 1.5f && m.speed < 0.1f;
  // the door clip: opening over the first second, open, closing over the last (doorOpenStart/Len, doorCloseStart/Len
  // of the head car's model)
  auto doors = [&](float t, float D) {
    Object* o = v.units.empty() || v.units[0].model < 0 ? nullptr : s.models[size_t(v.units[0].model)].object;
    if (!o) return -1.0f;
    float os = fieldFloat(vm, o, u"doorOpenStart", -1), ol = fieldFloat(vm, o, u"doorOpenLen", 0);
    float cst = fieldFloat(vm, o, u"doorCloseStart", -1), cl = fieldFloat(vm, o, u"doorCloseLen", 0);
    if (os < 0) return -1.0f;
    if (t < 1) return os + std::max(0.0f, t) * ol;
    if (t <= D - 1 || cst < 0) return os + ol;
    return cst + std::min(1.0f, t - D + 1) * cl;
  };
  v.doorPos = -1;
  // broken down (§4.5): it stops at the next stop without docking, opens its doors, waits out the repair
  if (v.broken && arrived) {
    v.brokenHalted = true;
    v.doorPos = doors(v.stopTime < 0 ? v.stopTime + 1 : 1, 1e9f);
    if (v.stopTime < 0) {
      v.stopTime += dt;
      if (v.stopTime >= 0) {
        v.stopTime = 60;
        vm.fireEvent(v.object.get(), u"onBroken", {});
      }
    } else {
      v.stopTime -= dt;
      if (v.stopTime <= 0) {
        v.broken = v.brokenHalted = false;
        v.stopTime = 0;
        v.boardingReserved = 0;
        m.waitTime = 0;
        vm.fireEvent(v.object.get(), u"onFixed", {});
      }
    }
    m.speed = 0;
    return;
  }
  if (arrived && (b.docked < 0 || b.docked == vi || v.halted)) {
    float D = std::clamp(line.headway + 4 - b.sinceDeparture, 4.0f, 15.0f);
    if (!v.halted && debugSim()) LOG_INFO("sim: vehicle %d (line %d) halts at stop %d (dwell %.1f s)", vi, v.line, v.stopB, D);
    // debugging: REGS_DEBUG_FOLLOW films the followed bus halting at a tram stop (docs/ui/lines.md §6), and
    // REGS_DEBUG_CAMERA every vehicle halting at the camera's stop item (halt-<line type>-<burst>-<n>.png)
    if (!v.halted && (s.followShots == 0 || line.type == 0)) {  // a bus takes over another vehicle's burst
      static const int camItem = regs::envVar("REGS_DEBUG_CAMERA") ? std::atoi(regs::envVar("REGS_DEBUG_CAMERA")) : -1;
      static int bursts = 0;
      if (s.followed == vi && line.type == 0 && b.stop && fieldInt(vm, b.stop.get(), u"type", 0) == 1)
        s.followShots = 16, s.followShotName = "follow-bustram-";
      else if (camItem >= 0 && b.item == camItem)
        s.followShots = 16, s.followShotName = "halt-" + std::to_string(line.type) + "-" + std::to_string(++bursts) + "-";
    }
    if (static const bool dbgHalt = regs::envVar("REGS_DEBUG_HALT") != nullptr; dbgHalt && !v.halted && !v.units.empty()) {
      const LineStopState& arr = line.stops[size_t(v.stopA)];
      int plat = arr.legEnd;
      if (plat >= 0) {
        const LaneNode& pn = s.graph.nodes[size_t(plat)];
        Vec3 P = (pn.p0 + pn.p1) * 0.5f, f = normalize(pn.p1 - pn.p0);
        float r = m.dist, front = 0, rear = 0;
        for (size_t k = 0; k < m.units.size(); ++k) {
          const Unit& u = m.units[k];
          if (k > 0) r -= unitLength(s, u);
          if (u.model < 0) continue;
          const UnitModel& um = s.models[size_t(u.model)];
          PlacedPose p = unitPose(s, m, u, r);
          float a = dot(p.position - P, f);
          if (k == 0) front = a + um.frontOffset;
          rear = a - um.rearOffset;
        }
        LOG_INFO("sim: halt at: line %d type %d stop %d platform %d: front %+.1f rear %+.1f middle %+.1f (units along the platform node from its centre)",
                 v.line, line.type, v.stopB, plat, front, rear, (front + rear) * 0.5f);
      }
    }
    v.halted = true;
    if (v.stopTime == 0) simSoundVehicleEvent(vm, s, vi, SimSoundEvent::DoorOpen);  // :311-314
    v.stopTime += dt;
    if (v.boardingReserved != 0) {  // citizens walking to the doors keep them open (0x101152a0.c:323-330)
      v.stopTime = std::min(v.stopTime, D - 1);
      m.waitTime += dt;
      if (m.waitTime >= 30) v.boardingReserved = 0;
    } else {
      m.waitTime = 0;
    }
    v.doorPos = v.stopTime < D ? doors(v.stopTime, D) : -1;
    if (v.stopTime >= D && line.type == 4 && m.rotorSpin < 12.0f) {
      // port change (boats-aircraft.md §6.3): a helicopter boards with a still rotor; done, it stays on the pad with
      // its doors shut while stepAnimatedParts spins the rotor up, and takes off at full speed. The next helicopter
      // for this pad keeps holding in the air meanwhile (stopB is unchanged, checkHelicopterPaths below)
      if (!v.spinUp && b.docked == vi) b.docked = -1, simSoundVehicleEvent(vm, s, vi, SimSoundEvent::DoorClose);
      v.spinUp = true;
      v.stopTime = D;
      v.doorPos = -1;
      m.speed = 0;
      return;
    }
    if (v.stopTime >= D) {  // depart
      v.spinUp = false;
      if (debugSim())
        LOG_INFO("sim: vehicle %d departs stop %d after %.1f s (%d aboard, %d boarded here so far, %d still waiting)", vi,
                 v.stopB, v.stopTime, v.passengerCount, b.passengersTransported, b.waiting);
      if (b.docked == vi) b.docked = -1, simSoundVehicleEvent(vm, s, vi, SimSoundEvent::DoorClose);
      if (b.docked < 0) b.sinceDeparture = 0;
      v.bellRung = v.announced = false;
      v.halted = false;
      v.travelDistance += float(b.path.size()) * 6;
      int arrivedOn = line.stops[size_t(v.stopA)].legEnd;
      v.stopA = v.stopB;
      v.stopB = (v.stopB + 1) % n;
      const LineStopState& next = line.stops[size_t(v.stopA)];
      if (next.reachable && next.legStart >= 0 && arrivedOn >= 0 && next.legStart != arrivedOn) {
        // a platform change (0x101152a0.c:362-369): rail vehicles and buses turn round, boats jump
        if (line.type == 3 || !reverseConsist(s, v, line, v.stopA, arrivedOn)) spawnLineVehicle(s, v, line, v.stopA), claimBody(s, v.mover);
      } else if (next.reachable) {
        appendLeg(s, v, line, v.stopA);
      }
      v.stopTime = 0;
    } else if (v.stopTime >= 1) {
      if (v.stopTime <= D - 1) {
        if (b.docked < 0) b.docked = vi;
      } else if (b.docked == vi) {  // closing
        b.docked = -1;
        simSoundVehicleEvent(vm, s, vi, SimSoundEvent::DoorClose);
      }
    }
    m.speed = 0;
    return;
  }
  // checkHelicopterPaths (FUN_10110900, boats-aircraft.md §6.2): a helicopter holds in the air while another one
  // bound for the same stop is within 30 units of its next three unclaimed nodes
  int limit = m.limit;
  if (line.type == 4 && m.holdEnd + 2 < int(m.track.nodes.size())) {
    for (const LineState& other : s.lines) {
      if (other.type != 4) continue;
      for (int oi : other.vehicles) {
        const VehicleState& ov = s.vehicles[size_t(oi)];
        if (oi == vi || ov.mover < 0 || !s.movers[size_t(ov.mover)].live || ov.stopB < 0 || v.stopB < 0) continue;
        if (other.stops[size_t(ov.stopB)].item != line.stops[size_t(v.stopB)].item) continue;
        const Mover& om = s.movers[size_t(ov.mover)];
        Vec3 op = s.graph.nodes[size_t(om.track.nodes[om.track.at(om.dist)])].p0;
        for (int k = 0; k < 3; ++k) {
          Vec3 d = s.graph.nodes[size_t(m.track.nodes[size_t(m.holdEnd + k)])].p0 - op;
          if (dot(d, d) < 900.0f) m.limit = std::min(m.limit, m.holdEnd - 1);
        }
      }
    }
  }
  moveMover(s, v.mover, dt);
  m.limit = limit;
  // the breakdown roll, once per second while the condition is below 0.99999 (0x101152a0.c:435-451)
  float condition = v.object ? fieldFloat(vm, v.object.get(), u"condition", 1.0f) : 1.0f;
  // test switch REGS_TEST_BREAKDOWN=<seconds>: the vehicle REGS_DEBUG_FOLLOW follows breaks down once after that much
  // simulated time (for its smoke, docs/renderer/particles.md)
  static const float testBreak = regs::envVar("REGS_TEST_BREAKDOWN") ? float(std::atof(regs::envVar("REGS_TEST_BREAKDOWN"))) : -1;
  if (testBreak >= 0 && s.followed == vi && !v.broken) {
    static float clock = 0;
    static bool done = false;
    if (!done && (clock += dt) >= testBreak) done = true, v.broken = true, v.stopTime = -1, ++s.breakdowns;
  }
  if (!v.broken && condition < 0.99999f) {
    v.breakdownClock += dt;
    if (v.breakdownClock >= 1) {
      v.breakdownClock -= 1;
      if (rndFloat(s, 0, 1 / (1 - condition) - 0.99999f) < 0.001f) {
        v.broken = true;
        v.stopTime = -1;
        ++s.breakdowns;
      }
    }
  }
  if (m.waitTime > 180 && freeAhead(s, m) > 1.5f) {  // onJammed (0x101152a0.c:192-197)
    vm.fireEvent(v.object.get(), u"onJammed", {});
    m.waitTime = 0;
    v.jammedFired = true;
  }
}

void stepLine(VM& vm, SimState& s, LineState& line, float dt) {
  // line tick (0x10114f30): stop clocks and the headway Σ(segment cost + 5) / vehicles
  float sum = 0;
  for (auto& st : line.stops) {
    st.sinceDeparture += dt;
    sum += st.cost + 5;
  }
  if (!line.vehicles.empty()) line.headway = sum / float(line.vehicles.size());
  if (line.stops.size() < 2) return;  // nothing to drive (the scripts only activate closed lines)
  for (int vi : line.vehicles) stepVehicle(vm, s, line, vi, dt);
}

bool lineActive(VM& vm, const LineState& line) {
  if (!line.object) return false;
  auto it = line.object->fieldValues.find(u"active");
  return it == line.object->fieldValues.end() || it->second.isNone() || vm.resolve(it->second).toBool();
}

// ---- poses ----

// FUN_10106a10 (gsGridVehicle::calculatePose): a point on the track at distance s, on the node's Bézier with its
// ends moved to the joints between nodes
Vec3 trackPoint(const LaneGraph& G, const Track& tr, float s) {
  if (tr.nodes.empty()) return {};
  if (s <= 0) return G.nodes[size_t(tr.nodes[0])].p0;
  size_t i = tr.at(s);
  const LaneNode& c = G.nodes[size_t(tr.nodes[i])];
  const LaneNode* p = i > 0 ? &G.nodes[size_t(tr.nodes[i - 1])] : nullptr;
  const LaneNode* n = i + 1 < tr.nodes.size() ? &G.nodes[size_t(tr.nodes[i + 1])] : nullptr;
  float L = c.length;
  Vec3 a = p ? (p->p1 + c.p0) * 0.5f : c.p0;
  Vec3 fa = p ? render::normalize(p->f1 + c.f0) : c.f0;
  Vec3 d = n ? (c.p1 + n->p0) * 0.5f : c.p1;
  Vec3 fd = n ? render::normalize(c.f1 + n->f0) : c.f1;
  Vec3 b = a + fa * (L / 3), cc = d - fd * (L / 3);
  float t = L > 0 ? std::clamp((s - tr.start[i]) / L, 0.0f, 1.0f) : 0.0f;
  float u = 1 - t;
  return a * (u * u * u) + b * (3 * u * u * t) + cc * (3 * u * t * t) + d * (t * t * t);
}

// a unit whose rear point is at distance r: rear and front anchor points on the track, the origin between them
PlacedPose unitPose(const SimState& s, const Mover& v, const Unit& u, float r) {
  const UnitModel& m = s.models[size_t(u.model)];
  float s1 = r + m.rearOffset - m.rearAnchor, s2 = s1 + m.rearAnchor + m.frontAnchor;
  Vec3 A = trackPoint(s.graph, v.track, s1), B = trackPoint(s.graph, v.track, s2);
  Vec3 dir = B - A;
  float span = m.frontAnchor + m.rearAnchor;
  PlacedPose p;
  p.position = span > 0 ? A + dir * (m.rearAnchor / span) : A;
  if (span < 0.22f) dir.y = 0;  // both anchors below 0.11: kept level
  if (dot(dir, dir) < 1e-8f) {
    size_t i = v.track.at(s1);
    dir = s.graph.nodes[size_t(v.track.nodes[i])].f0;
  }
  p.forward = render::normalize(u.reversed ? dir * -1.0f : dir);
  p.position = p.position + v.crashOffset;
  return p;
}

// model space: +Y up, the front towards −Z (gsQuaternion(up, −dir))
Mat4 poseMatrix(const PlacedPose& p) {
  Vec3 z = p.forward * -1.0f;
  Vec3 x = render::normalize(render::cross(Vec3{0, 1, 0}, z));
  if (dot(x, x) < 0.5f) x = {1, 0, 0};
  Vec3 y = render::cross(z, x);
  Mat4 w;
  w.at(0, 0) = x.x, w.at(1, 0) = x.y, w.at(2, 0) = x.z;
  w.at(0, 1) = y.x, w.at(1, 1) = y.y, w.at(2, 1) = y.z;
  w.at(0, 2) = z.x, w.at(1, 2) = z.y, w.at(2, 2) = z.z;
  w.at(0, 3) = p.position.x, w.at(1, 3) = p.position.y, w.at(2, 3) = p.position.z;
  return w;
}

// every unit of every live mover that keep(mover) lets through: line vehicles first, then the traffic
template <typename K, typename F>
void forEachUnitOf(const SimState& s, K keep, F fn) {
  auto units = [&](const Mover& m) {
    if (!keep(m)) return;
    float r = m.dist;
    for (size_t k = 0; k < m.units.size(); ++k) {
      const Unit& u = m.units[k];
      if (k > 0) r -= unitLength(s, u);
      if (u.model < 0) continue;
      fn(m, int(k), u, unitPose(s, m, u, r));
    }
  };
  for (const VehicleState& v : s.vehicles)
    if (v.mover >= 0 && s.movers[size_t(v.mover)].live && !s.movers[size_t(v.mover)].track.nodes.empty()) units(s.movers[size_t(v.mover)]);
  for (const Mover& m : s.movers)
    if (m.live && !m.transit && !m.track.nodes.empty()) units(m);
}

template <typename F>
void forEachUnit(const SimState& s, F fn) {
  forEachUnitOf(s, [](const Mover&) { return true; }, fn);
}

// the doors a boarding pedestrian walks to (FUN_10108350): the point beside the nearest car of the vehicle, on the
// pedestrian's side, along the car's length. Port: from the model bounds, not the model's door locators.
bool vehicleDoorPoint(const SimState& s, const VehicleState& v, const Vec3& from, Vec3& out) {
  if (v.mover < 0 || !s.movers[size_t(v.mover)].live) return false;
  const Mover& m = s.movers[size_t(v.mover)];
  if (m.track.nodes.empty()) return false;
  float best = -1, r = m.dist;
  for (size_t k = 0; k < m.units.size(); ++k) {
    const Unit& u = m.units[k];
    if (k > 0) r -= unitLength(s, u);
    if (u.model < 0) continue;
    const UnitModel& um = s.models[size_t(u.model)];
    PlacedPose p = unitPose(s, m, u, r);
    Vec3 center = render::transformPoint(poseMatrix(p), (um.boundsMin + um.boundsMax) * 0.5f);
    Vec3 half = (um.boundsMax - um.boundsMin) * 0.5f, fwd = normalize(p.forward);
    Vec3 side = normalize(cross(Vec3{0, 1, 0}, fwd)), d = from - center;
    float along = std::clamp(dot(d, fwd), -std::max(0.0f, half.z - 0.5f), std::max(0.0f, half.z - 0.5f));
    Vec3 door = center + fwd * along + side * ((dot(d, side) < 0 ? -1.0f : 1.0f) * (half.x + 0.4f));
    door.y = p.position.y;
    Vec3 e = door - from;
    float dist = dot(e, e);
    if (best < 0 || dist < best) best = dist, out = door;
  }
  return best >= 0;
}

namespace {
// the keys of the synced fields, built once (a key over 7 characters costs a heap string per lookup)
const std::u16string kPositionKey = u"position", kGlobalPosKey = u"globalPos", kOrientationKey = u"orientation";
const std::u16string kModelKey = u"<model>", kInstanceKey = u"<instance>";

void writePose(Object* o, FieldSlots* slots, const Vec3& p, float qx, float qy, float qz, float qw) {
  Value pos = Value::vector(p.x, p.y, p.z), q = Value::quat(qx, qy, qz, qw);
  if (!slots) {
    o->fieldValues[kPositionKey] = pos;
    o->fieldValues[kGlobalPosKey] = pos;
    o->fieldValues[kOrientationKey] = q;
    return;
  }
  FieldSlots& c = fieldSlots(o, *slots);
  if (!c.position) c.position = &o->fieldValues[kPositionKey];
  if (!c.globalPos) c.globalPos = &o->fieldValues[kGlobalPosKey];
  if (!c.orientation) c.orientation = &o->fieldValues[kOrientationKey];
  setSlot(*c.position, pos);
  setSlot(*c.globalPos, pos);
  setSlot(*c.orientation, q);
}
}  // namespace

FieldSlots& fieldSlots(Object* o, FieldSlots& slots) {
  if (slots.serial != o->serial) slots = FieldSlots{}, slots.serial = o->serial;
  return slots;
}

void setSlot(Value& slot, const Value& v) {
  if (slot.type() == v.type()) {
    switch (v.type()) {
      case VT::Vector:
      case VT::Quat:
        if (std::memcmp(slot.vec(), v.vec(), 4 * sizeof(float)) == 0) return;
        break;
      case VT::Bool:
        if (slot.b() == v.b()) return;
        break;
      case VT::Object:
        if (slot.heapPtr() == v.heapPtr()) return;
        break;
      default: break;
    }
  }
  slot = v;
}

// a character's orientation from its yaw: the pose matrix is a turn by yaw + π about +Y, so the quaternion below is
// ±(0, cos(yaw/2), 0, sin(yaw/2)), with the sign setPoseFields' trace branches pick (trace 1 − 2·cos yaw > 0: w ≥ 0;
// else the y branch: y ≤ 0); equal to the matrix path within float rounding
void setCharacterPoseFields(Object* o, const Vec3& pos, float yaw, FieldSlots* slots) {
  float c = std::cos(yaw), h = std::cos(yaw * 0.5f), sh = std::sin(yaw * 0.5f);
  bool neg = 1.0f - 2.0f * c > 0 ? sh < 0 : h > 0;
  if (neg) h = -h, sh = -sh;
  writePose(o, slots, pos, 0.0f, h, 0.0f, sh);
}

// position, globalPos, orientation of a gs3DElement from a pose (orientation in the engine's convention, the
// conjugate of ours: q·(0,0,−1) = forward there)
void setPoseFields(Object* o, const PlacedPose& p, FieldSlots* slots) {
  Mat4 w = poseMatrix(p);
  float m00 = w.at(0, 0), m11 = w.at(1, 1), m22 = w.at(2, 2), tr = m00 + m11 + m22;
  float qx, qy, qz, qw;
  if (tr > 0) {
    float k = std::sqrt(tr + 1) * 2;
    qw = 0.25f * k, qx = (w.at(2, 1) - w.at(1, 2)) / k, qy = (w.at(0, 2) - w.at(2, 0)) / k, qz = (w.at(1, 0) - w.at(0, 1)) / k;
  } else if (m00 > m11 && m00 > m22) {
    float k = std::sqrt(1 + m00 - m11 - m22) * 2;
    qw = (w.at(2, 1) - w.at(1, 2)) / k, qx = 0.25f * k, qy = (w.at(0, 1) + w.at(1, 0)) / k, qz = (w.at(0, 2) + w.at(2, 0)) / k;
  } else if (m11 > m22) {
    float k = std::sqrt(1 + m11 - m00 - m22) * 2;
    qw = (w.at(0, 2) - w.at(2, 0)) / k, qx = (w.at(0, 1) + w.at(1, 0)) / k, qy = 0.25f * k, qz = (w.at(1, 2) + w.at(2, 1)) / k;
  } else {
    float k = std::sqrt(1 + m22 - m00 - m11) * 2;
    qw = (w.at(1, 0) - w.at(0, 1)) / k, qx = (w.at(0, 2) + w.at(2, 0)) / k, qy = (w.at(1, 2) + w.at(2, 1)) / k, qz = 0.25f * k;
  }
  writePose(o, slots, p.position, -qx, -qy, -qz, qw);
}

// the gs3DElement fields of the vehicles' script objects (their leading unit) and the hidden fields the
// item-controller natives read (<model>, <instance>: gsGridController.getModel / hasInstance); through cached slots,
// writing only values that differ
void syncVehicleFields(SimState& s) {
  for (VehicleState& v : s.vehicles) {
    Object* o = v.object.get();
    if (!o) continue;
    FieldSlots& c = fieldSlots(o, v.objectSlots);
    const Unit* head = v.units.empty() || v.units[0].model < 0 ? nullptr : &v.units[0];
    if (head) {
      if (!c.model) c.model = &o->fieldValues[kModelKey];
      if (HeapObj* model = s.models[size_t(head->model)].object; !(c.model->type() == VT::Object && c.model->heapPtr() == model))
        *c.model = Value::heap(VT::Object, model);
    }
    const Mover* m = v.mover >= 0 ? &s.movers[size_t(v.mover)] : nullptr;
    bool live = head && m && m->live && !m->track.nodes.empty();
    if (!c.instance) c.instance = &o->fieldValues[kInstanceKey];
    setSlot(*c.instance, Value::boolean(live));
    if (live) setPoseFields(o, unitPose(s, *m, *head, m->dist), &v.objectSlots);
  }
  for (Mover& m : s.movers)
    if (m.live && !m.transit && m.controller && !m.track.nodes.empty())
      setPoseFields(m.controller.get(), unitPose(s, m, m.units[0], m.dist), &m.controllerSlots);
}

// ---- loading ----

// the background traffic of the save: every agent chain whose nodes resolve, with its saved path to the end
void loadTraffic(VM& vm, SimState& s, const std::vector<SavedAgent>& agents, std::vector<SavedAgentRef>& refs) {
  const LaneGraph& G = s.graph;
  refs.assign(agents.size(), SavedAgentRef());
  // carriages: every nextCarRef target (AI train carriages carry no prevCarRef)
  std::vector<uint8_t> trailer(agents.size(), 0);
  for (const SavedAgent& a : agents)
    if (a.nextCar >= 1 && a.nextCar <= agents.size()) trailer[a.nextCar - 1] = 1;
  int moving = 0, parked = 0, skipped = 0, boatsDropped = 0;
for (size_t ai = 0; ai < agents.size(); ++ai) {
    const SavedAgent& a = agents[ai];
    if ((a.flags & 0x200) || a.prevCar || trailer[ai] || (a.flags & 5) == 5 || a.model < 0) continue;  // line vehicles, trailers, invalid
    std::vector<const SavedAgent*> chain{&a};
    for (uint32_t c = a.nextCar; c >= 1 && c <= agents.size() && chain.size() < 64; c = agents[c - 1].nextCar) chain.push_back(&agents[c - 1]);
    const SavedAgent& last = *chain.back();
    bool selfDriving = a.stepsLeft != -1;
    std::vector<int> track;
    bool ok = true;
    std::vector<int> inl;  // the last car's flight nodes, made now
    for (const LaneNode& n : last.inlineNodes) inl.push_back(newInlineNode(s, n));
    auto node = [&](uint32_t ref) {
      if (!(ref & 0x80000000u) || (ref >> 24) == 0x7f) return G.resolve(ref);
      size_t i = ref & 0x7fffffffu;
      return i < inl.size() ? inl[i] : -1;
    };
    auto add = [&](uint32_t ref) {
      int n = node(ref);
      if (n < 0) ok = false;
      else track.push_back(n);
    };
    auto drop = [&] {
      for (int n : inl) freeInlineNode(s, n);
      ++skipped;
    };
    // the last car holds the whole chain's nodes: its own node, then the queue up to the head's front
    if (chain.size() == 1 && a.prevNode) {
      int p = G.resolve(a.prevNode), c = G.resolve(a.curNode);
      if (p >= 0 && c >= 0 && std::find(G.nodes[size_t(p)].next.begin(), G.nodes[size_t(p)].next.end(), c) != G.nodes[size_t(p)].next.end())
        track.push_back(p);
    }
    add(last.curNode);
    for (uint32_t r : last.ring) add(r);
    if (!ok || track.empty()) {
      drop();
      continue;
    }
    if (selfDriving && !decodePath(G, a, track.back(), track)) {
      drop();
      continue;
    }
    int head = &a == &last ? node(a.curNode) : G.resolve(a.curNode);
    auto at = std::find(track.begin(), track.end(), head);
    if (at == track.end()) {
      drop();
      continue;
    }
    // save quirk Q4 (boats-aircraft.md §2.7, §4.5): the original's save rebuilds the water lanes and marks every
    // agent on them with flag 1, which its boat manager despawns on the next tick, after loading too. The port
    // writes no flag 1 (its own saves keep their boats), so this only drops the boats of the original's saves.
    if (selfDriving && (a.flags & 1) && (G.nodes[size_t(head)].flags & 0x1000ff00) == kPathWater) {
      drop();
      ++boatsDropped;
      continue;
    }
    int id = newMover(s);
    Mover& m = s.movers[size_t(id)];
    for (const SavedAgent* c : chain) {
      Unit u;
      u.model = c->model;
      u.reversed = (c->flags & 2) != 0;
      u.seed = c->colorSeed;
      u.tint = paint(s, c->model, c->colorSeed);
      m.units.push_back(u);
    }
    m.live = true;
    m.parked = !selfDriving;
    m.train = selfDriving && (G.nodes[size_t(head)].flags & 0x1000f900) == kPathTrain;
    m.colorSeed = a.colorSeed;
    m.lengthNodes = std::max(1, a.lengthNodes);
    m.speed = a.speed;
    m.reckless = (a.flags & 0x800) != 0;
    m.crashing = (a.flags & 0x40000) != 0;
    m.crashOffset = a.crashOffset;
    for (int n : track) m.track.push(G, n);
    m.dist = m.track.start[size_t(at - track.begin())] + std::max(0.0f, a.posOnNode);
    refs[ai].mover = id;
    if (m.parked) {
      claimBody(s, id);  // a parked car holds its node like any agent on it
      ++parked;
      continue;
    }
    claimBody(s, id);
    attachController(vm, s, id);
    ++moving;
    if (m.train) {
      ++s.trainsLive;
    } else if ((G.nodes[size_t(head)].flags & 0x1000f900) == kPathAir) {  // an aircraft holds its whole route
      m.aircraft = true, ++s.aircraftLive;
      for (int n : m.track.nodes)
        if (s.rt[size_t(n)].occupant < 0) s.rt[size_t(n)].occupant = id;
      m.holdBegin = 0, m.holdEnd = int(m.track.nodes.size());
    } else if ((G.nodes[size_t(head)].flags & 0x1000ff00) == kPathWater) {
      m.boat = true, ++s.boatsLive;
    }
    else ++s.roadLive, s.roadConsists.push_back(m.units);
  }
  for (std::vector<Unit>& c : s.roadConsists)
    for (Unit& u : c) u.tint = 0;
  s.roadTarget = s.roadLive;
  LOG_INFO("traffic: %d moving vehicles (%d trains, %d boats), %d parked, %d not placed (%d boats with flag 1)", moving,
           s.trainsLive, s.boatsLive, parked, skipped, boatsDropped);
}

// the traffic sources: road trip ends and the train entry/exit sets (pathfinding.md §2.5)
void collectSources(SimState& s) {
  for (size_t i = 0; i < s.graph.nodes.size(); ++i) {
    uint32_t f = s.graph.nodes[i].flags;
    if (f & 0x4000000) continue;
    if ((f & 0x10100) == 0x10100 && !(f & kPathPark)) s.roadStarts.push_back(int(i));
    if ((f & 0x20100) == 0x20100) s.roadEnds.push_back(int(i));
    if ((f & 0x1000f900) == kPathAir) {
      if ((f & kPathStart) && !(f & kPathStop)) s.airEntries.push_back(int(i));
      else if ((f & kPathEnd) && (f & kPathStop)) s.airGatesIn.push_back(int(i));
      else if ((f & kPathStart) && (f & kPathStop)) s.airGatesOut.push_back(int(i));
      else if ((f & kPathEnd) && !(f & kPathStop)) s.airExits.push_back(int(i));
    }
    if ((f & 0x1000ff00) == kPathWater) {
      if (f & kPathStart) s.waterStarts.push_back(int(i));
      else if (f & kPathEnd) s.waterEnds.push_back(int(i));
    }
    if ((f & 0x1000f900) != kPathTrain) continue;
    if ((f & kPathStart) && !(f & kPathPark)) s.trainStarts[1].push_back(int(i));
    if ((f & kPathEnd) && !(f & kPathPark)) s.trainEnds[1].push_back(int(i));
    if ((f & kPathStart) && !(f & kPathStop)) s.trainStarts[0].push_back(int(i));
    if ((f & kPathEnd) && !(f & kPathStop)) s.trainEnds[0].push_back(int(i));
  }
  if (regs::envVar("REGS_DEBUG_SIM")) {  // with a path from a map-edge way in to a way out when there is one
    int ext = 0, routed = -1;
    for (int a : s.roadStarts)
      if (s.graph.nodes[size_t(a)].flags & kPathExternal) {
        ++ext;
        for (int b : s.roadEnds) {
          std::vector<int> path;
          float cost = 0;
          if (routed < 0 && (s.graph.nodes[size_t(b)].flags & kPathExternal) && s.graph.search(a, b, false, path, cost))
            routed = int(path.size());
        }
      }
    LOG_INFO("traffic sources: %zu road starts (%d at the map's edge), %zu road ends; edge to edge %s (%d nodes)",
             s.roadStarts.size(), ext, s.roadEnds.size(), routed >= 0 ? "routed" : "no route", routed);
  }
}

// the stop vector entry of a stop item (registration 0x10119070: type and catchment from the item type)

bool registerStop(VM& vm, GridObj* grid, SimState& s, size_t i) {
  const GridData& g = *grid->data;
  ClassInfo* stopClass = vm.findClass("gsTransportStopController");
  if (!stopClass || g.items[i].removed || s.stopOfItem.count(int(i))) return false;
  const auto& it = g.items[i];
  if (it.type >= g.types.size()) return false;
  auto t = grid->templates.find(g.types[it.type]);
  if (t == grid->templates.end()) return false;
  int tflags = fieldInt(vm, t->second.get(), u"flags", 0);
  // port (owner's request 2026-11-15): a train station's track (station-track, terminal-track) is a tram stop, its
  // platform tracks' tram lanes the platforms (trams on railway tracks)
  const bool station = simIsTrainStation(g.types[it.type]);
  if ((!(tflags & 0x2000) && !station) || (it.flags & 0x100000)) return false;  // GRID_ITEM_STOP, not GRID_ITEM_TEMP
  int type = station ? 10 : fieldInt(vm, t->second.get(), u"type", 0);
  int stopType = (type == 2 || type == 5 || type == 6) ? 0
                 : (type >= 10 && type <= 12)          ? 1
                 : (type >= 13 && type <= 15)          ? 2
                 : type == 0x11                        ? 3
                 : type == 0x12                        ? 4
                                                       : -1;
  if (stopType < 0) return false;
  // a train station's template is no stop template and has no $attractiveness, which ui.script's
  // transportStopCreated multiplies (metro/scripts/ui.script:357, "Operator 3 not defined"): the plain tram stop's 0.70
  if (station) try {
      Value cur = vm.resolve(vm.getMember(Value::heap(VT::Object, t->second.get()), u"$attractiveness"));
      if (cur.type() != VT::Float && cur.type() != VT::Int)
        vm.setMember(Value::heap(VT::Object, t->second.get()), u"$attractiveness", Value::real(0.70f));
    } catch (const ScriptError&) {
      vm.setMember(Value::heap(VT::Object, t->second.get()), u"$attractiveness", Value::real(0.70f));
    }
  // an item controller under the grid like the picking ones (<item>, <model>: gsGridController.getModel, getFlags)
  Ref<Object> o = vm.newObject(stopClass, grid);
  if (auto* to = transport(o.get())) to->index = int(s.stops.size());
  o->fieldValues[u"<model>"] = Value::heap(VT::Object, t->second.get());
  // the track templates of train stations have no $attractiveness and $maintenanceCost; ui.script's
  // transportStopCreated and player.script's monthly stop maintenance multiply them ("Operator 3 not defined" /
  // "Cannot read valueless variant", scenarios 4 and 5): the values of the first tram stops (stops/tram01_*.script)
  if (station && !t->second->findVar(u"attractiveness")) t->second->var(u"attractiveness") = Value::real(0.70);
  if (station && !t->second->findVar(u"maintenanceCost")) t->second->var(u"maintenanceCost") = Value::integer(1000);
  {
    int w = std::max(1, fieldInt(vm, t->second.get(), u"width", 1)), l = std::max(1, fieldInt(vm, t->second.get(), u"length", 1));
    int sx = (it.orient & 1) ? l : w, sz = (it.orient & 1) ? w : l;
    float cs = g.cellSize, ang = float(it.orient) * 1.57079633f;
    Value p = Value::vector(float(2 * it.x + sx - g.width) * cs * 0.5f, float(it.y) * cs / 256.0f, float(2 * it.z + sz - g.length) * cs * 0.5f);
    o->fieldValues[u"position"] = p;
    o->fieldValues[u"globalPos"] = p;
    o->fieldValues[u"orientation"] = Value::quat(0, -std::sin(ang * 0.5f), 0, std::cos(ang * 0.5f));
  }
  o->fieldValues[u"type"] = Value::integer(stopType);
  o->fieldValues[u"catchmentArea"] = Value::real(stopType <= 1 ? 100.0f : 150.0f);
  o->fieldValues[u"attractiveness"] = Value::real(1.0f);
  o->fieldValues[u"passengerCount"] = Value::integer(0);
  o->fieldValues[u"<item>"] = Value::integer(int(i));
  if (station && debugSim()) LOG_INFO("sim: train station item %zu (%s at %d,%d) is a tram stop too", i, g.types[it.type].c_str(), it.x, it.z);
  s.stopOfItem[int(i)] = int(s.stops.size());
  s.stops.push_back(o);
  if (auto* to = transport(o.get()); to && grid->sim.get() == &s) to->sim = grid->sim;
  // gsGridItem::gsGridItem (L:0x08187fc0.c:152-161): a STOP item's controller is announced with grid event 6; the game
  // makes the stop's bubble (passengers, mood) there and sets its attractiveness (ui.script:317)
  vm.fireEvent(grid, u"transportStopCreated", {Value::heap(VT::Object, o.get())});
  return true;
}

// Port QoL (docs/ui/lines.md §6): the road lane where a bus halts at a tram stop, −1 when no bus can halt there. A stop
// item owns no lanes; the road piece and the tram track piece under it do. Every road piece has a road STOP lane
// (0x340 small, 0x540 large/avenue: its curb lane at the piece's middle, where a bus stop item makes buses halt), and
// the tram track piece a tram STOP lane at the stop's platform. A tram stop in a street (small and large roads, straight
// and diagonal) has its tram running in the road's curb lane: the road STOP lane and the tram STOP lane coincide (same
// ends, same direction), so a bus halts where the tram does, at the platform. On its own track (grass, a tram-only
// track) there is no road lane in the footprint; in an avenue's middle strip the road STOP lanes are the carriageways'
// curb lanes, away from the platform. Both have no coincident pair and do not qualify. Only lanes whose chord midpoint
// lies in the stop's own footprint count, so a neighbouring piece's or the other carriageway's lane is never taken.
int busPlatformInFootprint(const SimState& s, int x, int z, int sx, int sz, const Vec3& centre) {
  const LaneGraph& G = s.graph;
  std::vector<int> found;
  G.query(x, z, sx, sz, kPathStop, found);
  std::vector<int> road, tram;
  for (int n : found) {
    const LaneNode& L = G.nodes[size_t(n)];
    if (L.cellX < x || L.cellX >= x + sx || L.cellZ < z || L.cellZ >= z + sz || (L.flags & (kPathDeleted | 0x4000000))) continue;
    if ((L.flags & kPathClassMask & ~0x600u) == kPathRoad && !(L.flags & kPathPark)) road.push_back(n);
    else if ((L.flags & kPathClassMask) == kPathTram) tram.push_back(n);
  }
  int best = -1;
  float bestD = 0;
  for (int r : road)
    for (int t : tram) {
      const LaneNode &a = G.nodes[size_t(r)], &b = G.nodes[size_t(t)];
      Vec3 d0 = a.p0 - b.p0, d1 = a.p1 - b.p1;
      if (d0.x * d0.x + d0.z * d0.z > 0.25f || d1.x * d1.x + d1.z * d1.z > 0.25f || std::fabs(d0.y) > 1 || std::fabs(d1.y) > 1) continue;
      Vec3 m = (a.p0 + a.p1) * 0.5f - centre;
      float d = m.x * m.x + m.z * m.z;
      if (best < 0 || d < bestD) best = r, bestD = d;
    }
  return best;
}

// FUN_10110c10: the platform nodes of a stop for its line's transport class (the primary STOP node nearest to the
// item centre, and its ALTERNATIVE_STOP node if the stop has one)
void stopPlatforms(VM& vm, GridObj* grid, SimState& s, const LineState& line, LineStopState& st) {
  const GridData& g = *grid->data;
  static const uint32_t kClass[5] = {kPathRoad, kPathTram, kPathMetro, kPathWater, kPathAir};
  uint32_t mask = kPathStop | (line.type >= 0 && line.type < 5 ? kClass[line.type] : 0);
  st.platform = st.altPlatform = -1;
  if (st.item < 0 || size_t(st.item) >= g.items.size()) return;
  const auto& it = g.items[size_t(st.item)];
  auto t = grid->templates.find(g.types[it.type]);
  int w = 1, l = 1, ty = 0, tf = 0;
  if (t != grid->templates.end()) {
    w = std::max(1, fieldInt(vm, t->second.get(), u"width", 1));
    l = std::max(1, fieldInt(vm, t->second.get(), u"length", 1));
    ty = fieldInt(vm, t->second.get(), u"type", 0);
    tf = fieldInt(vm, t->second.get(), u"flags", 0);
  }
  int sx = (it.orient & 1) ? l : w, sz = (it.orient & 1) ? w : l;
  float cs = g.cellSize;
  Vec3 c{(float(it.x) + float(sx) * 0.5f - float(g.width) * 0.5f) * cs, float(itemHeight(g, it, uint32_t(tf), ty)) * cs / 256.0f,
         (float(it.z) + float(sz) * 0.5f - float(g.length) * 0.5f) * cs};
  if (line.type == 3 || line.type == 4) c.y = -1.25f * cs;
  if (simIsTrainStation(g.types[it.type])) ty = 10;  // a train station serves trams (registerStop)
  if (line.type == 0 && ty >= 10 && ty <= 12) {  // port: a bus at a tram stop (the add-stop tool admits street ones)
    st.platform = busPlatformInFootprint(s, it.x, it.z, sx, sz, c);
    return;
  }
  st.platform = s.graph.nearest(c, mask);
  // port: trams on railway tracks (2026-11-15). A tram stop on a railway block or beside a railway curve has no tram STOP
  // lane of its own: its platform is the railway's tram lane on the stop's side (the one nearest to a point 3 units
  // out from the stop's centre toward its side: its connections, as the stop tool set them; for a curve's anchor item,
  // whose centre lies beside the track, its centre)
  if (line.type == 1 && ty >= 10 && ty <= 12) {
    const float reach = float(std::max(sx, sz) + 2) * cs;
    auto within = [&](int n) {
      if (n < 0) return false;
      Vec3 m = (s.graph.nodes[size_t(n)].p0 + s.graph.nodes[size_t(n)].p1) * 0.5f - c;
      return m.x * m.x + m.z * m.z <= reach * reach;
    };
    if (!within(st.platform)) {
      uint32_t conn = gridItemConnections(vm, grid, uint32_t(st.item)) & 0xffu;
      Vec3 side{conn & 0x04 ? 1.0f : conn & 0x40 ? -1.0f : 0.0f, 0, conn & 0x10 ? 1.0f : conn & 0x01 ? -1.0f : 0.0f};
      Vec3 at = c + side * 3.0f;
      std::vector<int> found;
      s.graph.query(it.x - 2, it.z - 2, sx + 4, sz + 4, kPathTram, found);
      int best = -1;
      float bestD = 0;
      for (int n : found) {
        const LaneNode& L = s.graph.nodes[size_t(n)];
        if (!L.railTwin || (L.flags & kPathDeleted) || !within(n)) continue;
        // the way that has the platform on its right (right-hand traffic), when the stop's side is known
        Vec3 dir = L.p1 - L.p0, right{-dir.z, 0, dir.x};
        if ((side.x != 0 || side.z != 0) && dot(right, side) <= 0) continue;
        Vec3 m = (L.p0 + L.p1) * 0.5f - at;
        float d = m.x * m.x + m.z * m.z + 16.0f * m.y * m.y;
        if (best < 0 || d < bestD) best = n, bestD = d;
      }
      // the other way: the opposite lane of the same track (the tram turns round there, reverseConsist)
      if (best >= 0) st.platform = best, st.altPlatform = oppositeNode(s.graph, best);
      if (debugSim() && best >= 0) {
        // how far the platform's lane leads on (successors followed straight on)
        int steps = 0, cur = best;
        while (steps < 200 && !s.graph.nodes[size_t(cur)].next.empty()) cur = s.graph.nodes[size_t(cur)].next[0], ++steps;
        const LaneNode& P = s.graph.nodes[size_t(best)];
        LOG_INFO("sim: tram stop item %d on railway: platform %d (%zu next, %d in, flags %x), other way %d; %d lanes on to %.1f,%.1f",
                 st.item, best, P.next.size(), P.inDegree, P.flags, st.altPlatform, steps, s.graph.nodes[size_t(cur)].p1.x,
                 s.graph.nodes[size_t(cur)].p1.z);
      }
      if (best >= 0) return;
    }
  }
  if (debugSim() && line.type == 1 && simIsTrainStation(g.types[it.type])) {
    const LaneNode* P = st.platform >= 0 ? &s.graph.nodes[size_t(st.platform)] : nullptr;
    LOG_INFO("sim: station item %d (%s) platform %d: %s, rail twin %d, item %d, %zu next, %d in, at %.1f,%.1f -> %.1f,%.1f (centre %.1f,%.1f)",
             st.item, g.types[it.type].c_str(), st.platform, P ? "found" : "none", P ? int(P->railTwin) : -1, P ? P->item : -1,
             P ? P->next.size() : 0, P ? P->inDegree : 0, P ? P->p0.x : 0, P ? P->p0.z : 0, P ? P->p1.x : 0, P ? P->p1.z : 0, c.x, c.z);
  }
  // FUN_10110c10 with alt: the ALTERNATIVE_STOP node, if the stop has one (not another stop's)
  st.altPlatform = s.graph.nearest(c, (mask & ~kPathStop) | kPathAlternativeStop);
  if (st.altPlatform >= 0) {
    Vec3 d = s.graph.nodes[size_t(st.altPlatform)].p0 - c;
    float reach = float(std::max(sx, sz) + 2) * cs;
    if (d.x * d.x + d.z * d.z > reach * reach) st.altPlatform = -1;
  }
  // port: a tram at a train station (its platform a railway track's tram lane) without another platform: the opposite
  // lane of its track, where it turns round
  if (line.type == 1 && st.platform >= 0 && st.altPlatform < 0 && s.graph.nodes[size_t(st.platform)].railTwin)
    st.altPlatform = oppositeNode(s.graph, st.platform);
}

// FUN_10114200: the leg from stop a's platform to stop b's. Metro and boat lines, or a primary platform with
// FIND_ALTERNATIVE, take the cheapest of the primary/alternative pairs; the others primary to primary; a helipad gets
// the "direct" result without a search (pathfinding.md §4.4). Returns the travel cost, 0 when unreachable (as the
// original, which treats a zero cost as no path).
float segmentBetween(SimState& s, const LineState& line, const LineStopState& a, const LineStopState& b, LineStopState* out) {
  bool reachable = false;
  std::vector<int> best;
  float bestCost = 0;
  int legStart = a.platform, legEnd = b.platform;
  if (a.platform >= 0 && b.platform >= 0 && (s.graph.nodes[size_t(a.platform)].flags & 0x1003f940) == 0x4040) {
    Vec3 d = s.graph.nodes[size_t(a.platform)].p1 - s.graph.nodes[size_t(b.platform)].p0;
    reachable = true, bestCost = std::sqrt(dot(d, d)) / 6.0f;
  } else {
    // port: trams at stops on railway tracks take their other platform too (the other way, stopPlatforms)
    auto railStop = [&](int p) { return p >= 0 && s.graph.nodes[size_t(p)].railTwin; };
    bool alt = line.type == 2 || line.type == 3 || (a.platform >= 0 && (s.graph.nodes[size_t(a.platform)].flags & kPathFindAlternative)) ||
               (line.type == 1 && (railStop(a.platform) || railStop(b.platform)));
    std::vector<int> path;
    float cost = 0;
    for (int from : {a.platform, alt ? a.altPlatform : -1})
      for (int to : {b.platform, alt ? b.altPlatform : -1}) {
        if (from < 0 || to < 0 || !s.graph.search(from, to, true, path, cost)) continue;
        if (reachable && cost >= bestCost) continue;
        reachable = true, best = path, bestCost = cost, legStart = from, legEnd = to;
      }
  }
  if (out) {
    out->reachable = reachable;
    out->path = reachable ? best : std::vector<int>();
    out->cost = reachable ? bestCost : 0;
    out->legStart = legStart, out->legEnd = legEnd;
  }
  return reachable ? std::max(bestCost, 1e-6f) : 0.0f;
}

// the segment of stop k to the next stop of the ring; cleared at the end of an open line (FUN_101131f0 with no path)
bool computeSegment(SimState& s, LineState& line, size_t k) {
  size_t n = line.stops.size();
  if (k >= n) return false;
  LineStopState& a = line.stops[k];
  if ((k + 1 == n && !line.closed) || n < 2) {
    a.reachable = false, a.path.clear(), a.cost = 0, a.legStart = a.platform, a.legEnd = -1;
    return false;
  }
  segmentBetween(s, line, a, line.stops[(k + 1) % n], &a);
  return a.reachable;
}

// platforms and segments of a whole line (FUN_10110c10, FUN_10114200); the unreachable count
int relinkLine(VM& vm, GridObj* grid, SimState& s, LineState& line) {
  for (auto& st : line.stops) stopPlatforms(vm, grid, s, line, st);
  int unreachable = 0;
  size_t n = line.stops.size();
  for (size_t k = 0; k < n; ++k)
    if (!computeSegment(s, line, k) && (k + 1 < n || line.closed)) ++unreachable;
  if (static const bool dbg = regs::envVar("REGS_DEBUG_LEGS") != nullptr; dbg)
    for (size_t k = 0; k < n; ++k) {
      const LineStopState& st = line.stops[k];
      LOG_INFO("legs: line type %d stop %zu (item %d): platform %d alt %d; leg %d -> %d, %zu nodes, cost %.0f, %s", line.type, k,
               st.item, st.platform, st.altPlatform, st.legStart, st.legEnd, st.path.size(), st.cost, st.reachable ? "reachable" : "unreachable");
    }
  return unreachable;
}

void loadTransport(VM& vm, GridObj* grid, SimState& s, const std::vector<uint8_t>& file) {
  Reader r{file};
  const GridData& g = *grid->data;
  size_t root = 8, end = std::min(file.size(), size_t(8) + r.u32(4));
  std::vector<SavedAgent> agents;
  std::vector<SavedAgentRef> refs;
  size_t linesBody = 0, linesEnd = 0;
  r.children(root + 24, end, [&](uint32_t tag, size_t body, size_t stop) {
    if (tag == 0x07040000) {
      std::vector<int> models;
      uint32_t modelCount = r.u32(body);
      r.children(body + 8, stop, [&](uint32_t t2, size_t b2, size_t s2) {
        if (t2 == 0x07040100) {
          for (const std::string& n : r.names(b2, s2, modelCount)) models.push_back(modelFor(vm, grid, s, n));
          return;
        }
        if (t2 != 0x07040200) return;
        SavedAgent a;
        uint16_t mi = r.u16(b2);
        a.model = mi < models.size() ? models[mi] : -1;
        a.lengthNodes = r.u16(b2 + 4);
        a.colorSeed = r.u16(b2 + 10);
        a.posOnNode = float(int32_t(r.u32(b2 + 12))) / 65536.0f;
        a.nextCar = r.u32(b2 + 16);
        a.prevCar = r.u32(b2 + 20);
        a.speed = r.f32(b2 + 24);
        a.stepsLeft = int32_t(r.u32(b2 + 60));
        a.flags = r.u32(b2 + 64);
        a.curNode = r.u32(b2 + 68);
        a.prevNode = r.u32(b2 + 72);
        a.crashOffset = {r.f32(b2 + 76), r.f32(b2 + 80), r.f32(b2 + 84)};
        uint16_t count = r.u16(b2 + 8);
        for (uint16_t i = 0; i < count && b2 + 88 + size_t(i) * 4 + 4 <= s2; ++i) a.ring.push_back(r.u32(b2 + 88 + size_t(i) * 4));
        r.children(b2 + 88 + size_t(count) * 4, s2, [&](uint32_t t3, size_t b3, size_t s3) {
          if (t3 == 0x07030200 && s3 - b3 >= 84) {  // vehicles-movement.md §2.6
            LaneNode n;
            n.flags = r.u32(b3);
            n.p0 = {r.f32(b3 + 12), r.f32(b3 + 16), r.f32(b3 + 20)};
            render::Quat q0{r.f32(b3 + 24), r.f32(b3 + 28), r.f32(b3 + 32), r.f32(b3 + 36)};
            n.p1 = {r.f32(b3 + 40), r.f32(b3 + 44), r.f32(b3 + 48)};
            render::Quat q1{r.f32(b3 + 52), r.f32(b3 + 56), r.f32(b3 + 60), r.f32(b3 + 64)};
            n.speedLimit = r.f32(b3 + 72);
            n.curveFactor = r.f32(b3 + 76);
            // forward = q·(0, 0, −1) in the engine's convention; the chord where that does not point along it
            Vec3 chord = render::normalize(n.p1 - n.p0);
            n.f0 = render::rotate(render::conjugate(q0), Vec3{0, 0, -1});
            n.f1 = render::rotate(render::conjugate(q1), Vec3{0, 0, -1});
            if (dot(n.f0, chord) < 0.2f) n.f0 = chord;
            if (dot(n.f1, chord) < 0.2f) n.f1 = chord;
            a.inlineNodes.push_back(n);
            return;
          }
          if (t3 != 0x07030100) return;
          a.nibbles = int32_t(r.u32(b3 + 4));
          a.cursor = r.u32(b3 + 12);
          for (size_t p = b3 + 20; p + 4 <= s3; p += 4) a.steps.push_back(r.u32(p));
        });
        agents.push_back(std::move(a));
      });
    } else if (tag == 0x07060000) {
      linesBody = body;
      linesEnd = stop;
    }
  });

  // the stop vector: a controller per stop item
  for (size_t i = 0; i < g.items.size(); ++i) registerStop(vm, grid, s, i);

  if (linesBody) {
    uint32_t modelCount = r.u32(linesBody);
    std::vector<std::string> modelNames;
    ClassInfo* lineClass = vm.findClass("gsTransportLine");
    ClassInfo* vehicleClass = vm.findClass("gsTransportVehicle");
    r.children(linesBody + 16, linesEnd, [&](uint32_t tag, size_t body, size_t stop) {
      if (tag == 0x07060100) {
        modelNames = r.names(body, stop, modelCount);
        return;
      }
      if (tag != 0x07060200 || !lineClass) return;
      LineState line;
      line.headway = r.f32(body);
      line.type = int(r.u32(body + 4));
      line.effAttractiveness = r.f32(body + 8);
      line.avgWait = r.f32(body + 20);
      line.closed = file[body + 32] != 0;
      int li = int(s.lines.size());
      line.object = vm.newObject(lineClass, s.linesModule.get());
      if (auto* to = transport(line.object.get())) to->index = li;
      Object* lo = line.object.get();
      lo->fieldValues[u"type"] = Value::integer(line.type);
      lo->fieldValues[u"ticketPrice"] = Value::real(r.f32(body + 24));
      lo->fieldValues[u"passengersTransported"] = Value::integer(int32_t(r.u32(body + 28)));
      lo->fieldValues[u"attractiveness"] = Value::real(1.0f);
      struct SavedVehicle {
        uint32_t agent, stopA, stopB, docked;
        float stopTime, attractiveness, travelDistance;
        int passengers, transported;
        bool broken;
        std::vector<Unit> units;
      };
      std::vector<SavedVehicle> saved;
      r.children(body + 36, stop, [&](uint32_t t2, size_t b2, size_t s2) {
        if (t2 == 0x07060220) {
          LineStopState st;
          uint32_t item = r.u32(b2);
          st.item = item >= 1 && item <= g.items.size() ? int(item) - 1 : -1;
          st.sinceDeparture = r.f32(b2 + 8);
          st.passengersTransported = int32_t(r.u32(b2 + 12));
          st.waitingTime = int32_t(r.u32(b2 + 16));
          if (auto k = s.stopOfItem.find(st.item); k != s.stopOfItem.end()) st.stop = s.stops[size_t(k->second)];
          line.stops.push_back(std::move(st));
        } else if (t2 == 0x07060210) {
          SavedVehicle sv;
          sv.agent = r.u32(b2);
          sv.stopA = r.u32(b2 + 4);
          sv.stopB = r.u32(b2 + 8);
          sv.docked = r.u32(b2 + 12);
          sv.stopTime = r.f32(b2 + 16);
          sv.passengers = int32_t(r.u32(b2 + 32));
          sv.attractiveness = r.f32(b2 + 40);
          sv.travelDistance = r.f32(b2 + 44);
          sv.broken = file[b2 + 55] != 0;
          sv.transported = int32_t(r.u32(b2 + 56));
          uint32_t units = r.u32(b2 + 48);
          for (uint32_t u = 0; u < units && b2 + 60 + size_t(u) * 4 + 4 <= s2; ++u) {
            uint16_t mi = r.u16(b2 + 60 + size_t(u) * 4);
            Unit un;
            un.model = mi < modelNames.size() ? modelFor(vm, grid, s, modelNames[mi]) : -1;
            un.reversed = u > 0 && file[b2 + 60 + size_t(u) * 4 + 2] != 0;
            sv.units.push_back(un);
          }
          saved.push_back(std::move(sv));
        }
      });
      lo->fieldValues[u"stopCount"] = Value::integer(int(line.stops.size()));
      lo->fieldValues[u"isClosed"] = Value::boolean(line.closed);

      int unreachable = relinkLine(vm, grid, s, line);
      size_t n = line.stops.size();
      LOG_INFO("transport line %d (type %d): %zu stops, %zu vehicles, %d segments unreachable", li, line.type, n, saved.size(),
               unreachable);

      // vehicles: children of the line, placed where the save left their head agent
      for (const SavedVehicle& sv : saved) {
        if (!vehicleClass) break;
        int vi = int(s.vehicles.size());
        VehicleState v;
        v.line = li;
        v.units = sv.units;
        v.stopTime = sv.broken ? sv.stopTime : std::max(0.0f, sv.stopTime);  // broken: −1..0 doors, then the repair
        v.attractiveness = sv.attractiveness;
        v.travelDistance = sv.travelDistance;
        v.passengerCount = sv.passengers;
        v.passengersTransported = sv.transported;
        v.broken = sv.broken;
        for (const Unit& u : v.units) v.capacity += u.model >= 0 ? s.models[size_t(u.model)].capacity : 0;
        v.object = vm.newObject(vehicleClass, lo);
        if (auto* to = transport(v.object.get())) to->index = vi;
        Object* vo = v.object.get();
        vo->fieldValues[u"line"] = Value::heap(VT::Object, lo);
        vo->fieldValues[u"condition"] = Value::real(1.0f);
        vo->fieldValues[u"attractiveness"] = Value::real(v.attractiveness);
        v.mover = newMover(s);
        s.movers[size_t(v.mover)].transit = true;
        s.movers[size_t(v.mover)].vehicle = vi;
        line.vehicles.push_back(vi);
        s.vehicles.push_back(std::move(v));
        VehicleState& vs = s.vehicles.back();
        int a = int(sv.stopA) - 1;
        if (sv.agent >= 1 && sv.agent <= agents.size() && a >= 0 && size_t(a) < n && line.stops[size_t(a)].reachable) {
          const SavedAgent& ag = agents[sv.agent - 1];
          spawnLineVehicle(s, vs, line, a);
          Mover& m = s.movers[size_t(vs.mover)];
          // the head's node on the leg; the agent's rear point is posOnNode into it
          int cur = s.graph.resolve(ag.curNode);
          for (size_t k = 0; k < m.track.nodes.size(); ++k)
            if (m.track.nodes[k] == cur) {
              m.dist = m.track.start[k] + std::max(0.0f, ag.posOnNode);
              break;
            }
          m.speed = ag.speed;
          vs.halted = (ag.flags & 0x400) != 0;
          vs.brokenHalted = vs.broken && (ag.flags & 0x10000) != 0;
          if (sv.docked) line.stops[size_t(vs.stopB)].docked = vi;
        }
      }
      line.firstNode = int(s.lineNodes.size());
      for (size_t k = 0; k < line.stops.size(); ++k) s.lineNodes.push_back({int(s.lines.size()), int(k)});
      s.lines.push_back(std::move(line));
    });
  }
  // line vehicles claim first: they were spawned before the traffic around them
  for (const VehicleState& v : s.vehicles)
    if (v.mover >= 0 && s.movers[size_t(v.mover)].live) claimBody(s, v.mover);
  loadTraffic(vm, s, agents, refs);
  for (const SavedAgentRef& ref : refs) s.loadedAgentMover.push_back(ref.mover);  // for sim_save.cpp's pass-through
  loadCitizens(vm, grid, s, file, refs);
}

}  // namespace sim

// ---------------------------------------------------------------------------------------------------------------
// hooks

void loadSimulation(VM& vm, GridObj* grid, const std::vector<uint8_t>& file) {
  if (!grid || !grid->data) return;
  if (const char* off = regs::envVar("REGS_NO_SIM"); off && *off && *off != '0') return;  // debugging: no simulation
  // the old state's script objects leave the lines module and the grid
  if (grid->sim) {
    grid->sim->tearingDown = true;
    for (auto& l : grid->sim->lines)
      if (l.object && !l.object->destroyed) vm.destroy(l.object.get());
    for (auto& m : grid->sim->movers)
      if (m.controller && !m.controller->destroyed) vm.destroy(m.controller.get());
    for (auto& st : grid->sim->stops)
      if (st && !st->destroyed) vm.destroy(st.get());
  }
  auto s = std::make_shared<SimState>();
  s->grid = grid;
  s->vm = &vm;
  for (auto [field, slot] : {std::pair{u"lines", &s->linesModule}, std::pair{u"vehicles", &s->vehiclesModule},
                             std::pair{u"citizens", &s->citizensModule}, std::pair{u"characters", &s->charactersModule}})
    if (auto f = grid->cls->fields.find(field); f != grid->cls->fields.end() && f->second.get)
      *slot = Ref<Object>(f->second.get(vm, grid).object());
  // the terrain flags as stored: the water lanes come from them before the dry-out (0x100c3310.c:119)
  std::vector<uint16_t> fileFlags;
  if (file.size() >= 32) {
    Reader r{file};
    r.children(32, std::min(file.size(), size_t(8) + r.u32(4)), [&](uint32_t tag, size_t body, size_t stop) {
      if (tag != 0x07010000) return;
      size_t n = size_t(grid->data->width) * size_t(grid->data->length);
      if (stop - body < n * 4) return;
      fileFlags.resize(n);
      for (size_t i = 0; i < n; ++i) fileFlags[i] = r.u16(body + i * 4 + 2);
    });
  }
  buildLaneGraph(vm, grid, *grid->data, s->graph, fileFlags.empty() ? nullptr : &fileFlags);
  rebuildCurveNodes(vm, grid, *grid->data, s->graph);  // port curved roads: their lanes after the items'
  s->rt.assign(s->graph.nodes.size(), NodeRt());
  collectSources(*s);
  buildDoors(vm, grid, *s);
  gridDebugConflicts(vm, grid);
  grid->sim = s;
  loadTransport(vm, grid, *s, file);
  for (auto& l : s->lines)
    if (auto* t = transport(l.object.get())) t->sim = s;
  for (auto& v : s->vehicles)
    if (auto* t = transport(v.object.get())) t->sim = s;
  for (auto& st : s->stops)
    if (auto* t = transport(st.get())) t->sim = s;
  syncVehicleFields(*s);
  if (debugSim())  // which tram stops a bus line may take (docs/ui/lines.md §6)
    for (const auto& [item, si] : s->stopOfItem) {
      if (fieldInt(vm, s->stops[size_t(si)].get(), u"type", -1) != 1) continue;
      const auto& it = grid->data->items[size_t(item)];
      int lane = simBusPlatformAtTramStop(vm, grid, item);
      LOG_INFO("sim: tram stop item %d (%s at %d,%d): %s", item, grid->data->types[it.type].c_str(), it.x, it.z,
               lane >= 0 ? ("buses halt on lane " + std::to_string(lane)).c_str() : "no road lane at the platform, not for buses");
    }
}

// gsGridVehicle::move (L:0x081adca0.c:604-690): the spin of a vehicle's animated parts, written to custom4 of its
// meshes (propeller.shader turns rotors and propellers by custom4.x · custom0.w and fades the blades into the blur disk
// by custom4.y; paddle.shader the paddle wheels). Vehicles with an anchor ≥ 0.11 (ground vehicles, boats) spin with
// their speed; aircraft spin up by 1/s to 12 while moving and run down by half per second while halted at a stop,
// broken down or crashing (agent flags 0x50400). The angle advances by dt · r.
void stepAnimatedParts(SimState& s, float dt) {
  for (Mover& m : s.movers) {
    if (!m.live || m.parked || m.units.empty() || m.units[0].model < 0) continue;
    const UnitModel& um = s.models[size_t(m.units[0].model)];
    float r;
    if (um.frontAnchor >= 0.11f || um.rearAnchor >= 0.11f) {
      r = m.speed <= 6.0f ? std::min(2.0f * m.speed, 6.0f) : 2.0f * m.speed - 6.0f;
    } else {
      bool halted = m.crashing;
      const VehicleState* lv = m.transit && m.vehicle >= 0 && size_t(m.vehicle) < s.vehicles.size() ? &s.vehicles[size_t(m.vehicle)] : nullptr;
      if (lv) halted = halted || lv->halted || lv->brokenHalted;
      // port change (boats-aircraft.md §6.3): a helicopter line vehicle halted at its pad winds the rotor down to a
      // standstill in 2 s (6/s) and boards with it still; then it spins up at 3/s and takes off at 12 (stepVehicle)
      bool heliAtPad = lv && lv->halted && !lv->brokenHalted && !m.crashing && lv->line >= 0 &&
                       size_t(lv->line) < s.lines.size() && s.lines[size_t(lv->line)].type == 4;
      if (heliAtPad) r = lv->spinUp ? std::min(std::max(m.rotorSpin, 0.0f) + 3.0f * dt, 12.0f) : std::max(0.0f, m.rotorSpin - 6.0f * dt);
      else if (!halted) r = m.rotorSpin < 0 ? 12.0f : std::min(m.rotorSpin + dt, 12.0f);
      else r = m.rotorSpin < 0 ? 0.0f : m.rotorSpin * std::pow(0.5f, dt);
    }
    m.rotorAngle += dt * r;
    m.rotorSpin = r;
  }
}

void tickSimulation(VM& vm, GridObj* grid, float dt) {
  SimState* s = simOf(grid);
  if (!s || dt <= 0) return;
  float x = dt * fieldFloat(vm, grid, u"timeSpeed", 1.0f);
  if (x <= 0) {  // game speed 0: the grid still ticks with dt = 0 (loops keep playing, docs/audio/README.md §14)
    stepDoors(*s, 0);  // the item update runs with step 0 too
    simSoundsTick(vm, grid, *s, 0);
    return;
  }
  int n = 1 + int(5.0f * x);  // 1 − ftol(dt·timeSpeed·−5): steps of at most 0.2 s
  float step = x / float(n);
  // debugging: REGS_DEBUG_FOLLOW=<line type>[:<n>] keeps the camera (ui.setCameraPos) on the n-th vehicle of that type
  if (const char* f = regs::envVar("REGS_DEBUG_FOLLOW")) {
    int type = std::atoi(f), nth = std::strchr(f, ':') ? std::atoi(std::strchr(f, ':') + 1) : 0;
    for (const LineState& line : s->lines)
      for (int vi : line.vehicles) {
        const VehicleState& v = s->vehicles[size_t(vi)];
        if (line.type != type || v.mover < 0 || !s->movers[size_t(v.mover)].live || nth-- > 0) continue;
        const Mover& m = s->movers[size_t(v.mover)];
        Vec3 p = unitPose(*s, m, m.units[0], m.dist).position;
        s->followed = vi;
        try {  // the ui script may not be there yet
          if (Value* main = vm.findGlobal(u"MAIN"))
            vm.call(vm.getMember(vm.getMember(*main, u"$gameUI"), u"setCameraPos"), {Value::real(p.x), Value::real(p.z), Value::boolean(true)});
        } catch (...) {
        }
        type = -1;
      }
  }
  // debugging: REGS_DEBUG_CAMERA=<item> puts the camera on that item once, 30 ticks into the game (for scripted clicks)
  if (const char* c = regs::envVar("REGS_DEBUG_CAMERA"); c && *c) {
    static int ticks = 0;
    int item = std::atoi(c);
    if (++ticks == 30 && item >= 0 && size_t(item) < grid->data->items.size()) {
      Vec3 p = itemCentre(vm, grid, item);
      try {
        if (Value* main = vm.findGlobal(u"MAIN"))
          vm.call(vm.getMember(vm.getMember(*main, u"$gameUI"), u"setCameraPos"), {Value::real(p.x), Value::real(p.z), Value::boolean(true)});
      } catch (...) {
      }
    }
  }
  // test harness: REGS_TEST_TERMINUS=<line> removes (gridEditRemoveItem) the stop items of that line except its first
  // two after 300 ticks, so the line's trains must turn round at both ends (transport.md §4.4)
  if (const char* t = regs::envVar("REGS_TEST_TERMINUS")) {
    static int ticks = 0;
    size_t li = size_t(std::atoi(t));
    if (++ticks == 300 && li < s->lines.size() && s->lines[li].stops.size() > 2) {
      std::vector<int> items;
      for (size_t k = 2; k < s->lines[li].stops.size(); ++k) items.push_back(s->lines[li].stops[k].item);
      gridEditBegin(vm, grid);
      for (int item : items)
        if (item >= 0) gridEditRemoveItem(vm, grid, uint32_t(item));
      gridEditEnd(vm, grid);
      LOG_INFO("sim: test terminus: line %zu keeps %zu stops", li, s->lines[li].stops.size());
    }
  }
  // test harness: REGS_TEST_LINE=<type> (0 bus, 1 tram, 2 metro) 60 ticks into the game: a new line of that type (the
  // line panel's createLine) through every stop of it on the map (bus stops, tram stops, metro stations by template
  // name, in map order), with one vehicle of REGS_TEST_LINE_MODEL (default bus01 / tram-01 / metro-01), switched on
  if (const char* t = regs::envVar("REGS_TEST_LINE")) {
    static int ticks = 0;
    if (++ticks == 60) {
      int type = std::atoi(t);
      static const char* kPrefix[] = {"stop_bus", "stop_tram", "station_metro"};
      static const char* kModel[] = {"bus01", "tram-01", "metro-01"};  // the game's own names (buses without a dash)
      const char* model = regs::envVar("REGS_TEST_LINE_MODEL");
      if (!model) model = kModel[std::clamp(type, 0, 2)];
      try {
        Value* main = vm.findGlobal(u"MAIN");
        Value panel = main ? vm.getMember(*main, u"$linepanel") : Value();
        Value line = vm.call(vm.getMember(panel, u"createLine"), {Value::integer(type)});
        int li = int(s->lines.size()) - 1;
        int added = 0;
        for (const auto& [item, stop] : s->stopOfItem) {
          const GridData& g = *grid->data;
          if (size_t(item) >= g.items.size() || g.items[size_t(item)].removed) continue;
          const std::string& name = g.types[g.items[size_t(item)].type];
          // trams also take the train stations (trams on railway tracks); REGS_TEST_LINE_STATIONS=1: only those,
          // REGS_TEST_LINE_ITEMS=a,b,...: only those items, in that order (added after this loop)
          const bool station = type == 1 && simIsTrainStation(name);
          if (regs::envVar("REGS_TEST_LINE_ITEMS")) continue;
          if (regs::envVar("REGS_TEST_LINE_STATIONS") && !station) continue;
          if (name.rfind(kPrefix[std::clamp(type, 0, 2)], 0) != 0 && !station) continue;
          if (lineAddStop(vm, grid, *s, li, item, false) >= 0) ++added;
        }
        if (const char* items = regs::envVar("REGS_TEST_LINE_ITEMS")) {
          int first = -1;
          for (const char* p = items; *p;) {
            int item = std::atoi(p);
            if (first < 0) first = item;
            if (lineAddStop(vm, grid, *s, li, item, false) >= 0) ++added;
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
          }
          if (first >= 0) lineAddStop(vm, grid, *s, li, first, false);  // the first again: the line becomes a loop
        }
        // REGS_TEST_LINE_STATIONS with REGS_DEBUG_SIM: which train stations' platforms reach which (trams)
        if (regs::envVar("REGS_TEST_LINE_STATIONS") && debugSim()) {
          std::vector<std::pair<int, int>> plat;  // item, platform
          for (const auto& [item, stop] : s->stopOfItem) {
            const GridData& g = *grid->data;
            if (size_t(item) >= g.items.size() || !simIsTrainStation(g.types[g.items[size_t(item)].type])) continue;
            LineState tmp;
            tmp.type = 1;
            LineStopState st;
            st.item = item;
            stopPlatforms(vm, grid, *s, tmp, st);
            if (st.platform >= 0) plat.push_back({item, st.platform});
          }
          int pairs = 0;
          std::string list;
          for (auto& a : plat)
            for (auto& b : plat) {
              std::vector<int> path;
              float cost = 0;
              if (a.first != b.first && s->graph.search(a.second, b.second, true, path, cost)) {
                ++pairs;
                if (pairs <= 12) list += " " + std::to_string(a.first) + ">" + std::to_string(b.first) + "(" + std::to_string(int(cost)) + ")";
              }
            }
          LOG_INFO("test: %zu station platforms, %d reachable pairs:%s", plat.size(), pairs, list.c_str());
        }
        Value tpl = vm.call(vm.getMember(vm.getMember(*main, u"$grid"), u"getGridObject"), {Value::string(utf8ToUtf16(model))});
        vm.call(vm.getMember(line, u"addVehicle"), {tpl});
        vm.setMember(line, u"active", Value::boolean(true));
        LOG_INFO("test: line %d (type %d) with %d stops and a %s", li, type, added, model);
        if (regs::envVar("REGS_DEBUG_LEGS") && li >= 0 && size_t(li) < s->lines.size()) relinkLine(vm, grid, *s, s->lines[size_t(li)]);
      } catch (const std::exception& e) {
        LOG_WARN("test: line failed: %s", e.what());
      }
    }
  }
  // debugging: REGS_DEBUG_SAVE_AT=<ticks>:<name> saves the game (main.saveGameData) after that many simulation ticks
  if (const char* at = regs::envVar("REGS_DEBUG_SAVE_AT")) {
    static int ticks = 0;
    const char* colon = std::strchr(at, ':');
    if (colon && ++ticks == std::atoi(at))
      if (Value* main = vm.findGlobal(u"MAIN")) {
        auto t0 = std::chrono::steady_clock::now();
        vm.call(vm.getMember(vm.getMember(*main, u"$main"), u"saveGameData"), {Value::string(utf8ToUtf16(colon + 1))});
        LOG_INFO("test: saveGameData(\"%s\") took %.0f ms on the main thread", colon + 1,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        // what the load list sees of it at once (the package may still be written in the background)
        std::string dir = std::string("userdata/saves/") + (colon + 1);
        std::vector<uint8_t> campaign;
        bool listed = false;
        for (const std::string& d : vm.vfs().listDirs(dir.substr(0, dir.rfind('/')))) listed = listed || d == dir.substr(dir.rfind('/') + 1);
        bool reads = vm.vfs().read(dir + "/game.campaign", campaign);
        LOG_INFO("test: right after the save: listed %s, %s/game.campaign %s (%zu bytes)%s", listed ? "yes" : "no", dir.c_str(),
                 reads ? "reads" : "missing", campaign.size(),
                 vm.vfs().writesPending() ? ", package still being written" : "");
      }
  }
  // test harness: REGS_TEST_SCENARIO_CALL=<ticks>:<function>[,<function>…] calls those scenario functions through
  // MAIN.$scenario.callFunction(name, 0) (scenario, location and default layers) after that many simulation ticks,
  // e.g. start_sandbox_task90 to start a random task at once (playable-todo.md §5)
  if (const char* at = regs::envVar("REGS_TEST_SCENARIO_CALL")) {
    static int ticks = 0;
    const char* colon = std::strchr(at, ':');
    if (colon && ++ticks == std::atoi(at))
      if (Value* main = vm.findGlobal(u"MAIN")) {
        std::string list = colon + 1;
        for (size_t p = 0; p <= list.size();) {
          size_t q = list.find(',', p);
          if (q == std::string::npos) q = list.size();
          std::string name = list.substr(p, q - p);
          p = q + 1;
          if (name.empty()) continue;
          int arg = 0;  // name=arg passes an integer argument (default 0)
          if (size_t eq = name.find('='); eq != std::string::npos) {
            arg = std::atoi(name.c_str() + eq + 1);
            name.resize(eq);
          }
          LOG_INFO("test: calling scenario function %s(%d)", name.c_str(), arg);
          try {
            vm.call(vm.getMember(vm.getMember(*main, u"$scenario"), u"callFunction"),
                    {Value::string(utf8ToUtf16(name)), Value::integer(arg)});
          } catch (...) {
            LOG_WARN("test: %s failed", name.c_str());
          }
        }
      }
  }
  stepJamTest(vm, *s, x);
  for (int i = 0; i < n; ++i) {
    stepDoors(*s, step);  // the active buckets' item updates come before the children (grid tick :231-276)
    stepCitizens(vm, *s, step);
    stepTraffic(vm, *s, step);
    for (auto& line : s->lines)
      if (lineActive(vm, line)) stepLine(vm, *s, line, step);
  }
  stepAnimatedParts(*s, x);
  syncVehicleFields(*s);
  syncCharacterFields(*s);
  simSoundsTick(vm, grid, *s, x);
  simParticlesTick(vm, grid, *s, x);
  stepSnowplows(vm, grid, *s, x);       // the mods' snowplows (engine/snowplows.h)
  simTrafficMapsTick(vm, grid, *s, x);  // the mods' shader hooks: recent traffic (engine/traffic_maps.h)
  if (debugSim()) {
    static int frames = 0;
    if (++frames % 600 == 0) {
      // overlap check: units of different vehicles whose bodies intersect (centres closer than their half lengths and
      // widths along a common heading)
      struct Body {
        const Mover* m;
        Vec3 c, f;
        float half;
      };
      std::vector<Body> bodies;
      forEachUnit(*s, [&](const Mover& m, int, const Unit& u, const PlacedPose& p) {
        if (m.parked) return;
        const UnitModel& um = s->models[size_t(u.model)];
        bodies.push_back({&m, p.position + p.forward * ((um.frontOffset - um.rearOffset) * 0.5f), p.forward, (um.frontOffset + um.rearOffset) * 0.5f});
      });
      int overlaps = 0;
      for (size_t i = 0; i < bodies.size(); ++i)
        for (size_t j = i + 1; j < bodies.size(); ++j) {
          if (bodies[i].m == bodies[j].m) continue;
          Vec3 d = bodies[j].c - bodies[i].c;
          if (std::fabs(d.y) > 2 || std::fabs(d.x) > 12 || std::fabs(d.z) > 12) continue;
          float along = std::fabs(dot(d, bodies[i].f)), side = std::sqrt(std::max(0.0f, dot(d, d) - along * along));
          if (along < (bodies[i].half + bodies[j].half) * 0.8f && side < 1.2f) ++overlaps;
        }
      LOG_INFO("sim: traffic %d road (%d citizen cars, avg %.0f; %d background at density %.2f), %d trains, %d boats (cap %d), %d aircraft (runway entries %zu, gates %zu/%zu, exits %zu); %d spawned, %d despawned (%d stuck); %d overlapping pairs",
               s->roadLive, s->roadLive - s->extraLive, s->citizenCarsAvg, s->extraLive, trafficDensity(*s), s->trainsLive, s->boatsLive, int(std::sqrt(float(s->waterStarts.size()))), s->aircraftLive, s->airEntries.size(), s->airGatesIn.size(), s->airGatesOut.size(), s->airExits.size(), s->spawned, s->despawned, s->stuck, overlaps);
      if (g_railShareWaits) LOG_INFO("sim: trams and trains waited for each other on shared track %d times", g_railShareWaits);
      for (const Mover& m : s->movers)
        if (m.live && m.aircraft) {
          Vec3 p = unitPose(*s, m, m.units[0], m.dist).position;
          LOG_INFO("sim: aircraft at (%.0f, %.0f, %.0f), speed %.1f, %zu of %zu nodes left", p.x, p.y, p.z, m.speed,
                   m.track.nodes.size() - 1 - m.track.at(m.dist), m.track.nodes.size());
        }
      int markers = 0, waitOcc = 0, waitCross = 0, waitMark = 0, waitOther = 0;
      for (const NodeRt& r : s->rt) markers += r.marker;
      for (size_t id = 0; id < s->movers.size(); ++id) {
        const Mover& m = s->movers[id];
        if (!m.live || m.parked || m.waitTime < 10 || m.holdEnd > lastUsable(m)) continue;
        int nd = m.track.nodes[size_t(m.holdEnd)];
        const NodeRt& r = s->rt[size_t(nd)];
        bool cross = false, mark = r.marker && !(s->graph.nodes[size_t(nd)].flags & kPathLaneChange);
        for (int c : s->graph.nodes[size_t(nd)].crossings) {
          cross = cross || (s->rt[size_t(c)].occupant >= 0 && s->rt[size_t(c)].occupant != int(id));
          mark = mark || s->rt[size_t(c)].marker;
        }
        if (r.occupant >= 0 && r.occupant != int(id)) ++waitOcc;
        else if (cross) ++waitCross;
        else if (mark) ++waitMark;
        else ++waitOther;
      }
      int states[5] = {}, real = 0, virt = 0, pop = 0;
      for (const Citizen& c : s->citizens)
        if (c.alive) ++pop, ++states[std::min(4u, (c.flags & 0xf0) >> 4)];
      int parkedNow = 0;
      for (const Mover& m : s->movers) parkedNow += m.live && m.parked;
      int walking = 0, birds = 0, beasts = 0;
      for (const Character& ch : s->characters)
        if (ch.alive && ch.animal) ++(ch.animal == 1 ? birds : beasts);
        else if (ch.alive) ++(ch.virtualLeft > 0 ? virt : real), walking += ch.virtualLeft < 0 && !ch.done();
      std::string riders;
      for (const VehicleState& v : s->vehicles) riders += " " + std::to_string(v.passengerCount) + "/" + std::to_string(v.capacity);
      int reserved = 0;
      for (const VehicleState& v : s->vehicles) reserved += v.boardingReserved;
      LOG_INFO("sim: citizens %d (idle %d, walk %d, drive %d, travel %d, other %d); %d trips, %d boarded (%d door walks, %d "
               "now reserved), %d alighted, %d cars; "
               "pedestrians %d drawn + %d off screen; passengers%s; parked: %d now, %d parked, %d taken again; %d utility trips; "
               "%d breakdowns, %d evacuated, %d replanned; animals %d flying, %d on the ground (%zu spawn points)",
               pop, states[0], states[1], states[2], states[3], states[4], s->trips, s->boarded, s->doorWalks, reserved,
               s->alighted, s->carsSpawned, real, virt,
               riders.c_str(), parkedNow, s->carsParked, s->parkedReused, s->utilityTrips, s->breakdowns, s->evacuated, s->replanned,
               birds, beasts, s->spawnPoints.size());
      {
        int onWater = 0, onBlocked = 0, shown = 0;
        const GridData& g = *s->grid->data;
        for (const Character& ch : s->characters) {
          if (!ch.alive || ch.virtualLeft > 0 || ch.animal) continue;
          ++shown;
          int qx = int(std::floor((ch.pos.x / g.cellSize + float(g.width) * 0.5f) * 4)), qz = int(std::floor((ch.pos.z / g.cellSize + float(g.length) * 0.5f) * 4));
          int cx = std::clamp(qx >> 2, 0, g.width - 1), cz = std::clamp(qz >> 2, 0, g.length - 1);
          uint32_t at = ch.path.empty() ? 0 : ch.path[std::min(ch.next, ch.path.size() - 1)];
          qz |= int(at >> 16) & ~kQzMask;  // the layer it walks on: decks over water are fine
          if ((qz >> kLayerShift) == 0 && (g.flags[size_t(cz) * size_t(g.width) + size_t(cx)] & 0x2000)) {
            ++onWater;
            const Citizen* cz2 = ch.citizen >= 0 ? &s->citizens[size_t(ch.citizen)] : nullptr;
            LOG_INFO("sim: over water at (%.1f, %.1f, %.1f) q (%d, %d) citizen %d state %x path %zu/%zu", ch.pos.x, ch.pos.y, ch.pos.z, qx, qz,
                     ch.citizen, cz2 ? cz2->flags : 0u, ch.next, ch.path.size());
          }
          if (s->walk.code(qx, qz) == 0) ++onBlocked;
        }
        LOG_INFO("sim: pedestrians shown %d: %d over water cells, %d on unwalkable quarter cells", shown, onWater, onBlocked);
      }
      LOG_INFO("sim: %d pedestrians walking a path; %d paths (%d failed: %d at the search cap, %d without a connection), "
               "path q-cells: %d pavement, %d crossing, %d open, %d blocked",
               walking, s->walkPaths, s->walkFailed, s->walkFailKind[0], s->walkFailKind[1], s->walkCodes[2], s->walkCodes[1],
               s->walkCodes[3], s->walkCodes[0]);
      LOG_INFO("sim: %d marked nodes; waiting > 10 s: %d behind a vehicle, %d at a crossing, %d at a marker, %d before a junction", markers,
               waitOcc, waitCross, waitMark, waitOther);
      for (const DoorItem& dr : s->doors) {  // door items near the camera: who holds their track nodes, who waits
        if (!dr.active) continue;
        std::string held, waiting;
        for (int n : dr.nodes) {
          int o = s->rt[size_t(n)].occupant;
          if (o < 0) continue;
          const Mover& m = s->movers[size_t(o)];
          char b[96];
          std::snprintf(b, sizeof b, " %d(%s%s v%.1f wait %.0f)", o, m.train ? "train" : "car", m.live ? "" : " dead", m.speed, m.waitTime);
          held += b;
        }
        for (size_t id = 0; id < s->movers.size(); ++id) {
          const Mover& m = s->movers[id];
          if (!m.live || m.parked || m.train || m.holdEnd > lastUsable(m) || m.speed > 0.1f) continue;
          int nd = m.track.nodes[size_t(m.holdEnd)];
          if (std::find(dr.nodes.begin(), dr.nodes.end(), nd) != dr.nodes.end()) continue;
          for (int c : s->graph.nodes[size_t(nd)].crossings)
            if (std::find(dr.nodes.begin(), dr.nodes.end(), c) != dr.nodes.end()) {
              waiting += " " + std::to_string(id);
              break;
            }
        }
        LOG_INFO("sim: door item %u phase %.2f clip %.2f; track held by%s; cars waiting:%s", dr.item, dr.phase, dr.time,
                 held.empty() ? " nobody" : held.c_str(), waiting.empty() ? " none" : waiting.c_str());
      }
    }
  }
}

// vehicles on removed lane nodes: traffic despawns, a citizen's car ends its drive, parked cars go, line vehicles
// leave the line and come back at a platform; the nodes' run-time state is cleared. Returns how many were taken off.
int takeOffRemovedNodes(VM& vm, SimState& s, const std::vector<uint8_t>& nodeGone) {
  int despawned = 0;
  for (size_t id = 0; id < s.movers.size(); ++id) {
    Mover& m = s.movers[id];
    if (!m.live) continue;
    bool hit = false;
    for (size_t k = size_t(std::max(0, m.holdBegin)); k < m.track.nodes.size() && !hit; ++k) hit = nodeGone[size_t(m.track.nodes[k])] != 0;
    if (!hit) continue;
    ++despawned;
    if (m.transit) {
      VehicleState& v = s.vehicles[size_t(m.vehicle)];
      for (auto& st : s.lines[size_t(v.line)].stops)
        if (st.docked == m.vehicle) st.docked = -1;
      vm.fireEvent(v.object.get(), u"onUnspawn", {});
      despawn(vm, s, int(id));
      v.stopTime = 0;
      v.halted = v.brokenHalted = false;
    } else if (m.parked) {
      if (m.citizen >= 0) {
        Citizen& c = s.citizens[size_t(m.citizen)];
        if (c.car == int(id)) c.car = -1, c.flags &= ~0x100000u;
      }
      m.citizen = -1;
      despawn(vm, s, int(id));
    } else if (m.citizen >= 0) {
      carArrived(vm, s, int(id), true);
    } else {
      despawn(vm, s, int(id));
    }
  }
  for (size_t n = 0; n < nodeGone.size(); ++n)
    if (nodeGone[n]) s.rt[n] = NodeRt();
  return despawned;
}

// cell rectangles (inclusive) merged into their bounding box while two lie within gap cells of each other (in x and
// in z), until no two do; the result in the order of each group's first rectangle. The same as merging the first
// such pair over and over (the merged box in the first one's place): boxes only grow and nearness only grows with
// them, so every order of merges ends in the same groups (a group of one order never spans two groups of a finished
// other order, whose boxes would then be near), and that loop keeps each group in its first member's place. Here a
// round puts every group into the buckets (8² cells) its box widened by gap to the right and below covers (two near
// boxes share a cell of those), unites the near ones met in a bucket, and repeats until a round unites none: about
// linear instead of the loop's restart after every merge (minutes after a large import).
struct TouchRect {
  int x0, z0, x1, z1;
};
void mergeNearRects(std::vector<TouchRect>& rects, int gap) {
  const size_t n = rects.size();
  if (n < 2) return;
  std::vector<int> parent(n);
  std::iota(parent.begin(), parent.end(), 0);
  auto find = [&](int i) {
    while (parent[size_t(i)] != i) i = parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
    return i;
  };
  auto near = [gap](const TouchRect& p, const TouchRect& q) {
    return !(p.x0 > q.x1 + gap || q.x0 > p.x1 + gap || p.z0 > q.z1 + gap || q.z0 > p.z1 + gap);
  };
  std::vector<TouchRect> box = rects;  // per root: its group's box
  std::vector<int> roots(n), start, list;
  std::iota(roots.begin(), roots.end(), 0);
  constexpr int B = 8;
  for (bool united = true; united;) {
    united = false;
    int gx0 = INT_MAX, gz0 = INT_MAX, gx1 = INT_MIN, gz1 = INT_MIN;
    for (int r : roots) {
      const TouchRect& b = box[size_t(r)];
      gx0 = std::min(gx0, b.x0), gz0 = std::min(gz0, b.z0), gx1 = std::max(gx1, b.x1 + gap), gz1 = std::max(gz1, b.z1 + gap);
    }
    const int64_t W = (int64_t(gx1) - gx0) / B + 1, H = (int64_t(gz1) - gz0) / B + 1;
    // the buckets as one list (counting sort): start[k] .. start[k + 1] are bucket k's groups
    auto span = [&](int r, auto fn) {
      const TouchRect& b = box[size_t(r)];
      for (int64_t bz = (int64_t(b.z0) - gz0) / B; bz <= (int64_t(b.z1) + gap - gz0) / B; ++bz)
        for (int64_t bx = (int64_t(b.x0) - gx0) / B; bx <= (int64_t(b.x1) + gap - gx0) / B; ++bx) fn(size_t(bz * W + bx));
    };
    start.assign(size_t(W * H) + 1, 0);
    for (int r : roots) span(r, [&](size_t k) { ++start[k + 1]; });
    for (size_t k = 0; k + 1 < start.size(); ++k) start[k + 1] += start[k];
    list.resize(size_t(start.back()));
    std::vector<int> fill(start.begin(), start.end() - 1);
    for (int r : roots) span(r, [&](size_t k) { list[size_t(fill[k]++)] = r; });
    for (size_t k = 0; k + 1 < start.size(); ++k)
      for (int i = start[k]; i < start[k + 1]; ++i)
        for (int j = i + 1; j < start[k + 1]; ++j) {
          int a = find(list[size_t(i)]), b = find(list[size_t(j)]);
          if (a == b || !near(box[size_t(a)], box[size_t(b)])) continue;
          if (b < a) std::swap(a, b);  // the root is the group's first rectangle
          parent[size_t(b)] = a;
          TouchRect &p = box[size_t(a)], &q = box[size_t(b)];
          p = {std::min(p.x0, q.x0), std::min(p.z0, q.z0), std::max(p.x1, q.x1), std::max(p.z1, q.z1)};
          united = true;
        }
    if (united) std::erase_if(roots, [&](int r) { return find(r) != r; });
  }
  // roots ascend: each group in its first rectangle's place
  std::vector<TouchRect> out;
  out.reserve(roots.size());
  for (int r : roots) out.push_back(box[size_t(r)]);
  rects = std::move(out);
}

void simItemsChanged(VM& vm, GridObj* grid, const std::vector<uint32_t>& added, const std::vector<uint32_t>& removed) {
  if (!grid || !grid->sim || !grid->data || (added.empty() && removed.empty())) return;
  SimState& s = *grid->sim;
  const GridData& g = *grid->data;
  LaneGraph& G = s.graph;
  auto t0 = std::chrono::steady_clock::now();
  // the cells to stamp again: the touched footprints, merged where they lie within 4 cells of each other
  using Rect = TouchRect;
  std::vector<Rect> rects;
  for (const auto* list : {&added, &removed})
    for (uint32_t i : *list) {
      if (i >= g.items.size()) continue;
      int sx = 1, sz = 1;
      itemCentre(vm, grid, int(i), &sx, &sz);
      const auto& it = g.items[i];
      rects.push_back({it.x, it.z, it.x + sx - 1, it.z + sz - 1});
    }
  mergeNearRects(rects, 4);

  // 1. lane graph: a removed item's nodes out, a new item's nodes in (removeItem FUN_100bff60, addObject's attach)
  std::vector<uint8_t> nodeGone(G.nodes.size(), 0);
  for (uint32_t i : removed) {
    if (i >= G.itemNodes.size()) continue;
    for (int n : G.itemNodes[i]) nodeGone[size_t(n)] = 1;
    removeItemNodes(G, i);
  }
  size_t before = G.nodes.size();
  addItemNodes(vm, grid, g, G, added);
  s.rt.resize(G.nodes.size());
  bool graphChanged = G.nodes.size() > before || std::find(nodeGone.begin(), nodeGone.end(), 1) != nodeGone.end();
  nodeGone.resize(G.nodes.size(), 0);

  // 2. vehicles on removed nodes: traffic despawns, a citizen's car ends its drive, parked cars go, line vehicles
  //    leave the line and come back at a platform
  int despawned = takeOffRemovedNodes(vm, s, nodeGone);
  // 3. stops: new stop items join the stop vector; stops on removed items leave their lines (removeItem :55-104)
  for (uint32_t i : added)
    if (i < g.items.size()) registerStop(vm, grid, s, i);
  std::vector<uint8_t> lineNodeGone(s.lineNodes.size(), 0), lineChanged(s.lines.size(), 0);
  std::vector<uint8_t> itemGone(g.items.size(), 0);
  for (uint32_t i : removed)
    if (i < itemGone.size()) itemGone[i] = 1;
  for (size_t li = 0; li < s.lines.size(); ++li)
    for (size_t k = 0; k < s.lines[li].stops.size(); ++k) {
      int item = s.lines[li].stops[k].item;
      if (item >= 0 && itemGone[size_t(item)]) lineNodeGone[size_t(s.lines[li].firstNode) + k] = 1, lineChanged[li] = 1;
    }
  bool linesChanged = std::find(lineChanged.begin(), lineChanged.end(), 1) != lineChanged.end();
  if (linesChanged) {
    citizensLeaveLineNodes(s, lineNodeGone, lineChanged);
    std::vector<int> newNode(s.lineNodes.size(), -1);
    std::vector<std::pair<int, int>> nodes;
    for (size_t li = 0; li < s.lines.size(); ++li) {
      LineState& line = s.lines[li];
      if (lineChanged[li]) {
        for (int vi : line.vehicles) {  // back to a platform once the line is whole again
          VehicleState& v = s.vehicles[size_t(vi)];
          if (v.mover >= 0 && s.movers[size_t(v.mover)].live) {
            vm.fireEvent(v.object.get(), u"onUnspawn", {});
            despawn(vm, s, v.mover);
          }
          v.stopTime = 0;
          v.halted = v.brokenHalted = false;
        }
        std::vector<LineStopState> kept;
        for (size_t k = 0; k < line.stops.size(); ++k)
          if (!lineNodeGone[size_t(line.firstNode) + k]) {
            newNode[size_t(line.firstNode) + k] = int(nodes.size() + kept.size());
            kept.push_back(std::move(line.stops[k]));
          }
        for (auto& st : kept) st.docked = -1;
        line.stops = std::move(kept);
        if (line.object) line.object->fieldValues[u"stopCount"] = Value::integer(int(line.stops.size()));
      } else {
        for (size_t k = 0; k < line.stops.size(); ++k) newNode[size_t(line.firstNode) + k] = int(nodes.size() + k);
      }
      line.firstNode = int(nodes.size());
      for (size_t k = 0; k < line.stops.size(); ++k) nodes.push_back({int(li), int(k)});
    }
    s.lineNodes = std::move(nodes);
    citizensRemapLineNodes(s, newNode);
  }
  // the stop vector without the removed stops
  bool stopsChanged = false;
  for (size_t k = 0; k < s.stops.size();) {
    auto at = s.stops[k]->fieldValues.find(u"<item>");
    int item = at == s.stops[k]->fieldValues.end() ? -1 : vm.toInt(at->second);
    if (item >= 0 && size_t(item) < itemGone.size() && itemGone[size_t(item)]) {
      if (!s.stops[k]->destroyed) vm.destroy(s.stops[k].get());
      s.stops.erase(s.stops.begin() + std::ptrdiff_t(k));
      stopsChanged = true;
    } else {
      ++k;
    }
  }
  if (stopsChanged) {
    s.stopOfItem.clear();
    for (size_t k = 0; k < s.stops.size(); ++k) {
      if (auto* to = transport(s.stops[k].get())) to->index = int(k);
      auto at = s.stops[k]->fieldValues.find(u"<item>");
      if (at != s.stops[k]->fieldValues.end()) s.stopOfItem[vm.toInt(at->second)] = int(k);
    }
  }
  // platforms and segments again where the graph or the stops changed
  if (graphChanged || linesChanged)
    for (LineState& line : s.lines) relinkLine(vm, grid, s, line);

  // 4. traffic sources
  if (graphChanged) {
    s.roadStarts.clear(), s.roadEnds.clear(), s.waterStarts.clear(), s.waterEnds.clear();
    s.airEntries.clear(), s.airGatesIn.clear(), s.airGatesOut.clear(), s.airExits.clear();
    s.landing = s.departing = AirPending();
    for (int k = 0; k < 2; ++k) s.trainStarts[k].clear(), s.trainEnds[k].clear();
    collectSources(s);
    buildDoors(vm, grid, s);
  }

  // 5. citizens: place pools and evictions; the walk layers of the touched cells
  citizensItemsChanged(s, added, removed);
  for (const Rect& r : rects) restampWalk(s, r.x0, r.z0, r.x1, r.z1);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  LOG_INFO("sim: map edit, %zu items added, %zu removed: %zu lane nodes now, %d vehicles taken off, lines %s, %zu walk areas, %.0f ms",
           added.size(), removed.size(), G.nodes.size(), despawned, linesChanged ? "changed" : "unchanged", rects.size(), ms);
  // port curved roads: a road or tram piece added or removed next to a curve changes the lanes its ends meet (a street
  // tram block laid up to a curve with tracks: the curve's tram lanes take that block's height and link with it)
  if (!g.curves.empty()) {
    bool near = false;
    for (const auto* list : {&added, &removed})
      for (uint32_t i : *list) {
        if (near || i >= g.items.size()) continue;
        int type = gridItemShape(vm, grid, i).type;
        if (type < 1 || type > 12) continue;
        for (const CurveRoad& c : g.curves) {
          int x0, z0, x1, z1;
          curveCells(g, c, x0, z0, x1, z1);
          const auto& it = g.items[i];
          if (it.x >= x0 - 8 && it.x <= x1 + 8 && it.z >= z0 - 8 && it.z <= z1 + 8) near = true;
        }
      }
    if (near) simCurvesChanged(vm, grid, {});
  }
}

void simTerrainChanged(VM& vm, GridObj* grid, int x0, int z0, int x1, int z1) {
  (void)vm;
  if (!grid || !grid->sim || !grid->data) return;
  restampWalk(*grid->sim, x0, z0, x1, z1);
}

void simCurvesChanged(VM& vm, GridObj* grid, const std::vector<std::array<int, 4>>& rects) {
  if (!grid || !grid->sim || !grid->data) return;
  SimState& s = *grid->sim;
  LaneGraph& G = s.graph;
  std::vector<uint8_t> nodeGone(G.nodes.size(), 0);
  for (int n : G.curveNodes) nodeGone[size_t(n)] = 1;
  rebuildCurveNodes(vm, grid, *grid->data, G);
  s.rt.resize(G.nodes.size());
  nodeGone.resize(G.nodes.size(), 0);
  int despawned = takeOffRemovedNodes(vm, s, nodeGone);
  for (LineState& line : s.lines) relinkLine(vm, grid, s, line);
  s.roadStarts.clear(), s.roadEnds.clear(), s.waterStarts.clear(), s.waterEnds.clear();
  s.airEntries.clear(), s.airGatesIn.clear(), s.airGatesOut.clear(), s.airExits.clear();
  s.landing = s.departing = AirPending();
  for (int k = 0; k < 2; ++k) s.trainStarts[k].clear(), s.trainEnds[k].clear();
  collectSources(s);
  buildDoors(vm, grid, s);
  for (const auto& r : rects) restampWalk(s, r[0], r[1], r[2], r[3]);
  LOG_INFO("sim: curved roads changed: %zu curves, %zu curve lane nodes, %d vehicles taken off",
           grid->data->curves.size(), G.curveNodes.size(), despawned);
}

// gsGridVehicle::updateVisibility (L:0x081ad310, docs/renderer/underground.md §5): in the underground view a car of a
// consist shows only while its node belongs to a TUNNEL item (lane-node flag 0x800000). The unit's node is the one
// at its rear distance (forEachUnit's r).
bool unitOnTunnel(const SimState& s, const Mover& m, int k) {
  float r = m.dist;
  for (int j = 1; j <= k && size_t(j) < m.units.size(); ++j) r -= unitLength(s, m.units[size_t(j)]);
  size_t at = m.track.at(r);
  return at < m.track.nodes.size() && (s.graph.nodes[size_t(m.track.nodes[at])].flags & 0x800000) != 0;
}

// the body at light height (the vertices between 20 % and 55 % of the model's height, within 75 % of its half width):
// front and rear surface (model z; the front is −Z), half width and height. Mirrors, roof parts and anything else
// the bounding box reaches stay out, so the lights sit on the bumpers.
struct LightFrame {
  float front = 0, rear = 0, halfWidth = 0.8f, minY = 0, height = 1.5f;
};
const LightFrame& lightFrame(const render::ObjectData& od) {
  static std::unordered_map<const render::ObjectData*, LightFrame> cache;
  if (auto it = cache.find(&od); it != cache.end()) return it->second;
  const render::MeshData& me = od.mesh;
  LightFrame f;
  f.minY = me.boundsMin[1];
  f.height = std::max(0.1f, me.boundsMax[1] - me.boundsMin[1]);
  float hw = 0.5f * (me.boundsMax[0] - me.boundsMin[0]), cx = 0.5f * (me.boundsMax[0] + me.boundsMin[0]);
  float lo = f.minY + 0.2f * f.height, hi = f.minY + 0.55f * f.height;
  float zMin = 1e30f, zMax = -1e30f, xMax = 0;
  for (size_t i = 0; i < me.vertexCount(); ++i) {
    float x = me.positions[i * 3] - cx, y = me.positions[i * 3 + 1], z = me.positions[i * 3 + 2];
    if (y < lo || y > hi) continue;
    xMax = std::max(xMax, std::fabs(x));
    if (std::fabs(x) > 0.75f * hw) continue;
    zMin = std::min(zMin, z), zMax = std::max(zMax, z);
  }
  if (zMin > zMax) zMin = me.boundsMin[2], zMax = me.boundsMax[2], xMax = hw;
  f.front = zMin, f.rear = zMax, f.halfWidth = std::max(0.3f, xMax);
  return cache.emplace(&od, f).first->second;
}

void simVehicleLights(GridObj* grid, const Vec3& eye, float range, std::vector<VehicleLightSource>& out) {
  out.clear();
  SimState* s = simOf(grid);
  if (!s) return;
  auto light = [&](const Mover& m) {
    if (m.parked || m.boat || m.aircraft || m.units.empty() || m.track.nodes.empty()) return;
    VehicleLightSource L;
    L.mover = int(&m - s->movers.data());
    L.rail = m.train || (m.transit && m.units.size() > 1);
    L.speed = m.speed;
    L.seed = uint32_t(m.colorSeed) * 2654435761u ^ uint32_t(L.mover) * 40503u;
    float r = m.dist;
    bool any = false;
    for (size_t k = 0; k < m.units.size(); ++k) {
      const Unit& u = m.units[k];
      if (k > 0) r -= unitLength(*s, u);
      if (u.model < 0 || (k == 0 || k + 1 == m.units.size()) == false) continue;
      if (unitOnTunnel(*s, m, int(k))) return;
      const UnitModel& um = s->models[size_t(u.model)];
      PlacedPose p = unitPose(*s, m, u, r);
      Vec3 dir = p.forward * (u.reversed ? -1.0f : 1.0f);  // travel direction (model front is −Z)
      if (!um.object || !um.object->data) return;
      const LightFrame& lf = lightFrame(*um.object->data);
      Mat4 w = poseMatrix(p);
      float h = lf.height;
      float cx = 0.5f * (um.boundsMin.x + um.boundsMax.x);
      if (k == 0) {
        L.front = render::transformPoint(w, Vec3{cx, lf.minY, u.reversed ? lf.rear : lf.front});
        L.forward = dir;
        L.halfWidth = lf.halfWidth;
        L.frontData = um.object->data.get();
        L.frontIsModelRear = u.reversed;
        L.frontY = std::clamp(0.3f * h, 0.55f, 1.0f);
        L.bus = !L.rail && h > 2.4f;
        any = true;
      }
      if (k + 1 == m.units.size()) {
        L.back = render::transformPoint(w, Vec3{cx, lf.minY, u.reversed ? lf.front : lf.rear});
        L.backData = um.object->data.get();
        L.backIsModelFront = u.reversed;
        L.backForward = dir;
        L.backY = std::clamp(0.4f * h, 0.7f, 1.2f);
      }
    }
    if (!any) return;
    Vec3 e = L.front - eye;
    if (dot(e, e) > range * range) return;
    out.push_back(L);
  };
  for (const VehicleState& v : s->vehicles)
    if (v.mover >= 0 && s->movers[size_t(v.mover)].live) light(s->movers[size_t(v.mover)]);
  for (const Mover& m : s->movers)
    if (m.live && !m.transit) light(m);
}

// a line vehicle's display texts in the sign atlas: "N label", "label N" and "N", where the label is the line's name
// ($name, linepanel.script) or, for a line without one, its last stop's name
static void signTexts(VM& vm, int number, const std::u16string& label, float out[3][4]) {
  std::u16string n = utf8ToUtf16(std::to_string(number));
  const std::u16string texts[3] = {label.empty() ? n : n + u" " + label, label.empty() ? n : label + u" " + n, n};
  for (int k = 0; k < 3; ++k) {
    SignText t = signText(vm, texts[k]);
    out[k][0] = float(t.slot), out[k][1] = t.ledWidth, out[k][2] = t.printWidth, out[k][3] = 0;
  }
}

static std::u16string lineLabel(VM& vm, const LineState& line) {
  if (!line.object) return {};
  std::u16string name = vm.toStr(vm.resolve(vm.getMember(Value::heap(VT::Object, line.object.get()), u"$name")));
  if (!name.empty() || line.stops.empty() || !line.stops.back().stop) return name;
  return vm.toStr(vm.resolve(vm.getMember(Value::heap(VT::Object, line.stops.back().stop.get()), u"name")));
}

// a vehicle model's GPU mesh and materials with the night lights' lamp mask (made once)
static void prepareVehicleObject(Display& d, VM& vm, ObjectObj* o) {
  if (o->gpu) return;
  o->gpu = std::make_shared<render::GpuMesh>(d.r3d().upload(o->data->mesh));
  o->materials = std::make_shared<std::vector<render::GpuMaterial>>();
  for (size_t i = 0; i < o->data->materials.size(); ++i) {
    o->materials->push_back(objectMaterial(d, vm, o, o->data->materials[i]));
    VehicleLampInfo li;
    o->materials->back().windowMask = vehicleLampMask(d, vm, *o->data, int(i), &li);  // port night lights
    applyVehicleLamps(o->materials->back(), li);
    o->materials->back().windowKind = 1;
  }
}

// REGS_TEST_LINEUP=<x>,<z>[,<load 0..1>] (screenshots, 2026-11-24): every bus, tram and metro part the game has loaded
// (the vehicle templates under metro/objects/vehicles/), side by side in one row per kind around that world point,
// lengthwise along x, with head and tail lamps, the cabin light and the given load (default full), so all their
// lights, windows and passengers can be checked in one picture. Drawn only; nothing in the simulation changes.
static void collectLineup(Display& d, VM& vm, GridObj* grid, const char* spec, std::vector<DrawItem>& items) {
  struct Entry {
    ObjectObj* o;
    Mat4 world;
  };
  static std::vector<Entry> lineup;
  static bool built = false;
  float load = 1.0f;
  float ax = 0, az = 0, turnDeg = 90;  // turn: the models' heading about y (90: lengthwise along x; 0 or 180: facing)
  std::sscanf(spec, "%f,%f,%f,%f", &ax, &az, &load, &turnDeg);
  if (!built && grid->data) {
    built = true;
    const char* kinds[] = {"/bus", "/tram", "/metro"};
    std::vector<std::pair<std::string, ObjectObj*>> rows[3];
    for (auto& [name, obj] : grid->templates) {
      auto* o = dynamic_cast<ObjectObj*>(obj.get());
      if (!o || !o->data || o->pendingPath.find("metro/objects/vehicles/") == std::string::npos) continue;
      std::string p = o->pendingPath.substr(o->pendingPath.find("vehicles/") + 8);
      int k = p.rfind("/bus", 0) == 0 || p.rfind("/double-decker", 0) == 0 ? 0 : p.rfind("/tram", 0) == 0 ? 1 : p.rfind("/metro", 0) == 0 ? 2 : -1;
      if (k < 0 || o->data->mesh.vertexCount() < 50) continue;  // doors and other small parts drawn with their car
      rows[k].emplace_back(o->pendingPath, o);
    }
    const GridData& g = *grid->data;
    auto ground = [&](float x, float z) {
      int cx = std::clamp(int(x / g.cellSize + float(g.width) * 0.5f), 0, g.width - 1);
      int cz = std::clamp(int(z / g.cellSize + float(g.length) * 0.5f), 0, g.length - 1);
      return float(g.height[size_t(cz) * size_t(g.width) + size_t(cx)]) * g.cellSize / 256.0f;
    };
    const float ha = turnDeg * 3.14159265f / 360.0f;
    const render::Quat turn{0, std::sin(ha), 0, std::cos(ha)};
    for (int k = 0; k < 3; ++k) {
      std::sort(rows[k].begin(), rows[k].end());
      float x = ax - 120.0f, z = az + (float(k) - 1.0f) * 40.0f;  // rows 40 apart: one row per close view
      for (auto& [path, o] : rows[k]) {
        float l = o->data->mesh.boundsMax[2] - o->data->mesh.boundsMin[2], wd = o->data->mesh.boundsMax[0] - o->data->mesh.boundsMin[0];
        float len = l * std::fabs(std::sin(2 * ha)) + wd * std::fabs(std::cos(2 * ha)) + (std::fabs(std::cos(2 * ha)) > 0.5f ? 1.0f : 0.0f);
        x += len * 0.5f + 1.5f;
        lineup.push_back({o, render::translation({x, ground(x, z), z}) * render::rotation(turn)});
        LOG_INFO("lineup: x %6.1f z %4.0f  %s", x, z, path.c_str());
        x += len * 0.5f + 1.5f;
      }
      LOG_INFO("lineup: %zu %s parts in a row at z %.0f", rows[k].size(), kinds[k] + 1, z);
    }
  }
  float code = vehicleLampCode(1, 2, true, 1.0f);
  for (const Entry& e : lineup) {
    prepareVehicleObject(d, vm, e.o);
    size_t first = items.size();
    if (isSkinnedModel(vm, e.o)) {
      collectSkinned(d, vm, e.o, e.world, Pose(), items);
    } else {
      DrawItem it;
      it.mesh = e.o->gpu.get();
      it.materials = e.o->materials.get();
      it.world = e.world;
      items.push_back(it);
    }
    float vseed = std::fmod(float(&e - lineup.data()) * 0.618034f, 1.0f);
    for (size_t k = first; k < items.size(); ++k)
      items[k].lampCode = code, items[k].cabinLight = 1.0f, items[k].passengers = load, items[k].vehicleSeed = vseed,
      items[k].lineNumber = float(int(&e - lineup.data()) * 37 % 199 + 1);  // 1-3 digits, a different number each
    {  // and a stop name each, some long enough to scroll on an LED display
      static const char16_t* names[] = {u"Hauptbahnhof", u"Stadtplatz", u"Zoo", u"Museumsinsel", u"Ostkreuz",
                                        u"Breitenbachplatz", u"Messe Nord / ICC", u"Rathaus"};
      int idx = int(&e - lineup.data());
      float texts[3][4];
      signTexts(vm, int(items[first].lineNumber), names[idx % 8], texts);
      for (size_t k = first; k < items.size(); ++k) std::copy(&texts[0][0], &texts[0][0] + 12, &items[k].signText[0][0]);
    }
  }
}

void collectSimVehicles(Display& d, VM& vm, GridObj* grid, const Vec3& eye, const Mat4& viewProj, float farZ,
                        std::vector<DrawItem>& items) {
  if (static const char* lineup = regs::envVar("REGS_TEST_LINEUP"); lineup) collectLineup(d, vm, grid, lineup, items);
  SimState* s = simOf(grid);
  if (!s) return;
  collectCharacters(d, vm, *s, eye, viewProj, farZ, items);
  collectLinePaths(d, vm, *s, eye, farZ, items);
  collectBoatWakes(d, vm, *s, eye, farZ, items);
  // the followed vehicle turned round or halts: a screenshot burst (every 12 frames; every 4 for the short halts)
  if (s->followShots > 0 && ++s->followFrame % (s->followShotName.rfind("halt-", 0) == 0 ? 4 : 12) == 0)
    d.requestScreenshot(s->followShotName + std::to_string(16 - s->followShots--) + ".png");
  const bool underground = grid->underground();
  const double clockNow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  forEachUnit(*s, [&](const Mover& m, int unit, const Unit& u, const PlacedPose& p) {
    Vec3 e = p.position - eye;
    if (dot(e, e) > farZ * farZ) return;
    if (underground && !unitOnTunnel(*s, m, unit)) return;
    // in front of the camera and roughly inside the view, with a margin of the unit's size
    float clip[4];
    for (int r = 0; r < 4; ++r)
      clip[r] = viewProj.at(r, 0) * p.position.x + viewProj.at(r, 1) * p.position.y + viewProj.at(r, 2) * p.position.z + viewProj.at(r, 3);
    float margin = std::fabs(clip[3]) * 0.1f + 20.0f;
    if (clip[3] < -20 || std::fabs(clip[0]) > clip[3] + margin || std::fabs(clip[1]) > clip[3] + margin) return;
    ObjectObj* o = s->models[size_t(u.model)].object;
    if (!o || !o->data) return;
    if (!o->gpu) {
      o->gpu = std::make_shared<render::GpuMesh>(d.r3d().upload(o->data->mesh));
      o->materials = std::make_shared<std::vector<render::GpuMaterial>>();
      for (size_t i = 0; i < o->data->materials.size(); ++i) {
        o->materials->push_back(objectMaterial(d, vm, o, o->data->materials[i]));
        VehicleLampInfo li;
        o->materials->back().windowMask = vehicleLampMask(d, vm, *o->data, int(i), &li);  // port night lights
        applyVehicleLamps(o->materials->back(), li);
        o->materials->back().windowKind = 1;
      }
    }
    size_t first = items.size();
    if (isSkinnedModel(vm, o)) {
      float doorPos = m.transit && m.vehicle >= 0 ? s->vehicles[size_t(m.vehicle)].doorPos : m.animTime;
      collectSkinned(d, vm, o, poseMatrix(p), doorPos >= 0 ? poseFirstAnimation(vm, o, doorPos) : Pose(), items);
    } else {
      DrawItem it;
      it.mesh = o->gpu.get();
      it.materials = o->materials.get();
      it.world = poseMatrix(p);
      items.push_back(it);
    }
    if (float night = vehicleNight01(); vehicleCycleOn() && !m.parked && !m.boat && !m.aircraft) {
      // port night lights: the lamps at the consist's leading and trailing ends (the model's own ends if reversed)
      uint32_t seed = uint32_t(m.colorSeed) * 2654435761u ^ uint32_t(&m - s->movers.data()) * 40503u;
      int front = 0, rear = 0;
      if (unit == 0) (u.reversed ? rear : front) = 1;
      if (size_t(unit) + 1 == m.units.size()) (u.reversed ? front : rear) = m.speed < 0.5f ? 3 : 2;
      // buses, trams and trains: the interior is lit, so their windows glow, at half strength by day and full when
      // the vehicle's lights are on
      bool cabin = m.transit || m.train;
      float on = night > 0 ? vehicleLightsOn(seed, night) : 0.0f;
      if (on <= 0.01f) front = rear = 0;
      float code = vehicleLampCode(front, rear, cabin, std::max(on, cabin ? 0.02f : 0.0f));
      float cabinLight = cabin ? 0.5f + 0.5f * on : 0.0f;
      for (size_t k = first; k < items.size(); ++k)
        items[k].lampCode = code, items[k].cabinLight = cabinLight, items[k].vehicleSeed = float(seed >> 16) / 65536.0f;
      // passengers in the windows: the line vehicle's load in steps the eye can count (empty, up to a quarter, half,
      // three quarters, full: 0, 0.25, 0.5, 0.75, 1), eased so the silhouettes fade in and out, and snapped to the
      // step at the end (an empty car shows nobody, not a last figure fading forever)
      if (m.transit && m.vehicle >= 0) {
        static std::vector<float> shown;
        size_t mi = size_t(&m - s->movers.data());
        if (shown.size() <= mi) shown.resize(mi + 1, 0.0f);
        const VehicleState& vs = s->vehicles[size_t(m.vehicle)];
        float load = vs.capacity > 0 ? std::clamp(float(vs.passengerCount) / float(vs.capacity), 0.0f, 1.0f) : 0.0f;
        float target = vs.passengerCount > 0 ? std::ceil(load * 4.0f - 1e-4f) / 4.0f : 0.0f;
        target = std::clamp(target, vs.passengerCount > 0 ? 0.25f : 0.0f, 1.0f);
        // REGS_TEST_PASSENGERS=<0..1> (screenshots): every line vehicle shows that load
        static const char* testLoad = regs::envVar("REGS_TEST_PASSENGERS");
        if (testLoad) target = std::clamp(float(std::atof(testLoad)), 0.0f, 1.0f);
        if (unit == 0) {
          shown[mi] += (target - shown[mi]) * std::min(1.0f, vm.frameDt() * 0.8f);
          if (std::fabs(target - shown[mi]) < 0.01f) shown[mi] = target;
        }
        // the line's number for the destination displays ($index, linepanel.script: counted per transport type)
        float lineNo = 0;
        float texts[3][4] = {{-1, 0, 0, 0}, {-1, 0, 0, 0}, {-1, 0, 0, 0}};
        if (vs.line >= 0 && size_t(vs.line) < s->lines.size() && s->lines[size_t(vs.line)].object) {
          const LineState& ln = s->lines[size_t(vs.line)];
          lineNo = float(vm.toInt(vm.resolve(vm.getMember(Value::heap(VT::Object, ln.object.get()), u"$index"))));
          if (lineNo > 0) signTexts(vm, int(lineNo), lineLabel(vm, ln), texts);
        }
        for (size_t k = first; k < items.size(); ++k) {
          items[k].passengers = shown[mi], items[k].lineNumber = lineNo;
          std::copy(&texts[0][0], &texts[0][0] + 12, &items[k].signText[0][0]);
        }
      }
    }
    for (size_t k = first; k < items.size(); ++k) {  // animated parts: custom4 = (angle, spin), stepAnimatedParts
      items[k].hasCustom4 = true;
      items[k].custom4[0] = m.rotorAngle;
      items[k].custom4[1] = m.rotorSpin;
      items[k].surfaceClass = render::kSurfaceVehicle;  // the mods' shader hooks: a vehicle, 1 when parked
      items[k].hookValue = m.parked ? 1.0f : 0.0f;
    }
    uint32_t argb = u.tint;
    // line vehicles: the line colour tints the colorize materials (gsTransportLine.color, 0x10114ec0)
    if (m.transit && m.vehicle >= 0) {
      const LineState& line = s->lines[size_t(s->vehicles[size_t(m.vehicle)].line)];
      if (line.object)
        if (auto c = line.object->fieldValues.find(u"color"); c != line.object->fieldValues.end() && !c->second.isNone())
          argb = uint32_t(vm.toInt(c->second));
    }
    if (argb)
      for (size_t k = first; k < items.size(); ++k) {
        items[k].colorize[0] = float((argb >> 16) & 0xFF) / 255.0f;
        items[k].colorize[1] = float((argb >> 8) & 0xFF) / 255.0f;
        items[k].colorize[2] = float(argb & 0xFF) / 255.0f;
      }
    if (m.plow && unit == 0) collectPlowParts(d, s->models[size_t(u.model)], p, clockNow, items);  // the mods' plough
  });
}

Object* simStopController(GridObj* grid, int item) {
  SimState* s = simOf(grid);
  if (!s) return nullptr;
  auto it = s->stopOfItem.find(item);
  return it == s->stopOfItem.end() ? nullptr : s->stops[size_t(it->second)].get();
}

int simBusPlatformAtTramStop(VM& vm, GridObj* grid, int item) {
  SimState* s = simOf(grid);
  if (!s || !grid->data || item < 0 || size_t(item) >= grid->data->items.size()) return -1;
  const GridData& g = *grid->data;
  const auto& it = g.items[size_t(item)];
  auto t = it.type < g.types.size() ? grid->templates.find(g.types[it.type]) : grid->templates.end();
  if (it.removed || t == grid->templates.end()) return -1;
  int ty = fieldInt(vm, t->second.get(), u"type", 0);
  if (ty < 10 || ty > 12) return -1;
  int sx = 1, sz = 1;
  Vec3 c = itemCentre(vm, grid, item, &sx, &sz);
  return busPlatformInFootprint(*s, it.x, it.z, sx, sz, c);
}

bool simBusAtTramStops(VM& vm) { return settingNumber(vm, u"regsBusAtTramStops", 1.0f) != 0.0f; }

namespace {

// The pick cull of a mover (SimPickCull). A unit's origin lies on the line through its two anchor points on the
// track, at the fraction f = rearAnchor / span (unitPose), and the track curve moves at most 5 world units per unit
// of track distance: each node is a cubic from its start to its end point with end tangents of a third of its chord
// (|b − a| = |d − c| = L/3, |c − b| ≤ |d − a| + 2L/3 = 5L/3, so |dP/ds| ≤ 3·(5L/3)/L), and consecutive nodes meet
// (trackPoint's halfway points). So every origin lies within 5·D·g of the point where the head's node starts, D the
// farthest anchor distance from that node's start, g = 1 for f in [0, 1], else 1 + 2|f|. Factor 6 and a cell's
// margin cover lanes whose ends do not quite meet. A hit lies within 0.8 + 1.3 + |model centre| + max(half length,
// 0.5) of its unit's origin and on the segment or at most 2.1 units past its end (intersectGrid).
bool moverMayBeHit(const SimState& s, const Mover& m, const SimPickCull& c) {
  // only a vehicle with an object can be picked (SimVehicleView::object)
  if (!(m.transit && m.vehicle >= 0 ? s.vehicles[size_t(m.vehicle)].object.get() : m.controller.get())) return false;
  const Track& tr = m.track;
  const size_t i = tr.at(m.dist);
  const LaneNode& n = s.graph.nodes[size_t(tr.nodes[i])];
  const Vec3 ref = i > 0 ? (s.graph.nodes[size_t(tr.nodes[i - 1])].p1 + n.p0) * 0.5f : n.p0;
  const float si = tr.start[i], end = tr.end(s.graph);
  float lo = si, hi = si, g = 1, reach = 0, r = m.dist;
  for (size_t k = 0; k < m.units.size(); ++k) {
    const Unit& u = m.units[k];
    if (k > 0) r -= unitLength(s, u);
    if (u.model < 0) continue;
    const UnitModel& um = s.models[size_t(u.model)];
    const float span = um.frontAnchor + um.rearAnchor, s1 = r + um.rearOffset - um.rearAnchor, s2 = s1 + span;
    lo = std::min({lo, s1, s2});
    hi = std::max({hi, s1, s2});
    const float f = span > 0 ? um.rearAnchor / span : 0;
    if (f < 0 || f > 1) g = std::max(g, 1 + 2 * std::fabs(f));
    const Vec3 mc = (um.boundsMin + um.boundsMax) * 0.5f;
    reach = std::max(reach, 2.1f + std::sqrt(dot(mc, mc)) + std::max((um.boundsMax.z - um.boundsMin.z) * 0.5f, 0.5f));
  }
  lo = std::clamp(lo, 0.0f, end), hi = std::clamp(hi, 0.0f, end);
  const float spread = 6 * std::max(si - lo, hi - si) * g + std::sqrt(dot(m.crashOffset, m.crashOffset)) + c.cellSize;
  // a unit's cell can be in the rectangle (int() truncates toward zero: cell 0 starts one cell early)
  const float minX = float(c.x0 - 1) * c.cellSize - c.halfWidth, maxX = float(c.x1 + 1) * c.cellSize - c.halfWidth;
  const float minZ = float(c.z0 - 1) * c.cellSize - c.halfLength, maxZ = float(c.z1 + 1) * c.cellSize - c.halfLength;
  if (ref.x + spread < minX || ref.x - spread > maxX || ref.z + spread < minZ || ref.z - spread > maxZ) return false;
  // and near the segment
  const Vec3 d = c.end - c.start, w = ref - c.start;
  const float dd = dot(d, d), t = dd > 0 ? std::clamp(dot(w, d) / dd, 0.0f, 1.0f) : 0.0f;
  const Vec3 e = w - d * t;
  const float lim = spread + reach + 2.5f;
  return dot(e, e) <= lim * lim;
}

}  // namespace

void forEachSimVehicle(GridObj* grid, const std::function<void(const SimVehicleView&)>& fn, const SimPickCull* cull) {
  SimState* s = simOf(grid);
  if (!s) return;
  forEachUnitOf(*s, [&](const Mover& mv) { return !cull || moverMayBeHit(*s, mv, *cull); },
                [&](const Mover& mv, int k, const Unit& u, const PlacedPose& p) {
    const UnitModel& m = s->models[size_t(u.model)];
    Mat4 w = poseMatrix(p);
    Vec3 mc = (m.boundsMin + m.boundsMax) * 0.5f, half = (m.boundsMax - m.boundsMin) * 0.5f;
    SimVehicleView view;
    view.object = mv.transit && mv.vehicle >= 0 ? s->vehicles[size_t(mv.vehicle)].object.get() : mv.controller.get();
    view.unit = k;
    view.position = p.position;
    view.forward = p.forward;
    view.yaw = std::atan2(p.forward.x, p.forward.z);
    view.center = render::transformPoint(w, mc);
    view.halfExtents = half;
    view.radius = std::sqrt(dot(half, half));
    fn(view);
  });
}

// ---------------------------------------------------------------------------------------------------------------
// natives

void registerSimNatives(VM& vm) {
  registerPathNatives(vm);
  registerSimSoundNatives(vm);
  for (auto [cls, name] : {std::pair{"gsGridCitizens", u"regsPopulationDensity"}, std::pair{"gsGridVehicles", u"regsTrafficDensity"}})
    if (ClassInfo* c = vm.findClass(cls)) {
      FieldInfo& f = c->fields[name];
      f.name = name;
      f.type = 0;
      f.get = [key = std::u16string(name)](VM&, Object* o) {
        auto it = o->fieldValues.find(key);
        return it == o->fieldValues.end() || it->second.isNone() ? Value::real(1.0f) : it->second;
      };
      f.set = [key = std::u16string(name)](VM& v, Object* o, const Value& x) { o->fieldValues[key] = Value::real(v.toFloat(v.resolve(x))); };
    }
  registerCitizenNatives(vm);
  registerSimSave();
  for (const char* cls : {"gsTransportLine", "gsTransportVehicle", "gsTransportStopController"})
    if (ClassInfo* c = vm.findClass(cls)) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new TransportObj(ci)); };

  auto gridOf = [](VM& v, Object* s) -> GridObj* {
    for (Object* p = s; p; p = p->parent)
      if (auto* g = dynamic_cast<GridObj*>(p)) return g;
    (void)v;
    return nullptr;
  };
  // gsGridVehicles.setPassengerTrains / setCargoTrains (0x1010a360): no arguments clear the list, each argument
  // (a name or an array of names) is appended as one consist
  // gsGridVehicles.setAirplanes (0x1010aac0): no argument clears the list, each argument is a model name
  implement(vm, "gsGridVehicles", u"setAirplanes", [](VM& v, Object* s, int argc, const Value* a) {
    Value& slot = s->fieldValues[u"<airplanes>"];
    if (argc == 0 || !slot.array()) slot = Value::heap(VT::Array, make<ArrayObj>().get());
    for (int i = 0; i < argc; ++i) slot.array()->items.push_back(v.resolve(a[i]));
    return Value();
  });
  // setBoats (0x1010abe0) keeps its consists the same way
  for (const char16_t* m : {u"setPassengerTrains", u"setCargoTrains", u"setBoats"}) {
    std::u16string key = m[3] == u'P' ? u"<passengerTrains>" : m[3] == u'B' ? u"<boats>" : u"<cargoTrains>";
    implement(vm, "gsGridVehicles", m, [key](VM& v, Object* s, int argc, const Value* a) {
      Value& slot = s->fieldValues[key];
      if (argc == 0 || !slot.array()) slot = Value::heap(VT::Array, make<ArrayObj>().get());
      auto* list = slot.array();
      for (int i = 0; i < argc; ++i) {
        Value x = v.resolve(a[i]);
        if (x.array()) {
          list->items.push_back(x);
        } else {
          auto one = make<ArrayObj>();
          one->items.push_back(x);
          list->items.push_back(Value::heap(VT::Array, one.get()));
        }
      }
      return Value();
    });
  }
  // gsGridVehicleController.getID(): the agent's colour seed (0x10107bd0)
  implement(vm, "gsGridVehicleController", u"getID", [](VM&, Object* s, int, const Value*) {
    auto it = s->fieldValues.find(u"<id>");
    return it == s->fieldValues.end() ? Value::integer(0) : it->second;
  });
  // gsTransportLines: the stop vector (0x10114530, 0x10115080)
  implement(vm, "gsTransportLines", u"getStopCount", [gridOf](VM& v, Object* s, int, const Value*) {
    SimState* st = simOf(gridOf(v, s));
    return Value::integer(st ? int(st->stops.size()) : 0);
  });
  implement(vm, "gsTransportLines", u"getStop", [gridOf](VM& v, Object* s, int, const Value* a) {
    SimState* st = simOf(gridOf(v, s));
    int i = v.toInt(a[0]);
    if (!st || i < 0 || size_t(i) >= st->stops.size()) return Value::null();
    return Value::heap(VT::Object, st->stops[size_t(i)].get());
  });
  // stopExists(type, name): a stop of the type whose item carries the name (0x101165f0)
  implement(vm, "gsTransportLines", u"stopExists", [gridOf](VM& v, Object* s, int, const Value* a) {
    GridObj* g = gridOf(v, s);
    SimState* st = simOf(g);
    if (!st || !g->data) return Value::boolean(false);
    int type = v.toInt(a[0]);
    std::string name = utf16ToUtf8(v.toStr(a[1]));
    for (const auto& o : st->stops) {
      if (fieldInt(v, o.get(), u"type", -1) != type) continue;
      int item = fieldInt(v, o.get(), u"<item>", -1);
      if (item >= 0 && size_t(item) < g->data->names.size() && g->data->names[size_t(item)] == name) return Value::boolean(true);
    }
    return Value::boolean(false);
  });

  // routeExists(mask, posA, [posB], [directOnly]) (0x1011b8e0, handlers/batch-A.md): with posA alone the (line, stop)
  // memberships whose catchment covers it; else whether active lines of the mask lead from a stop covering posA to
  // a stop within ±50 cells of posB, on one line with directOnly. Existence does not depend on the randomised
  // costs, so a plain search over the line nodes and their transfers answers it (the original's rand() calls are not
  // made)
  implement(vm, "gsTransportLines", u"routeExists", [gridOf](VM& v, Object* self, int argc, const Value* a) {
    GridObj* g = gridOf(v, self);
    SimState* st = simOf(g);
    auto vec = [&](const Value& x) {
      Value r = v.resolve(x);
      return r.type() == VT::Vector ? Vec3{r.vec()[0], r.vec()[1], r.vec()[2]} : Vec3{};
    };
    if (!st || !g->data) return argc >= 3 ? Value::boolean(false) : Value::integer(0);
    SimState& s = *st;
    const GridData& gd = *g->data;
    uint32_t mask = uint32_t(v.toInt(a[0]));
    auto cellOf = [&](const Vec3& p, int& cx, int& cz) {
      cx = int(p.x / gd.cellSize + float(gd.width) * 0.5f), cz = int(p.z / gd.cellSize + float(gd.length) * 0.5f);
    };
    auto usable = [&](int node) {
      const LineState& line = s.lines[size_t(s.lineNodes[size_t(node)].first)];
      // FUN_10114560 / the ride edges: temporary nodes (the add-stop tool's hover) take no part
      return ((mask >> line.type) & 1) && lineActive(v, line) && !(line.stops[size_t(s.lineNodes[size_t(node)].second)].flags & 1);
    };
    auto stopOf = [&](int node) -> const LineStopState& {
      auto [l, k] = s.lineNodes[size_t(node)];
      return s.lines[size_t(l)].stops[size_t(k)];
    };
    auto centre = [&](int item, int& cx, int& cz) {
      int sx = 1, sz = 1;
      itemCentre(v, g, item, &sx, &sz);
      cx = gd.items[size_t(item)].x + (sx >> 1), cz = gd.items[size_t(item)].z + (sz >> 1);
    };
    int ax, az;
    cellOf(vec(a[1]), ax, az);
    std::vector<int> open;
    for (size_t n = 0; n < s.lineNodes.size(); ++n) {
      const LineStopState& ls = stopOf(int(n));
      if (ls.item < 0 || !usable(int(n))) continue;
      int cx, cz;
      centre(ls.item, cx, cz);
      if (std::abs(cx - ax) > 50 || std::abs(cz - az) > 50) continue;
      float ca = ls.stop ? fieldFloat(v, ls.stop.get(), u"catchmentArea", 100.0f) : 100.0f;
      float dx = float(cx - ax) * gd.cellSize, dz = float(cz - az) * gd.cellSize;
      if (dx * dx + dz * dz <= ca * ca) open.push_back(int(n));
    }
    if (argc < 3) return Value::integer(int(open.size()));
    int bx, bz;
    cellOf(vec(a[2]), bx, bz);
    bool directOnly = argc >= 4 && v.truthy(a[3]);
    auto nearGoal = [&](int node) {
      int cx, cz, item = stopOf(node).item;
      if (item < 0) return false;
      centre(item, cx, cz);
      return std::abs(cx - bx) <= 50 && std::abs(cz - bz) <= 50;
    };
    std::vector<uint8_t> seen(s.lineNodes.size(), 0);
    for (int n : open) seen[size_t(n)] = 1;
    while (!open.empty()) {
      int n = open.back();
      open.pop_back();
      if (nearGoal(n)) return Value::boolean(true);
      auto [l, k] = s.lineNodes[size_t(n)];
      const LineState& line = s.lines[size_t(l)];
      int next = k + 1 < int(line.stops.size()) ? line.firstNode + k + 1 : line.closed ? line.firstNode : -1;
      if (next >= 0 && !seen[size_t(next)]) seen[size_t(next)] = 1, open.push_back(next);  // ride on
      if (directOnly) continue;
      for (int m : stopOf(n).transfers)  // change at the arrival stop
        if (!seen[size_t(m)] && usable(m)) seen[size_t(m)] = 1, open.push_back(m);
    }
    return Value::boolean(false);
  });

  // gsTransportStopController.getPassengersByLine() (0x10116300): per line serving the stop, the waiting citizens
  // with the worst mood tier in bits 24-25 (3 if any is at mood 3, 2, 1)
  implement(vm, "gsTransportStopController", u"getPassengersByLine", [gridOf](VM& v, Object* self, int, const Value*) {
    auto out = make<MapObj>();
    if (SimState* s = simOf(gridOf(v, self)))
      for (const LineState& line : s->lines)
        for (const LineStopState& st : line.stops)
          if (st.stop.get() == self && line.object) {
            uint32_t tier = st.mood[2] ? 0x3000000u : st.mood[1] ? 0x2000000u : st.mood[0] ? 0x1000000u : 0u;
            out->at(Value::heap(VT::Object, line.object.get())) = Value::integer(int32_t(uint32_t(st.waiting) | tier));
          }
    return Value::heap(VT::Map, out.get());
  });

  // countLines(mask, activeOnly) (0x10116700)
  implement(vm, "gsTransportLines", u"countLines", [gridOf](VM& v, Object* self, int, const Value* a) {
    SimState* s = simOf(gridOf(v, self));
    int n = 0;
    uint32_t mask = uint32_t(v.toInt(a[0]));
    bool activeOnly = v.truthy(a[1]);
    for (const LineState& line : s ? s->lines : std::vector<LineState>())
      if (!line.deleted && ((mask >> line.type) & 1) && (!activeOnly || lineActive(v, line))) ++n;
    return Value::integer(n);
  });

  // countStops(mask, onLinesOnly, [name]) (0x10119dd0): stops of the mask's types, or the distinct stops active lines
  // of the mask serve; a name must equal the stop template's displayName (a stop of another type with that name
  // counts too, as in the original)
  implement(vm, "gsTransportLines", u"countStops", [gridOf](VM& v, Object* self, int argc, const Value* a) {
    GridObj* g = gridOf(v, self);
    SimState* s = simOf(g);
    if (!s || !g->data) return Value::integer(0);
    uint32_t mask = uint32_t(v.toInt(a[0]));
    bool onLines = v.truthy(a[1]);
    bool filter = argc >= 3;
    std::u16string name = filter ? v.toStr(a[2]) : std::u16string();
    auto displayName = [&](const Object* stop) {
      auto m = stop->fieldValues.find(u"<model>");
      Object* tpl = m == stop->fieldValues.end() ? nullptr : v.resolve(m->second).object();
      if (!tpl) return std::u16string();
      auto d = tpl->fieldValues.find(u"displayName");
      return d == tpl->fieldValues.end() || d->second.isNone() ? std::u16string() : v.toStr(d->second);
    };
    int n = 0;
    if (!onLines) {
      for (const auto& o : s->stops) {
        int item = fieldInt(v, o.get(), u"<item>", -1);
        if (item < 0 || size_t(item) >= g->data->items.size() || (g->data->items[size_t(item)].flags & 0x100000)) continue;
        bool typeMatch = (mask >> fieldInt(v, o.get(), u"type", 31)) & 1;
        std::u16string dn = displayName(o.get());
        if ((typeMatch && !filter) || dn == name) ++n;
      }
      return Value::integer(n);
    }
    std::set<int> items;
    for (const LineState& line : s->lines) {
      if (!((mask >> line.type) & 1) || !lineActive(v, line)) continue;
      for (const LineStopState& st : line.stops)
        if (st.item >= 0 && (!filter || (st.stop && displayName(st.stop.get()) == name))) items.insert(st.item);
    }
    return Value::integer(int(items.size()));
  });

  // gsTransportLine
  implement(vm, "gsTransportLine", u"getVehicles", [](VM& v, Object* s, int, const Value*) {
    auto arr = make<ArrayObj>();
    auto* t = transport(s);
    auto st = t ? t->sim.lock() : nullptr;
    if (LineState* l = lineOf(s); l && st)
      for (int vi : l->vehicles) arr->items.push_back(Value::heap(VT::Object, st->vehicles[size_t(vi)].object.get()));
    (void)v;
    return Value::heap(VT::Array, arr.get());
  });
  implement(vm, "gsTransportLine", u"getStop", [](VM& v, Object* s, int, const Value* a) {
    LineState* l = lineOf(s);
    int i = v.toInt(a[0]);
    if (!l || i < 0 || size_t(i) >= l->stops.size() || !l->stops[size_t(i)].stop) return Value::null();
    return Value::heap(VT::Object, l->stops[size_t(i)].stop.get());
  });
  implement(vm, "gsTransportLine", u"getPassengersTransported", [](VM& v, Object* s, int, const Value* a) {
    LineState* l = lineOf(s);
    int i = v.toInt(a[0]);
    return Value::integer(l && i >= 0 && size_t(i) < l->stops.size() ? l->stops[size_t(i)].passengersTransported : 0);
  });
  implement(vm, "gsTransportLine", u"getWaitingTime", [](VM& v, Object* s, int, const Value* a) {
    LineState* l = lineOf(s);
    int i = v.toInt(a[0]);
    return Value::integer(l && i >= 0 && size_t(i) < l->stops.size() ? l->stops[size_t(i)].waitingTime : 0);
  });
  implement(vm, "gsTransportLine", u"hasTempStops", [](VM&, Object* s, int, const Value*) {
    LineState* l = lineOf(s);
    bool temp = false;
    if (l)
      for (const auto& st : l->stops) temp = temp || (st.flags & 1);
    return Value::boolean(temp);
  });
  // calculateLength(): the route's length in world units over all segments
  implement(vm, "gsTransportLine", u"calculateLength", [](VM&, Object* s, int, const Value*) {
    LineState* l = lineOf(s);
    auto* t = transport(s);
    auto st = t ? t->sim.lock() : nullptr;
    float len = 0;
    if (l && st)
      for (const auto& stop : l->stops)
        for (int nd : stop.path) len += st->graph.nodes[size_t(nd)].length;
    return Value::real(len);
  });

  // gsTransportVehicle: the fields the game keeps natively
  if (ClassInfo* c = vm.findClass("gsTransportVehicle")) {
    auto ro = [c](const char16_t* name, std::function<Value(const VehicleState&)> get) {
      FieldInfo& f = c->fields[name];
      f.name = name;
      f.readOnly = true;
      f.get = [name = std::u16string(name), get](VM&, Object* o) {
        if (VehicleState* v = vehicleOf(o)) return get(*v);
        auto it = o->fieldValues.find(name);
        return it == o->fieldValues.end() ? Value() : it->second;
      };
    };
    ro(u"capacity", [](const VehicleState& v) { return Value::integer(v.capacity); });
    ro(u"passengerCount", [](const VehicleState& v) { return Value::integer(v.passengerCount); });
    auto rw = [c](const char16_t* name, std::function<Value(const VehicleState&)> get,
                  std::function<void(VehicleState&, const Value&)> set) {
      FieldInfo& f = c->fields[name];
      f.name = name;
      f.get = [name = std::u16string(name), get](VM&, Object* o) {
        if (VehicleState* v = vehicleOf(o)) return get(*v);
        auto it = o->fieldValues.find(name);
        return it == o->fieldValues.end() ? Value() : it->second;
      };
      f.set = [name = std::u16string(name), set](VM& vm, Object* o, const Value& val) {
        Value x = vm.resolve(val);
        if (VehicleState* v = vehicleOf(o)) set(*v, x);
        else o->fieldValues[name] = x;
      };
    };
    rw(u"stopTime", [](const VehicleState& v) { return Value::real(v.stopTime); }, [](VehicleState& v, const Value& x) { v.stopTime = x.toFloat(); });
    rw(u"attractiveness", [](const VehicleState& v) { return Value::real(v.attractiveness); },
       [](VehicleState& v, const Value& x) { v.attractiveness = x.toFloat(); });
    rw(u"travelDistance", [](const VehicleState& v) { return Value::real(v.travelDistance); },
       [](VehicleState& v, const Value& x) { v.travelDistance = x.toFloat(); });
    rw(u"passengersTransported", [](const VehicleState& v) { return Value::integer(v.passengersTransported); },
       [](VehicleState& v, const Value& x) { v.passengersTransported = x.toInt(); });
    rw(u"broken", [](const VehicleState& v) { return Value::boolean(v.broken); }, [](VehicleState& v, const Value& x) { v.broken = x.toBool(); });
  }
  implement(vm, "gsTransportVehicle", u"isSpawned", [](VM&, Object* s, int, const Value*) { return Value::boolean(vehicleSpawned(s)); });
  registerLineNatives(vm);
}

}  // namespace regs::engine
