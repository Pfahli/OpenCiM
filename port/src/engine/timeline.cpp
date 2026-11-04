// gsGridTimeline (timeline.h): the city's dated history, replayed onto the grid. Decoded from the Linux build
// (gsGridTimeline::method_applyChanges L:0x081a55d0, readTimelineChunk L:0x081a6920, saveToFile L:0x081a79e0 and the
// gsGridTimelineAction_* apply/load/save functions); file layout in docs/save-format/lines-timeline.md Â§2.6-2.15.
#include "engine/timeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/interaction.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

namespace {

constexpr uint32_t kTimeline = 0x07080000, kDay = 0x07081000, kGroup = 0x07081010, kCreate = 0x07081020,
                   kCreateWithParent = 0x07081030, kDelete = 0x07081040, kBrush = 0x07081050, kWater = 0x07081060,
                   kName8 = 0x07081070, kName = 0x07081071, kVariation = 0x07081080;
constexpr uint32_t kFlagLockGround = 0x8, kFlagOptional = 0x200000, kFlagHidden = 0x400000, kFlagAutoChild = 0x2000000,
                   kFlagConflict = 0x10000000, kFlagDemolishMark = 0x20000000, kPreview = 0x500000;

// ObjRef (Â§2.8): template name, cell x, height y (engine units, 1/256 cell), cell z, orientation, flags
struct ObjRef {
  std::string name;
  int16_t x = 0, y = 0, z = 0;
  uint16_t orient = 0;
  uint32_t flags = 0;
};

struct Action {
  uint32_t tag = 0;
  ObjRef obj, parent;                // createObject (parent: 0x07081030), deleteObject, name, variation
  std::string label;                 // group: 32 bytes as stored
  std::vector<Action> children;      // group
  int8_t brushType = 0, radius = 0;  // brush
  int16_t strength = 0;
  std::vector<std::pair<int16_t, int16_t>> points;
  int16_t wx = 0, wz = 0;            // water
  uint16_t on = 0;
  std::string text;                  // name (UTF-8, as the network's name table)
  uint32_t variation = 0;            // variation
  std::vector<uint8_t> raw;          // unknown tags, kept as read (body)
};

struct TimelineData {
  std::map<uint32_t, std::vector<Action>> days;  // day since 1 Jan 1920 â†’ actions in stored (newest-first) order
  uint32_t dayCount() const { return days.empty() ? 0 : days.rbegin()->first + 1; }
};

std::unordered_map<const Object*, std::shared_ptr<TimelineData>> g_timelines;

TimelineData& timelineOf(Object* s) {
  auto& p = g_timelines[s];
  if (!p) p = std::make_shared<TimelineData>();
  return *p;
}

// ---------------------------------------------------------------------------------------------------------------
// reading and writing (Â§2.6-2.15)

struct Reader {
  const std::vector<uint8_t>& d;
  uint32_t u32(size_t at) const {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  }
  int16_t i16(size_t at) const {
    int16_t v = 0;
    if (at + 2 <= d.size()) std::memcpy(&v, d.data() + at, 2);
    return v;
  }
  uint16_t u16(size_t at) const { return uint16_t(i16(at)); }
  std::string cstr(size_t at, size_t max) const {
    std::string s;
    for (size_t i = 0; i < max && at + i < d.size() && d[at + i]; ++i) s += char(d[at + i]);
    return s;
  }
};

ObjRef readRef(const Reader& r, size_t at) {
  ObjRef o;
  o.name = r.cstr(at, 32);
  o.x = r.i16(at + 32);
  o.y = r.i16(at + 34);
  o.z = r.i16(at + 36);
  o.orient = r.u16(at + 38);
  o.flags = r.u32(at + 40);
  return o;
}

// the action chunks in [at, end), appended in file order (gsGridTimelineAction::create factory L:0x081a6400)
void readActions(const Reader& r, size_t at, size_t end, std::vector<Action>& out) {
  while (at + 8 <= end) {
    uint32_t tag = r.u32(at), size = r.u32(at + 4);
    size_t body = at + 8, stop = std::min(end, body + size);
    Action a;
    a.tag = tag;
    switch (tag) {
      case kGroup:
        a.label = r.cstr(body, 32);
        readActions(r, body + 32, stop, a.children);
        break;
      case kCreate: a.obj = readRef(r, body); break;
      case kCreateWithParent:
        a.obj = readRef(r, body);
        a.parent = readRef(r, body + 44);
        break;
      case kDelete: a.obj = readRef(r, body); break;
      case kBrush: {
        a.brushType = int8_t(r.d[body]);
        a.radius = int8_t(r.d[body + 1]);
        uint16_t count = r.u16(body + 2);
        a.strength = r.i16(body + 4);
        for (size_t k = 0; k < count && body + 8 + k * 4 + 4 <= stop; ++k)
          a.points.emplace_back(r.i16(body + 8 + k * 4), r.i16(body + 10 + k * 4));
        break;
      }
      case kWater:
        a.wx = r.i16(body);
        a.wz = r.i16(body + 4);
        a.on = r.u16(body + 6);
        break;
      case kName8:
      case kName: {
        a.obj = readRef(r, body);
        std::string s = r.cstr(body + 44, stop > body + 44 ? stop - body - 44 : 0);
        a.text = tag == kName ? s : cp1252ToUtf8(s);  // 0x07081070: one byte per character
        a.tag = kName;
        break;
      }
      case kVariation:
        a.obj = readRef(r, body);
        a.variation = r.u32(body + 44);
        break;
      default: a.raw.assign(r.d.begin() + std::ptrdiff_t(body), r.d.begin() + std::ptrdiff_t(stop)); break;
    }
    out.push_back(std::move(a));
    at = body + size;
  }
}

struct Writer {
  std::vector<uint8_t> out;
  void u32(uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); }
  void u16(uint16_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 2); }
  void fixed(const std::string& s, size_t n) {
    for (size_t i = 0; i < n; ++i) out.push_back(i < s.size() ? uint8_t(s[i]) : 0);
  }
  size_t begin(uint32_t tag) {
    u32(tag);
    u32(0);
    return out.size();
  }
  void end(size_t body) {
    uint32_t size = uint32_t(out.size() - body);
    std::memcpy(out.data() + body - 4, &size, 4);
  }
  void ref(const ObjRef& o) {
    fixed(o.name, 32);
    u16(uint16_t(o.x));
    u16(uint16_t(o.y));
    u16(uint16_t(o.z));
    u16(o.orient);
    u32(o.flags);
  }
};

