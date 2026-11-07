// Laying tram and metro tracks: gsGridPlayer's GRIDMODE_LAY_TRAM_TRACK (0x11) and GRIDMODE_LAY_TRACK (6), and the
// track blocks other tools build on (docs/ui/construction.md §6-§9). The functions follow the engine's
// (decompiled/Linux/functions/): mouseMoveLayTram L:0x08193b00, createTramBlock L:0x081926b0, checkTram L:0x0818d690,
// mouseMoveLayTrack L:0x0819cc70, createTrackBlock L:0x08195400, createRoadItem L:0x0818c3b0, checkRoadSlopes
// L:0x0818c840, checkConflicts L:0x0818d5c0, placeObject L:0x08196b00 (the parts of placed track pieces), softenArea
// L:0x0818cfb0, gsGrid::getObject L:0x0813f520 (templates by type, variation, connections and flags) and
// gsGridItem::fixY L:0x08187760.
//
// The engine builds its previews as loose gsGridItems and adds them to the grid only when a tool commits. The port
// works the same way on its own piece list (Piece) while a preview is built, and only then adds the pieces to the map
// as TEMP items (tools.h), so the map sees one batch per rebuilt preview.
#include "engine/tool_tracks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/grid_render.h"
#include "engine/gui.h"
#include "engine/sim_graph.h"
#include "engine/timeline.h"
#include "engine/tools.h"

namespace regs::engine {

using namespace regs::script;

namespace {

constexpr int kNoY = -0x8000;  // "no height": fixY finds it (gsGridItem ctor), intersectGrid picks the ground
constexpr uint32_t kFlagEnableBase = 0x1, kFlagJoin = 0x2, kFlagCover = 0x4, kFlagStop = 0x2000, kFlagBridge = 0x4000,
                   kFlagTunnel = 0x8000, kFlagDisableTram = 0x20000, kFlagAlign4 = 0x8000000;
constexpr uint32_t kKeyMask = 0xcf0f0;  // the flags gsGrid::getObject matches: slopes, bridge, tunnel, special0/1
constexpr int kTypeMetroStation = 0x10, kTypeDock = 0x11;

int fieldI(VM& vm, const Object* o, const char16_t* name, int def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : int(vm.toInt(it->second));
}
float fieldF(VM& vm, const Object* o, const char16_t* name, float def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toFloat(it->second);
}

uint32_t rot(uint32_t c, int n) { return rotateConnections(c, n); }
int sgn(int v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }

// ---------------------------------------------------------------------------------------------------------------
// templates: gsGridObject fields and the grid's search map (+0x1d0)

struct TInfo {
  std::string name;
  Object* obj = nullptr;
  int width = 1, length = 1, height = 0, negHeight = 0, type = 0, variation = 0, offx = 0, offz = 0;
  int minY = -255 * 256;  // +0x358 minY (default −255.0) · 256
  uint32_t flags = 0, conn = 0, conn2 = 0, diag = 0;
};

struct Key {
  uint32_t type, var, conn, conn2, flags;
  bool operator<(const Key& o) const {
    return std::tie(type, var, conn, conn2, flags) < std::tie(o.type, o.var, o.conn, o.conn2, o.flags);
  }
};

struct Catalog {
  size_t built = size_t(-1);
  std::unordered_map<std::string, TInfo> info;
  std::map<Key, std::pair<int, const TInfo*>> search;  // → (orientation, template)
  std::weak_ptr<GridData> data;
  std::vector<const TInfo*> byType;  // map type index → template
};
std::unordered_map<const GridObj*, Catalog> g_catalogs;

const TInfo* templateInfo(VM& vm, GridObj* grid, Catalog& c, const std::string& name) {
  auto f = c.info.find(name);
  if (f != c.info.end()) return f->second.obj ? &f->second : nullptr;
  TInfo& t = c.info[name];
  auto it = grid->templates.find(name);
  if (it == grid->templates.end()) return nullptr;
  Object* o = it->second.get();
  t.name = name;
  t.obj = o;
  t.width = std::max(1, fieldI(vm, o, u"width", 1));
  t.length = std::max(1, fieldI(vm, o, u"length", 1));
  t.height = int(fieldF(vm, o, u"height", 1) * 256.0f);  // ctor default: one cell
  t.negHeight = fieldI(vm, o, u"negHeight", 0);
  t.type = fieldI(vm, o, u"type", 0);
  t.variation = fieldI(vm, o, u"variation", 0);
  t.offx = fieldI(vm, o, u"offsetx", 0);
  t.offz = fieldI(vm, o, u"offsetz", 0);
  t.minY = int(std::lround(fieldF(vm, o, u"minY", -255.0f) * 256.0f));
  t.flags = uint32_t(fieldI(vm, o, u"flags", 0));
  t.conn = uint32_t(fieldI(vm, o, u"connections", 0));
  t.conn2 = uint32_t(fieldI(vm, o, u"connections2", 0));
  t.diag = uint32_t(fieldI(vm, o, u"diagonals", 0));
  return &t;
}

// method_loadGridObject (L:0x08149f70.c:620-850): every template is filed under (type, variation, connections,
// connections2, flags & 0xcf0f0) with orientation 0 and, unless both masks are empty, under its masks turned by 90°
// (gsRotateConnections(·, 6)) with orientations 3, 2, 1 — only once (180°) when both masks are symmetric. A later
// template with the same key takes the entry over (its orientation stays).
Catalog& catalog(VM& vm, GridObj* grid) {
  Catalog& c = g_catalogs[grid];
  if (c.built != grid->templateOrder.size()) {
    c.info.clear();
    c.search.clear();
    c.byType.clear();
    for (const std::string& name : grid->templateOrder) {
      c.info.erase(name);  // a duplicate id: the latest object
      const TInfo* t = templateInfo(vm, grid, c, name);
      if (!t) continue;
      uint32_t f = t->flags & kKeyMask, a = t->conn, b = t->conn2;
      auto file = [&](uint32_t ca, uint32_t cb, int orient) {
        auto [it, inserted] = c.search.try_emplace(Key{uint32_t(t->type), uint32_t(t->variation), ca, cb, f}, orient, t);
        if (!inserted) it->second.second = t;
      };
      file(a, b, 0);
      if (a == 0 && b == 0) continue;
      int orient = 0;
      for (int count = 1;; ++count) {
        a = rot(a, 6), b = rot(b, 6), orient = (orient + 3) & 3;
        file(a, b, orient);
        if (count == 3 || (count == 1 && rot(a, 4) == a && rot(b, 4) == b)) break;
      }
    }
    c.built = grid->templateOrder.size();
  }
  if (c.data.lock() != grid->data) {
    c.byType.clear();
    c.data = grid->data;
  }
  return c;
}

// ---------------------------------------------------------------------------------------------------------------
// items: map items and pieces of a preview in the engine's terms (gsGridItem +0x2c..+0x6c)

struct Piece {
  const TInfo* t = nullptr;
  int live = -1;  // the map item (a live one, or a preview already in the map); −1 for a piece of this preview
  int x = 0, y = 0, z = 0, orient = 0, sx = 1, sz = 1;  // y in the engine's 1/256 cells
  uint32_t iflags = 0, conn = 0, conn2 = 0;              // the item's own flags; masks turned with it
  int parentLive = -1, parentPiece = -1;                 // +0x1c
  uint32_t variation = 0;                                // +0x6c
  std::string name;                                      // +0x60
  uint32_t flags() const { return iflags | (t ? t->flags : 0); }
  int type() const { return t ? t->type : 0; }
  int height() const { return t ? t->height : 0; }
  int tvar() const { return t ? t->variation : 0; }
};

struct Ctx {
  VM& vm;
  ControlObj* player;
  GridObj* grid;
  GridData& g;
  Catalog& cat;
  ToolState& t;
  std::vector<Piece> W;  // the preview being built (+0x27c)
  int editMode, toolMode, toolType, toolSize;
  int shift;

  Ctx(VM& v, ControlObj* p, GridObj* gr)
      : vm(v), player(p), grid(gr), g(*gr->data), cat(catalog(v, gr)), t(toolState(p)) {
    editMode = fieldI(vm, p, u"editMode", 1);
    toolMode = fieldI(vm, p, u"toolMode", 0);
    toolType = fieldI(vm, p, u"toolType", 0);
    toolSize = fieldI(vm, p, u"toolSize", 0);
    shift = 0x200 - int(g.heightBase);
  }

  const TInfo* typeInfo(int type) {
    if (type < 0 || size_t(type) >= g.types.size()) return nullptr;
    while (cat.byType.size() <= size_t(type))
      cat.byType.push_back(templateInfo(vm, grid, cat, g.types[cat.byType.size()]));
    return cat.byType[size_t(type)];
  }

  void shape(Piece& p) {
    p.sx = (p.orient & 1) ? p.t->length : p.t->width;
    p.sz = (p.orient & 1) ? p.t->width : p.t->length;
    p.conn = rot(p.t->conn, p.orient * 2);
    p.conn2 = rot(p.t->conn2, p.orient * 2);
  }

  Piece live(int i) {
    const GridData::Item& it = g.items[size_t(i)];
    Piece o;
    o.t = typeInfo(it.type);
    o.live = i;
    o.x = it.x, o.y = it.y + shift, o.z = it.z, o.orient = it.orient & 3;
    o.iflags = it.flags;
    if (o.t) shape(o);
    o.parentLive = it.parent ? int(it.parent) - 1 : -1;
    o.variation = it.variation;
    if (size_t(i) < g.names.size()) o.name = g.names[size_t(i)];
    return o;
  }

  // gsGrid::getObjects(x, z, w, l): the live items overlapping the area
  std::vector<Piece> getObjects(int x, int z, int w, int l) {
    std::vector<Piece> out;
    for (uint32_t i : gridGetObjects(vm, grid, x, z, w, l)) {
      Piece o = live(int(i));
      if (o.t) out.push_back(std::move(o));
    }
    return out;
  }

  int parentY(const Piece& p) {
    if (p.parentPiece >= 0 && size_t(p.parentPiece) < W.size()) return W[size_t(p.parentPiece)].y;
    if (p.parentLive >= 0) return g.items[size_t(p.parentLive)].y + shift;
    return p.y;
  }

