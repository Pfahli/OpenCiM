// Line and vehicle management (docs/simulation/transport.md §3, §4.1, docs/ui/lines.md): creating and deleting lines,
// adding (also as the add-stop tool's temporary stops), deleting and moving stops, buying vehicles onto a line and
// taking them off, pausing a line, and the line statistics the panels read.
//
// The port keeps a line's stops as a vector in ring order (stops[0] = the ring's first node) instead of the engine's
// linked list of 0x6c-byte nodes; every edit maps the old positions to the new ones so that waiting and planned
// passengers (global line-node ids), docked vehicles and the vehicles' stop indices follow their nodes. Deleted lines
// and vehicles stay in the state as tombstones (LineState/VehicleState::deleted), so indices stay valid.
#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/grid.h"
#include "engine/scene3d.h"
#include "engine/sim.h"
#include "engine/sim_state.h"

namespace regs::engine {

using namespace regs::script;
using render::Vec3;
using namespace sim;

namespace sim {
namespace {

GridObj* gridOfObject(Object* o) {
  for (Object* p = o; p; p = p->parent)
    if (auto* g = dynamic_cast<GridObj*>(p)) return g;
  return nullptr;
}

std::shared_ptr<SimState> stateOf(Object* o) {
  auto* t = transport(o);
  return t ? t->sim.lock() : nullptr;
}

int lineIndex(Object* o) {
  auto* t = transport(o);
  auto s = stateOf(o);
  if (!s || t->index < 0 || size_t(t->index) >= s->lines.size() || s->lines[size_t(t->index)].deleted) return -1;
  return t->index;
}

// gsTransportVehicle::statusUpdated (0x10110840, transport.md §4.1): leaving the active state (the line paused, a
// stop edit under it, destruction) frees the agent and fires onUnspawn; reservations and the dock go
void unspawnVehicle(VM& vm, SimState& s, LineState& line, int vi, bool event) {
  VehicleState& v = s.vehicles[size_t(vi)];
  v.boardingReserved = 0;
  for (auto& st : line.stops)
    if (st.docked == vi) st.docked = -1;
  bool live = v.mover >= 0 && s.movers[size_t(v.mover)].live;
  if (live) despawn(vm, s, v.mover);
  v.stopTime = 0;
  v.halted = v.brokenHalted = false;
  v.doorPos = -1;
  if (live && event && v.object) vm.fireEvent(v.object.get(), u"onUnspawn", {});
}

// the vehicle leaves the simulation (slot 29 0x101151b0: off the line's vector, undocked, the agent freed)
void dropVehicle(VM& vm, SimState& s, int vi) {
  VehicleState& v = s.vehicles[size_t(vi)];
  if (v.deleted) return;
  LineState& line = s.lines[size_t(v.line)];
  unspawnVehicle(vm, s, line, vi, false);
  if (v.mover >= 0) {  // the transit mover goes back to the pool
    Mover& m = s.movers[size_t(v.mover)];
    m = Mover();
    s.freeMovers.push_back(v.mover);
    v.mover = -1;
  }
  std::erase(line.vehicles, vi);
  v.deleted = true;
  v.passengerCount = 0;
  v.object = Ref<Object>();
}

// ---- edits: positions before → after ----

std::vector<int> identityMap(size_t n) {
  std::vector<int> m(n);
  for (size_t k = 0; k < n; ++k) m[k] = int(k);
  return m;
}

// before an edit: passengers waiting at, or planning through, a node the edit removes end their trip (the engine's
// citizens see the node's deleted flag 8, 0x100cc990.c:194,234,310-312); with lineGone also those riding its vehicles
void editBegin(SimState& s, int li, const std::vector<int>& oldToNew, bool lineGone) {
  std::vector<uint8_t> gone(s.lineNodes.size(), 0), changed(s.lines.size(), 0);
  bool any = lineGone;
  const LineState& line = s.lines[size_t(li)];
  for (size_t k = 0; k < oldToNew.size(); ++k)
    if (oldToNew[k] < 0 && size_t(line.firstNode) + k < gone.size()) gone[size_t(line.firstNode) + k] = 1, any = true;
  if (lineGone) changed[size_t(li)] = 1;
  if (any) citizensLeaveLineNodes(s, gone, changed);
}

// after an edit: the global node ids again (line by line, in ring order, as the save writes them), the passengers'
// nodes and the transfers; the line's vehicles keep their stops, or leave the line when one went or the ring opened
void editEnd(VM& vm, SimState& s, int li, const std::vector<int>& oldToNew) {
  size_t oldCount = s.lineNodes.size();
  std::vector<int> newNode(oldCount, -1);
  std::vector<std::pair<int, int>> nodes;
  for (size_t l = 0; l < s.lines.size(); ++l) {
    LineState& L = s.lines[l];
    int base = int(nodes.size());
    size_t oldN = int(l) == li ? oldToNew.size() : L.stops.size();
    for (size_t k = 0; k < oldN; ++k) {
      size_t g = size_t(L.firstNode) + k;
      if (g >= oldCount) continue;
      int nk = int(l) == li ? oldToNew[k] : int(k);
      newNode[g] = nk >= 0 ? base + nk : -1;
    }
    L.firstNode = base;
    for (size_t k = 0; k < L.stops.size(); ++k) nodes.push_back({int(l), int(k)});
  }
  s.lineNodes = std::move(nodes);
  citizensRemapLineNodes(s, newNode);
  LineState& line = s.lines[size_t(li)];
  auto map = [&](int k) { return k >= 0 && size_t(k) < oldToNew.size() ? oldToNew[size_t(k)] : -1; };
  for (int vi : std::vector<int>(line.vehicles)) {
    VehicleState& v = s.vehicles[size_t(vi)];
    bool live = v.mover >= 0 && s.movers[size_t(v.mover)].live;
    int a = map(v.stopA), b = map(v.stopB);
    if (live && (a < 0 || b < 0 || !line.closed || line.stops.size() < 2)) {
      unspawnVehicle(vm, s, line, vi, true);  // re-enters at a platform once the line runs again (§4.2)
      v.stopA = v.stopB = -1;
    } else {
      v.stopA = a, v.stopB = b;
    }
  }
}

LineStopState newStop(VM& vm, GridObj* grid, SimState& s, const LineState& line, int item, bool temp) {
  LineStopState st;
  st.item = item;
  st.flags = temp ? 1u : 0u;
  if (auto k = s.stopOfItem.find(item); k != s.stopOfItem.end()) st.stop = s.stops[size_t(k->second)];
  stopPlatforms(vm, grid, s, line, st);
  return st;
}

}  // namespace

// gsTransportLine::addStop(item, isTemp) (L:0x081c2fa0, transport.md §3.1): the first stop; on an open line a new last
// stop, or the first stop again, which closes the ring (a temporary closing marks the first node temporary); on a
// closed ring the stop goes into the edge that lengthens the ring least, of those whose two new legs have a path.
// Returns the new stop's position (0 for the closing), −1 when the item is on the line already or nothing fits.
int lineAddStop(VM& vm, GridObj* grid, SimState& s, int li, int item, bool temp) {
  LineState& line = s.lines[size_t(li)];
  size_t n = line.stops.size();
  if (n == 0) {
    editBegin(s, li, {}, false);
    line.stops.push_back(newStop(vm, grid, s, line, item, temp));
    computeSegment(s, line, 0);
    editEnd(vm, s, li, {});
    return 0;
  }
  if (!line.closed) {
    for (size_t k = 1; k < n; ++k)
      if (line.stops[k].item == item) return -1;
    std::vector<int> same = identityMap(n);
    if (line.stops[0].item == item) {
      if (n == 1) return -1;
      editBegin(s, li, same, false);
      line.closed = true;
      if (temp) line.stops[0].flags |= 1;
      computeSegment(s, line, n - 1);
      editEnd(vm, s, li, same);
      return 0;
    }
    editBegin(s, li, same, false);
    line.stops.push_back(newStop(vm, grid, s, line, item, temp));
    computeSegment(s, line, n - 1);
    computeSegment(s, line, n);
    editEnd(vm, s, li, same);
    return int(n);
  }
  for (const LineStopState& st : line.stops)
    if (st.item == item) return -1;
  LineStopState cand = newStop(vm, grid, s, line, item, temp);
  int best = -1;
  float bestLonger = 0;
  for (size_t k = 0; k < n; ++k) {
    const LineStopState& a = line.stops[k];
    const LineStopState& b = line.stops[(k + 1) % n];
    float d1 = segmentBetween(s, line, a, cand, nullptr);
    if (d1 == 0) continue;
    float d2 = segmentBetween(s, line, cand, b, nullptr);
    if (d2 == 0) continue;
    float d3 = a.reachable ? a.cost : 0;  // the stored leg a → b: the same search's result
    float longer = d1 + d2 - d3;
    if (best < 0 || longer < bestLonger) best = int(k), bestLonger = longer;
  }
  if (best < 0) return -1;
  size_t p = size_t(best) + 1;
  std::vector<int> shift(n);
  for (size_t k = 0; k < n; ++k) shift[k] = k < p ? int(k) : int(k) + 1;
  editBegin(s, li, shift, false);
  line.stops.insert(line.stops.begin() + std::ptrdiff_t(p), std::move(cand));
  computeSegment(s, line, size_t(best));
  computeSegment(s, line, p);
  editEnd(vm, s, li, shift);
  return int(p);
}

// gsTransportLine::removeStop (L:0x081c2830, §3.3): the stop leaves the ring and its predecessor's leg runs to the next
// stop (or ends: open lines); the first stop of a closed ring is not removed, the ring opens instead (isClosed = 0, its
// temporary flag cleared). Port: a ring left with one stop is open (the engine keeps it closed onto itself).
void lineRemoveStop(VM& vm, SimState& s, int li, size_t k) {
  LineState& line = s.lines[size_t(li)];
  size_t n = line.stops.size();
  if (k >= n) return;
  if (line.closed && k == 0) {
    std::vector<int> same = identityMap(n);
    editBegin(s, li, same, false);
    line.closed = false;
    line.stops[0].flags &= ~1u;
    computeSegment(s, line, n - 1);
    editEnd(vm, s, li, same);
    return;
  }
  std::vector<int> map(n);
  for (size_t j = 0; j < n; ++j) map[j] = j < k ? int(j) : j == k ? -1 : int(j) - 1;
  editBegin(s, li, map, false);
  line.stops.erase(line.stops.begin() + std::ptrdiff_t(k));
  if (line.stops.size() < 2) line.closed = false;
  if (k > 0) computeSegment(s, line, k - 1);
  editEnd(vm, s, li, map);
}

// gsTransportLine::exchangeStops (L:0x081c24a0): the two stops trade places; the four legs around them again
void lineExchangeStops(VM& vm, SimState& s, int li, size_t a, size_t b) {
  LineState& line = s.lines[size_t(li)];
  size_t n = line.stops.size();
  if (a >= n || b >= n || a == b) return;
  std::vector<int> map = identityMap(n);
  std::swap(map[a], map[b]);
  editBegin(s, li, map, false);
  std::swap(line.stops[a], line.stops[b]);
  for (size_t k : {a, b})
    for (size_t j : {k, k == 0 ? (line.closed ? n - 1 : n) : k - 1})
      if (j < n) computeSegment(s, line, j);
  editEnd(vm, s, li, map);
}

// gsTransportLine::removeTempStops (L:0x081c2ee0)
bool lineRemoveTempStops(VM& vm, SimState& s, int li) {
  LineState& line = s.lines[size_t(li)];
  bool any = false;
  for (size_t k = 0; k < line.stops.size();) {
    if (!(line.stops[k].flags & 1)) {
      ++k;
      continue;
    }
    lineRemoveStop(vm, s, li, k);  // the first of a closed ring stays (the ring opens, its flag goes): seen again
    any = true;
  }
  return any;
}

void lineDeleted(VM& vm, SimState& s, int li) {
  LineState& line = s.lines[size_t(li)];
  std::vector<int> gone(line.stops.size(), -1);
  editBegin(s, li, gone, true);
  for (int vi : std::vector<int>(line.vehicles)) dropVehicle(vm, s, vi);
  line.stops.clear();
  line.vehicles.clear();
  line.deleted = true;
  line.object = Ref<Object>();
  editEnd(vm, s, li, gone);
}

}  // namespace sim

void TransportObj::onDestroy(VM& vm) {
  auto s = sim.lock();
  if (!s || s->tearingDown || index < 0) return;
  if (cls->name == "gsTransportLine") {
    if (size_t(index) < s->lines.size() && !s->lines[size_t(index)].deleted && s->lines[size_t(index)].object.get() == this)
      lineDeleted(vm, *s, index);
  } else if (cls->name == "gsTransportVehicle") {
    if (size_t(index) < s->vehicles.size() && s->vehicles[size_t(index)].object.get() == this) dropVehicle(vm, *s, index);
  }
}

// ---- the add-stop tool's view of a line (engine/tool_addstop.cpp) ----

int simLineType(Object* line) {
  int li = lineIndex(line);
  return li < 0 ? -1 : stateOf(line)->lines[size_t(li)].type;
}

int simLineAddStop(VM& vm, Object* line, int item, bool temp) {
  int li = lineIndex(line);
  auto s = stateOf(line);
  if (li < 0 || !s->grid || !s->grid->data || item < 0 || size_t(item) >= s->grid->data->items.size()) return -1;
  return lineAddStop(vm, s->grid, *s, li, item, temp);
}

std::vector<int> simLineTempStopItems(Object* line) {
  std::vector<int> out;
  int li = lineIndex(line);
  if (li < 0) return out;
  for (const LineStopState& st : stateOf(line)->lines[size_t(li)].stops)
    if (st.flags & 1) out.push_back(st.item);
  return out;
}

void simLineRemoveTempStops(VM& vm, Object* line) {
  int li = lineIndex(line);
  if (li >= 0) lineRemoveTempStops(vm, *stateOf(line), li);
}

// finishTool case 9 (L:0x0819a3e0.c:554-618): a temporary stop whose leg in or out has no path takes every temporary
// stop with it; the others become permanent (flag 1 cleared). True when one did.
bool simLineCommitTempStops(VM& vm, Object* line, std::vector<int>& unselect) {
  int li = lineIndex(line);
  if (li < 0) return false;
  SimState& s = *stateOf(line);
  LineState& L = s.lines[size_t(li)];
  size_t n = L.stops.size();
  if (n == 0) return false;
  // from the second node round to the first (closed rings) or the end
  for (size_t step = 1; step <= n; ++step) {
    size_t k = step % n;
    if (k == 0 && !L.closed) break;
    LineStopState& cur = L.stops[k];
    if (cur.flags & 1) {
      const LineStopState& prev = L.stops[(k + n - 1) % n];
      bool hasNext = k + 1 < n || L.closed;
      if (!prev.reachable || (hasNext && !cur.reachable)) {
        unselect.push_back(cur.item);
        lineRemoveTempStops(vm, s, li);
        if (L.stops.empty()) return false;
        break;
      }
    }
    if (k == 0) break;
  }
  bool any = false;
  for (LineStopState& st : L.stops)
    if (st.flags & 1) {
      any = true;
      unselect.push_back(st.item);
      st.flags &= ~1u;
    }
  if (any) citizensRemapLineNodes(s, identityMap(s.lineNodes.size()));  // the new legs join the transfers
  return any;
}

// ---- natives ----

void registerLineNatives(VM& vm) {
  registerLinePanelNatives(vm);
  ClassInfo* lineClass = vm.findClass("gsTransportLine");
  ClassInfo* vehicleClass = vm.findClass("gsTransportVehicle");
  // port: onRegsJamMoved(pos), the jam relief moved the vehicle on (engine/sim_jams.cpp; the ticker message is the
  // port's extension of metro/ui/linepanel/linepanel.script)
  if (vehicleClass) addEvent(vehicleClass, u"onRegsJamMoved", 14, false);

  // gsTransportLines.createLine() (0x10118940): a new line under the manager, empty, open, type 0
  implement(vm, "gsTransportLines", u"createLine", [lineClass](VM& v, Object* self, int, const Value*) {
    GridObj* grid = gridOfObject(self);
    if (!grid || !grid->sim || !lineClass) return Value::null();
    std::shared_ptr<SimState> s = grid->sim;
    LineState line;
    line.firstNode = int(s->lineNodes.size());
    line.effAttractiveness = 1;
    line.object = v.newObject(lineClass, self);
    if (auto* t = transport(line.object.get())) t->index = int(s->lines.size()), t->sim = s;
    Object* lo = line.object.get();
    lo->fieldValues[u"ticketPrice"] = Value::real(1.0f);        // ctor 0x10118830.c:35-39
    lo->fieldValues[u"attractiveness"] = Value::real(1.0f);
    lo->fieldValues[u"passengersTransported"] = Value::integer(0);
    s->lines.push_back(std::move(line));
    return Value::heap(VT::Object, lo);
  });

  if (lineClass) {
    auto field = [lineClass](const char16_t* name, std::function<Value(VM&, LineState&)> get,
                             std::function<void(VM&, Object*, LineState&, const Value&)> set) {
      FieldInfo& f = lineClass->fields[name];
      f.name = name;
      f.readOnly = !set;
      f.get = [key = std::u16string(name), get](VM& v, Object* o) {
        if (LineState* l = lineOf(o); l && !l->deleted) return get(v, *l);
        auto it = o->fieldValues.find(key);
        return it == o->fieldValues.end() ? Value() : it->second;
      };
      if (set)
        f.set = [key = std::u16string(name), set](VM& v, Object* o, const Value& val) {
          Value x = v.resolve(val);
          if (LineState* l = lineOf(o); l && !l->deleted) set(v, o, *l, x);
          o->fieldValues[key] = x;
        };
    };
    field(u"type", [](VM&, LineState& l) { return Value::integer(l.type); },
          [](VM& v, Object*, LineState& l, const Value& x) { l.type = v.toInt(x); });
    field(u"stopCount", [](VM&, LineState& l) { return Value::integer(int(l.stops.size())); }, nullptr);
    field(u"isClosed", [](VM&, LineState& l) { return Value::boolean(l.closed); }, nullptr);
    // active (bit 0x10000, setter 0x10110bb0 → statusUpdated of every vehicle): pausing takes the vehicles off the map
    field(u"active", [](VM& v, LineState& l) { return Value::boolean(lineActive(v, l)); },
          [](VM& v, Object* o, LineState& l, const Value& x) {
            bool was = lineActive(v, l), on = v.truthy(x);
            o->fieldValues[u"active"] = Value::boolean(on);
            if (was == on) return;
            auto s = stateOf(o);
            for (int vi : std::vector<int>(l.vehicles)) unspawnVehicle(v, *s, l, vi, true);
          });
  }

  // deleteStop(i) (L:0x081c2f30): the i-th node of the ring
  implement(vm, "gsTransportLine", u"deleteStop", [](VM& v, Object* self, int, const Value* a) {
    int li = lineIndex(self);
    int i = v.toInt(a[0]);
    if (li >= 0 && i >= 0) lineRemoveStop(v, *stateOf(self), li, size_t(i));
    return Value();
  });
  // moveStop(i, j) (L:0x081c2790): the stops at positions i and j trade places when both exist
  implement(vm, "gsTransportLine", u"moveStop", [](VM& v, Object* self, int, const Value* a) {
    int li = lineIndex(self);
    int i = v.toInt(a[0]), j = v.toInt(a[1]);
    if (li >= 0 && i >= 0 && j >= 0) lineExchangeStops(v, *stateOf(self), li, size_t(i), size_t(j));
    return Value();
  });

  // addVehicle(model) (0x10118f20, §4.1): a gsGridObject or an idName; the vehicle is a child of the line, unspawned
  // (it enters at a free platform when the line runs), unit 0 = the model, capacity its capacity
  implement(vm, "gsTransportLine", u"addVehicle", [vehicleClass](VM& v, Object* self, int, const Value* a) {
    int li = lineIndex(self);
    auto s = stateOf(self);
    if (li < 0 || !vehicleClass || !s->grid) return Value::null();
    Value m = v.resolve(a[0]);
    std::string name;
    if (Object* o = m.object()) {
      if (auto id = o->fieldValues.find(u"idName"); id != o->fieldValues.end() && !id->second.isNone()) name = utf16ToUtf8(v.toStr(id->second));
      if (name.empty())
        for (const auto& [key, tpl] : s->grid->templates)
          if (tpl.get() == o) name = key;
    } else if (m.type() == VT::String) {
      name = utf16ToUtf8(v.toStr(m));
    }
    int model = name.empty() ? -1 : modelFor(v, s->grid, *s, name);
    if (model < 0) return Value::null();
    int vi = int(s->vehicles.size());
    VehicleState vs;
    vs.line = li;
    Unit u;
    u.model = model;
    vs.units.push_back(u);
    vs.capacity = s->models[size_t(model)].capacity;
    vs.object = v.newObject(vehicleClass, self);
    if (auto* t = transport(vs.object.get())) t->index = vi, t->sim = s;
    Object* vo = vs.object.get();
    vo->fieldValues[u"line"] = Value::heap(VT::Object, self);
    vo->fieldValues[u"condition"] = Value::real(1.0f);
    vo->fieldValues[u"<model>"] = Value::heap(VT::Object, s->models[size_t(model)].object);
    vo->fieldValues[u"<instance>"] = Value::boolean(false);
    // placed at the first stop's platform (slot 41 0x10118e80)
    const LineState& line = s->lines[size_t(li)];
    if (!line.stops.empty() && line.stops[0].platform >= 0) {
      Vec3 p = s->graph.nodes[size_t(line.stops[0].platform)].p0;
      vo->fieldValues[u"position"] = vo->fieldValues[u"globalPos"] = Value::vector(p.x, p.y, p.z);
    } else if (!line.stops.empty() && line.stops[0].stop) {
      auto p = line.stops[0].stop->fieldValues.find(u"position");
      if (p != line.stops[0].stop->fieldValues.end()) vo->fieldValues[u"position"] = vo->fieldValues[u"globalPos"] = p->second;
    }
    vs.mover = newMover(*s);
    s->movers[size_t(vs.mover)].transit = true;
    s->movers[size_t(vs.mover)].vehicle = vi;
    s->vehicles.push_back(std::move(vs));
    s->lines[size_t(li)].vehicles.push_back(vi);
    return Value::heap(VT::Object, vo);
  });
  // addTrailer(model, [reversed]) (0x10118c10): one more unit, its capacity added
  implement(vm, "gsTransportVehicle", u"addTrailer", [](VM& v, Object* self, int argc, const Value* a) {
    VehicleState* vs = vehicleOf(self);
    auto s = stateOf(self);
    if (!vs || vs->deleted || !s || !s->grid) return Value();
    Value m = v.resolve(a[0]);
    std::string name;
    if (Object* o = m.object()) {
      if (auto id = o->fieldValues.find(u"idName"); id != o->fieldValues.end() && !id->second.isNone()) name = utf16ToUtf8(v.toStr(id->second));
    } else if (m.type() == VT::String) {
      name = utf16ToUtf8(v.toStr(m));
    }
    int model = name.empty() ? -1 : modelFor(v, s->grid, *s, name);
    if (model < 0) return Value();
    Unit u;
    u.model = model;
    u.reversed = argc > 1 && v.truthy(a[1]);
    vs->units.push_back(u);
    vs->capacity += s->models[size_t(model)].capacity;
    if (vs->mover >= 0 && s->movers[size_t(vs->mover)].live) s->movers[size_t(vs->mover)].units = vs->units;
    return Value();
  });

  // calculateCoverage() (L:0x081b7670): the share of GRID_ITEM_BUILDING items whose corner cell lies strictly inside
  // the catchment of one of the line's stops; positions are item corners, (x − width/2)·cellSize, not centres
  implement(vm, "gsTransportLine", u"calculateCoverage", [](VM& v, Object* self, int, const Value*) {
    int li = lineIndex(self);
    auto s = stateOf(self);
    if (li < 0 || !s->grid || !s->grid->data) return Value::real(0);
    const GridData& g = *s->grid->data;
    float cs = g.cellSize;
    struct Circle {
      float x, z, r;
    };
    std::vector<Circle> stops;
    for (const LineStopState& st : s->lines[size_t(li)].stops)
      if (st.stop && st.item >= 0) {
        const auto& it = g.items[size_t(st.item)];
        stops.push_back({float(it.x - (g.width >> 1)) * cs, float(it.z - (g.length >> 1)) * cs, fieldFloat(v, st.stop.get(), u"catchmentArea", 100.0f)});
      }
    std::vector<int> typeOf(g.types.size(), -1);
    for (size_t t = 0; t < g.types.size(); ++t)
      if (auto tp = s->grid->templates.find(g.types[t]); tp != s->grid->templates.end() && tp->second) typeOf[t] = fieldInt(v, tp->second.get(), u"type", -1);
    int total = 0, covered = 0;
    for (const auto& it : g.items) {
      if (it.removed || it.type >= typeOf.size() || typeOf[it.type] != 0x14) continue;
      ++total;
      float x = float(it.x - (g.width >> 1)) * cs, z = float(it.z - (g.length >> 1)) * cs;
      for (const Circle& c : stops)
        if (c.r * c.r > (x - c.x) * (x - c.x) + (z - c.z) * (z - c.z)) {
          ++covered;
          break;
        }
    }
    return Value::real(float(covered) / float(total));  // 0/0 as the engine
  });
  // countConnectedStops(mask) (L:0x081b9d80): the line's nodes with a transfer to a node of an active line of the mask
  implement(vm, "gsTransportLine", u"countConnectedStops", [](VM& v, Object* self, int, const Value* a) {
    int li = lineIndex(self);
    auto s = stateOf(self);
    if (li < 0) return Value::integer(0);
    uint32_t mask = uint32_t(v.toInt(a[0]));
    int n = 0;
    for (const LineStopState& st : s->lines[size_t(li)].stops) {
      bool any = false;
      for (int m : st.transfers) {
        if (m < 0 || size_t(m) >= s->lineNodes.size()) continue;
        const LineState& other = s->lines[size_t(s->lineNodes[size_t(m)].first)];
        any = any || (((mask >> (other.type & 31)) & 1) && lineActive(v, other));
      }
      n += any;
    }
    return Value::integer(n);
  });
}

}  // namespace regs::engine