void writeAction(Writer& w, const Action& a) {
  uint32_t tag = a.tag == kCreate && !a.parent.name.empty() ? kCreateWithParent : a.tag;
  size_t body = w.begin(tag);
  switch (a.tag) {
    case kGroup:
      w.fixed(a.label, 32);
      for (const Action& c : a.children) writeAction(w, c);
      break;
    case kCreate:
    case kCreateWithParent:
      w.ref(a.obj);
      if (!a.parent.name.empty()) w.ref(a.parent);
      break;
    case kDelete: w.ref(a.obj); break;
    case kBrush:
      w.out.push_back(uint8_t(a.brushType));
      w.out.push_back(uint8_t(a.radius));
      w.u16(uint16_t(a.points.size()));
      w.u16(uint16_t(a.strength));
      w.u16(0);
      for (auto [x, z] : a.points) {
        w.u16(uint16_t(x));
        w.u16(uint16_t(z));
      }
      break;
    case kWater:
      w.u16(uint16_t(a.wx));
      w.u16(0);
      w.u16(uint16_t(a.wz));
      w.u16(a.on);
      break;
    case kName: {
      w.ref(a.obj);
      size_t start = w.out.size();  // UTF-8, NUL included, zero-padded to 4 (FUN_1007d8f0)
      w.out.insert(w.out.end(), a.text.begin(), a.text.end());
      w.out.push_back(0);
      while ((w.out.size() - start) % 4) w.out.push_back(0);
      break;
    }
    case kVariation:
      w.ref(a.obj);
      w.u32(a.variation);
      break;
    default: w.out.insert(w.out.end(), a.raw.begin(), a.raw.end()); break;
  }
  w.end(body);
}