  // gsGridItem::fixY (L:0x08187760): the height of an item placed without one (y = −0x8000) or with a base
  void fixY(Piece& p) {
    uint32_t f = p.flags();
    if (p.x < 0 || p.z < 0 || p.x + p.sx > g.width || p.z + p.sz > g.length) return;
    int ex = std::min(p.sx, g.width - p.x - 1), ez = std::min(p.sz, g.length - p.z - 1);
    auto H = [&](int px, int pz) { return int(g.height[size_t(p.z + pz) * size_t(g.width) + size_t(p.x + px)]); };
    int y0 = p.y, y = y0;
    if (f & kFlagEnableBase) {
      int lo = 0x8000, hi = -0x8000;
      for (int pz = 0; pz <= ez; ++pz)
        for (int px = 0; px <= ex; ++px) hi = std::max(hi, H(px, pz)), lo = std::min(lo, H(px, pz));
      if (y0 == kNoY) {
        y = hi;
        if (p.type() == kTypeMetroStation || (f & kFlagAlign4)) y = (hi + 0x7f) & ~0x7f;
        else if (p.type() == kTypeDock) y = hi < 0x181 ? 0x100 : 0x200;
      }
    } else if ((f & 0x40014004) == kFlagBridge) {  // a bridge: over the highest corner
      int hi = -0x8000;
      for (int pz = 0; pz <= ez; ++pz)
        for (int px = 0; px <= ex; ++px) hi = std::max(hi, H(px, pz));
      y = (hi + 0xc0) & ~0x7f;
    } else if ((f & 0x40018004) == kFlagTunnel) {  // a tunnel: under the lowest
      int lo = 0x8000;
      for (int pz = 0; pz <= ez; ++pz)
        for (int px = 0; px <= ex; ++px) lo = std::min(lo, H(px, pz));
      y = (lo - 0x1c0) & ~0x7f;
    } else if ((f & 0x40000008) == 8) {  // LOCK_GROUND: the mean of the corners it stands on, or a locked corner
      ItemFootprint fp = itemFootprint(p.t->width, p.t->length, p.t->diag, p.orient);
      long sum = 0;
      int count = 0, hi = -0x8000;
      bool locked = false;
      for (int pz = 0; pz <= ez && !locked; ++pz)
        for (int px = 0; px <= ex && !locked; ++px) {
          if (fp.diag) {  // a corner outside the diagonal cuts has no height (getY 0x7fffffff)
            bool inside = false;
            for (int dz = -1; dz <= 0 && !inside; ++dz)
              for (int dx = -1; dx <= 0 && !inside; ++dx)
                if (px + dx >= 0 && pz + dz >= 0 && px + dx < p.sx && pz + dz < p.sz &&
                    itemCellCover(fp, f, px + dx, pz + dz))
                  inside = true;
            if (!inside) continue;
          }
          int h = H(px, pz);
          hi = std::max(hi, h);
          if (g.flags[size_t(p.z + pz) * size_t(g.width) + size_t(p.x + px)] & 0x1000) {
            y = h;
            locked = true;
            break;
          }
          sum += h;
          ++count;
        }
      if (!locked && count) y = int(sum / count);
      if (p.type() == 1) y = (y + 0x20) & ~0x3f;
      else {
        y = (y + 0x40) & ~0x7f;
        if (p.type() == kTypeDock) y = hi < 0x181 ? 0x100 : 0x200;
      }
    } else if (f & 0x40000000) {
    } else if (p.parentLive >= 0 || p.parentPiece >= 0) {
      y = parentY(p);
    } else {
      long sum = 0;
      for (int pz = 0; pz <= ez; ++pz)
        for (int px = 0; px <= ex; ++px) sum += H(px, pz);
      y = int(sum / long((ez + 1) * (ex + 1)));
    }
    p.y = std::max(y, p.t->minY);
  }

  // a new item of a template (the gsGridItem ctor as the tools call it, TEMP)
  Piece piece(const TInfo* tpl, int x, int y, int z, int orient, uint32_t iflags) {
    Piece p;
    p.t = tpl;
    p.x = x, p.z = z, p.orient = orient & 3;
    p.iflags = iflags | kItemTemp;
    shape(p);
    if (tpl->type == kTypeMetroStation || (p.flags() & kFlagAlign4)) y = (y + 0x7f) & ~0x7f;
    p.y = y;
    if (y == kNoY || (p.flags() & kFlagEnableBase)) fixY(p);
    return p;
  }

  // gsGrid::getObject(type, variation, connections, connections2, flags, &orient)
  const TInfo* lookup(uint32_t type, uint32_t var, uint32_t conn, uint32_t conn2, uint32_t flags, int* orient) {
    auto it = cat.search.find(Key{type, var, conn, conn2, flags & kKeyMask});
    if (it == cat.search.end()) return nullptr;
    if (orient) *orient = it->second.first;
    return it->second.second;
  }

