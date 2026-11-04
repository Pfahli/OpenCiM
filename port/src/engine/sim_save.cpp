// Saving the simulation into map.grid (gsGrid.saveToFile through engine/grid_save.h): vehicles 0x07040000, characters
// 0x07050000, transport lines 0x07060000 and citizens 0x07070000 in the layouts engine/sim.cpp and
// engine/sim_citizens.cpp read (docs/save-format/vehicles-movement.md, terrain-characters-citizens.md,
// lines-timeline.md; the original writer is FUN_100c3840 with FUN_1010f8a0 for the vehicles).
// Live state is written from the simulation; loaded records it did not take over (boats, aircraft with their inline
// air-lane nodes, animals: SimState::loadedAgentMover / loadedCharacter) go back verbatim from the loaded file, after
// the live ones, with their indices remapped.
// Agents queue the nodes they hold and carry the rest of their route as movement-path step codes, like the original.
// Pedestrians save their walk ahead as direction codes (writeWalkPath), which the loader decodes again.
// Written as 0 because the port does not model them (0 is also the original's reset value): the agents' smoothed
// acceleration, lateral, straight run and last tick, the line vehicles' re-path timer, next-leg steps and bell /
// announce flags, and the pedestrians' seated bits (the port has no sitting). Line vehicles keep no pre-loaded leg.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>

#include "core/log.h"
#include "core/text.h"
#include "engine/grid.h"
#include "engine/grid_save.h"
#include "engine/sim.h"
#include "engine/sim_state.h"