// ---------------------------------------------------------------------------------------------------------------
// applying (gsGridTimelineAction_*::apply)

struct Apply {
  VM& vm;
  GridObj* grid;
  float weight = 0;
  float pos[3] = {0, 0, 0};
  std::vector<std::pair<Ref<Object>, std::pair<std::u16string, Value>>> events;  // fired once the batch has ended
  std::unordered_set<std::string> unknown;
  size_t real = 0, previews = 0;  // actions applied (for the log)
};

int heightShift(const GridData& g) { return 0x200 - int(g.heightBase); }

// the live item an ObjRef names: template, x, z and orientation; name and variation also compare y and flag
// 0x2000000 (Â§2.8)
int findItem(Apply& ap, const ObjRef& r, bool exact) {
  const GridData& g = *ap.grid->data;
  for (uint32_t i : gridItemsAt(ap.vm, ap.grid, r.x, r.z)) {
    const GridData::Item& it = g.items[i];
    if (g.types[it.type] != r.name || it.orient != (r.orient & 3)) continue;
    if (exact && (it.y + heightShift(g) != r.y ||
                  (gridItemShape(ap.vm, ap.grid, i).flags & kFlagAutoChild) != (r.flags & kFlagAutoChild)))
      continue;
    return int(i);
  }
  return -1;
}

// climb while the item is an auto-generated part (the "root" createObject compares)
uint32_t rootOf(const GridData& g, Apply& ap, uint32_t i) {
  for (int guard = 0; guard < 32; ++guard) {
    const GridData::Item& it = g.items[i];
    if (!it.parent || it.parent > g.items.size()) break;
    if (!(gridItemShape(ap.vm, ap.grid, i).flags & kFlagAutoChild)) break;
    i = it.parent - 1;
  }
  return i;
}

bool hasHandler(Object* o, const char16_t* event) {
  auto it = o->events.find(event);
  return it != o->events.end() && !it->second.empty();
}

void queueTemplateEvent(Apply& ap, uint32_t item, const char16_t* event) {
  const GridData& g = *ap.grid->data;
  auto t = ap.grid->templates.find(g.types[g.items[item].type]);
  if (t == ap.grid->templates.end() || !hasHandler(t->second.get(), event)) return;
  if (Object* c = gridItemController(ap.vm, ap.grid, item))
    ap.events.push_back({t->second, {event, Value::heap(VT::Object, c)}});
}

void queueGridEvent(Apply& ap, uint32_t item, const char16_t* event) {
  if (!hasHandler(ap.grid, event)) return;
  if (Object* c = gridItemController(ap.vm, ap.grid, item))
    ap.events.push_back({Ref<Object>(ap.grid), {event, Value::heap(VT::Object, c)}});
}