  // gsGridPlayer::createRoadItem (L:0x0818c3b0): the piece for a block (x, z) with these connections. Without an exact
  // template the type's plain piece stands there as an error (0x200); the block's corner is moved by the template's
  // offsets as its orientation turns them.
  Piece createRoadItem(int x, int y, int z, uint32_t conn, uint32_t conn2, int type, int var, uint32_t f) {
    int o = 0;
    uint32_t extra = f & kFlagJoin;
    const TInfo* tp = lookup(uint32_t(type), uint32_t(var), conn, conn2, f, &o);
    if (!tp) {
      tp = lookup(uint32_t(type), uint32_t(var), 0, 0, f & 0xc000, &o);
      if (!tp) tp = lookup(uint32_t(type), uint32_t(var), 0, 0, 0, &o);
      if (!tp) tp = lookup(uint32_t(type), 0, 0, 0, 0, &o);
      extra |= kItemNegative;
    }
    if (!tp) {  // the engine has an orphan piece for every type; without one the block is an error
      Piece bad;
      bad.x = x, bad.y = y, bad.z = z;
      bad.iflags = kItemTemp | kItemNegative;
      return bad;
    }
    int s = type != 1 ? 4 : 1;
    switch (o) {
      case 1: x += tp->offz, z += s - tp->offx - tp->width; break;
      case 0: x += tp->offx, z += tp->offz; break;
      case 2: z += s - tp->offz - tp->length, x += s - tp->offx - tp->width; break;
      case 3: z += tp->offx, x += s - tp->offz - tp->length; break;
    }
    return piece(tp, x, y, z, o, extra);
  }
};

// gsGridItem::getRoadXZ (L:0x08181510): the corner of the 4×4 block an item of a road or track belongs to
std::pair<int, int> roadXZ(const Piece& o) {
  int x = o.x, z = o.z;
  const TInfo* t = o.t;
  const int s = t->type != 1 ? 4 : 1;  // alleys: 1-cell blocks (as createRoadItem places them)
  switch (o.orient) {
    case 1: x -= t->offz, z += t->offx - s + o.sz; break;
    case 0: x -= t->offx, z -= t->offz; break;
    case 2: x += t->offx - s + o.sx, z += t->offz - s + o.sz; break;
    case 3: x += t->offz - s + o.sx, z -= t->offx; break;
  }
  return {x, z};
}

// the 8 directions as connection bits (1 north = −z, 2 north-east, 4 east = +x, … 0x80 north-west) for a step (vz, vx)
uint32_t dirBit(int vz, int vx) {
  if (vz == 0 || vx == 0) {
    if (vz < 0) return 1;
    if (vx > 0) return 4;
    if (vz > 0) return 0x10;
    return vx < 0 ? 0x40 : 0;
  }
  if (vx > 0) return vz < 0 ? 2 : 8;
  return vz > 0 ? 0x20 : 0x80;
}
// the unit step (dz, dx) of a single direction bit; other values leave the step as it is
void bitStep(uint32_t b, int& dz, int& dx) {
  switch (b) {
    case 1: dz = -1, dx = 0; break;
    case 2: dz = -1, dx = 1; break;
    case 4: dz = 0, dx = 1; break;
    case 8: dz = 1, dx = 1; break;
    case 0x10: dz = 1, dx = 0; break;
    case 0x20: dz = 1, dx = -1; break;
    case 0x40: dz = 0, dx = -1; break;
    case 0x80: dz = -1, dx = -1; break;
    default: break;
  }
}
uint32_t spread(uint32_t v) { return v >> 24 | v << 16 | v >> 16 | v << 8 | v >> 8 | v | v << 24; }

// ---------------------------------------------------------------------------------------------------------------
// the preview in the map

// the old preview goes: its cells and the items under it lose the highlight, the replaced set is emptied
void clearPreview(Ctx& c) {
  for (auto i = c.t.newItems.rbegin(); i != c.t.newItems.rend(); ++i) {
    if (*i >= c.g.items.size() || c.g.items[*i].removed) continue;
    int x, z, w, l;
    gridItemRect(c.vm, c.grid, *i, x, z, w, l);
    gridUnselectArea(c.vm, c.grid, x, z, w, l);
    gridSetSelected(c.vm, c.grid, *i, false, false);
    gridEditRemoveItem(c.vm, c.grid, *i);
  }
  for (uint32_t r : c.t.replaced) gridSetSelected(c.vm, c.grid, r, false, false);
  c.t.newItems.clear();
  c.t.replaced.clear();
}

// the pieces into the map as TEMP items, in order (newItems)
void materialize(Ctx& c) {
  std::vector<int> index(c.W.size(), -1);
  c.t.newItems.clear();
  for (size_t k = 0; k < c.W.size(); ++k) {
    Piece& p = c.W[k];
    if (p.live < 0 && p.t) {
      int parent = p.parentPiece >= 0 ? index[size_t(p.parentPiece)] : p.parentLive;
      int y = std::clamp(p.y, -0x7fff + 0x400, 0x7fff - 0x400);
      int i = gridEditAddItem(c.vm, c.grid, p.t->name, p.x, y, p.z, p.orient, p.iflags | kItemTemp, parent, false);
      if (i < 0) continue;
      c.g.items[size_t(i)].variation = p.variation;
      if (!p.name.empty()) {
        if (c.g.names.size() <= size_t(i)) c.g.names.resize(size_t(i) + 1);
        c.g.names[size_t(i)] = p.name;
      }
      p.live = i;
    }
    if (p.live < 0) continue;
    index[k] = p.live;
    c.t.newItems.push_back(uint32_t(p.live));
  }
}

// the current newItems as pieces (a block added to another tool's preview)
void loadPreview(Ctx& c) {
  c.W.clear();
  for (uint32_t i : c.t.newItems)
    if (i < c.g.items.size() && !c.g.items[i].removed) {
      Piece p = c.live(int(i));
      if (p.t) c.W.push_back(std::move(p));
    }
}

void markError(Ctx& c, uint32_t item) {
  if (item < c.g.items.size()) c.g.items[item].flags |= kItemNegative;
}

// ---------------------------------------------------------------------------------------------------------------
// checkRoadSlopes (L:0x0818c840): pieces laid on the ground follow it by at most `step` per block; a piece between
// two heights becomes its type's slope (SLOPE_1p8 0x20 for a step of 0x80, 1p4 0x40 for 0x100 on roads and trams)

void checkRoadSlopes(Ctx& c, int step) {
  auto& W = c.W;
  size_t n = W.size();
  if (!n) return;
  auto flat = [](const Piece& p) { return p.parentLive < 0 && p.parentPiece < 0; };
  int prev = W[0].y, cur = prev;
  for (size_t i = 0; i < n; ++i) {
    Piece& it = W[i];
    if (flat(it) && !(it.flags() & kFlagJoin) && it.t) {
      cur = prev;
      if (c.lookup(uint32_t(it.type()), uint32_t(it.tvar()), it.conn, it.conn2, it.flags() | 0x20, nullptr)) {
        const Piece& a = i == 0 ? it : W[i - 1];
        const Piece& b = i + 1 < n ? W[i + 1] : it;
        int y = it.y, ay = a.y;
        if (ay < y) {
          if (b.y < y) {
            y = b.y;
            if (b.y < ay) y = ay;
          }
        } else if (y < ay && y < b.y) {
          y = b.y;
          if (ay < b.y) y = ay;
        }
        cur = y >= prev + step ? prev + step : y <= prev - step ? prev - step : y;
      }
      it.y = cur;
    } else {
      cur = it.y;
    }
    prev = cur;
  }
  // backwards: a piece that has to climb (or drop) to the one before it becomes a slope
  int prevY = cur;
  for (int i = int(n) - 1; i >= 0; --i) {
    Piece& it = W[size_t(i)];
    if (!flat(it) || !it.t) {
      prevY = it.y;
      continue;
    }
    if (it.flags() & kFlagJoin) {  // a joined neighbour keeps its height: a step to it is an error
      if (it.y != prevY) it.iflags |= kItemNegative;
      goto keep;
    }
    if (!c.lookup(uint32_t(it.type()), uint32_t(it.tvar()), it.conn, it.conn2, it.flags() | 0x20, nullptr)) goto keep;
    {
      const Piece& a = i != 0 ? W[size_t(i - 1)] : it;
      int cy = prevY + step;
      if (a.y < prevY + step) {
        cy = prevY - step;
        if (prevY - step <= a.y) cy = a.y;
      }
      if (i < 2) {
      aa0:
        if (cy == prevY) goto keep;
      } else {
        int by = W[size_t(i - 2)].y;
        if (prevY < cy) {
          if (by < cy) {
            cy = by;
            if (by < prevY) cy = prevY;
            if (cy == prevY) goto keep;
          } else {
            goto aa0;
          }
        } else if (prevY <= cy) {
          goto aa0;
        } else if (cy < by) {
          cy = by;
          if (prevY < by) goto keep;
          goto aa0;
        }
      }
      // the piece becomes the slope from prevY to cy, its low end toward the lower side
      int d = std::abs(cy - prevY), t = it.type();
      uint32_t sf;
      if (t == 1) sf = d == 0x80 ? 0x80 : 0x40;
      else if (t >= 5 && t <= 7 && it.t->diag == 0) sf = d == 0x100 ? 0x20 : 0x10;
      else sf = d == 0x100 ? 0x40 : 0x20;
      int o = 0;
      const TInfo* tp = c.lookup(uint32_t(t), uint32_t(it.tvar()), it.conn, it.conn2, sf | it.flags(), &o);
      if (!tp) goto keep;  // the engine has the slope of every piece that has a 1:8 one
      if ((o & 1) == 0) o = (prevY < cy) == (a.x <= it.x) ? 2 : 0;
      else o = (prevY < cy) == (a.z <= it.z) ? 1 : 3;
      Piece s = c.piece(tp, it.x, std::min(cy, prevY), it.z, o, 0);
      s.variation = it.variation;
      it = s;
      prevY = cy;
      continue;
    }
  keep:
    it.y = prevY;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// tram tracks (GRIDMODE_LAY_TRAM_TRACK)

// checkTram (L:0x0818d690): the block (x, z) already has a tram track on its road, also through the block a large
// road or avenue piece there belongs to
bool checkTram(Ctx& c, int x, int z, int depth = 0) {
  if (depth > 8) return false;
  for (const Piece& o : c.getObjects(x + 1, z + 1, 2, 2)) {
    unsigned t = unsigned(o.type());
    if (t - 10u < 3 && o.parentLive >= 0) {
      Piece p = c.live(o.parentLive);
      if (p.t && (p.flags() & kFlagCover)) return true;
    }
    if (t - 5u < 2 && (o.flags() & kFlagCover)) {
      auto [rx, rz] = roadXZ(o);
      if (rx != x || rz != z) return checkTram(c, rx, rz, depth + 1);
    }
  }
  return false;
}

// createTramBlock (L:0x081926b0): the tram piece of the block (x, z) on `road` (or on its own) with the connections
// `conn`; at the ends of a drag (start / end) it joins the tram tracks around, replacing them by their junction pieces
void createTramBlock(Ctx& c, int x, int z, uint32_t conn, bool isStart, bool isEnd, int road) {
  std::optional<Piece> res;
  size_t mark = c.W.size();
  Piece R;
  if (road >= 0) R = c.live(road);
  bool large = false;
  int T, kind;
  if (road < 0) T = 10, kind = 0;
  else if (R.type() == 5) large = true, T = 11, kind = 3;
  else T = R.type() == 6 ? 12 : 10, kind = R.type() == 6 ? 4 : 0;
  auto keep = [&](const Piece& o) {
    if (unsigned(o.type()) - 10u >= 3) return false;
    if (o.parentLive < 0) return true;
    uint32_t pf = c.live(o.parentLive).flags();
    return (pf & 0x4004) != 0x4000 && (pf & 0x8004) != 0x8000;
  };
  auto shiftKind = [&](uint32_t& d, const Piece* P) {
    if (kind == 4) {
      if (P) {
        if (P->type() < 6) {
          if (P->type() > 4) d <<= 8;
        } else {
          d <<= 16;
        }
      }
    } else if (kind == 3 && P && P->type() > 4) {
      d <<= 8;
    }
  };
  if (isStart || isEnd) {
    std::vector<Piece> list = c.getObjects(x - 7, z - 7, 18, 18);
    for (const Piece& o : list) {  // neighbours along the axes, and a track already on this block
      if (!keep(o)) continue;
      auto [rx, rz] = roadXZ(o);
      std::optional<Piece> P;
      if (o.parentLive >= 0) P = c.live(o.parentLive);
      int span = kind != 0 ? 8 : P ? (P->type() != 2 ? 8 : 4) : 4;
      uint32_t d;
      if (rx + span == x && rz == z) d = 4;
      else if (rx == x + span && rz == z) d = 0x40;
      else if (x != rx) d = 0;
      else if (rz + span == z) d = 0x10;
      else d = rz == z + span ? 1 : 0;
      bool ew = d == 0x40 || d == 4;
      if (road >= 0 && (R.flags() & 0xc0f0) && (((R.orient & 1) == 0) != ew)) d = 0;
      if (P && (P->flags() & 0xc0f0) && (((P->orient & 1) == 0) != ew)) d = 0;
      shiftKind(d, P ? &*P : nullptr);
      uint32_t r4 = rot(d, 4);
      uint32_t m7 = rot(d, 7) | rot(d, 1), m6 = rot(r4, 7) | rot(r4, 1);
      if (!P || (P->conn & d) == 0) m7 = spread(m7), m6 = spread(m6);
      if (rx == x && rz == z && o.type() == T && (o.flags() & 0xf0) == 0) {
        uint32_t f = o.flags();
        if (!(f & 0xc000)) f &= ~0xf0u;
        Piece p = c.createRoadItem(x, o.y, z, conn | o.conn, o.conn2, T, o.tvar(), f);
        p.variation = o.variation;
        if (p.name.empty()) p.name = o.name;
        if (!(p.flags() & kItemNegative) && !(o.flags() & kFlagStop)) {
          p.iflags |= kFlagJoin;
          c.t.replaced.insert(uint32_t(o.live));
          conn = p.conn;
          res = p;
        }
      } else {
        uint32_t oc = o.conn;
        if (d != (d & oc) && (conn & r4) != r4 && (m7 & oc) == 0 && (conn & m6) == 0) {
          uint32_t f = o.flags() & 0xfff3ffffu;
          if (!(o.flags() & 0xc000)) f &= 0xffffff0fu;
          Piece q = c.createRoadItem(rx, o.y, rz, d | oc, o.conn2, o.type(), o.tvar(), f);
          q.variation = o.variation;
          if (q.name.empty()) q.name = o.name;
          res = c.createRoadItem(x, kNoY, z, r4 | conn, 0, T, c.toolType, 0);
          if (!(q.flags() & kItemNegative) && !(res->flags() & kItemNegative) && !(o.flags() & kFlagStop)) {
            q.iflags |= kFlagJoin;
            q.parentLive = o.parentLive;
            c.W.push_back(q);
            c.t.replaced.insert(uint32_t(o.live));
            conn |= r4;
          } else {
            res.reset();
          }
        }
      }
    }
    for (const Piece& o : list) {  // diagonal neighbours (and those of large roads, two blocks wide)
      if (!keep(o)) continue;
      auto [rx, rz] = roadXZ(o);
      std::optional<Piece> P;
      if (o.parentLive >= 0) P = c.live(o.parentLive);
      uint32_t d = 0, special = 0;
      bool viaLarge = false;
      if (rx + 4 == x && (rz + 4 == z || rz == z + 4)) d = rz + 4 == z ? 8 : 2;
      else if (rx == x + 4 && (rz + 4 == z || rz == z + 4)) d = rz + 4 == z ? 0x20 : 0x80;
      else if (P && (kind == 3 || P->type() == 5)) {
        if (x == rx + 4) {
          if (rz + 8 == z) d = 8, special = 0x40000, viaLarge = large;
          if (rz == z + 8) d = 2, special = 0x80000, viaLarge = large;
        }
        if (rx == x + 4) {
          if (rz + 8 == z) d = 0x20, viaLarge = large, special = 0x80000;
          if (rz == z + 8) d = 0x80, viaLarge = large, special = 0x40000;
        }
        if (rx + 8 == x && rz + 4 == z) d = 8, special = 0x80000, viaLarge = large;
        if (rx == x + 8 && rz + 4 == z) d = 0x20, viaLarge = large, special = 0x40000;
        if (x == rx + 8 && z + 4 == rz) d = 2, viaLarge = large, special = 0x40000;
        if (rx == x + 8 && z + 4 == rz) d = 0x80, special = 0x80000, viaLarge = large;
        if ((d & P->conn) == 0) d = 0, special = 0, viaLarge = false;
      }
      shiftKind(d, P ? &*P : nullptr);
      uint32_t r4 = rot(d, 4);
      uint32_t m5 = rot(d, 1) | rot(d, 7), m7 = rot(r4, 1) | rot(r4, 7);
      if (special == 0) m5 = spread(m5), m7 = spread(m7);
      uint32_t oc = o.conn;
      if (d != (oc & d) && (conn & r4) != r4 && (oc & m5) == 0 && (conn & m7) == 0) {
        uint32_t f;
        if (special == 0 || !P || P->type() != 5 || oc != 0) f = o.flags() & 0xfff3ffffu;
        else f = special | o.flags();
        if (!(f & 0xc000)) f &= 0xffffff0fu;
        Piece q = c.createRoadItem(rx, o.y, rz, d | oc, o.conn2, o.type(), o.tvar(), f);
        q.variation = o.variation;
        if (q.name.empty()) q.name = o.name;
        res = c.createRoadItem(x, kNoY, z, r4 | conn, 0, T, c.toolType, viaLarge && conn == 0 ? special : 0);
        if (!(q.flags() & kItemNegative) && !(res->flags() & kItemNegative) && !(o.flags() & kFlagStop)) {
          q.iflags |= kFlagJoin;
          q.parentLive = o.parentLive;
          c.W.push_back(q);
          c.t.replaced.insert(uint32_t(o.live));
          conn |= r4;
        } else {
          res.reset();
        }
      }
    }
    if (isStart && isEnd) mark = (c.W.size() + mark) >> 1;
  }
  if (!res) res = c.createRoadItem(x, kNoY, z, conn, 0, T, c.toolType, 0);
  if (road >= 0 && res->t) {
    uint32_t f = R.flags();
    if ((f & 0xf0) == 0 || (f & kFlagBridge) || (f & kFlagTunnel)) {
      res->y = R.y;
    } else {  // on a sloped road: the slope piece of the same connections, turned with the road
      const TInfo* tp = c.lookup(uint32_t(res->type()), uint32_t(res->tvar()), res->conn, res->conn2,
                                 (f & 0xf0) | (res->flags() & 0xfff3ffffu), nullptr);
      if (!tp) {
        res->iflags |= kItemNegative;
      } else {
        Piece s = c.piece(tp, res->x, R.y, res->z, R.orient, 0);
        res = s;
      }
    }
    res->parentLive = road;
    if (res->name.empty()) res->name = R.name;
  }
  if (isEnd) c.W.insert(c.W.begin() + std::ptrdiff_t(std::min(mark, c.W.size())), *res);
  else c.W.push_back(*res);
}

// REGS_DEBUG_TOOLS=2: the pieces of each rebuilt preview
void logPieces(Ctx& c) {
  static const bool verbose = std::getenv("REGS_DEBUG_TOOLS") && std::string(std::getenv("REGS_DEBUG_TOOLS")) == "2";
  if (!verbose) return;
  for (const Piece& p : c.W) {
    Piece par = p.parentLive >= 0 ? c.live(p.parentLive) : Piece{};
    LOG_INFO("  %s x %d y %d z %d o %d conn %08x flags %08x parent %d (%s y %d x %d z %d o %d)",
             p.t ? p.t->name.c_str() : "-", p.x, p.y, p.z, p.orient, p.conn, p.flags(), p.parentLive,
             par.t ? par.t->name.c_str() : "-", par.y, par.x, par.z, par.orient);
  }
}

// mouseMoveLayTram (L:0x08193b00): the drag from the start block to the block under the cursor, following the roads
// (large roads and avenues by their two-block pieces, slopes and bridges to their far end), one tram piece per block
void moveLayTram(VM& vm, ControlObj* player, GridObj* grid, float mx, float my) {
  static const bool debugTiming = std::getenv("REGS_DEBUG_TOOLS") != nullptr;
  auto t0 = std::chrono::steady_clock::now();
  ToolPick pick = toolPick(vm, player, mx, my, false);
  if (!pick.hit) return;
  int px = pick.cellX, pz = pick.cellZ;
  int bx = px & ~3, bz = pz & ~3;
  if (((bx + bz) & 4) != 0) {  // the checkerboard's odd blocks: the cursor is pushed toward the nearer edge
    int ox = (px & 3) * 2 - 3, oz = (pz & 3) * 2 - 3;
    if (std::abs(oz) < std::abs(ox)) px += ox < 0 ? -4 : 4;
    else pz += oz < 0 ? -4 : 4;
  }
  int hint = (bz <= pz + 1 ? 2 : 0) | (bx <= px + 1 ? 1 : 0);  // only part of the cache key (+0x1ac)
  Ctx c(vm, player, grid);
  ToolState& t = c.t;
  if (bx == t.cellX && bz == t.cellZ && t.cellY == hint) return;
  gridEditBegin(vm, grid);
  if (!t.inProgress) t.startX = bx, t.startY = hint, t.startZ = bz;
  if (t.newItems.empty() && t.cellX != -1) gridUnselectArea(vm, grid, t.cellX, t.cellZ, 4, 4);
  clearPreview(c);

  int cx = t.startX, cz = t.startZ, sx = t.startX, sz = t.startZ, ex = bx, ez = bz;
  int dz = 0, dx = 0;
  bool skip = false, viaBridge = false, error = false;
  int prev = -1, road = -1;
  Piece P, R;
  for (int guard = 0; guard < 2048; ++guard) {
    uint32_t conn = 0;  // local_100: the connection back to the previous block
    int s = 4, ox = 0, oz = 0, n8 = 0, n9 = 0, nEz = 0;
    bool bridgeExit = false;
    uint32_t bt = 0;
    if (!skip) {
      uint32_t back = dirBit(-dz, -dx), fwd = dirBit(dz, dx);
      conn = back;
      auto step2 = [&] {
        if (std::abs(dx) == 4 && dz == 0) cx += dx * 2;
        else {
          cx += dx;
          if (std::abs(dz) == 4 && dx == 0) cz += dz * 2;
          else cz += dz;
        }
      };
      if (prev < 0) {
        cz += dz, cx += dx;
      } else if (P.type() == 5) {  // a large road: its lanes are the second byte
        conn = (P.conn & fwd << 8) ? back << 8 : back;
        step2();
      } else if (P.type() == 6) {  // an avenue: the third byte, else the second
        conn = (P.conn & fwd << 16) ? back << 16 : (P.conn & fwd << 8) ? back << 8 : back;
        step2();
      } else {
        cz += dz, cx += dx;
      }
    }
    // the road under the block: one at the previous height carrying connections2 after a bridge, else a covering one
    road = -1;
    {
      std::vector<Piece> list = c.getObjects(cx + 1, cz + 1, 2, 2);
      for (const Piece& o : list) {
        if (unsigned(o.type()) - 2u >= 5) continue;
        if (viaBridge) {
          if (o.y != P.y) continue;
          if (o.conn2 != 0) {
            road = o.live;
            break;
          }
        } else if (o.flags() & kFlagCover) {
          road = o.live;
          break;
        }
      }
      if (road < 0) viaBridge = false;
    }
    if (road < 0) goto place;
    {
      R = c.live(road);
      auto [rx0, rz0] = roadXZ(R);
      int rx = rx0, rz = rz0;
      int rtype = R.type();
      s = rtype < 5 ? 4 : 8;
      if (R.conn2 != 0) {  // a piece over several blocks: the block of it under (cx, cz)
        int offx = rx - R.x, offz = rz - R.z;
        if (R.x + s <= cx) do rx += s; while (s - offx + rx <= cx);
        if (R.z + s <= cz) do rz += s; while (s - offz + rz <= cz);
      }
      if (rtype > 4) {  // two-block roads: the drag's ends snap to the road's blocks
        if (prev < 0 || P.type() == 2) {
          if (std::abs(dx) == 4 && dz == 0 && rx != cx && rz == cz) {
            if (cx == sx) sx = cx + (skip ? -dx : dx);
            cx += skip ? -dx : dx;
            goto l427d;
          }
          if (std::abs(dz) != 4 || dx != 0 || rz == cz || cx != rx) goto l427d;
          if (cz == sz) sz = cz + (skip ? -dz : dz);
          cz += skip ? -dz : dz;
          if (sx == cx) goto l4fd0;
        l4287:
          if (sz != cz) goto l4295;
        } else {
        l427d:
          if (sx != cx) goto l4287;
        l4fd0:
          if (ex == sx) ex = rx;
          sx = rx, cx = rx;
          if (sz != cz) goto l4295;
        }
        if (ez == sz) ez = rz;
        cz = rz, sz = rz;
      }
    l4295:
      // where a slope or bridge piece leads: the far end block (ox, oz from its block) and the new step (n8, n9)
      bt = R.flags() & 0xc000;
      if (bt == 0) {
        if (R.flags() & 0xf0) {
          uint32_t k = R.conn;
          if ((k & 0x101010) && dz <= 0) n8 = -s, oz = s;
          else if ((k & 0x404040) && dx >= 0) n9 = s, ox = -s;
          else if ((k & 0x10101) && dz >= 0) n8 = s, oz = -s;
          else if ((k & 0x40404) && dx <= 0) n9 = -s, ox = s;
          else if ((k & 0x202020) && dz <= 0) n8 = -4, n9 = 4, oz = 4, ox = -4;
          else if ((k & 0x808080) && dx >= 0) n8 = 4, n9 = 4, oz = -4, ox = -4;
          else if ((k & 0x20202) && dz >= 0) n9 = -4, ox = 4, n8 = 4, oz = -4;
          else if ((k & 0x80808) && dx <= 0) n9 = n8 = -4, ox = oz = 4;
        }
      } else {
        uint32_t k = R.conn2;
        if ((k & 0x10) && dz <= 0) bridgeExit = true, n8 = -s, oz = s;
        else if ((k & 0x40) && dx >= 0) bridgeExit = true, n9 = s, ox = -s;
        else if ((k & 1) && dz >= 0) bridgeExit = true, n8 = s, oz = -s;
        else if ((k & 4) && dx <= 0) bridgeExit = true, n9 = -s, ox = s;
      }
      if (sx == cx && sz == cz) {  // the drag starts on such a piece: it starts at the end facing the drag
        bool turn = false;
        if (ox != 0 && sgn(ex - sx) != sgn(ox)) turn = true;
        else if (oz != 0 && sgn(ez - cz) != sgn(oz)) turn = true;
        if (turn) {
          dz = n8, dx = n9;
          if (bridgeExit || !checkTram(c, rx + ox, rz + oz)) {
            cz = rz + oz, cx = rx + ox;
            skip = true;
            sx = cx, sz = cz;
            viaBridge = bridgeExit;
            goto next;
          }
        }
      }
      // passing over such a piece: the drag goes on behind it
      viaBridge = false;
      nEz = ez;
      n8 = n9 = 0;
      uint32_t k = 0;
      if (bt == 0) {
        if (!(R.flags() & 0xf0)) goto l47c0;
        k = R.conn;
        if ((k & 0x101010) && dz >= 0) { n8 = s; goto l4750; }
        if ((k & 0x404040) && dx <= 0) { n8 = 0, n9 = -s; goto j478d; }
        if ((k & 0x10101) && dz <= 0) { n8 = -s; goto l4750; }
        if ((k & 0x40404) && dx >= 0) { n8 = 0, n9 = s; goto j478d; }
        if (k & 0x202020) { n8 = 4, n9 = -4; if (dz >= 0) goto j478d; }
        if (k & 0x808080) { n8 = -4, n9 = -4; if (dx <= 0) goto j478d; }
        if (k & 0x20202) { n8 = -4, n9 = 4; if (dz <= 0) goto j478d; }
        if (!(k & 0x80808)) goto l47c0;
        n8 = 4, n9 = 4;
        if (dx >= 0) goto j478d;
        if (rx == cx) goto l4890;
      l47cd:
        error = true;
        goto place;
      } else {
        k = R.conn2;
        bridgeExit = false;
        if ((k & 0x10) && dz >= 0) { bridgeExit = true, n8 = s; goto l4750; }
        if ((k & 0x40) && dx <= 0) { n8 = 0, n9 = -s, viaBridge = true; goto l4967; }
        if ((k & 1) && dz <= 0) { bridgeExit = true, n8 = -s; goto l4750; }
        if (!(k & 4) || dx < 0) goto l47c0;
        n8 = 0, viaBridge = true, n9 = s;
        goto l4a9f;
      }
    l4750:
      n9 = 0;
    j478d:
      if (!bridgeExit && checkTram(c, rx + n9, rz + n8)) goto l47c0;
      viaBridge = bridgeExit;
      if (n9 < 1) goto l4967;
    l4a9f:
      ex = std::max(rx + n9, ex);
      goto l4ez;
    l4967:
      ex = std::min(rx + n9, ex);
    l4ez:
      nEz = rz + n8, dx = n9, dz = n8;
      if (dz < 1) nEz = std::min(nEz, ez);
      else nEz = std::max(nEz, ez);
    l47c0:
      ez = nEz;
      if (rx != cx) goto l47cd;
    l4890:
      if (rz != cz) goto l47cd;
      if (R.flags() & kFlagDisableTram) error = true;
    }
  place: {
    // the step toward the end: along the longer axis, diagonal while both are about equal
    int a0 = cx - ex;
    bool b15 = sgn(a0) == sgn(dx), b1 = a0 >= 0;
    int i9 = a0;
    if (b15) dx = 0, i9 = 0, ex = cx;
    int a1 = cz - ez;
    bool b16 = sgn(a1) == sgn(dz), b2 = a1 >= 0;
    int i11 = a1;
    if (b16) ez = cz, dz = 0, i11 = 0;
    int lim = road >= 0 && R.type() >= 5 ? 4 : 0;
    int A = (b15 || b1) ? i9 : -i9, B = (b16 || b2) ? i11 : -i11;
    if (lim < std::abs(A - B)) {
      if (dx == 0 && dz == 0) {
        if (A < B) dz = sgn(ez - cz) * 4;
        else dx = sgn(ex - cx) * 4;
      }
    } else {
      dx = sgn(ex - cx) * 4;
      dz = sgn(ez - cz) * 4;
    }
    if (!b15 && !b1) i9 = -i9;
    if (!b16 && !b2) i11 = -i11;
    uint32_t m16 = 0, m8 = 0, m0 = 0;
    if (lim < i11 + i9) {
      uint32_t f = dirBit(dz, dx);
      if (f == 0) dz = 0;
      m0 = f, m8 = f << 8, m16 = f << 16 | f << 8;
    } else {
      dx = 0, dz = 0, ez = cz, ex = cx;
    }
    uint32_t cn;
    if (road < 0) cn = m0 | conn;
    else if (R.type() == 5) cn = (R.conn & m8) ? (conn | m8) : (m0 | conn);
    else if (R.type() == 6 && (m16 & R.conn)) cn = conn | (m16 & R.conn);
    else cn = m0 | conn;
    createTramBlock(c, cx, cz, cn, cx == sx && cz == sz, cx == ex && cz == ez, road);
    skip = false;
  }
  next:
    prev = road;
    if (road >= 0) P = R;
    if (cx == ex && cz == ez) break;
  }

  checkRoadSlopes(c, 0x100);
  materialize(c);
  if (c.W.size() != c.t.newItems.size()) error = true;  // a piece without any template
  for (size_t k = 0; k < t.newItems.size(); ++k) {
    uint32_t i = t.newItems[k];
    if (error) markError(c, i);
    if (gridCheckConflict(vm, grid, i, t.replaced)) error = true;
    if (k > 0 && gridItemsConflict(vm, grid, t.newItems[k - 1], i)) error = true;
  }
  for (uint32_t i : t.newItems) gridSelectObjectArea(vm, grid, i, error);
  if (debugTiming) logPieces(c);
  t.cellX = bx, t.cellY = hint, t.cellZ = bz;
  gridEditEnd(vm, grid);
  if (debugTiming)
    LOG_INFO("lay tram: %zu pieces, %zu replaced, %.2f ms", t.newItems.size(), t.replaced.size(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  vm.fireEvent(player, u"onHoverConstruct", {});
}

// ---------------------------------------------------------------------------------------------------------------
// metro tracks (GRIDMODE_LAY_TRACK)

// createTrackBlock (L:0x08195400): the track piece of block (x, z) with connections `conn` (byte 3) at height y; at
// the ends of a drag it joins the tracks of the same kind around (ground and elevated join each other), merging with
// a track already on the block. Returns the start connection when a drag starting on a track had to turn.
uint32_t createTrackBlock(Ctx& c, int x, int z, uint32_t conn, bool a, bool b, int T, int y) {
  size_t mark = c.W.size();
  if (T == 0) T = c.toolMode == 7 ? 0xd : c.toolMode == 8 ? 0xe : c.toolMode == 9 ? 0xf : 9;
  uint32_t E = T == 0xe ? kFlagTunnel : T == 0xf ? kFlagBridge : 0;
  // at the map's edge the piece leads off the map (connections2, the SPECIAL0 templates)
  uint32_t c2 = (x == 0 ? 0x40u : 0) | (x == c.g.width - 4 ? 4u : 0) | (z == c.g.length - 4 ? 0x10u : 0) | (z == 0 ? 1u : 0);
  uint32_t flags = c2 ? (E | 0x40000) : E;
  // editor bridge and tunnel modes (0xb, 0xc) pick other flags; the game has no such tools
  std::optional<Piece> res;
  int resRepl = -1;  // the track the merged piece replaces (local_30)
  uint32_t b0 = conn, ret = 0, extra = 0;
  auto okType = [&](int ot) { return ot == T || ((ot == 0xd || ot == 0xf) && (T == 0xd || T == 0xf)); };
  auto okY = [&](const Piece& o) { return o.y == y || unsigned(T - 0xe) > 1; };
  if (a || b) {
    std::vector<Piece> list = c.getObjects(x - 5, z - 5, 14, 14);
    for (const Piece& o : list) {
      int ot = o.type();
      uint32_t of = o.flags();
      if (!okType(ot) || (of & kFlagStop) || !okY(o)) continue;
      if ((of & 0x4004) == 0x4000 && ot != 0xf) continue;
      if ((of & 0x8004) == 0x8000 && ot != 0xe) continue;
      auto [rx, rz] = roadXZ(o);
      uint32_t d;
      if (rx + 4 == x && rz == z) d = 0x04000000;
      else if (rx == x + 4 && rz == z) d = 0x40000000;
      else if (x != rx) d = 0;
      else if (rz + 4 == z) d = 0x10000000;
      else d = rz == z + 4 ? 0x01000000 : 0;
      uint32_t r4 = rot(d, 4);
      if (rx == x && rz == z && (y == kNoY || o.y == y) && !(of & 0xf0)) {  // a track on this block: merge
        if (b0 == conn && a && !b) {
          int k = 0;
          if (b0 == rot(o.conn, 2) || b0 == rot(o.conn, 1)) k = 3;
          else if (b0 == rot(o.conn, 6) || b0 == rot(o.conn, 7)) k = 5;
          if (k) b0 = rot(o.conn, k), ret = b0;
        }
        uint32_t f = of;
        if (!(f & 0xc000)) f &= ~0xf0u;
        Piece p = c.createRoadItem(rx, o.y, rz, b0 | o.conn, o.conn2, ot, o.tvar(), f);
        if (!(p.flags() & kItemNegative)) {
          p.iflags |= kFlagJoin;
          flags |= kFlagJoin;
          c.t.replaced.insert(uint32_t(o.live));
          if (resRepl >= 0) c.t.replaced.erase(uint32_t(resRepl));
          resRepl = o.live;
          b0 = p.conn;
          res = p;
        }
        continue;
      }
      uint32_t oc = o.conn;
      if (d != (d & oc) && (b0 & r4) != r4 && ((rot(d, 6) | rot(d, 2)) & oc) == 0 &&
          (b0 & (rot(r4, 6) | rot(r4, 2))) == 0 &&
          (((rot(d, 7) | rot(d, 1)) & oc) == 0 || ((rot(d, 5) | rot(d, 3) | r4) & oc) != 0) &&
          ((b0 & (rot(r4, 7) | rot(r4, 1))) == 0 || (b0 & (rot(r4, 5) | rot(r4, 3) | r4)) != 0)) {
        uint32_t f = of;
        if (!(f & 0xc000)) f &= ~0xf0u;
        Piece q = c.createRoadItem(rx, o.y, rz, d | oc, o.conn2, ot, o.tvar(), f);
        int yy = y;
        if (res) yy = res->y, res.reset();
        uint32_t nb = r4 | b0;
        res = c.createRoadItem(x, yy, z, nb, c2, T, c.toolType, flags);
        if (!(q.flags() & kItemNegative) && !(res->flags() & kItemNegative)) {
          q.iflags |= kFlagJoin;
          c.W.push_back(q);
          c.t.replaced.insert(uint32_t(o.live));
          if (rot(d, 1) & o.conn) extra |= rot(d, 3) | rot(d, 1);
          if (rot(d, 7) & o.conn) extra |= rot(d, 5) | rot(d, 7);
          b0 = nb;
        } else {
          if (resRepl >= 0) c.t.replaced.erase(uint32_t(resRepl)), resRepl = -1, b0 = conn;
          res.reset();
        }
      }
    }
    for (const Piece& o : list) {  // diagonal neighbours
      int ot = o.type();
      uint32_t of = o.flags();
      if (!okType(ot) || !okY(o)) continue;
      if ((of & 0x4004) == 0x4000 && ot != 0xf) continue;
      if ((of & 0x8004) == 0x8000 && ot != 0xe) continue;
      auto [rx, rz] = roadXZ(o);
      uint32_t d = 0;
      if (rx + 4 == x) d = rz + 4 == z ? 0x08000000u : rz == z + 4 ? 0x02000000u : 0;
      else if (rx == x + 4) d = rz + 4 == z ? 0x20000000u : rz == z + 4 ? 0x80000000u : 0;
      uint32_t r4 = rot(d, 4), oc = o.conn;
      if (d != (d & oc) && ((b0 | extra) & r4) != r4 && ((rot(d, 6) | rot(d, 2)) & oc) == 0 &&
          (b0 & (rot(r4, 6) | rot(r4, 2))) == 0 &&
          (((rot(d, 7) | rot(d, 1)) & oc) == 0 || ((rot(d, 5) | rot(d, 3) | r4) & oc) != 0) &&
          ((b0 & (rot(r4, 7) | rot(r4, 1))) == 0 || (b0 & (rot(r4, 5) | rot(r4, 3) | r4)) != 0)) {
        uint32_t f = of;
        if (!(f & 0xc000)) f &= ~0xf0u;
        Piece q = c.createRoadItem(rx, o.y, rz, d | oc, o.conn2, ot, o.tvar(), f);
        int yy = y;
        if (res) yy = res->y, res.reset();
        res = c.createRoadItem(x, yy, z, r4 | b0, c2, T, c.toolType, flags);
        if (!(q.flags() & kItemNegative) && !(res->flags() & kItemNegative)) {
          q.iflags |= kFlagJoin;
          c.W.push_back(q);
          c.t.replaced.insert(uint32_t(o.live));
          b0 |= r4;
        } else {
          if (resRepl >= 0) c.t.replaced.erase(uint32_t(resRepl)), resRepl = -1, b0 = conn;
          res.reset();
        }
      }
    }
    if (a && b) mark = (c.W.size() + mark) >> 1;
  }
  // double track (9) over a small road becomes its crossing piece: editor maps only, not ported
  if (!res) res = c.createRoadItem(x, y, z, b0, c2, T, c.toolType, flags);
  if (b) c.W.insert(c.W.begin() + std::ptrdiff_t(std::min(mark, c.W.size())), *res);
  else c.W.push_back(*res);
  return ret;
}

void createRoadBlock(Ctx& c, int x, int z, uint32_t conn, bool a, bool b, int variant, int y);

// placeObject (L:0x08196b00): a template's onPlace(parts, x, y, z, orient) lists its parts, five values each: a
// template name or a road/track type (| variation << 16), x, y (cells), z and the orientation (connections for
// blocks), in the item's frame. Named parts become TEMP children (0x2000000) of the item; blocks become road or
// track blocks joined to what is around them.
void placeParts(Ctx& c, size_t k) {
  if (k >= c.W.size() || !c.W[k].t || !c.W[k].t->obj) return;
  Piece it = c.W[k];
  Object* tpl = it.t->obj;
  if (!c.vm.hasHandler(tpl, u"onPlace")) return;
  Ref<ArrayObj> arr = make<ArrayObj>();
  try {
    c.vm.fireEvent(tpl, u"onPlace",
                   {Value::heap(VT::Array, arr.get()), Value::integer(it.x), Value::real(float(it.y) * (1.0f / 256.0f)),
                    Value::integer(it.z), Value::integer(it.orient)});
  } catch (const std::exception& e) {
    LOG_ERROR("onPlace: %s", e.what());
    return;
  }
  int savedMode = c.toolMode, savedType = c.toolType;
  size_t n = arr->items.size() / 5;
  for (size_t j = 0; j < n; ++j) {
    auto val = [&](size_t q) { return c.vm.resolve(arr->items[j * 5 + q]); };
    Value v0 = val(0);
    int v1 = c.vm.toInt(val(1)), v3 = c.vm.toInt(val(3)), v4 = c.vm.toInt(val(4));
    int dy = int(std::lround(c.vm.toFloat(val(2)) * 256.0f));
    if (v0.type() != VT::Int) {
      const TInfo* pt = templateInfo(c.vm, c.grid, c.cat, utf16ToUtf8(c.vm.toStr(v0)));
      if (!pt) continue;
      int xo = 0, zo = 0;
      switch (it.orient) {
        case 1: xo = v3, zo = it.sz - pt->width - v1; break;
        case 0: xo = v1, zo = v3; break;
        case 2: xo = it.sx - pt->width - v1, zo = it.sz - pt->length - v3; break;
        case 3: xo = it.sx - pt->length - v3, zo = v1; break;
      }
      Piece p = c.piece(pt, it.x + xo, it.y + dy, it.z + zo, (it.orient + v4) & 3, kItemAutoChild);
      if (p.x + p.sx / 2 < 0 || p.z + p.sz / 2 < 0 || p.x + p.sx / 2 >= c.g.width || p.z + p.sz / 2 >= c.g.length)
        p.iflags |= kItemNegative;  // gsGrid::addObject off the map
      if (it.live >= 0) p.parentLive = it.live;
      else p.parentPiece = int(k);
      c.W.push_back(p);
      continue;
    }
    uint32_t v = uint32_t(v0.i());
    int type = int(v & 0xffff);
    c.toolType = int(v >> 16);
    int xo = 0, zo = 0;
    switch (it.orient) {
      case 1: xo = v3, zo = it.sz - 4 - v1; break;
      case 0: xo = v1, zo = v3; break;
      case 2: xo = it.sx - 4 - v1, zo = it.sz - 4 - v3; break;
      case 3: xo = it.sx - 4 - v3, zo = v1; break;
    }
    uint32_t pc = rot(uint32_t(v4), it.orient * 2);
    if (type == 9 || type == 8 || (type >= 0xd && type <= 0xf)) {
      if (type == 9) c.toolMode = 6;
      else if (type >= 0xd) c.toolMode = type - 6;
      createTrackBlock(c, it.x + xo, it.z + zo, pc, true, true, type, it.y + dy);
    } else if (type >= 2 && type <= 7) {
      if (type == 2) c.toolMode = 0;
      else if (type >= 5) c.toolMode = type - 2;
      createRoadBlock(c, it.x + xo, it.z + zo, pc, true, true, -1, it.y + dy);
    }
  }
  c.toolMode = savedMode, c.toolType = savedType;
}

// createRoadBlock (L:0x08190700; editor only): the road piece of block (x, z) with connections `conn` in the byte of
// its road kind, joined at the ends of a drag to the roads of the same type around it, as createTrackBlock joins
// tracks (the original's own function differs in the map-edge and ramp pieces, which this one takes from the same
// template search). Road types by toolMode (GRIDROAD_*): 0 small (2), 1 ground ramp (3), 2 elevated ramp (4, BRIDGE),
// 3 large (5), 4 avenue (6), 5 express (7); alleys (GRIDMODE_LAY_ALLEY) are type 1 on 1-cell blocks. The toolType is
// the template variation (1: the crossing pieces), toolSize the texture variation of the items.
int roadType(const Ctx& c) {
  static const int kRoadType[] = {2, 3, 4, 5, 6, 7};
  if (c.editMode == kGridModeLayAlley) return 1;
  return c.toolMode >= 0 && c.toolMode < 6 ? kRoadType[c.toolMode] : 2;
}
int roadShift(int type) { return type == 5 ? 8 : type == 6 ? 16 : type == 7 ? 24 : 0; }

void createRoadBlock(Ctx& c, int x, int z, uint32_t conn, bool a, bool b, int variant, int y) {
  (void)variant;
  const int T = roadType(c), S = roadShift(T), B = T == 1 ? 1 : 4;
  size_t mark = c.W.size();
  uint32_t E = T == 4 ? kFlagBridge : 0;
  uint32_t c2 = 0;
  if (T != 1)  // at the map's edge the piece leads off the map (connections2, the SPECIAL templates)
    c2 = (x == 0 ? 0x40u : 0) | (x == c.g.width - 4 ? 4u : 0) | (z == c.g.length - 4 ? 0x10u : 0) | (z == 0 ? 1u : 0);
  uint32_t flags = c2 ? (E | 0x40000) : E;
  const int texture = c.toolSize;  // selectTool(..., texture): the item variation
  auto make = [&](int px, int py, int pz, uint32_t pc, uint32_t pc2, int type, int var, uint32_t f) {
    Piece p = c.createRoadItem(px, py, pz, pc, pc2, type, var, f);
    return p;
  };
  std::optional<Piece> res;
  int resRepl = -1;
  uint32_t b0 = conn, extra = 0;
  auto bit = [&](uint32_t v) { return v << S; };
  {  // a road on this block merges anywhere along the drag (junctions); the neighbours join at the ends
    std::vector<Piece> list = c.getObjects(x - B - 1, z - B - 1, 3 * B + 2, 3 * B + 2);
    for (const Piece& o : list) {
      uint32_t of = o.flags();
      if (o.type() != T || (of & kFlagStop)) continue;
      if (y != kNoY && o.y != y && T == 4) continue;
      auto [rx, rz] = roadXZ(o);
      uint32_t d;
      if (rx + B == x && rz == z) d = bit(0x04);
      else if (rx == x + B && rz == z) d = bit(0x40);
      else if (x != rx) d = 0;
      else if (rz + B == z) d = bit(0x10);
      else d = rz == z + B ? bit(0x01) : 0;
      uint32_t r4 = rot(d, 4);
      if (rx == x && rz == z && !(of & 0xf0)) {  // a road on this block: the new piece merges with it
        uint32_t f = of;
        if (!(f & 0xc000)) f &= ~0xf0u;
        Piece p = make(rx, o.y, rz, b0 | o.conn, o.conn2, T, o.tvar(), f);
        if (!(p.flags() & kItemNegative)) {
          p.iflags |= kFlagJoin;
          p.variation = o.variation;
          flags |= kFlagJoin;
          c.t.replaced.insert(uint32_t(o.live));
          if (resRepl >= 0) c.t.replaced.erase(uint32_t(resRepl));
          resRepl = o.live;
          b0 = p.conn;
          res = p;
        }
        continue;
      }
      if (!(a || b)) continue;
      uint32_t oc = o.conn;
      if (d && d != (d & oc) && (b0 & r4) != r4 && ((rot(d, 6) | rot(d, 2)) & oc) == 0 &&
          (b0 & (rot(r4, 6) | rot(r4, 2))) == 0 &&
          (((rot(d, 7) | rot(d, 1)) & oc) == 0 || ((rot(d, 5) | rot(d, 3) | r4) & oc) != 0) &&
          ((b0 & (rot(r4, 7) | rot(r4, 1))) == 0 || (b0 & (rot(r4, 5) | rot(r4, 3) | r4)) != 0)) {
        uint32_t f = of;
        if (!(f & 0xc000)) f &= ~0xf0u;
        Piece q = make(rx, o.y, rz, d | oc, o.conn2, T, o.tvar(), f);
        q.variation = o.variation;
        int yy = y;
        if (res) yy = res->y, res.reset();
        uint32_t nb = r4 | b0;
        res = make(x, yy, z, nb, c2, T, c.toolType, flags);
        if (!(q.flags() & kItemNegative) && !(res->flags() & kItemNegative)) {
          q.iflags |= kFlagJoin;
          c.W.push_back(q);
          c.t.replaced.insert(uint32_t(o.live));
          if (rot(d, 1) & o.conn) extra |= rot(d, 3) | rot(d, 1);
          if (rot(d, 7) & o.conn) extra |= rot(d, 5) | rot(d, 7);
          b0 = nb;
        } else {
          if (resRepl >= 0) c.t.replaced.erase(uint32_t(resRepl)), resRepl = -1, b0 = conn;
          res.reset();
        }
      }
    }
    for (const Piece& o : list) {  // diagonal neighbours
      uint32_t of = o.flags();
      if (!(a || b) || o.type() != T || (of & kFlagStop)) continue;
      auto [rx, rz] = roadXZ(o);
      uint32_t d = 0;
      if (rx + B == x) d = rz + B == z ? bit(0x08) : rz == z + B ? bit(0x02) : 0;
      else if (rx == x + B) d = rz + B == z ? bit(0x20) : rz == z + B ? bit(0x80) : 0;
      uint32_t r4 = rot(d, 4), oc = o.conn;
      if (d && d != (d & oc) && ((b0 | extra) & r4) != r4 && ((rot(d, 6) | rot(d, 2)) & oc) == 0 &&
          (b0 & (rot(r4, 6) | rot(r4, 2))) == 0 &&
          (((rot(d, 7) | rot(d, 1)) & oc) == 0 || ((rot(d, 5) | rot(d, 3) | r4) & oc) != 0) &&
          ((b0 & (rot(r4, 7) | rot(r4, 1))) == 0 || (b0 & (rot(r4, 5) | rot(r4, 3) | r4)) != 0)) {
        uint32_t f = of;
        if (!(f & 0xc000)) f &= ~0xf0u;
        Piece q = make(rx, o.y, rz, d | oc, o.conn2, T, o.tvar(), f);
        q.variation = o.variation;
        int yy = y;
        if (res) yy = res->y, res.reset();
        res = make(x, yy, z, r4 | b0, c2, T, c.toolType, flags);
        if (!(q.flags() & kItemNegative) && !(res->flags() & kItemNegative)) {
          q.iflags |= kFlagJoin;
          c.W.push_back(q);
          c.t.replaced.insert(uint32_t(o.live));
          b0 |= r4;
        } else {
          if (resRepl >= 0) c.t.replaced.erase(uint32_t(resRepl)), resRepl = -1, b0 = conn;
          res.reset();
        }
      }
    }
    if (a && b) mark = (c.W.size() + mark) >> 1;
  }
  if (!res) {
    res = make(x, y, z, b0, c2, T, c.toolType, flags);
    res->variation = uint32_t(std::max(0, texture));
  }
  if (b) c.W.insert(c.W.begin() + std::ptrdiff_t(std::min(mark, c.W.size())), *res);
  else c.W.push_back(*res);
}

// checkConflicts (L:0x0818d5c0): a conflicting optional piece (pillars) is hidden, any other one makes it red
bool checkConflicts(Ctx& c) {
  bool error = false;
  int last = -1;
  for (uint32_t i : c.t.newItems) {
    GridItemShape s = gridItemShape(c.vm, c.grid, i);
    if (!(s.flags & kItemOptional)) {
      bool a1 = gridCheckConflict(c.vm, c.grid, i, c.t.replaced);
      bool a2 = last >= 0 && gridItemsConflict(c.vm, c.grid, uint32_t(last), i);
      static const bool dbg = std::getenv("REGS_DEBUG_TOOLS") != nullptr;
      if (dbg && (a1 || a2)) LOG_INFO("conflict: %s (%d,%d) map %d, with previous %d", c.g.types[c.g.items[i].type].c_str(), c.g.items[i].x, c.g.items[i].z, a1, a2);
      if (a1 || a2) error = true;
      last = int(i);
    } else if (gridCheckConflict(c.vm, c.grid, i, c.t.replaced)) {
      c.g.items[i].flags |= kItemHidden;
    }
  }
  return error;
}

// mouseMoveLayRoad (L:0x081975b0) and mouseMoveLayAlley (L:0x0819eab0): the drag from the start block to the block under
// the cursor. Blocks are 4 cells (alleys 1); large roads, avenues and express roads take every other block (the
// blocks with (x + z) & 4 == 0, the cursor snapping to the nearer one) and go two blocks per straight step; avenues
// never go diagonal. The elevated ramp lies two cells over the ground (or over the item under the cursor). One
// createRoadBlock per block with the connections back and forward; then the slopes (0x100 per block), the parts and
// the conflicts.
void moveLayRoad(VM& vm, ControlObj* player, GridObj* grid, float mx, float my) {
  Ctx c(vm, player, grid);
  ToolState& t = c.t;
  ToolPick pick = toolPick(vm, player, mx, my, false);
  if (!pick.hit) return;
  const int T = roadType(c), S = roadShift(T), B = T == 1 ? 1 : 4;
  const bool wide = T >= 5, avenue = T == 6;
  int y = kNoY;
  if (T == 4) {  // elevated ramp
    y = pick.y;
    if (pick.item < 0) y += 0x200;
    else {
      Piece h = c.live(pick.item);
      if (unsigned(h.type() - 2) > 2) y = (std::max(y, h.y + h.height() + 0x100) + 0x40) & ~0x7f;
    }
  }
  int ex = pick.cellX & ~(B - 1), ez = pick.cellZ & ~(B - 1);
  if (wide && ((ex + ez) & 4)) {
    int fx = (pick.cellX & 3) * 2 - 3, fz = (pick.cellZ & 3) * 2 - 3;
    if (std::abs(fz) < std::abs(fx)) ex = (pick.cellX + 4 + (fx < 0 ? -8 : 0)) & ~3;
    else ez = (pick.cellZ + 4 + (fz < 0 ? -8 : 0)) & ~3;
  }
  if (ex == t.cellX && ez == t.cellZ && y == t.cellY) return;
  gridEditBegin(vm, grid);
  if (!t.inProgress) t.startX = ex, t.startZ = ez, t.startY = y;
  clearPreview(c);
  int cx = t.startX, cz = t.startZ, dz = 0, dx = 0;
  uint32_t back = 0;
  for (int guard = 0; guard < 4096; ++guard) {
    int ax = ex - cx, az = ez - cz;
    bool end = ax == 0 && az == 0;
    uint32_t fwd = 0;
    int step = B;
    if (!end) {
      if (!avenue && ax != 0 && az != 0 && std::abs(ax) == std::abs(az)) {
        dx = sgn(ax), dz = sgn(az);
      } else if (std::abs(ax) >= std::abs(az)) {
        dx = sgn(ax), dz = 0;
        if (wide) step = 8;
      } else {
        dz = sgn(az), dx = 0;
        if (wide) step = 8;
      }
      if (wide && dx != 0 && dz != 0) step = 4;
      if (std::abs(dx * step) > std::abs(ax) && dx) step = std::abs(ax);
      if (std::abs(dz * step) > std::abs(az) && dz) step = std::abs(az);
      fwd = dirBit(dz, dx);
    }
    bool isStart = cx == t.startX && cz == t.startZ;
    int by = T == 4 ? (isStart ? t.startY : (t.startY < y && !end) ? t.startY : y) : kNoY;
    createRoadBlock(c, cx, cz, (back | fwd) << S, isStart, end, 0, by);
    if (end) break;
    cx += dx * step;
    cz += dz * step;
    back = dirBit(-dz, -dx);
  }
  auto& W = c.W;
  if (T == 4) {  // the elevated ramp keeps over what is under it, rising and falling half a cell per block
    Piece* prevP = nullptr;
    for (Piece& p : W) {
      if (p.flags() & 6) {
        prevP = nullptr;
        continue;
      }
      for (int pz = 0; pz <= p.sz; ++pz)
        for (int px = 0; px <= p.sx; ++px) {
          int x2 = p.x + px, z2 = p.z + pz;
          if (x2 < 0 || z2 < 0 || x2 >= c.g.width || z2 >= c.g.length) continue;
          p.y = std::max(p.y, (int(c.g.height[size_t(z2) * size_t(c.g.width) + size_t(x2)]) + 0x240) & ~0x7f);
        }
      if (prevP) p.y = std::max(p.y, prevP->y - 0x80), prevP->y = std::max(prevP->y, p.y);
      prevP = &p;
    }
  }
  checkRoadSlopes(c, 0x100);
  for (size_t k = 0; k < W.size(); ++k) placeParts(c, k);
  materialize(c);
  bool error = checkConflicts(c);
  if (c.W.size() != t.newItems.size()) error = true;
  for (uint32_t i : t.newItems) gridSelectObjectArea(vm, grid, i, error);
  static const bool debugTools = std::getenv("REGS_DEBUG_TOOLS") != nullptr;
  if (debugTools) logPieces(c);
  t.cellX = ex, t.cellZ = ez, t.cellY = y;
  gridEditEnd(vm, grid);
  vm.fireEvent(player, u"onHoverConstruct", {});
}

// mouseMoveLayTrack (L:0x0819cc70): the drag from the start block to the block under the cursor, straight or 45°
// diagonal, turning by at most 45° per block; one createTrackBlock per block. Elevated tracks keep above what is
// under them; then the slopes, the pieces' parts (pillars) and the conflicts.
void moveLayTrack(VM& vm, ControlObj* player, GridObj* grid, float mx, float my) {
  static const bool debugTiming = std::getenv("REGS_DEBUG_TOOLS") != nullptr;
  auto t0 = std::chrono::steady_clock::now();
  Ctx c(vm, player, grid);
  ToolState& t = c.t;
  int planeY = c.toolMode == 8 ? c.toolSize << 9 : kNoY;
  ToolPick pick = toolPickLevel(vm, player, mx, my, planeY);
  if (!pick.hit) return;
  int y = pick.y;
  if (c.toolMode == 9) {  // elevated: two cells over the ground, above the item hit, or at an elevated track's height
    if (pick.item < 0) {
      y += 0x200;
    } else {
      Piece h = c.live(pick.item);
      if (h.type() != 0xd && h.type() != 0xf) {
        y = std::max(y, h.y + h.height() + 0x100);
        y = (y + 0x40) & ~0x7f;
      }
    }
  } else if (c.toolMode != 8) {
    y = kNoY;
  }
  int ex = pick.cellX & ~3, ez = pick.cellZ & ~3;
  if (ex == t.cellX && ez == t.cellZ && y == t.cellY) return;
  gridEditBegin(vm, grid);
  if (!t.inProgress) t.startX = ex, t.startZ = ez, t.startY = y;
  clearPreview(c);

  int cx = t.startX, cz = t.startZ, dz = 0, dx = 0;
  bool done = false;
  for (int guard = 0; guard < 2048; ++guard) {
    uint32_t back = dirBit(-dz, -dx);
    cx += dx * 4;
    cz += dz * 4;
    if (dx != 0 && sgn(ex - cx) != (dx != -1 ? 1 : -1)) dx = 0;
    if (dz != 0 && sgn(ez - cz) != (dz != -1 ? 1 : -1)) dz = 0;
    int ax = cx - ex, az = cz - ez;
    uint32_t fwd = 0;
    // the step toward the end: diagonal while |Δx| = |Δz|, else along an axis
    {
      bool xMove, zMove;
      if (std::abs(std::abs(ax) - std::abs(az)) == 0) {
        dx = sgn(ex - cx), xMove = ex != cx;
        dz = sgn(ez - cz), zMove = ez != cz;
      } else if (dx == 0 || dz == 0) {
        if (std::abs(ax) < std::abs(az)) {
          dz = sgn(ez - cz), zMove = ez != cz;
          xMove = dx != 0;
        } else {
          dx = sgn(ex - cx), xMove = ex != cx;
          zMove = dz != 0;
        }
      } else {
        xMove = zMove = true;
      }
      if (xMove && zMove) fwd = dirBit(dz, dx);
      else if (dz == -1) fwd = 1;
      else if (dx == 1) fwd = 4;
      else if (zMove) fwd = 0x10;
      else fwd = xMove ? 0x40 : 0;
      if (xMove && zMove) bitStep(fwd, dz, dx);
    }
    // no sharper turn than 45°
    if (rot(back, 2) == fwd || rot(back, 1) == fwd) fwd = rot(back, 3), bitStep(fwd, dz, dx);
    else if (rot(back, 6) == fwd || rot(back, 7) == fwd) fwd = rot(back, 5), bitStep(fwd, dz, dx);
    bool end;
    uint32_t mask = back;
    if (std::abs(ax) + std::abs(az) < 5 && std::abs(ax + dx) + std::abs(az + dz) >= 4) {
      end = true;
    } else if (cx == ex && cz == ez) {
      end = true;
    } else {
      mask = back | fwd;
      end = false;
    }
    done = end;
    bool isStart = cx == t.startX && cz == t.startZ;
    int by;
    if (c.toolMode == 8) by = isStart ? t.startY : done ? pick.y : c.toolSize << 9;
    else if (c.toolMode == 9) by = isStart ? t.startY : (t.startY < y && !done) ? t.startY : y;
    else by = kNoY;
    uint32_t r = createTrackBlock(c, cx, cz, mask << 24, isStart, end, 0, by) >> 24;
    bitStep(r, dz, dx);
    if (done || (cx == ex && cz == ez)) break;
  }

  // elevated and subway tracks: over the items under them (not replaced ones), two cells over the ground and the
  // water, rising and falling by at most half a cell per block
  auto& W = c.W;
  if (c.toolMode == 8 || c.toolMode == 9) {
    Piece* prevP = nullptr;
    for (Piece& p : W) {
      if (p.flags() & 6) {
        prevP = nullptr;
        continue;
      }
      if (p.flags() & kFlagBridge) {
        for (const Piece& u : c.getObjects(p.x, p.z, p.sx, p.sz)) {
          if (unsigned(u.type()) >= 0x15 || (u.flags() & kItemOptional)) continue;
          if (c.t.replaced.count(uint32_t(u.live))) continue;
          // gsGrid::getObjects(item): footprints and height ranges overlap
          if (!(u.y - u.t->negHeight * 0x100 < p.y + p.height() && p.y - p.t->negHeight * 0x100 < u.y + u.height()))
            continue;
          if (u.y - 0xff <= p.height() + p.y) p.y = (std::max(p.y, u.y + u.height() + 0x100) + 0x40) & ~0x7f;
        }
        for (int pz = 0; pz <= p.sz; ++pz)
          for (int px = 0; px <= p.sx; ++px) {
            int cx2 = p.x + px, cz2 = p.z + pz;
            if (cx2 < 0 || cz2 < 0 || cx2 >= c.g.width || cz2 >= c.g.length) continue;
            size_t cell = size_t(cz2) * size_t(c.g.width) + size_t(cx2);
            p.y = std::max(p.y, (int(c.g.height[cell]) + 0x240) & ~0x7f);
            if (c.g.flags[cell] & 0x2000) p.y = std::max(p.y, 0x200);
          }
      }
      if (prevP) {
        p.y = std::max(p.y, prevP->y - 0x80);
        prevP->y = std::max(prevP->y, p.y);
      }
      prevP = &p;
    }
    Piece* nextP = nullptr;
    for (auto it = W.rbegin(); it != W.rend(); ++it) {
      if (it->flags() & 6) {
        nextP = nullptr;
        continue;
      }
      if (nextP) it->y = std::max(it->y, nextP->y - 0x80);
      nextP = &*it;
    }
  }
  checkRoadSlopes(c, 0x80);
  for (size_t k = 0; k < W.size(); ++k) placeParts(c, k);
  materialize(c);
  bool error = checkConflicts(c);
  if (c.W.size() != t.newItems.size()) error = true;
  for (uint32_t i : t.newItems) gridSelectObjectArea(vm, grid, i, error);
  if (debugTiming) logPieces(c);
  t.cellX = ex, t.cellZ = ez, t.cellY = y;
  gridEditEnd(vm, grid);
  if (debugTiming)
    LOG_INFO("lay track: %zu pieces, %zu replaced, %.2f ms", t.newItems.size(), t.replaced.size(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  vm.fireEvent(player, u"onHoverConstruct", {});
}

// ---------------------------------------------------------------------------------------------------------------
// committing and cancelling (finishTool / cancelTool cases 5, 6, 0x11)

// softenArea (L:0x0818cfb0): the unlocked corners in a disc of radius r around (cx, cz) take the mean of themselves
// and their four neighbours, all read before any is written (the engine's 16 passes read the same copy: one pass)
void softenArea(GridObj* grid, int cx, int cz, int r) {
  GridData& g = *grid->data;
  int n = 2 * r + 4;
  std::vector<int> buf(size_t(n) * size_t(n), kNoY);
  auto at = [&](int dx, int dz) -> int& { return buf[size_t(dz + r + 1) * size_t(n) + size_t(dx + r + 1)]; };
  for (int dz = -r - 1; dz <= r + 2; ++dz)
    for (int dx = -r - 1; dx <= r + 2; ++dx) {
      int x = cx + dx, z = cz + dz;
      if (x >= 0 && z >= 0 && x < g.width && z < g.length) at(dx, dz) = g.height[size_t(z) * size_t(g.width) + size_t(x)];
    }
  for (int dz = -r; dz <= r + 1; ++dz)
    for (int dx = -r; dx <= r + 1; ++dx) {
      int x = cx + dx, z = cz + dz;
      if (x < 0 || z < 0 || x >= g.width || z >= g.length) continue;
      int ix = dx - (dx > 0), iz = dz - (dz > 0);
      size_t cell = size_t(z) * size_t(g.width) + size_t(x);
      if (ix * ix + iz * iz > r * r || (g.flags[cell] & 0x1000)) continue;
      int sum = 0, count = 0;
      for (int v : {at(dx, dz), at(dx, dz - 1), at(dx - 1, dz), at(dx, dz + 1), at(dx + 1, dz)})
        if (v != kNoY) sum += v, ++count;
      if (count) g.height[cell] = int16_t(sum / count);
    }
  gridEditCellsChanged(grid, cx - r, cz - r, cx + r + 1, cz + r + 1, true);
}

bool finishLaying(VM& vm, ControlObj* player, GridObj* grid) {
  if (!grid) return true;
  ToolState& t = toolState(player);
  if (!t.inProgress) {  // nothing started: only the highlight around the cursor goes
    gridEditBegin(vm, grid);
    gridUnselectArea(vm, grid, t.cellX - 4, t.cellZ - 4, 12, 12);
    gridEditEnd(vm, grid);
    return true;
  }
  // items on the ground soften the terrain around them, except while the markers show (the editor)
  Value markers = grid->fieldValues.count(u"showMarkers") ? grid->fieldValues[u"showMarkers"] : Value();
  bool editor = !markers.isNone() && vm.truthy(markers);
  return toolCommit(vm, player, grid, [&](uint32_t preview, uint32_t) {
    const GridData& g = *grid->data;
    if (editor || g.items[preview].parent) return;
    int x, z, w, l;
    gridItemRect(vm, grid, preview, x, z, w, l);
    softenArea(grid, x + (w >> 1), z + (l >> 1), 4);
  });
}

void cancelLaying(VM& vm, ControlObj* player, GridObj* grid) {
  if (!grid) {
    toolState(player).newItems.clear();
    toolState(player).replaced.clear();
    return;
  }
  Ctx c(vm, player, grid);
  gridEditBegin(vm, grid);
  clearPreview(c);
  gridEditEnd(vm, grid);
}

}  // namespace

void toolSoftenArea(GridObj* grid, int cx, int cz, int r) {
  if (grid && grid->data) softenArea(grid, cx, cz, r);
}

void registerTrackTools(VM& vm) {
  ToolMode tram;
  tram.move = moveLayTram;
  tram.finish = finishLaying;
  tram.cancel = cancelLaying;
  registerToolMode(kGridModeLayTramTrack, tram);
  ToolMode track;
  track.move = moveLayTrack;
  track.finish = finishLaying;
  track.cancel = cancelLaying;
  registerToolMode(kGridModeLayTrack, track);
  ToolMode road;  // the editor's roads and alleys
  road.move = moveLayRoad;
  road.finish = finishLaying;
  road.cancel = cancelLaying;
  registerToolMode(kGridModeLayRoad, road);
  registerToolMode(kGridModeLayAlley, road);
  // blocks added to another tool's preview (a station's track parts): the preview so far, then the block, joined to
  // the tracks around; toolType is the block's variation
  toolBlocks().track = [](VM& v, ControlObj* player, GridObj* grid, int x, int z, uint32_t conn, bool a, bool b,
                          int type, int y) {
    if (!grid || !grid->data) return;
    Ctx c(v, player, grid);
    gridEditBegin(v, grid);
    loadPreview(c);
    createTrackBlock(c, x, z, conn, a, b, type, y);
    materialize(c);
    gridEditEnd(v, grid);
  };
  toolBlocks().road = [](VM& v, ControlObj* player, GridObj* grid, int x, int z, uint32_t conn, bool a, bool b,
                         int variant, int y) {
    if (!grid || !grid->data) return;
    Ctx c(v, player, grid);
    gridEditBegin(v, grid);
    loadPreview(c);
    createRoadBlock(c, x, z, conn, a, b, variant, y);
    materialize(c);
    gridEditEnd(v, grid);
  };
  (void)vm;
}

// ---- port curved roads (engine/tool_curves.cpp): the small road's block ends ----

std::vector<RoadEnd> findSmallRoadEnds(VM& vm, ControlObj* player, GridObj* grid, float wx, float wz, float radius,
                                       int roadType) {
  std::vector<RoadEnd> out;
  if (!grid || !grid->data) return out;
  Ctx c(vm, player, grid);
  const GridData& g = c.g;
  const float cs = g.cellSize;
  int cx = int(std::floor(wx / cs + float(g.width) * 0.5f)), cz = int(std::floor(wz / cs + float(g.length) * 0.5f));
  int r = int(std::ceil(radius / cs)) + 8;
  for (const Piece& o : c.getObjects(cx - r, cz - r, 2 * r + 1, 2 * r + 1)) {
    // roads of the type, and with small roads tram tracks on the grass (a tram piece of no road)
    bool tram = roadType == 2 && o.type() == 10 && o.parentLive < 0;
    if ((o.type() != roadType && !tram) || (o.iflags & kItemTemp)) continue;
    uint32_t b = (o.conn >> roadShift(o.type())) & 0xff;
    auto [bx, bz] = roadXZ(o);
    const float B = roadType == 1 ? 1.0f : 4.0f;  // the block (cells), its centre the piece's
    const float half = roadType == 1 ? 1.5f : roadType == 5 || roadType == 6 ? 12.0f : 6.0f;  // half a piece
    render::Vec3 centre{(2.0f * float(bx) + B - float(g.width)) * cs * 0.5f,
                float(itemHeight(g, g.items[size_t(o.live)], o.t->flags, o.type())) * cs / 256.0f,
                (2.0f * float(bz) + B - float(g.length)) * cs * 0.5f};
    auto edge = [&](uint32_t bit, bool deadEnd) {  // the edge on the side of `bit`, the way out through it
      int dz = 0, dx = 0;
      bitStep(bit, dz, dx);
      RoadEnd e;
      e.item = uint32_t(o.live);
      e.dir = render::Vec3{float(dx), 0, float(dz)};
      e.point = centre + e.dir * half;
      e.deadEnd = deadEnd;
      e.tram = tram;
      float ex = e.point.x - wx, ez = e.point.z - wz;
      if (ex * ex + ez * ez <= radius * radius) out.push_back(e);
    };
    for (uint32_t bit : {1u, 4u, 0x10u, 0x40u}) {
      uint32_t opp = ((bit << 4) | (bit >> 4)) & 0xff;
      if (b == bit) edge(opp, true);                       // a dead end: the way out is its closed side
      else if (b == (bit | opp) && bit < opp) edge(bit, false), edge(opp, false);  // a straight piece: both edges
    }
  }
  std::sort(out.begin(), out.end(), [&](const RoadEnd& a, const RoadEnd& b) {
    float da = (a.point.x - wx) * (a.point.x - wx) + (a.point.z - wz) * (a.point.z - wz);
    float db = (b.point.x - wx) * (b.point.x - wx) + (b.point.z - wz) * (b.point.z - wz);
    return da < db;
  });
  return out;
}

int replaceRoadBlockEnd(VM& vm, ControlObj* player, GridObj* grid, const RoadEnd& end, bool open, Object* timeline) {
  if (!grid || !grid->data || end.item >= grid->data->items.size() || grid->data->items[end.item].removed) return -1;
  Ctx c(vm, player, grid);
  Piece o = c.live(int(end.item));
  if (!o.t) return -1;
  uint32_t out = dirBit(int(std::lround(end.dir.z)), int(std::lround(end.dir.x)));
  uint32_t back = ((out << 4) | (out >> 4)) & 0xff;
  uint32_t conn = (open ? (out | back) : back) << roadShift(o.type());  // a straight piece through the edge, or a dead end
                                                                         // open to the road (in its type's byte)
  auto [bx, bz] = roadXZ(o);
  Piece p = c.createRoadItem(bx, o.y, bz, conn, 0, o.type(), o.tvar(), 0);
  if (!p.t || (p.iflags & kItemNegative)) return -1;
  std::string name = size_t(end.item) < c.g.names.size() ? c.g.names[end.item] : std::string();
  uint32_t variation = o.variation;
  timelineRecordDelete(vm, timeline, grid, end.item);
  gridEditRemoveItem(vm, grid, end.item);
  int i = gridEditAddItem(vm, grid, p.t->name, p.x, p.y, p.z, p.orient, p.iflags & ~(kItemTemp | kItemNegative), -1, false);
  if (i < 0) return -1;
  c.g.items[size_t(i)].variation = variation;
  if (!name.empty()) {
    if (c.g.names.size() <= size_t(i)) c.g.names.resize(size_t(i) + 1);
    c.g.names[size_t(i)] = name;
  }
  timelineRecordCreate(vm, timeline, grid, uint32_t(i));
  return i;
}

int openTramBlockEnd(VM& vm, ControlObj* player, GridObj* grid, const RoadEnd& end, Object* timeline) {
  if (!grid || !grid->data || end.item >= grid->data->items.size()) return -1;
  Ctx c(vm, player, grid);
  uint32_t out = dirBit(int(std::lround(end.dir.z)), int(std::lround(end.dir.x)));
  uint32_t back = ((out << 4) | (out >> 4)) & 0xff;
  for (size_t i = 0; i < c.g.items.size(); ++i) {
    const GridData::Item& it = c.g.items[i];
    if (it.removed || it.parent != end.item + 1 || (it.flags & kItemTemp)) continue;
    Piece o = c.live(int(i));
    if (!o.t || o.type() < 10 || o.type() > 12) continue;  // the block's street tram piece (small, large, avenue)
    const int sh = o.type() == 11 ? 8 : o.type() == 12 ? 16 : 0;
    if (((o.conn >> sh) & 0xff) != back) return -1;  // not a dead end closed toward the curve
    auto [bx, bz] = roadXZ(o);
    Piece p = c.createRoadItem(bx, o.y, bz, (out | back) << sh, 0, o.type(), o.tvar(), 0);
    if (!p.t || (p.iflags & kItemNegative)) return -1;
    uint32_t variation = o.variation;
    timelineRecordDelete(vm, timeline, grid, uint32_t(i));
    gridEditRemoveItem(vm, grid, uint32_t(i));
    int n = gridEditAddItem(vm, grid, p.t->name, p.x, p.y, p.z, p.orient, p.iflags & ~(kItemTemp | kItemNegative),
                            int(end.item), false);
    if (n < 0) return -1;
    c.g.items[size_t(n)].variation = variation;
    timelineRecordCreate(vm, timeline, grid, uint32_t(n));
    return n;
  }
  return -1;
}

}  // namespace regs::engine
