// Door items (docs/simulation/grid-traffic.md §5.1.1): items with flag 0x1000 (GRID_ITEM_CROSS_TRACK) and a
// doorOpenLen animate their skeleton by the occupancy of their own train-track nodes (node flag 0x1000). Vanilla has
// one, the level crossing track_double_cross_small (metro/objects/rail-props/double_cross_small): its two barriers
// lower over the clip's first second while a train holds a track node of the crossing and rise over the close clip
// once none does. gsGridItem::move L:0x08182b10.c:96-167, W:0x100eb5a0.c:63-121; gsGridItem::animate L:0x08181ff0.
// Road traffic is not blocked by the barrier itself: cars wait because the crossing's road nodes cross its track
// nodes (canEnter's conflict list, §6.3), exactly while the train holds them. The bell is engine/sim_sounds.cpp.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "core/log.h"
#include "engine/display.h"
#include "engine/sim.h"
#include "engine/sim_state.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::engine::sim;
using render::Vec3;

namespace sim {

void buildDoors(VM& vm, GridObj* grid, SimState& s) {
  std::unordered_map<uint32_t, float> phase;  // live edits keep the phases of the items that stay
  for (const DoorItem& d : s.doors) phase[d.item] = d.phase;
  s.doors.clear();
  s.doorTimes.clear();
  if (!grid || !grid->data) return;
  const GridData& g = *grid->data;
  struct Type {
    bool known = false, door = false;
    uint32_t flags = 0;
    float os = 0, ol = 0, cs = 0, cl = 0;
  };
  std::vector<Type> types(g.types.size());
  for (size_t i = 0; i < g.items.size(); ++i) {
    const GridData::Item& it = g.items[i];
    if (it.removed || it.type >= types.size()) continue;
    Type& t = types[it.type];
    if (!t.known) {
      t.known = true;
      auto tp = grid->templates.find(g.types[it.type]);
      if (tp != grid->templates.end()) {
        const Object* o = tp->second.get();
        t.flags = uint32_t(fieldInt(vm, o, u"flags", 0));
        t.os = fieldFloat(vm, o, u"doorOpenStart", 0);
        t.ol = fieldFloat(vm, o, u"doorOpenLen", 0);
        t.cs = fieldFloat(vm, o, u"doorCloseStart", 0);
        t.cl = fieldFloat(vm, o, u"doorCloseLen", 0);
        t.door = t.ol != 0;  // doorOpenLen == 0: nothing animates (:96-104)
      }
    }
    // item flags = the ctor's flags | the template's (L:0x08187fc0); hidden items (0x400000) are skipped (:44)
    uint32_t flags = it.flags | t.flags;
    if (!t.door || !(flags & 0x1000) || (flags & 0x400000)) continue;
    DoorItem d;
    d.item = uint32_t(i);
    if (i < s.graph.itemNodes.size())
      for (int n : s.graph.itemNodes[i])
        if (s.graph.nodes[size_t(n)].flags & kPathTrain) d.nodes.push_back(n);
    d.centre = itemCentre(vm, grid, int(i));
    d.openStart = t.os, d.openLen = t.ol, d.closeStart = t.cs, d.closeLen = t.cl;
    d.nearOnly = (flags & 0x4000000) != 0;
    if (auto p = phase.find(d.item); p != phase.end()) d.phase = p->second;
    s.doorTimes[d.item] = -1;
    s.doors.push_back(std::move(d));
  }
  if (debugSim() && !s.doors.empty()) {
    size_t nodes = 0, crossings = 0;
    for (const DoorItem& d : s.doors) {
      nodes += d.nodes.size();
      for (int n : d.nodes) crossings += s.graph.nodes[size_t(n)].crossings.size();
    }
    LOG_INFO("sim: %zu door items, %zu track nodes, %zu crossing road nodes", s.doors.size(), nodes, crossings);
  }
}

void stepDoors(SimState& s, float dt) {
  for (DoorItem& d : s.doors) {
    // gsGridItem::move runs only for the items of active buckets (the grid tick, grid-traffic.md §3); the port takes
    // the horizontal distance to the last drawn eye. An inactive item keeps its phase.
    float eyeDist = 0;
    if (s.eyeKnown) {
      Vec3 e = d.centre - s.eye;
      d.active = e.x * e.x + e.z * e.z < kActiveRadius * kActiveRadius;
      eyeDist = std::sqrt(dot(e, e));
    } else {
      d.active = true;
    }
    if (!d.active) continue;
    if (d.nearOnly && eyeDist >= 300.0f) {  // :96-104: no animation, the sound slot is freed
      d.opening = false;
      continue;
    }
    bool occupied = false;
    for (int n : d.nodes)
      if (s.rt[size_t(n)].occupant >= 0) {
        occupied = true;
        break;
      }
    if (occupied) {  // :122-146: a closing phase continues as the opening one, 1 s to open
      float p = d.phase < 0 ? d.phase + 1.0f : d.phase;
      // test switch REGS_SHOT_DOORS=<prefix>: a screenshot <prefix>-<item>.png when a crossing's barriers are down
      if (p < 1.0f && p + dt >= 1.0f)
        if (const char* shot = std::getenv("REGS_SHOT_DOORS"))
          if (Display* disp = display())
            disp->requestScreenshot(std::string(shot) + "-" + std::to_string(d.item) + ".png");
      p = std::min(p + dt, 1.0f);
      d.phase = p;
      d.time = d.openStart + p * d.openLen;
    } else {  // :148-167: an opening phase continues as the closing one, 1 s to close, then the bind pose
      float p = d.phase > 0 ? d.phase - 1.0f : d.phase;
      p -= dt;
      if (p <= -1.0f) {
        d.phase = -1.0f;
        d.time = -1.0f;
      } else {
        d.phase = p;
        d.time = d.closeStart - p * d.closeLen;
      }
    }
    if (d.opening != occupied && debugSim())
      LOG_INFO("sim: door item %u %s (phase %.2f)", d.item, occupied ? "closes the road" : "opens the road", d.phase);
    d.opening = occupied;
    s.doorTimes[d.item] = d.time;
  }
}

}  // namespace sim

const std::unordered_map<uint32_t, float>* simDoorTimes(GridObj* grid) {
  SimState* s = simOf(grid);
  return s && !s->doorTimes.empty() ? &s->doorTimes : nullptr;
}

}  // namespace regs::engine