// gsGridTimelineAction_createObject::apply (L:0x081a81b0)
void applyCreate(Apply& ap, const Action& a, bool preview) {
  GridData& g = *ap.grid->data;
  int parent = -1;
  if (!a.parent.name.empty()) {
    parent = findItem(ap, a.parent, false);
    if (parent < 0) return;  // the parent is gone: nothing is created
    while ((gridItemShape(ap.vm, ap.grid, uint32_t(parent)).flags & kFlagOptional) && g.items[size_t(parent)].parent)
      parent = int(g.items[size_t(parent)].parent - 1);
  }
  uint32_t flags = a.obj.flags | (preview ? kPreview : 0);
  int index = gridEditAddItem(ap.vm, ap.grid, a.obj.name, a.obj.x, a.obj.y, a.obj.z, a.obj.orient, flags, parent);
  if (index < 0) {
    if (ap.unknown.insert(a.obj.name).second) LOG_WARN("timeline: Unknown object: %s", a.obj.name.c_str());
    return;
  }
  uint32_t item = uint32_t(index);
  GridItemShape s = gridItemShape(ap.vm, ap.grid, item);
  const GridData::Item& it = g.items[item];
  if ((s.flags & kFlagLockGround) && it.x >= 0 && it.z >= 0 && it.x + s.sx <= g.width && it.z + s.sz <= g.length) {
    // the ground under a building to come shows as a construction site (0x40) until it stands
    for (int z = 0; z < s.sz; ++z)
      for (int x = 0; x < s.sx; ++x) {
        uint16_t& f = g.flags[size_t(it.z + z) * size_t(g.width) + size_t(it.x + x)];
        if (preview) {
          if (!(f & 0x8)) f |= 0x40;
        } else {
          f &= 0xFFBF;
        }
      }
    gridEditCellsChanged(ap.grid, it.x, it.z, it.x + s.sx - 1, it.z + s.sz - 1, false);
  }
  if ((s.type >= 1 && s.type <= 0x12) || s.type == 0x14) {
    // network items and buildings make room: what conflicts with them (or is the same object, e.g. its preview
    // placeholder) goes, by its root; a preview only marks the conflicts and reports where they are
    uint32_t root = rootOf(g, ap, item);
    std::set<uint32_t> victims;
    for (uint32_t o : gridItemsNear(ap.vm, ap.grid, it.x, it.z, it.x + s.sx - 1, it.z + s.sz - 1)) {
      if (o == item) continue;
      const GridData::Item& oi = g.items[o];
      GridItemShape os = gridItemShape(ap.vm, ap.grid, o);
      bool same = oi.type == it.type && oi.x == it.x && oi.z == it.z && oi.orient == (a.obj.orient & 3) && oi.y == it.y;
      if (!same && ((os.flags & kFlagHidden) || !gridItemsConflict(ap.vm, ap.grid, o, item))) continue;
      uint32_t r = rootOf(g, ap, o);
      if (r != root) victims.insert(r);
    }
    for (uint32_t v : victims) {
      GridData::Item& vi = g.items[v];
      if (vi.removed) continue;
      GridItemShape vs = gridItemShape(ap.vm, ap.grid, v);
      if (preview) {
        if (vs.flags & (kFlagDemolishMark | kFlagOptional)) continue;
        vi.flags |= kFlagConflict;
        float area = float(vs.sx * vs.sz), cs = g.cellSize;
        ap.weight += area;
        ap.pos[0] += float(vi.x + (vs.sx >> 1) - (g.width >> 1)) * cs * area;
        ap.pos[1] += float(vi.y + heightShift(g)) / 256.0f * cs * area;
        ap.pos[2] += float(vi.z + (vs.sz >> 1) - (g.length >> 1)) * cs * area;
      } else {
        if (vs.flags & kFlagConflict) queueGridEvent(ap, v, u"forcedDemolish");
        gridEditRemoveItem(ap.vm, ap.grid, v);
      }
    }
  } else {
    // trees, props, markers: a preview is not kept, a real one only where nothing is in the way (gsGrid::checkConflict)
    bool conflict = preview;
    if (!conflict)
      for (uint32_t o : gridItemsNear(ap.vm, ap.grid, it.x, it.z, it.x + s.sx - 1, it.z + s.sz - 1))
        if (gridItemsConflict(ap.vm, ap.grid, o, item)) {
          conflict = true;
          break;
        }
    if (conflict) {
      gridEditRemoveItem(ap.vm, ap.grid, item);
      return;
    }
  }
  queueTemplateEvent(ap, item, preview ? u"onConstructInstance" : u"onCreateInstance");
}