namespace regs::engine {

using namespace regs::script;
using render::Vec3;

namespace sim {
namespace {

struct Out {
  std::vector<uint8_t>& b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { put(&v, 2); }
  void u32(uint32_t v) { put(&v, 4); }
  void i32(int32_t v) { put(&v, 4); }
  void f32(float v) { put(&v, 4); }
  void put(const void* p, size_t n) { b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
  void pad4() {
    while (b.size() % 4) b.push_back(0);
  }
  size_t open(uint32_t tag) {  // a chunk header; close() writes its size
    u32(tag);
    u32(0);
    return b.size();
  }
  void close(size_t body) {
    uint32_t size = uint32_t(b.size() - body);
    std::memcpy(b.data() + body - 4, &size, 4);
  }
  void patch16(size_t at, uint16_t v) { std::memcpy(b.data() + at, &v, 2); }
  void patch32(size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
};

// names padded as a whole table (0x07040100, 0x07060100, 0x07050100)
void nameTable(Out& o, const std::vector<std::string>& names) {
  for (const std::string& n : names) {
    o.put(n.data(), n.size());
    o.u8(0);
  }
  o.pad4();
}

// a name table with stable indices
struct Names {
  std::vector<std::string> list;
  std::map<std::string, int> slot;
  uint16_t of(const std::string& n) {
    auto it = slot.find(n);
    if (it != slot.end()) return uint16_t(it->second);
    slot[n] = int(list.size());
    list.push_back(n);
    return uint16_t(list.size() - 1);
  }
};

// ---- the loaded file ----

uint32_t rd32(const std::vector<uint8_t>& d, size_t at) {
  uint32_t v = 0;
  if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
  return v;
}

template <typename F>
void chunks(const std::vector<uint8_t>& d, size_t from, size_t to, F f) {  // f(tag, begin, body, end)
  for (size_t p = from; p + 8 <= to;) {
    size_t end = std::min(to, p + 8 + rd32(d, p + 4));
    f(rd32(d, p), p, p + 8, end);
    p = end;
  }
}

std::vector<std::string> readNames(const std::vector<uint8_t>& d, size_t body, size_t end, uint32_t count) {
  std::vector<std::string> out;
  for (size_t p = body; out.size() < count && p < end;) {
    std::string n;
    while (p < end && d[p]) n += char(d[p++]);
    ++p;
    out.push_back(n);
  }
  return out;
}

std::vector<uint32_t> readList(const std::vector<uint8_t>& d, size_t body, size_t end) {
  std::vector<uint32_t> out;
  uint32_t n = rd32(d, body);
  for (uint32_t i = 0; i < n && body + 8 + size_t(i) * 4 <= end; ++i) out.push_back(rd32(d, body + 4 + size_t(i) * 4));
  return out;
}

struct SourceRecord {
  size_t begin = 0, end = 0;  // the whole chunk
  std::string name;           // its model / character type
  uint32_t next = 0;          // agents: nextCarRef
};

struct Source {
  std::vector<SourceRecord> agents, characters;
  std::vector<uint32_t> agentLists[3];     // 0x07040300 trains, 0x07040400 aircraft, 0x07040500 boats
  std::vector<uint32_t> animalLists[2];    // 0x07050300 flying, 0x07050400 ground
  std::vector<uint32_t> lineVehicleAgents; // head agent of every 0x07060210
};

Source readSource(const std::vector<uint8_t>& d) {
  Source s;
  size_t end = std::min(d.size(), size_t(8) + rd32(d, 4));
  chunks(d, 32, end, [&](uint32_t tag, size_t, size_t body, size_t stop) {
    if (tag == 0x07040000) {
      std::vector<std::string> models;
      chunks(d, body + 8, stop, [&](uint32_t t, size_t b0, size_t b, size_t e) {
        if (t == 0x07040100) models = readNames(d, b, e, rd32(d, body));
        else if (t == 0x07040200) {
          uint16_t mi = uint16_t(rd32(d, b) & 0xffff);
          s.agents.push_back({b0, e, mi < models.size() ? models[mi] : std::string(), rd32(d, b + 16)});
        } else if (t >= 0x07040300 && t <= 0x07040500 && (t & 0xff) == 0) s.agentLists[(t >> 8 & 0xf) - 3] = readList(d, b, e);
      });
    } else if (tag == 0x07050000) {
      std::vector<std::string> types;
      chunks(d, body + 8, stop, [&](uint32_t t, size_t b0, size_t b, size_t e) {
        if (t == 0x07050100) types = readNames(d, b, e, rd32(d, body));
        else if (t == 0x07050200) {
          uint16_t ti = uint16_t(rd32(d, b) & 0xffff);
          s.characters.push_back({b0, e, ti < types.size() ? types[ti] : std::string(), 0});
        } else if (t == 0x07050300 || t == 0x07050400) s.animalLists[t == 0x07050400] = readList(d, b, e);
      });
    } else if (tag == 0x07060000) {
      chunks(d, body + 16, stop, [&](uint32_t t, size_t, size_t b, size_t e) {
        if (t != 0x07060200) return;
        chunks(d, b + 36, e, [&](uint32_t t2, size_t, size_t b2, size_t) {
          if (t2 == 0x07060210) s.lineVehicleAgents.push_back(rd32(d, b2));
        });
      });
    }
  });
  return s;
}

// ---- the plan: indices every section shares, fixed when the vehicles are written (the first simulation section) ----

struct Plan {
  std::vector<uint32_t> itemRef;                   // item → 1-based index in the map.grid written now, 0 = removed
  std::vector<uint32_t> nodeRef;                   // lane node → (item1 << 8) | lane
  std::vector<std::vector<uint32_t>> moverAgents;  // per mover: 1-based agent index per unit (0 = not written)
  std::vector<uint32_t> characterRecord;           // per character: 1-based record
  Names agentModels;                               // 0x07040100
  std::shared_ptr<const std::vector<uint8_t>> file;  // the loaded map.grid
  Source src;
  std::vector<size_t> passAgents, passCharacters;  // loaded records written back as they were, after the live ones
  std::map<uint32_t, uint32_t> passAgentRef, passCharacterRef;  // loaded 1-based index → written index
  uint32_t item(int i) const { return i < 0 ? 0u : size_t(i) < itemRef.size() ? itemRef[size_t(i)] : uint32_t(i + 1); }
};
std::map<const SimState*, Plan> plans;

std::string idName(VM& vm, const SimState& s, int model) {
  Object* o = model >= 0 ? s.models[size_t(model)].object : nullptr;
  if (!o) return "";
  auto it = o->fieldValues.find(u"idName");
  return it == o->fieldValues.end() ? std::string() : utf16ToUtf8(vm.toStr(it->second));
}

Plan& buildPlan(VM& vm, SimState& s) {
  Plan& p = plans[&s];
  p = Plan();
  if (s.grid && s.grid->data) p.itemRef = savedItemRefs(*s.grid->data);
  p.nodeRef.assign(s.graph.nodes.size(), 0);
  if (s.graph.latticeCount > 0 && s.grid && s.grid->data) {
    // the lanes the next load lays from the flags written now (as the original rebuilds them at save): a lane of
    // ours that is not among them has no ref, and whoever stands on it is not written (save quirk Q4)
    std::vector<LaneNode> next;
    waterLattice(*s.grid->data, s.grid->data->flags, next);
    auto key = [](const LaneNode& n) {
      auto q = [](float v) { return int(std::lround(v * 8.0f)); };
      return std::tuple<int, int, int, int>{q(n.p0.x), q(n.p0.z), q(n.p1.x), q(n.p1.z)};
    };
    std::map<std::tuple<int, int, int, int>, uint32_t> at;
    for (size_t i = 0; i < next.size(); ++i) at[key(next[i])] = uint32_t(next.size() - 1 - i);
    for (int n = 0; n < s.graph.latticeCount; ++n) {
      const LaneNode& nd = s.graph.nodes[size_t(n)];
      auto it = at.find(key(nd));
      if (it != at.end()) p.nodeRef[size_t(n)] = 0x7f000000u | it->second;
    }
  }
  for (size_t item = 0; item < s.graph.itemNodes.size(); ++item) {
    uint32_t ref = p.item(int(item));
    for (size_t lane = 0; ref && lane < s.graph.itemNodes[item].size() && lane < 256; ++lane)
      p.nodeRef[size_t(s.graph.itemNodes[item][lane])] = ref << 8 | uint32_t(lane);
  }
  p.moverAgents.assign(s.movers.size(), {});
  uint32_t next = 1;
  for (size_t id = 0; id < s.movers.size(); ++id) {
    const Mover& m = s.movers[id];
    if (!m.live || m.track.nodes.empty()) continue;
    bool ok = true;
    for (int n : m.track.nodes) ok = ok && (p.nodeRef[size_t(n)] != 0 || (s.graph.nodes[size_t(n)].flags & 0x2000000u));
    if (!ok) continue;
    for (const Unit& u : m.units) {
      p.moverAgents[id].push_back(next++);
      p.agentModels.of(idName(vm, s, u.model));
    }
  }
  p.characterRecord.assign(s.characters.size(), 0);
  uint32_t rec = 1;
  for (size_t i = 0; i < s.characters.size(); ++i)
    if (s.characters[i].alive && s.characters[i].model >= 0) p.characterRecord[i] = rec++;

  // the loaded records nobody took over: agents outside the chains of placed heads and line vehicles, characters
  // that were not loaded
  p.file = s.grid ? s.grid->sourceFile : nullptr;
  if (!p.file) return p;
  p.src = readSource(*p.file);
  size_t n = p.src.agents.size();
  std::vector<uint8_t> taken(n, 0);
  auto chain = [&](size_t i) {
    for (int guard = 0; i < n && !taken[i] && guard < 256; ++guard) {
      taken[i] = 1;
      uint32_t nx = p.src.agents[i].next;
      if (!nx || nx > n) break;
      i = nx - 1;
    }
  };
  for (size_t i = 0; i < n && i < s.loadedAgentMover.size(); ++i)
    if (s.loadedAgentMover[i] >= 0) chain(i);
  for (uint32_t r : p.src.lineVehicleAgents)
    if (r >= 1 && r <= n) chain(r - 1);
  for (size_t i = 0; i < n; ++i)
    if (!taken[i]) {
      p.passAgents.push_back(i);
      p.passAgentRef[uint32_t(i + 1)] = next++;
      p.agentModels.of(p.src.agents[i].name);
    }
  for (size_t i = 0; i < p.src.characters.size(); ++i)
    if (i >= s.loadedCharacter.size() || s.loadedCharacter[i] < 0) {
      p.passCharacters.push_back(i);
      p.passCharacterRef[uint32_t(i + 1)] = rec++;
    }
  return p;
}

Plan& planOf(VM& vm, SimState& s) {
  auto it = plans.find(&s);
  return it == plans.end() ? buildPlan(vm, s) : it->second;
}

uint32_t remap(const std::map<uint32_t, uint32_t>& m, uint32_t ref) {
  auto it = m.find(ref);
  return it == m.end() ? 0 : it->second;
}

// a loaded record back as it was: its type index and the given u32 references remapped
void passRecord(Out& o, const std::vector<uint8_t>& d, const SourceRecord& r, uint16_t type,
                std::initializer_list<size_t> refFields, const std::map<uint32_t, uint32_t>& refs) {
  size_t at = o.b.size();
  o.put(d.data() + r.begin, r.end - r.begin);
  o.patch16(at + 8, type);
  for (size_t f : refFields) o.patch32(at + 8 + f, remap(refs, rd32(d, r.begin + 8 + f)));
}

// a pedestrian's walk ahead (pathfinding.md §5.4): from the q-cell the record stores (the path step it walks to) to the
// goal, one direction code 0..7 per new direction, then 8 + (n − 1) for n ≤ 8 more steps the same way; the layer is
// re-resolved from the height at every step, so it is not stored. Stops at a step that is no unit move.
void writeWalkPath(Out& o, const Character& ch) {
  static const int dxs[8] = {1, 0, -1, 0, 1, 1, -1, -1}, dzs[8] = {0, 1, 0, -1, 1, -1, 1, -1};
  std::vector<uint8_t> codes;
  int steps = 0, last = -1, run = 0;
  auto flush = [&] {
    if (run) codes.push_back(uint8_t(7 + run));
    run = 0;
  };
  for (size_t k = std::min(ch.next, ch.path.size() - 1); k + 1 < ch.path.size(); ++k) {
    int x0 = int(ch.path[k] & 0xffff), z0 = int(ch.path[k] >> 16) & kQzMask;
    int x1 = int(ch.path[k + 1] & 0xffff), z1 = int(ch.path[k + 1] >> 16) & kQzMask;
    int d = 0;
    while (d < 8 && (dxs[d] != x1 - x0 || dzs[d] != z1 - z0)) ++d;
    if (d == 8) break;
    if (d == last && run < 8) {
      ++run;
    } else {
      flush();
      codes.push_back(uint8_t(d));
      last = d;
    }
    ++steps;
  }
  flush();
  size_t b = o.open(0x07030100);
  o.i32(steps), o.i32(int32_t(codes.size())), o.f32(float(steps)), o.u32(0), o.u32(0);  // cursor 0, generation 0
  for (size_t w = 0; w < (codes.size() + 7) / 8; ++w) {
    uint32_t word = 0;
    for (size_t k = 0; k < 8 && w * 8 + k < codes.size(); ++k) word |= uint32_t(codes[w * 8 + k]) << (4 * k);
    o.u32(word);
  }
  o.close(b);
}

void emptyPath(Out& o) {  // 0x07030100 with no steps
  size_t b = o.open(0x07030100);
  o.i32(0), o.i32(0), o.f32(0), o.u32(0), o.u32(0);
  o.close(b);
}

// movement path step codes (FUN_100e5fe0, vehicles-movement.md §2.5) from node `from` through `nodes`: link k ≥ 1 of
// the node's successor list is code k − 1, runs of 1..8 link-0 steps are codes 8..15; false when a step is not a
// successor or beyond the 9th link
bool encodePath(const LaneGraph& G, int from, const int* nodes, size_t count, std::vector<uint8_t>& codes, float& cost) {
  int prev = from, run = 0;
  auto flush = [&] {
    if (run) codes.push_back(uint8_t(7 + run));
    run = 0;
  };
  for (size_t i = 0; i < count; ++i) {
    const std::vector<int>& next = G.nodes[size_t(prev)].next;
    auto it = std::find(next.begin(), next.end(), nodes[i]);
    if (it == next.end() || it - next.begin() > 8) return false;
    if (it == next.begin()) {
      if (++run == 8) flush();
    } else {
      flush();
      codes.push_back(uint8_t(it - next.begin() - 1));
    }
    const LaneNode& nd = G.nodes[size_t(nodes[i])];
    cost += nd.speedLimit > 0 ? nd.length / nd.speedLimit : 0;  // travelCost ≈ Σ length / speed limit
    prev = nodes[i];
  }
  flush();
  return true;
}

void writePath(Out& o, int stepCount, const std::vector<uint8_t>& codes, float cost) {
  size_t b = o.open(0x07030100);
  o.i32(stepCount), o.i32(int32_t(codes.size())), o.f32(cost), o.u32(0), o.u32(0);  // cursor 0, graph generation 0
  for (size_t w = 0; w < (codes.size() + 7) / 8; ++w) {
    uint32_t word = 0;
    for (size_t k = 0; k < 8 && w * 8 + k < codes.size(); ++k) word |= uint32_t(codes[w * 8 + k]) << (4 * k);
    o.u32(word);
  }
  o.close(b);
}

// ---- 0x07040000 ----
// a flight node as a 0x07030200 child (vehicles-movement.md §2.6, writer FUN_100e1eb0): the quaternions turn −Z
// into the node's end directions with +Y up
render::Quat headingQuat(const Vec3& f) {
  float yaw = std::atan2(-f.x, -f.z), pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
  render::Quat qy{0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)}, qx{std::sin(pitch * 0.5f), 0, 0, std::cos(pitch * 0.5f)};
  render::Quat q{qy.w * qx.x, qy.y * qx.w, -qy.y * qx.x, qy.w * qx.w};  // qy · qx
  return render::conjugate(q);  // the engine's forward is q·(0, 0, −1) with the conjugate convention (sim_graph.cpp)
}
void writeInlineNode(Out& o, const LaneNode& n) {
  size_t b = o.open(0x07030200);
  o.u32(n.flags & ~kPathDeleted);
  o.i32(n.cellX), o.i32(n.cellZ);
  render::Quat q0 = headingQuat(n.f0), q1 = headingQuat(n.f1);
  o.f32(n.p0.x), o.f32(n.p0.y), o.f32(n.p0.z), o.f32(q0.x), o.f32(q0.y), o.f32(q0.z), o.f32(q0.w);
  o.f32(n.p1.x), o.f32(n.p1.y), o.f32(n.p1.z), o.f32(q1.x), o.f32(q1.y), o.f32(q1.z), o.f32(q1.w);
  o.f32(n.length), o.f32(n.speedLimit), o.f32(n.curveFactor), o.f32(0);
  o.close(b);
}

void writeVehicles(VM& vm, SimState& s, Plan& p, Out& o) {
  size_t root = o.open(0x07040000);
  uint32_t count = uint32_t(p.passAgents.size());
  for (const auto& a : p.moverAgents) count += uint32_t(a.size());
  o.u32(uint32_t(p.agentModels.list.size()));
  o.u32(count);
  {
    size_t b = o.open(0x07040100);
    nameTable(o, p.agentModels.list);
    o.close(b);
  }
  std::vector<uint32_t> trains, aircraft, boats;
  for (size_t id = 0; id < s.movers.size(); ++id) {
    const auto& agents = p.moverAgents[id];
    if (agents.empty()) continue;
    const Mover& m = s.movers[id];
    if (m.train) trains.push_back(agents[0]);
    if (m.aircraft) aircraft.push_back(agents[0]);
    if (m.boat) boats.push_back(agents[0]);
    const VehicleState* v = m.vehicle >= 0 && size_t(m.vehicle) < s.vehicles.size() ? &s.vehicles[size_t(m.vehicle)] : nullptr;
    int lineType = v && v->line >= 0 && size_t(v->line) < s.lines.size() ? s.lines[size_t(v->line)].type : -1;
    const std::vector<int>& tn = m.track.nodes;
    int last = int(tn.size()) - 1;
    std::vector<size_t> curs;  // each car's node, head first
    float d = m.dist;
    for (size_t k = 0; k < m.units.size(); ++k) {
      if (k > 0) d -= unitLength(s, m.units[k]);
      curs.push_back(m.track.at(std::max(0.0f, d)));
    }
    // the head queues the nodes it holds ahead; the rest of the route goes into its movement path, whose step codes
    // continue from the last queued node (FUN_1010de50); following cars queue up to that same node (the loaders read
    // the last car's queue, then the head's path)
    int ringEnd = m.parked ? int(curs[0]) : std::clamp(m.holdEnd - 1, int(curs[0]), last);
    std::vector<uint8_t> codes;
    float cost = 0;
    int steps = m.parked ? -1 : 0;
    if (!m.parked && ringEnd < last) {
      if (encodePath(s.graph, tn[size_t(ringEnd)], tn.data() + ringEnd + 1, size_t(last - ringEnd), codes, cost)) steps = last - ringEnd;
      else ringEnd = last, codes.clear();  // not expressible as codes: the queue takes the rest
    }
    float r = m.dist;
    for (size_t k = 0; k < m.units.size(); ++k) {
      const Unit& u = m.units[k];
      if (k > 0) r -= unitLength(s, u);
      size_t cur = curs[k];
      // refs in record order (curNode, prevNode, ring); flight nodes become this record's 0x07030200 children
      std::vector<int> kids;
      auto refOf = [&](int n) {
        if (p.nodeRef[size_t(n)] || !(s.graph.nodes[size_t(n)].flags & 0x2000000u)) return p.nodeRef[size_t(n)];
        kids.push_back(n);
        return 0x80000000u | uint32_t(kids.size() - 1);
      };
      uint32_t curRef = refOf(tn[cur]), prevRef = cur > 0 ? refOf(tn[cur - 1]) : 0;
      std::vector<uint32_t> ring;
      if (!m.parked)
        for (int j = int(cur) + 1; j <= ringEnd && ring.size() < 60000; ++j) ring.push_back(refOf(tn[size_t(j)]));
      // lengthNodes = max(model length, spawn length): AI trains give every car the train's, line vehicles their cars'
      // own except a metro set's head and last car (FUN_101152a0), other following cars (trailers) their own
      int own = u.model >= 0 ? std::max(1, s.models[size_t(u.model)].length) : 1, set = std::max(1, m.lengthNodes);
      int len = m.parked || m.train ? set
                : m.transit       ? (lineType >= 2 && (k == 0 || k + 1 == m.units.size()) ? set : own)
                : k == 0          ? set
                                  : own;
      int cap = 2 * len + 4;  // ctor capacity, grown by 2 (FUN_101089d0)
      while (cap < int(ring.size())) cap += 2;
      // claims: the head occupies its node and the queued ones, a following car the nodes up to the car ahead
      int occupied = m.parked ? 1 : k == 0 ? int(ring.size()) + 1 : std::max(1, int(curs[k - 1]) - int(cur));
      int reserved = m.parked ? 0 : occupied - 1;
      size_t b = o.open(0x07040200);
      o.u16(p.agentModels.of(idName(vm, s, u.model)));
      o.u16(uint16_t(std::min(cap, 0xfffe)));                // ring capacity
      o.u16(uint16_t(len));                                  // lengthNodes
      o.u16(0);                                              // ring head
      o.u16(uint16_t(ring.size()));
      o.u16(u.seed ? u.seed : m.colorSeed);
      o.i32(int32_t((r - m.track.start[cur]) * 65536.0f));   // posOnNode, 16.16
      o.u32(k + 1 < m.units.size() ? agents[k + 1] : 0);     // next car
      o.u32(k > 0 && !m.train ? agents[0] : 0);              // the head for every following car (0 for AI trains)
      o.f32(m.speed);
      o.u32(0);
      o.f32(m.waitTime);
      o.f32(0), o.f32(0);                                    // smoothed acceleration, lateral
      o.i32(reserved);                                       // reservedCount
      o.i32(occupied);                                       // occupiedCount
      o.i32(0);                                              // straightRun
      o.i32(m.parked ? -1 : 0);                              // lastTick
      o.i32(k == 0 ? steps : -1);                            // pathStepsLeft: −1 parked cars and following cars
      // 0x200 line vehicle (every car), 0x1000 water bus cars, 0x2 reversed, 0x2000 a citizen's parked car, 0x800
      // reckless, 0x40000 crashing (every car); the head of a line vehicle: 0x400 halted at its stop, 0x10000 broken
      // down and halted (FUN_101152a0)
      uint32_t flags = (m.transit ? 0x200u : 0u) | (lineType == 3 ? 0x1000u : 0u) | (u.reversed ? 2u : 0u) |
                       (m.parked && m.citizen >= 0 ? 0x2000u : 0u) | (m.reckless ? 0x800u : 0u) |
                       (m.crashing ? 0x40000u : 0u);
      if (k == 0 && v) flags |= (v->halted ? 0x400u : 0u) | (v->brokenHalted ? 0x10000u : 0u);
      o.u32(flags);
      o.u32(curRef);
      o.u32(prevRef);
      o.f32(m.crashOffset.x), o.f32(m.crashOffset.y), o.f32(m.crashOffset.z);
      for (uint32_t ref : ring) o.u32(ref);
      for (int n : kids) writeInlineNode(o, s.graph.nodes[size_t(n)]);
      if (k == 0 && steps > 0) writePath(o, steps, codes, cost);
      else emptyPath(o);
      o.close(b);
    }
  }
  // boats, aircraft and anything else not placed: back as loaded (inline air-lane nodes are record-local refs)
  for (size_t i : p.passAgents) {
    const SourceRecord& r = p.src.agents[i];
    passRecord(o, *p.file, r, p.agentModels.of(r.name), {16, 20}, p.passAgentRef);
  }
  for (uint32_t ref : p.src.agentLists[0])
    if (uint32_t w = remap(p.passAgentRef, ref)) trains.push_back(w);
  for (int list = 0; list < 3; ++list) {
    size_t b = o.open(0x07040300u + uint32_t(list) * 0x100u);
    std::vector<uint32_t> refs;
    if (list == 0) refs = trains;
    else if (list == 1) refs = aircraft;
    else refs = boats;
    if (list > 0)
      for (uint32_t ref : p.src.agentLists[list])
        if (uint32_t w = remap(p.passAgentRef, ref)) refs.push_back(w);
    o.u32(uint32_t(refs.size()));
    for (uint32_t a : refs) o.u32(a);
    o.close(b);
  }
  o.close(root);
}

// ---- 0x07050000 ----
void writeCharacters(VM& vm, SimState& s, Plan& p, Out& o) {
  Names types;
  uint32_t count = uint32_t(p.passCharacters.size());
  for (size_t i = 0; i < s.characters.size(); ++i)
    if (p.characterRecord[i]) {
      ++count;
      types.of(idName(vm, s, s.characters[i].model));
    }
  for (size_t i : p.passCharacters) types.of(p.src.characters[i].name);
  size_t root = o.open(0x07050000);
  o.u32(uint32_t(types.list.size()));
  o.u32(count);
  {
    size_t b = o.open(0x07050100);
    nameTable(o, types.list);
    o.close(b);
  }
  const GridData& g = *s.grid->data;
  for (size_t i = 0; i < s.characters.size(); ++i) {
    if (!p.characterRecord[i]) continue;
    const Character& ch = s.characters[i];
    uint32_t q = ch.path.empty() ? 0 : ch.path[std::min(ch.next, ch.path.size() - 1)];
    int qx = ch.path.empty() ? int((ch.pos.x / g.cellSize + float(g.width) * 0.5f) * 4) : int(q & 0xffff);
    int qz = ch.path.empty() ? int((ch.pos.z / g.cellSize + float(g.length) * 0.5f) * 4) : int((q >> 16) & kQzMask);
    int y = int(ch.pos.y * 256.0f / g.cellSize);
    // on a road crossing: traffic waits for it (the loader raises it again); the path step's layer bits pick the deck
    bool crossing = (ch.path.empty() ? s.walk.code(qx, qz) : s.walk.code(int(q & 0xffff), int(q >> 16))) == 1;
    render::Vec3 v = ch.done() ? render::Vec3{} : render::Vec3{std::sin(ch.yaw), 0, std::cos(ch.yaw)} * ch.speed;
    float heading = 3.14159265f - ch.yaw;      // the file's heading is atan2(vx, −vz), 0..2π
    heading -= 6.2831853f * std::floor(heading / 6.2831853f);
    int yLayer = y;
    if (ch.animal == 1) qx = ch.target[0], yLayer = ch.target[1], qz = ch.target[2];  // a bird: where it flies to
    size_t b = o.open(0x07050200);
    o.u16(types.of(idName(vm, s, ch.model)));
    o.u16(ch.anchor);                 // animals' home block, 0 for people
    o.i32(qx), o.i32(yLayer), o.i32(y), o.i32(qz);
    o.i32(-1);                        // frame stamp
    if (crossing) o.i32(qx), o.i32(y), o.i32(qz);
    else o.i32(-1), o.i32(0), o.i32(0);
    o.i32(qx), o.i32(y), o.i32(qz);   // reserved cell: the one it stands on (the loader claims it again)
    o.f32(ch.pos.x), o.f32(ch.pos.y), o.f32(ch.pos.z);
    o.f32(v.x), o.f32(v.y), o.f32(v.z);
    o.f32(heading);
    o.u32(1);                         // obeys crossings
    if (!ch.animal && ch.virtualLeft < 0 && !ch.path.empty()) writeWalkPath(o, ch);  // the walk ahead
    else emptyPath(o);
    o.close(b);
  }
  // records not loaded: back as loaded
  for (size_t i : p.passCharacters) {
    const SourceRecord& r = p.src.characters[i];
    passRecord(o, *p.file, r, types.of(r.name), {}, p.passCharacterRef);
  }
  for (int list = 0; list < 2; ++list) {  // flying, ground animals
    size_t b = o.open(0x07050300u + uint32_t(list) * 0x100u);
    std::vector<uint32_t> refs;
    for (size_t i = 0; i < s.characters.size(); ++i)
      if (p.characterRecord[i] && s.characters[i].animal == list + 1) refs.push_back(p.characterRecord[i]);
    for (uint32_t ref : p.src.animalLists[list])
      if (uint32_t w = remap(p.passCharacterRef, ref)) refs.push_back(w);
    o.u32(uint32_t(refs.size()));
    for (uint32_t a : refs) o.u32(a);
    o.close(b);
  }
  o.close(root);
}

// ---- 0x07060000 ----
// the 1-based record of every line vehicle in the order the lines write them (deleted lines and vehicles have none);
// the citizens' vehicle refs and the loader count vehicles that way
std::vector<uint32_t> vehicleRecords(const SimState& s) {
  std::vector<uint32_t> rec(s.vehicles.size(), 0);
  uint32_t next = 0;
  for (const LineState& line : s.lines)
    if (!line.deleted)
      for (int vi : line.vehicles) rec[size_t(vi)] = ++next;
  return rec;
}

void writeLines(VM& vm, SimState& s, Plan& p, Out& o) {
  Names models;
  uint32_t vehicles = 0, lines = 0;
  for (const LineState& line : s.lines) {
    if (line.deleted) continue;
    ++lines;
    for (int vi : line.vehicles) {
      ++vehicles;
      for (const Unit& u : s.vehicles[size_t(vi)].units)
        if (u.model >= 0) models.of(idName(vm, s, u.model));
    }
  }
  size_t root = o.open(0x07060000);
  o.u32(uint32_t(models.list.size()));
  o.u32(lines);
  o.u32(vehicles);
  o.u32(uint32_t(s.lineNodes.size()));
  {
    size_t b = o.open(0x07060100);
    nameTable(o, models.list);
    o.close(b);
  }
  for (const LineState& line : s.lines) {
    if (line.deleted) continue;
    size_t b = o.open(0x07060200);
    Object* lo = line.object.get();
    o.f32(line.headway);
    o.u32(uint32_t(line.type));
    o.f32(line.effAttractiveness);
    o.u32(uint32_t(line.vehicles.size()));
    o.u32(uint32_t(line.stops.size()));
    o.f32(line.avgWait);
    o.f32(lo ? fieldFloat(vm, lo, u"ticketPrice", 0) : 0);
    o.i32(lo ? fieldInt(vm, lo, u"passengersTransported", 0) : 0);
    o.u8(line.closed), o.u8(0), o.u8(0), o.u8(0);
    for (const LineStopState& st : line.stops) {
      size_t sb = o.open(0x07060220);
      o.u32(p.item(st.item));
      o.u32(st.flags);
      o.f32(st.sinceDeparture);
      o.i32(st.passengersTransported);
      o.i32(st.waitingTime);
      o.close(sb);
    }
    for (int vi : line.vehicles) {
      const VehicleState& v = s.vehicles[size_t(vi)];
      size_t vb = o.open(0x07060210);
      const auto* agents = v.mover >= 0 && size_t(v.mover) < p.moverAgents.size() ? &p.moverAgents[size_t(v.mover)] : nullptr;
      o.u32(agents && !agents->empty() ? (*agents)[0] : 0);
      o.u32(v.stopA >= 0 ? uint32_t(v.stopA + 1) : 0);
      o.u32(v.stopB >= 0 ? uint32_t(v.stopB + 1) : 0);
      o.u32(v.stopB >= 0 && line.stops[size_t(v.stopB)].docked == vi ? uint32_t(v.stopB + 1) : 0);
      o.f32(v.stopTime);
      o.f32(0);                       // re-path timer
      o.i32(0);                       // next leg steps
      o.i32(v.capacity);
      o.i32(v.passengerCount);
      o.i32(v.boardingReserved);
      o.f32(v.attractiveness);
      o.f32(v.travelDistance);
      o.u32(uint32_t(v.units.size()));
      o.u8(0), o.u8(0), o.u8(0);      // bell rung, announced, at stop B
      o.u8(v.broken);
      o.i32(v.passengersTransported);
      for (size_t k = 0; k < v.units.size(); ++k) {
        o.u16(v.units[k].model >= 0 ? models.of(idName(vm, s, v.units[k].model)) : 0);
        o.u8(v.units[k].reversed && k > 0);
        o.u8(0);
      }
      o.close(vb);
    }
    o.close(b);
  }
  o.close(root);
}

// ---- 0x07070000 ----
void writeCitizens(VM& vm, SimState& s, Plan& p, Out& o) {
  size_t root = o.open(0x07070000);
  uint32_t count = 0;
  for (const Citizen& c : s.citizens) count += c.alive;
  o.u32(count);
  for (const int* stat : {s.walkCount, s.driveCount, s.travelCount, s.canTravelCount, s.priceTooHighCount})
    for (int k = 0; k < 7; ++k) o.i32(stat[k]);
  auto item = [&p](int i) { return p.item(i); };
  std::vector<uint32_t> vehicleRec = vehicleRecords(s);
  for (const Citizen& c : s.citizens) {
    if (!c.alive) continue;
    size_t b = o.open(0x07070100);
    o.u32(c.flags & ~0x100u);  // 0x100 is the port's "driving"; the loader sets it again from the car
    o.u32(c.id);
    o.u32(c.tripQueue);
    o.u32(c.usedTransport);
    o.i32(c.cycleTimer);
    o.i32(c.waitTime);
    o.u32(c.character >= 0 && size_t(c.character) < p.characterRecord.size() ? p.characterRecord[size_t(c.character)] : 0);
    const auto* agents = c.car >= 0 && size_t(c.car) < p.moverAgents.size() ? &p.moverAgents[size_t(c.car)] : nullptr;
    o.u32(agents && !agents->empty() ? (*agents)[0] : 0);
    o.u32(c.lineNode >= 0 ? uint32_t(c.lineNode + 1) : 0);
    o.u32(c.vehicle >= 0 && size_t(c.vehicle) < vehicleRec.size() ? vehicleRec[size_t(c.vehicle)] : 0);
    o.u32(item(c.home)), o.u32(item(c.work)), o.u32(item(c.start)), o.u32(item(c.target));
    o.u32(uint32_t(std::max(0, c.charModel)));
    o.u32(c.carModel >= 0 ? uint32_t(c.carModel) : 0xffffffffu);
    if (c.hasRoute && !c.route.nodes.empty()) {
      size_t rb = o.open(0x07061000);
      o.f32(c.route.cost);
      o.u32(uint32_t(c.route.nodes.size()));
      o.u32(uint32_t(c.route.leg));
      for (int n : c.route.nodes) o.u32(uint32_t(n + 1));
      o.close(rb);
    }
    o.close(b);
  }
  o.close(root);
  (void)vm;
}

}  // namespace

void registerSimSave() {
  setGridSectionWriter([](VM& vm, GridObj* grid, uint32_t tag, std::vector<uint8_t>& out) {
    SimState* s = grid ? grid->sim.get() : nullptr;
    if (!s || tag < 0x07040000 || tag > 0x07070000) return false;
    out.clear();
    Out o{out};
    switch (tag) {
      case 0x07040000: writeVehicles(vm, *s, buildPlan(vm, *s), o); return true;
      case 0x07050000: writeCharacters(vm, *s, planOf(vm, *s), o); return true;
      case 0x07060000: writeLines(vm, *s, planOf(vm, *s), o); return true;
      case 0x07070000:
        if (!s->citizensLoaded) return false;
        writeCitizens(vm, *s, planOf(vm, *s), o);
        {
          Plan& p = planOf(vm, *s);
          int agents = 0, moving = 0, parked = 0, chars = 0, waiting = 0, riding = 0, cits = 0;
          for (size_t id = 0; id < p.moverAgents.size(); ++id)
            if (!p.moverAgents[id].empty()) agents += int(p.moverAgents[id].size()), ++(s->movers[id].parked ? parked : moving);
          for (uint32_t r : p.characterRecord) chars += r != 0;
          for (const Citizen& c : s->citizens) cits += c.alive, waiting += c.alive && c.lineNode >= 0, riding += c.alive && c.vehicle >= 0;
          LOG_INFO("sim: saved %d agents (%d moving vehicles incl. line vehicles, %d parked) + %zu as loaded, %d pedestrians + "
                   "%zu as loaded, %zu lines, %d citizens (%d waiting, %d riding)",
                   agents, moving, parked, p.passAgents.size(), chars, p.passCharacters.size(), s->lines.size(), cits, waiting,
                   riding);
        }
        plans.erase(s);  // the last simulation section: the next save plans afresh
        return true;
      default: return false;
    }
  });
}

}  // namespace sim
}  // namespace regs::engine