// gsGridTimelineAction_brush::apply (L:0x081a3ce0); in preview only the yard paints run (marking 0x40)
void applyBrush(Apply& ap, const Action& a, bool preview) {
  GridData& g = *ap.grid->data;
  int type = a.brushType, r = a.radius;
  if (preview && !(type >= 1 && type <= 8)) return;
  auto cell = [&](int x, int z) -> size_t {
    return x < 0 || z < 0 || x >= g.width || z >= g.length ? SIZE_MAX : size_t(z) * size_t(g.width) + size_t(x);
  };
  auto falloff = [&](int d2) {
    float r2 = r * r > 0 ? float(r * r) : 1.0f;
    return float(std::sin((float(d2) / r2 + 0.5f) * 3.1415927f)) * 0.5f + 0.5f;
  };
  bool heights = type >= 19 && type <= 21;
  for (auto [px, pz] : a.points) {
    if (type == 20 || type == 22) {  // shift / ruin
      for (int dz = -r; dz <= r + 1; ++dz)
        for (int dx = -r; dx <= r + 1; ++dx) {
          size_t c = cell(px + dx, pz + dz);
          if (c == SIZE_MAX || (g.flags[c] & 0x1000)) continue;
          int ex = dx - (dx > 0), ez = dz - (dz > 0), d2 = ex * ex + ez * ez;
          if (d2 > r * r) continue;
          float f = falloff(d2);
          if (type == 20) {
            g.height[c] = int16_t(std::max(-0x600, int(std::nearbyint(float(a.strength) * f)) + g.height[c]));
          } else if (!(g.flags[c] & 0x28)) {
            int wear = int(std::nearbyint(float(a.strength) * f * 0.0001f * float((std::rand() & 0x1f) + 0x44))) +
                       (g.flags[c] & 7);
            g.flags[c] = uint16_t((g.flags[c] & 0xFFF8) | std::clamp(wear, 0, 7));
          }
        }
    } else if (type == 21) {  // soften: 5-tap average over a snapshot of the dab
      std::map<size_t, int16_t> before;
      for (int dz = -r - 1; dz <= r + 2; ++dz)
        for (int dx = -r - 1; dx <= r + 2; ++dx)
          if (size_t c = cell(px + dx, pz + dz); c != SIZE_MAX) before[c] = g.height[c];
      auto old = [&](int x, int z, int& v) {
        size_t c = cell(x, z);
        auto it = c == SIZE_MAX ? before.end() : before.find(c);
        if (it == before.end()) return false;
        v = it->second;
        return true;
      };
      for (int dz = -r; dz <= r + 1; ++dz)
        for (int dx = -r; dx <= r + 1; ++dx) {
          size_t c = cell(px + dx, pz + dz);
          int ex = dx - (dx > 0), ez = dz - (dz > 0);
          if (c == SIZE_MAX || ex * ex + ez * ez > r * r || (g.flags[c] & 0x1000)) continue;
          int sum = 0, n = 0, v;
          for (auto [ox, oz] : {std::pair{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}})
            if (old(px + dx + ox, pz + dz + oz, v)) sum += v, ++n;
          if (n) g.height[c] = int16_t(sum / n);
        }
    } else {  // level (19) and the paints (1-18)
      for (int dz = -r; dz <= r + 1; ++dz)
        for (int dx = -r; dx <= r + 1; ++dx) {
          size_t c = cell(px + dx, pz + dz);
          if (c == SIZE_MAX) continue;
          int ex = dx - (dx > 0), ez = dz - (dz > 0), d2 = ex * ex + ez * ez;
          if (type == 19 && d2 <= r * r && !(g.flags[c] & 0x1000))
            g.height[c] = int16_t(g.height[c] + int(std::nearbyint(falloff(d2) * float(a.strength - g.height[c]))));
          if (dz * dz + dx * dx > (r + 1) * (r + 1) || dz > r || dx > r) continue;
          uint16_t& f = g.flags[c];
          if (type >= 1 && type <= 8) {
            if (!preview) f = uint16_t((f & 0xFF98) | 0x8 | (type - 1));
            else if (!(f & 0x8)) f = uint16_t((f & 0xFFD0) | 0x40);
          } else if (type == 9) {
            f &= 0xFFF0;
          } else if (type >= 10 && type <= 17) {
            f = uint16_t((f & 0xFFF0) | 0x20 | (type - 10));
          } else if (type == 18) {
            f &= 0xFFD8;
          }
        }
    }
    if (heights)  // water stays only where a corner is below the water level
      for (int dz = -r; dz <= r + 1; ++dz)
        for (int dx = -r; dx <= r + 1; ++dx) {
          int x = px + dx, z = pz + dz;
          size_t c = cell(x, z);
          if (c == SIZE_MAX || !(g.flags[c] & 0x2000) || g.height[c] <= -0x141) continue;
          int16_t h1 = x < g.width - 1 ? g.height[c + 1] : g.height[c];
          int16_t h2 = z < g.length - 1 ? g.height[c + size_t(g.width)] : g.height[c];
          int16_t h3 = x < g.width - 1 && z < g.length - 1 ? g.height[c + size_t(g.width) + 1] : g.height[c];
          if (h1 > -0x141 && h2 > -0x141 && h3 > -0x141) g.flags[c] &= 0xDFFF;
        }
    gridEditCellsChanged(ap.grid, px - r - 1, pz - r - 1, px + r + 2, pz + r + 2, heights);
  }
}

void applyAction(Apply& ap, const Action& a, bool preview) {
  GridData& g = *ap.grid->data;
  ++(preview ? ap.previews : ap.real);
  switch (a.tag) {
    case kGroup:  // children newest-first: applied from the back
      for (auto it = a.children.rbegin(); it != a.children.rend(); ++it) applyAction(ap, *it, preview);
      break;
    case kCreate:
    case kCreateWithParent: applyCreate(ap, a, preview); break;
    case kDelete: {
      int i = findItem(ap, a.obj, false);
      if (i < 0) break;
      if (preview) g.items[size_t(i)].flags |= kFlagDemolishMark;
      else gridEditRemoveItem(ap.vm, ap.grid, uint32_t(i));
      break;
    }
    case kBrush: applyBrush(ap, a, preview); break;
    case kWater: {
      if (preview) break;
      auto h = [&](int x, int z) -> int {
        return x < 0 || z < 0 || x >= g.width || z >= g.length ? 0x7FFF : g.height[size_t(z) * size_t(g.width) + size_t(x)];
      };
      int x = a.wx, z = a.wz;
      if (h(x, z) == 0x7FFF || h(x + 1, z) == 0x7FFF || h(x, z + 1) == 0x7FFF || h(x + 1, z + 1) == 0x7FFF) break;
      if (std::min({h(x, z), h(x + 1, z), h(x, z + 1), h(x + 1, z + 1)}) >= -0x140) break;
      uint16_t& f = g.flags[size_t(z) * size_t(g.width) + size_t(x)];
      f = a.on ? uint16_t(f | 0x2000) : uint16_t(f & 0xDFFF);
      gridEditCellsChanged(ap.grid, x, z, x, z, true);
      break;
    }
    case kName: {
      if (preview) break;
      int i = findItem(ap, a.obj, true);
      if (i < 0) break;
      if (g.names.size() < g.items.size()) g.names.resize(g.items.size());
      g.names[size_t(i)] = a.text;
      queueGridEvent(ap, uint32_t(i), u"objectRenamed");
      break;
    }
    case kVariation: {
      if (preview) break;
      int i = findItem(ap, a.obj, true);
      if (i < 0) break;
      g.items[size_t(i)].variation = a.variation;
      gridEditItemChanged(ap.grid, uint32_t(i));
      break;
    }
    default: break;
  }
}

GridObj* timelineGrid(VM& vm, Object* s) {
  try {
    return dynamic_cast<GridObj*>(vm.resolve(vm.getMember(Value::heap(VT::Object, s), u"grid")).object());
  } catch (const ScriptError&) {
    return nullptr;
  }
}

int currentDayOf(VM& vm, Object* s) {
  auto it = s->fieldValues.find(u"currentDay");
  return it == s->fieldValues.end() || it->second.isNone() ? 0 : vm.toInt(it->second);
}

// the labels getActions returns (gsGridTimelineAction::getName): groups their own, the others their %!NN code
std::u16string labelOf(const Action& a) {
  switch (a.tag) {
    case kGroup: return utf8ToUtf16(a.label);
    case kCreate:
    case kCreateWithParent: return u"%!11";
    case kDelete: return u"%!12";
    case kBrush: {
      static const char16_t* names[] = {u"%!13", u"%!14", u"%!15", u"%!16", u"%!17", u"%!18", u"%!19", u"%!20"};
      int t = a.brushType;
      int k = t >= 1 && t <= 8 ? 0 : t == 9 ? 1 : t >= 10 && t <= 17 ? 2 : t == 18 ? 3 : t >= 19 && t <= 22 ? t - 15 : -1;
      return k >= 0 ? names[k] : u"%!21";
    }
    case kWater: return a.on ? u"%!22" : u"%!23";
    case kName: return u"%!24";
    case kVariation: return u"%!25";
    default: return u"";
  }
}

}  // namespace

void registerTimelineNatives(VM& vm) {
  // loadFromFile(path) (L:0x081a6b20): the actions per day; currentDay from the header
  implement(vm, "gsGridTimeline", u"loadFromFile", [](VM& v, Object* s, int, const Value* a) {
    std::string path = utf16ToUtf8(v.resolvePath(v.toStr(a[0]), s));
    std::vector<uint8_t> file;
    Reader r{file};
    if (!v.vfs().read(path, file) || file.size() < 16 || r.u32(0) != kTimeline) return Value::boolean(false);
    TimelineData& t = timelineOf(s);
    t.days.clear();
    size_t end = std::min(file.size(), size_t(8) + r.u32(4));
    for (size_t at = 16; at + 8 <= end;) {
      uint32_t tag = r.u32(at), size = r.u32(at + 4);
      size_t body = at + 8, stop = std::min(end, body + size);
      if (tag == kDay && body + 8 <= stop) readActions(r, body + 8, stop, t.days[r.u32(body)]);
      at = body + size;
    }
    s->fieldValues[u"currentDay"] = Value::integer(int32_t(r.u32(12)));
    size_t actions = 0;
    for (auto& [d, list] : t.days) actions += list.size();
    LOG_INFO("timeline %s: %zu days with %zu actions, current day %u", path.c_str(), t.days.size(), actions, r.u32(12));
    v.fireEvent(s, u"onChange", {});
    return Value::boolean(true);
  });
  // saveToFile(path) (L:0x081a79e0): {dayCount, currentDay}, then the days in ascending order
  implement(vm, "gsGridTimeline", u"saveToFile", [](VM& v, Object* s, int, const Value* a) {
    std::string path = utf16ToUtf8(v.toStr(a[0]));
    TimelineData& t = timelineOf(s);
    Writer w;
    size_t root = w.begin(kTimeline);
    w.u32(t.dayCount());
    w.u32(uint32_t(currentDayOf(v, s)));
    for (auto& [day, list] : t.days) {
      if (list.empty()) continue;
      size_t body = w.begin(kDay);
      w.u32(day);
      w.u32(0);
      for (const Action& act : list) writeAction(w, act);
      w.end(body);
    }
    w.end(root);
    return Value::boolean(v.vfs().write(path, w.out));
  });
  // applyChanges(from, to, lookahead[, ignored]) (L:0x081a55d0): every day in [from, to] (capped at the last day):
  // the actions `lookahead` days ahead as previews (on day 1 all of days 1 â€¦ 1 + lookahead), then the day itself;
  // returns the area-weighted centre of the conflicts the previews marked, or unset
  implement(vm, "gsGridTimeline", u"applyChanges", [](VM& v, Object* s, int argc, const Value* a) {
    GridObj* grid = timelineGrid(v, s);
    TimelineData& t = timelineOf(s);
    if (!grid || !grid->data || t.days.empty()) return Value();
    int from = std::max(0, v.toInt(a[0])), to = std::min(v.toInt(a[1]), int(t.dayCount()) - 1);
    int lookahead = argc > 2 ? v.toInt(a[2]) : 0;
    if (from > to) return Value();
    Apply ap{v, grid};
    auto t0 = std::chrono::steady_clock::now();
    size_t items0 = grid->data->items.size();
    auto runDay = [&](int day, bool preview) {
      auto it = t.days.find(uint32_t(day));
      if (it == t.days.end()) return;
      for (auto act = it->second.rbegin(); act != it->second.rend(); ++act) applyAction(ap, *act, preview);
    };
    gridEditBegin(v, grid);
    for (int d = from; d <= to; ++d) {
      if (d != 0 && lookahead != 0) {
        int last = std::min(d + lookahead, int(t.dayCount()) - 1);
        for (int k = d == 1 ? 1 : d + lookahead; k <= last; ++k) runDay(k, true);
      }
      runDay(d, false);
    }
    gridEditEnd(v, grid);  // cover, meshes, picking and the simulation first; then the scripts hear of it
    if (ap.real + ap.previews > 20)
      LOG_INFO("timeline: days %d..%d applied, %zu actions + %zu previews, %zu items added, %.0f ms", from, to, ap.real,
               ap.previews, grid->data->items.size() - items0,
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    for (auto& [target, ev] : ap.events)
      if (!target->destroyed) v.fireEvent(target.get(), ev.first, {ev.second});
    if (ap.weight <= 0) return Value();
    return Value::vector(ap.pos[0] / ap.weight, ap.pos[1] / ap.weight, ap.pos[2] / ap.weight);
  });
  // checkDay(day) / checkDay(from, to) (L:0x081a2e10): are there actions on the day, or on any day in the range
  implement(vm, "gsGridTimeline", u"checkDay", [](VM& v, Object* s, int argc, const Value* a) {
    TimelineData& t = timelineOf(s);
    int from = v.toInt(a[0]);
    if (argc < 2) {
      if (from == 0) return Value::boolean(true);
      return Value::boolean(from > 0 && uint32_t(from) < t.dayCount() && t.days.count(uint32_t(from)));
    }
    if (from == 0) return Value::boolean(true);
    int to = std::min(v.toInt(a[1]), int(t.dayCount()) - 1);
    if (to < from) return Value::boolean(false);
    auto it = t.days.lower_bound(uint32_t(std::max(0, from)));
    return Value::boolean(it != t.days.end() && int(it->first) <= to && !it->second.empty());
  });
  // the editor's undo of the current day (L:0x081a37e0, L:0x081a3770, L:0x081a36c0) and its action list (L:0x081a5d50)
  implement(vm, "gsGridTimeline", u"removeDay", [](VM& v, Object* s, int, const Value*) {
    if (timelineOf(s).days.erase(uint32_t(currentDayOf(v, s)))) v.fireEvent(s, u"onChange", {});
    return Value();
  });
  implement(vm, "gsGridTimeline", u"removeLastAction", [](VM& v, Object* s, int, const Value*) {
    auto& days = timelineOf(s).days;
    auto it = days.find(uint32_t(currentDayOf(v, s)));
    if (it == days.end() || it->second.empty()) return Value();
    it->second.erase(it->second.begin());  // newest first
    if (it->second.empty()) days.erase(it);
    v.fireEvent(s, u"onChange", {});
    return Value();
  });
  implement(vm, "gsGridTimeline", u"removeAction", [](VM& v, Object* s, int, const Value* a) {
    auto& days = timelineOf(s).days;
    auto it = days.find(uint32_t(currentDayOf(v, s)));
    int i = v.toInt(a[0]);
    if (it == days.end() || i < 0 || size_t(i) >= it->second.size()) return Value();
    it->second.erase(it->second.begin() + i);
    if (it->second.empty()) days.erase(it);
    v.fireEvent(s, u"onChange", {});
    return Value();
  });
  implement(vm, "gsGridTimeline", u"getActions", [](VM& v, Object* s, int, const Value*) {
    auto arr = make<ArrayObj>();
    auto& days = timelineOf(s).days;
    if (auto it = days.find(uint32_t(currentDayOf(v, s))); it != days.end())
      for (const Action& act : it->second) arr->items.push_back(Value::string(labelOf(act)));
    return Value::heap(VT::Array, arr.get());
  });
  implement(vm, "gsGridTimeline", u"removeRedundantActions", [](VM&, Object*, int, const Value*) {
    return Value();  // editor clean-up (L:0x081a8e80); not needed for play
  });
}

}  // namespace regs::engine
