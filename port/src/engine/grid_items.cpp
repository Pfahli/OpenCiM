// Grid items of a loaded map (roads, tracks, props, buildings, trees), docs/renderer/grid-items.md. Every item is
// merged into its division's mesh per LOD, grouped by material and tint: the engine's storeMesh path (§3.2). The
// engine draws non-storeMesh templates (buildings, trees, stations) as one instance each instead; placement, LOD bands
// and materials are the same (§2.2, §3.4), but shader effects that read model-space positions see division space
// here; for the trees.shader wind (§3.6), whose sway grows with the height squared, merged trees carry their base
// height in uv1.x and the shader subtracts it (shaders.cpp, portShaderPatch). Not yet: terrain normals on upward faces, the vertexGeneration triangle order,
// base walls, skeletal animation and the fade between 800 and 880 m.
#include <algorithm>
#include <cmath>
#include <map>

#include "core/log.h"
#include "core/text.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/grid_render.h"
#include "engine/scene3d.h"
#include "engine/skinning.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

// GRID_ITEM_* types and flags (L:0x08188450.c) and engine-only item bits (grid-items.md §1.4)
constexpr int kTypeMetroStation = 0x10, kTypeTree = 0x13, kTypeMarker = 0x15, kTypeSpawnPoint = 0x16;
constexpr uint32_t kFlagHidden = 0x400000, kFlagHideLod = 0x4000000, kFlagAlign4 = 0x8000000, kFlagAutoChild = 0x2000000;
constexpr uint32_t kMatKeepOnVariation = 0x08000000;

int fieldInt(VM& vm, const Object* o, const char16_t* name, int def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toInt(it->second);
}

float fieldFloat(VM& vm, const Object* o, const char16_t* name, float def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toFloat(it->second);
}

// gsPseudoRandom15 (L:0x08108fb0)
uint32_t pseudoRandom15(uint32_t seed) { return (seed * 0x687faU + 0x4d3d86U) >> 17; }

constexpr uint32_t kFlagEnableBase = 0x1, kFlagCover = 0x4, kFlagConcave = 0x400, kFlagBridge = 0x4000, kFlagTunnel = 0x8000;

// An item's footprint as fixGrid sees it: size after the orientation swap and the diagonals nibbles rotated with it
// (item ctor L:0x08187fc0.c:120-126: each 16-bit half rotated right by 4·orient).
struct Footprint {
  int sx, sz;
  uint32_t diag;
};

Footprint footprintOf(const GridTemplate& tp, uint32_t diagonals, int orient) {
  Footprint f;
  f.sx = (orient & 1) ? tp.length : tp.width;
  f.sz = (orient & 1) ? tp.width : tp.length;
  int r = 4 * (orient & 3);
  auto rotr16 = [r](uint32_t v) { return r ? ((v >> r) | (v << (16 - r))) & 0xFFFF : v & 0xFFFF; };
  f.diag = rotr16(diagonals & 0xFFFF) | rotr16(diagonals >> 16) << 16;
  return f;
}

// gsGridItem::pointCover (L:0x08182ff0): is footprint grid point (px, pz) inside the diagonal cuts
bool pointCover(const Footprint& f, int px, int pz) {
  uint32_t d = f.diag;
  int rx = f.sx - px, rz = f.sz - pz;
  int a = int(d & 0xF), b = int(d >> 4 & 0xF), c = int(d >> 8 & 0xF), e = int(d >> 12 & 0xF);
  int n4 = int(d >> 16 & 0xF), n5 = int(d >> 20 & 0xF), n6 = int(d >> 24 & 0xF), n7 = int(d >> 28);
  if (!(a <= rx + pz || a - n5 <= pz || a - n4 <= rx)) return false;
  if (!(b <= rx + rz || b - n5 <= rz || b - n6 <= rx)) return false;
  if (!(c <= px + rz || c - n7 <= rz || c - n6 <= px)) return false;
  if (px + pz < e && pz < e - n7) return e - n4 <= px;
  return true;
}

// gsGridItem::cellCover (L:0x08183120): the covered quadrants of footprint cell (x, z) of a diagonal item; 0 = the
// cell is not part of the item
uint32_t cellCover(const Footprint& f, uint32_t flags, int x, int z) {
  uint32_t d = f.diag;
  if (flags & kFlagConcave) {
    int ix = f.sx - 1 - x, iz = f.sz - 1 - z;
    if (std::max(z, ix) < int(d & 0xF) || std::max(ix, iz) < int(d >> 4 & 0xF) || std::max(iz, x) < int(d >> 8 & 0xF) ||
        std::max(z, x) < int(d >> 12 & 0xF))
      return 0;
    return 0xF00;
  }
  bool tl = pointCover(f, x, z), tr = pointCover(f, x + 1, z), br = pointCover(f, x + 1, z + 1), bl = pointCover(f, x, z + 1);
  uint32_t q = (tl && tr ? 0x100 : 0) | (tr && br ? 0x200 : 0) | (br && bl ? 0x400 : 0) | (bl && tl ? 0x800 : 0);
  if (tl && tr && br && bl) return 0xF00;
  // a lone quadrant only at the tip of a cut
  switch (q) {
    case 0x100: return x == int(d >> 8 & 0xF) - 1 && x == f.sx - int(d >> 4 & 0xF) ? q : 0;
    case 0x200: return z == int(d >> 12 & 0xF) - 1 && z == f.sz - int(d >> 8 & 0xF) ? q : 0;
    case 0x400: return x == int(d >> 12 & 0xF) - 1 && x == f.sx - int(d & 0xF) ? q : 0;
    case 0x800: return z == int(d & 0xF) - 1 && z == f.sz - int(d >> 4 & 0xF) ? q : 0;
    default: return q;
  }
}

// the packed up normal of a cover item, tilted by its SLOPE_* flag along the orientation (fixGrid :250-330)
uint32_t coverNormal(uint32_t flags, int orient, bool diagonal) {
  if (!(flags & 0xF0) || ((flags & 0xC000) && (flags & kFlagCover))) return 0x7FFE7F;
  float s = (flags & 0x10) ? 0.0625f : (flags & 0x20) ? 0.125f : (flags & 0x40) ? 0.25f : 0.5f;
  Vec3 n{0, 1, 0};
  if (!diagonal) {
    static const float kx[4] = {-1, 0, 1, 0}, kz[4] = {0, 1, 0, -1};
    n = {kx[orient & 3] * s, 1, kz[orient & 3] * s};
  } else {
    static const float kx[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, kz[4] = {0.5f, 0.5f, -0.5f, -0.5f};
    n = {kx[orient & 3] * s, 1, kz[orient & 3] * s};
  }
  n = normalize(n);
  auto b = [](float v) { return uint32_t(std::lrint((v + 1.0f) * 127.0f)); };
  return b(n.x) << 16 | b(n.y) << 8 | b(n.z);
}

struct Group {
  std::vector<float> positions, normals, uv0, uv1, tangents;
  std::vector<uint32_t> colors, indices;
  uint32_t vertexCount() const { return uint32_t(positions.size() / 3); }
};

}  // namespace

ItemFootprint itemFootprint(int width, int length, uint32_t diagonals, int orient) {
  GridTemplate tp;
  tp.width = width;
  tp.length = length;
  Footprint f = footprintOf(tp, diagonals, orient);
  return {f.sx, f.sz, f.diag};
}

uint32_t itemCellCover(const ItemFootprint& f, uint32_t flags, int x, int z) {
  return f.diag ? cellCover(Footprint{f.sx, f.sz, f.diag}, flags, x, z) : 0xF00;
}

namespace {

// the grid templates of the map's types not registered yet: sizes, flags, materials, texture variations, tints
void registerTypes(Display& d, VM& vm, GridObj* grid, TerrainCache& tc) {
  const GridData& g = *grid->data;
  auto read = [&](const std::string& p, std::vector<uint8_t>& out) { return vm.vfs().read(p, out); };
  for (size_t t = tc.typeTemplate.size(); t < g.types.size(); ++t) {
    tc.typeTemplate.push_back(-1);
    auto it = grid->templates.find(g.types[t]);
    if (it == grid->templates.end()) continue;
    auto* o = dynamic_cast<ObjectObj*>(it->second.get());
    if (!o || !o->data) continue;
    GridTemplate tp;
    tp.object = o;
    tp.width = std::max(1, fieldInt(vm, o, u"width", 1));
    tp.length = std::max(1, fieldInt(vm, o, u"length", 1));
    tp.type = fieldInt(vm, o, u"type", 0);
    tp.flags = uint32_t(fieldInt(vm, o, u"flags", 0));
    tp.storeMesh = fieldInt(vm, o, u"storeMesh", 0) != 0;
    tp.vertexGeneration = fieldInt(vm, o, u"vertexGeneration", 0) != 0;
    tp.bake = o->loadYaw != 0;
    tp.bakeScaleX = o->loadScale[0];
    tp.bakeScaleZ = o->loadScale[2];
    tp.skinned = isSkinnedModel(vm, o);
    tp.animStart = fieldFloat(vm, o, u"doorOpenStart", 0);
    tp.animLength = fieldFloat(vm, o, u"doorOpenLen", 0);
    tp.materialBase = int(tc.materials.size());
    for (const MaterialDef& m : o->data->materials) tc.materials.push_back(objectMaterial(d, vm, o, m));
    for (int n = 1; n <= 4; ++n) {
      auto v = o->fieldValues.find(u"<variation" + utf8ToUtf16(std::to_string(n)) + u">");
      Object* mo = v == o->fieldValues.end() ? nullptr : v->second.object();
      if (!mo) continue;
      auto f = mo->fieldValues.find(u"<materialFile>");
      if (f == mo->fieldValues.end()) continue;
      std::string path = utf16ToUtf8(f->second.toString());
      auto known = tc.fileMaterials.find(path);  // texture-variation .material files are shared by many templates
      if (known == tc.fileMaterials.end()) {
        MaterialDef def;
        int index = -1;
        if (loadMaterialFile(path, read, def)) {
          index = int(tc.materials.size());
          tc.materials.push_back(objectMaterial(d, vm, nullptr, def));
        } else {
          LOG_WARN("texture variation %s: not found", path.c_str());
        }
        known = tc.fileMaterials.emplace(path, index).first;
      }
      tp.variation[n - 1] = known->second;
    }
    if (auto c = o->fieldValues.find(u"colorMap"); c != o->fieldValues.end())
      if (auto* cm = dynamic_cast<ColormapObj*>(c->second.object()); cm && !cm->argb.empty()) tp.colorMap = cm;
    tc.typeTemplate[t] = int(tc.templates.size());
    tc.templates.push_back(tp);
  }
}

// the division an item is drawn in: by footprint centre (L:0x08146470.c:190-196); hidden, marker, removed and
// out-of-map items are not drawn by the divisions (−1)
int bucketOf(const GridData& g, const TerrainCache& tc, uint32_t i) {
  const GridData::Item& it = g.items[i];
  int ti = it.type < tc.typeTemplate.size() ? tc.typeTemplate[it.type] : -1;
  if (ti < 0 || it.removed) return -1;
  const GridTemplate& tp = tc.templates[size_t(ti)];
  uint32_t flags = it.flags | tp.flags;
  if ((flags & kFlagHidden) || tp.type == kTypeMarker || tp.type == kTypeSpawnPoint) return -1;
  return itemDivision(g, tc, i);
}

// buckets the items from tc.itemsBucketed on; returns how many were placed and, for live edits, marks their divisions
size_t bucketItems(const GridData& g, TerrainCache& tc, bool markDirty) {
  int heightShift = 0x200 - int(g.heightBase);
  size_t from = tc.itemsBucketed, placed = 0;
  tc.itemColor.resize(g.items.size(), 0);
  for (size_t i = from; i < g.items.size(); ++i) {
    int division = bucketOf(g, tc, uint32_t(i));
    if (division < 0) continue;
    const GridData::Item& it = g.items[i];
    const GridTemplate& tp = tc.templates[size_t(tc.typeTemplate[it.type])];
    tc.divisionItems[size_t(division)].push_back(uint32_t(i));
    if (markDirty) tc.dirtyItems.insert(division);
    ++placed;
    if (tp.colorMap) {  // seed 101·z + 13·x + y picks a pixel of the colour map (§2.5)
      uint32_t seed = uint32_t(101 * it.z + 13 * it.x + (it.y + heightShift));
      tc.itemColor[i] = tp.colorMap->argb[pseudoRandom15(seed) % tp.colorMap->argb.size()];
    }
  }
  for (size_t i = from; i < g.items.size(); ++i) {  // auto-generated children take the parent's colour
    const GridData::Item& it = g.items[i];
    int ti = it.type < tc.typeTemplate.size() ? tc.typeTemplate[it.type] : -1;
    if (ti < 0 || tc.itemColor[i] || !it.parent || it.parent > g.items.size()) continue;
    if ((it.flags | tc.templates[size_t(ti)].flags) & kFlagAutoChild) tc.itemColor[i] = tc.itemColor[it.parent - 1];
  }
  tc.itemsBucketed = g.items.size();
  return placed;
}

}  // namespace

void initGridItems(Display& d, VM& vm, GridObj* grid, TerrainCache& tc) {
  const GridData& g = *grid->data;
  tc.typeTemplate.clear();
  tc.fileMaterials.clear();
  registerTypes(d, vm, grid, tc);
  tc.divisionItems.assign(size_t(tc.cols) * size_t(tc.rows), {});
  tc.itemColor.clear();
  tc.itemsBucketed = 0;
  size_t placed = bucketItems(g, tc, false);
  LOG_INFO("grid items: %zu of %zu placed, %zu templates, %zu materials", placed, g.items.size(), tc.templates.size(),
           tc.materials.size());
}

void syncGridItems(Display& d, VM& vm, GridObj* grid, TerrainCache& tc) {
  if (tc.typeTemplate.size() < grid->data->types.size()) registerTypes(d, vm, grid, tc);
  if (tc.itemsBucketed < grid->data->items.size()) bucketItems(*grid->data, tc, true);
}

int itemDivision(const GridData& g, const TerrainCache& tc, uint32_t item) {
  if (item >= g.items.size() || g.items[item].type >= tc.typeTemplate.size()) return -1;
  const GridData::Item& it = g.items[item];
  int ti = tc.typeTemplate[it.type];
  if (ti < 0) return -1;
  const GridTemplate& tp = tc.templates[size_t(ti)];
  int sx = (it.orient & 1) ? tp.length : tp.width, sz = (it.orient & 1) ? tp.width : tp.length;
  if (it.x < 0 || it.z < 0 || it.x + sx > g.width || it.z + sz > g.length) return -1;
  int shift = int(std::min(g.bucketShift, 8u));
  return std::min((it.z + sz / 2) >> shift, tc.rows - 1) * tc.cols + std::min((it.x + sx / 2) >> shift, tc.cols - 1);
}

void fixGridCover(VM& vm, GridData& g, const TerrainCache& tc) {
  // gsGridItem::fixGrid (L:0x08184890.c:330-420) for every item, in file order, normal view (viewMode 0xfe has 0x20):
  // COVER items set the covered quadrants of their footprint cells, which the terrain builder then leaves out, and
  // cover/base/road-type items without -y.bmp data write a packed normal into the cells' override map
  g.normals.assign(g.flags.size(), 0);
  std::vector<int8_t> hasYData(tc.templates.size(), -1);
  size_t covered = 0;
  for (const GridData::Item& it : g.items) {
    int ti = it.type < tc.typeTemplate.size() ? tc.typeTemplate[it.type] : -1;
    if (ti < 0 || it.removed) continue;
    const GridTemplate& tp = tc.templates[size_t(ti)];
    uint32_t flags = it.flags | tp.flags, cc = flags;
    if ((flags & 0x500000) || tp.type == kTypeMarker) continue;  // gsGridItem::fixGrid() L:0x08185a40: not temporary or hidden
    if (it.parent && it.parent <= g.items.size() && tp.type >= 10 && tp.type <= 12) {  // tram tracks follow the road
      const GridData::Item& p = g.items[it.parent - 1];
      int pti = p.type < tc.typeTemplate.size() ? tc.typeTemplate[p.type] : -1;
      if (pti >= 0) cc = (cc | 0xC000) & (p.flags | tc.templates[size_t(pti)].flags);
    }
    Footprint f = footprintOf(tp, uint32_t(fieldInt(vm, tp.object, u"diagonals", 0)), it.orient);
    if (it.x < 0 || it.z < 0 || it.x + f.sx > g.width || it.z + f.sz > g.length) continue;
    if (hasYData[size_t(ti)] < 0) {
      const std::string& p = tp.object->pendingPath;
      hasYData[size_t(ti)] = vm.vfs().exists(p.substr(0, p.rfind('.')) + "-y.bmp") ? 1 : 0;
    }
    bool writeNormal = !(cc & (kFlagBridge | kFlagTunnel | kFlagEnableBase)) && !hasYData[size_t(ti)] &&
                       ((flags & (kFlagEnableBase | kFlagCover)) || (tp.type >= 1 && tp.type <= 0x12));
    uint32_t normal = coverNormal(flags, it.orient, f.diag != 0);
    for (int z = 0; z < f.sz; ++z)
      for (int x = 0; x < f.sx; ++x) {
        uint32_t q = f.diag ? cellCover(f, flags, x, z) : 0xF00;
        if (!q) continue;
        size_t cell = size_t(it.z + z) * size_t(g.width) + size_t(it.x + x);
        if (cc & kFlagCover) {
          g.flags[cell] |= uint16_t(q);
          ++covered;
        }
        if (writeNormal) g.normals[cell] = normal;
      }
  }
  LOG_INFO("grid cover: %zu cell covers", covered);
}

void fixGridCoverArea(VM& vm, GridObj* grid, int x0, int z0, int x1, int z1) {
  GridData& g = *grid->data;
  x0 = std::max(x0, 0), z0 = std::max(z0, 0), x1 = std::min(x1, g.width - 1), z1 = std::min(z1, g.length - 1);
  if (x1 < x0 || z1 < z0) return;
  if (g.normals.size() != g.flags.size()) g.normals.assign(g.flags.size(), 0);  // before the first full pass (load)
  for (int z = z0; z <= z1; ++z)
    for (int x = x0; x <= x1; ++x) {
      size_t cell = size_t(z) * size_t(g.width) + size_t(x);
      g.flags[cell] &= 0xF0FF;
      g.normals[cell] = 0;
    }
  // the templates as fixGridCover sees them, from the grid's registered objects (new types included)
  struct Info {
    GridTemplate tp;
    uint32_t diagonals = 0;
    bool hasYData = false, valid = false;
  };
  std::vector<Info> info(g.types.size());
  for (size_t t = 0; t < g.types.size(); ++t) {
    auto it = grid->templates.find(g.types[t]);
    auto* o = it == grid->templates.end() ? nullptr : dynamic_cast<ObjectObj*>(it->second.get());
    if (!o || !o->data) continue;
    Info& i = info[t];
    i.tp.object = o;
    i.tp.width = std::max(1, fieldInt(vm, o, u"width", 1));
    i.tp.length = std::max(1, fieldInt(vm, o, u"length", 1));
    i.tp.type = fieldInt(vm, o, u"type", 0);
    i.tp.flags = uint32_t(fieldInt(vm, o, u"flags", 0));
    i.diagonals = uint32_t(fieldInt(vm, o, u"diagonals", 0));
    const std::string& p = o->pendingPath;
    i.hasYData = vm.vfs().exists(p.substr(0, p.rfind('.')) + "-y.bmp");
    i.valid = true;
  }
  for (const GridData::Item& it : g.items) {  // file order, as fixGridCover
    if (it.removed || it.type >= info.size() || !info[it.type].valid) continue;
    const Info& ti = info[it.type];
    const GridTemplate& tp = ti.tp;
    Footprint f = footprintOf(tp, ti.diagonals, it.orient);
    if (it.x > x1 || it.z > z1 || it.x + f.sx <= x0 || it.z + f.sz <= z0) continue;
    if (it.x < 0 || it.z < 0 || it.x + f.sx > g.width || it.z + f.sz > g.length) continue;
    uint32_t flags = it.flags | tp.flags, cc = flags;
    if ((flags & 0x500000) || tp.type == kTypeMarker) continue;  // as fixGridCover
    if (it.parent && it.parent <= g.items.size() && tp.type >= 10 && tp.type <= 12) {  // tram tracks follow the road
      const GridData::Item& p = g.items[it.parent - 1];
      if (p.type < info.size() && info[p.type].valid) cc = (cc | 0xC000) & (p.flags | info[p.type].tp.flags);
    }
    bool writeNormal = !(cc & (kFlagBridge | kFlagTunnel | kFlagEnableBase)) && !ti.hasYData &&
                       ((flags & (kFlagEnableBase | kFlagCover)) || (tp.type >= 1 && tp.type <= 0x12));
    uint32_t normal = coverNormal(flags, it.orient, f.diag != 0);
    for (int z = std::max(0, z0 - it.z); z < std::min(f.sz, z1 - it.z + 1); ++z)
      for (int x = std::max(0, x0 - it.x); x < std::min(f.sx, x1 - it.x + 1); ++x) {
        uint32_t q = f.diag ? cellCover(f, flags, x, z) : 0xF00;
        if (!q) continue;
        size_t cell = size_t(it.z + z) * size_t(g.width) + size_t(it.x + x);
        if (cc & kFlagCover) g.flags[cell] |= uint16_t(q);
        if (writeNormal) g.normals[cell] = normal;
      }
  }
}

void buildGridItems(const GridData& g, const TerrainCache& tc, int division, int lod, MeshData& out) {
  const int n = tc.n;
  const float cs = g.cellSize;
  const int x0 = (division % tc.cols) * n, z0 = (division / tc.cols) * n;
  const int heightShift = 0x200 - int(g.heightBase);
  std::map<std::pair<int, uint32_t>, Group> groups;
  for (uint32_t i : tc.divisionItems[size_t(division)]) {
    const GridData::Item& it = g.items[i];
    if (it.removed) continue;
    const GridTemplate& tp = tc.templates[size_t(tc.typeTemplate[it.type])];
    if (tp.skinned) continue;  // drawn per item (collectSkinnedItems)
    const ObjectData& od = *tp.object->data;
    uint32_t flags = it.flags | tp.flags;
    // LOD 1 = the template's lod1, else LOD 0 unless HIDE_LOD (§3.4)
    const MeshData* mesh = lod == 0 ? &od.mesh : od.hasLod1 ? &od.lod1 : (flags & kFlagHideLod) ? nullptr : &od.mesh;
    if (!mesh || mesh->positions.empty()) continue;
    int sx = (it.orient & 1) ? tp.length : tp.width, sz = (it.orient & 1) ? tp.width : tp.length;
    int y = it.y + heightShift;
    if ((flags & kFlagAlign4) || tp.type == kTypeMetroStation) y = (y + 0x7f) & ~0x7f;
    // world(p) = p·Ry((orient+2)·90°) + T, in division space (§2.2); the diagonal bake Ry(45°)·scale comes first
    Vec3 T{(2.0f * float(it.x - x0) + float(sx - n)) * cs * 0.5f, float(y) * cs / 256.0f,
           (2.0f * float(it.z - z0) + float(sz - n)) * cs * 0.5f};
    static const float kCos[4] = {-1, 0, 1, 0}, kSin[4] = {0, -1, 0, 1};
    float c = kCos[it.orient & 3], s = kSin[it.orient & 3];
    float cb = tp.bake ? 0.70710678f : 1.0f, sb = tp.bake ? 0.70710678f : 0.0f;
    float bx = tp.bake ? tp.bakeScaleX : 1.0f, bz = tp.bake ? tp.bakeScaleZ : 1.0f;
    auto bakeDir = [&](float x, float z, float& ox, float& oz) {
      ox = x * cb + z * sb;
      oz = -x * sb + z * cb;
    };
    auto place = [&](float x, float z, float& ox, float& oz) {
      ox = x * c + z * s;
      oz = -x * s + z * c;
    };
    uint32_t tint = tc.itemColor[i] ? tc.itemColor[i] : 0xFFFFFFFFu;
    bool hasUv1 = mesh->uv1.size() >= mesh->vertexCount() * 2;
    bool hasTangents = mesh->tangents.size() >= mesh->vertexCount() * 4;
    uint32_t vcount = uint32_t(mesh->vertexCount());
    for (const Submesh& sm : mesh->submeshes) {
      if (sm.firstVertex + sm.vertexCount > vcount || sm.firstIndex + sm.indexCount > mesh->indices.size()) continue;
      bool ok = true;
      for (uint32_t k = 0; k < sm.indexCount && ok; ++k) {
        uint32_t idx = mesh->indices[sm.firstIndex + k];
        ok = idx >= sm.firstVertex && idx < sm.firstVertex + sm.vertexCount;
      }
      if (!ok) continue;
      int mat = tp.materialBase + sm.material;
      int var = int(it.variation);
      if (tp.storeMesh && var >= 1 && var <= 4 && tp.variation[var - 1] >= 0 &&
          !(od.materials[size_t(sm.material)].flags & kMatKeepOnVariation))
        mat = tp.variation[var - 1];
      Group& gr = groups[{mat, tint}];
      uint32_t base = gr.vertexCount();
      for (uint32_t v = sm.firstVertex; v < sm.firstVertex + sm.vertexCount; ++v) {
        const float* p = &mesh->positions[size_t(v) * 3];
        const float* nn = &mesh->normals[size_t(v) * 3];
        float px, pz, nx, nz, wx, wz;
        bakeDir(p[0] * bx, p[2] * bz, px, pz);
        Vec3 nrm;
        if (tp.vertexGeneration && (mesh->colors[v] & 0x00FFFFFF) == 0) {
          nrm = normalize(Vec3{px, p[1], pz});  // foliage: spherical normals in the baked template space (§1.5)
        } else {
          bakeDir(nn[0], nn[2], nx, nz);
          nrm = {nx, nn[1], nz};
        }
        place(px, pz, wx, wz);
        gr.positions.insert(gr.positions.end(), {wx + T.x, p[1] + T.y, wz + T.z});
        place(nrm.x, nrm.z, nx, nz);
        gr.normals.insert(gr.normals.end(), {nx, nrm.y, nz});
        gr.uv0.insert(gr.uv0.end(), {mesh->uv0[size_t(v) * 2], mesh->uv0[size_t(v) * 2 + 1]});
        // trees: uv1 = (base height, 0), the engine's own layout for merged trees (§3.2.3); the wind shader subtracts
        // it to get the height above the tree's base (shaders.cpp, portShaderPatch)
        if (tp.type == kTypeTree) gr.uv1.insert(gr.uv1.end(), {T.y, 0.0f});
        else if (hasUv1) gr.uv1.insert(gr.uv1.end(), {mesh->uv1[size_t(v) * 2], mesh->uv1[size_t(v) * 2 + 1]});
        else gr.uv1.insert(gr.uv1.end(), {0.0f, 0.0f});
        if (hasTangents) {
          const float* t = &mesh->tangents[size_t(v) * 4];
          float tx, tz, ux, uz;
          bakeDir(t[0], t[2], tx, tz);
          place(tx, tz, ux, uz);
          gr.tangents.insert(gr.tangents.end(), {ux, t[1], uz, t[3]});
        } else {
          gr.tangents.insert(gr.tangents.end(), {1, 0, 0, 1});
        }
        gr.colors.push_back(mesh->colors[v]);
      }
      for (uint32_t k = 0; k < sm.indexCount; ++k) gr.indices.push_back(base + mesh->indices[sm.firstIndex + k] - sm.firstVertex);
    }
  }
  float r2 = 0;
  out.boundsMin = {1e30f, 1e30f, 1e30f};
  out.boundsMax = {-1e30f, -1e30f, -1e30f};
  for (auto& [key, gr] : groups) {
    uint32_t base = uint32_t(out.vertexCount());
    Submesh s;
    s.firstIndex = uint32_t(out.indices.size());
    s.indexCount = uint32_t(gr.indices.size());
    s.firstVertex = base;
    s.vertexCount = gr.vertexCount();
    s.material = key.first;
    s.colorize = key.second;
    for (uint32_t idx : gr.indices) out.indices.push_back(base + idx);
    out.positions.insert(out.positions.end(), gr.positions.begin(), gr.positions.end());
    out.normals.insert(out.normals.end(), gr.normals.begin(), gr.normals.end());
    out.uv0.insert(out.uv0.end(), gr.uv0.begin(), gr.uv0.end());
    out.uv1.insert(out.uv1.end(), gr.uv1.begin(), gr.uv1.end());
    out.tangents.insert(out.tangents.end(), gr.tangents.begin(), gr.tangents.end());
    out.colors.insert(out.colors.end(), gr.colors.begin(), gr.colors.end());
    out.submeshes.push_back(s);
  }
  for (size_t i = 0; i < out.vertexCount(); ++i) {
    const float* p = &out.positions[i * 3];
    for (size_t k = 0; k < 3; ++k) {
      out.boundsMin[k] = std::min(out.boundsMin[k], p[k]);
      out.boundsMax[k] = std::max(out.boundsMax[k], p[k]);
    }
    r2 = std::max(r2, p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
  }
  out.radius = std::sqrt(r2);
}

Mat4 itemPlacement(const GridData& g, const TerrainCache& tc, uint32_t item, int division) {
  const int n = tc.n;
  const float cs = g.cellSize;
  const int x0 = (division % tc.cols) * n, z0 = (division / tc.cols) * n;
  const GridData::Item& it = g.items[item];
  const GridTemplate& tp = tc.templates[size_t(tc.typeTemplate[it.type])];
  uint32_t flags = it.flags | tp.flags;
  int sx = (it.orient & 1) ? tp.length : tp.width, sz = (it.orient & 1) ? tp.width : tp.length;
  int y = it.y + (0x200 - int(g.heightBase));
  if ((flags & kFlagAlign4) || tp.type == kTypeMetroStation) y = (y + 0x7f) & ~0x7f;
  static const float kCos[4] = {-1, 0, 1, 0}, kSin[4] = {0, -1, 0, 1};
  float c = kCos[it.orient & 3], s = kSin[it.orient & 3];
  float cb = tp.bake ? 0.70710678f : 1.0f, sb = tp.bake ? 0.70710678f : 0.0f;
  // the same maps as buildGridItems: x' = x·c + z·s, z' = −x·s + z·c (and with cb, sb for the bake)
  Mat4 place, bake, scale;
  place.at(0, 0) = c, place.at(0, 2) = s, place.at(2, 0) = -s, place.at(2, 2) = c;
  bake.at(0, 0) = cb, bake.at(0, 2) = sb, bake.at(2, 0) = -sb, bake.at(2, 2) = cb;
  scale.at(0, 0) = tp.bake ? tp.bakeScaleX : 1.0f;
  scale.at(2, 2) = tp.bake ? tp.bakeScaleZ : 1.0f;
  Vec3 T{(2.0f * float(it.x - x0) + float(sx - n)) * cs * 0.5f, float(y) * cs / 256.0f,
         (2.0f * float(it.z - z0) + float(sz - n)) * cs * 0.5f};
  return translation(T) * place * bake * scale;
}

void collectSkinnedItems(Display& d, VM& vm, const GridData& g, const TerrainCache& tc, int division,
                         const Mat4& divisionWorld, float distance, int lod, double gridTime, bool shadowOnly,
                         std::map<const ObjectObj*, std::shared_ptr<const std::vector<Mat4>>>& poses,
                         std::vector<DrawItem>& out) {
  for (uint32_t i : tc.divisionItems[size_t(division)]) {
    const GridData::Item& it = g.items[i];
    int ti = it.type < tc.typeTemplate.size() ? tc.typeTemplate[it.type] : -1;
    if (ti < 0 || it.removed || !tc.templates[size_t(ti)].skinned) continue;
    const GridTemplate& tp = tc.templates[size_t(ti)];
    uint32_t flags = it.flags | tp.flags;
    if (lod > 0 && !tp.object->data->hasLod1 && (flags & kFlagHideLod)) continue;
    // gsGridItem::move (L:0x08182b10.c:110-120): phase += dt, wrapped at doorOpenLen; animate(phase + doorOpenStart);
    // gsGridItem::animate (L:0x08181ff0): no pose (bind pose) at 400 units and beyond. Items start at phase 0, so
    // all items of a template run in step.
    Pose pose;
    if (tp.animLength > 0 && distance < 400.0f && !(it.flags & 0x1000) && !(flags & kFlagHidden)) {
      auto cached = poses.find(tp.object);
      if (cached == poses.end()) {
        float t = float(std::fmod(gridTime, double(tp.animLength))) + tp.animStart;
        cached = poses.emplace(tp.object, poseFirstAnimation(vm, tp.object, t)).first;
      }
      pose = cached->second;
    }
    size_t before = out.size();
    collectSkinned(d, vm, tp.object, divisionWorld * itemPlacement(g, tc, i, division), pose, out, lod);
    for (size_t k = before; k < out.size(); ++k) {
      out[k].shadowOnly = shadowOnly;
      if (uint32_t c = tc.itemColor[i]) {  // the colorMap tint, as merged items carry it per part
        out[k].colorize[0] = float(c >> 16 & 0xFF) / 255.0f;
        out[k].colorize[1] = float(c >> 8 & 0xFF) / 255.0f;
        out[k].colorize[2] = float(c & 0xFF) / 255.0f;
        out[k].colorize[3] = float(c >> 24 & 0xFF) / 255.0f;
      }
    }
  }
}

// The engine draws a highlighted cell as a fan of the terrain cell with positiveMaterial / negativeMaterial
// (terrain.md §5, group "highlight + / −") and a highlighted item as a copy of its mesh with the same material as a
// layer (grid-items.md §3.1). Port: one overlay mesh in world space for the whole map (only the highlighted things),
// cells as flat-shaded quads through their four corners, items as their LOD 0 geometry; both selection materials are
// decals (MATERIAL_DECAL), drawn after the scene with a depth offset.
void buildHighlightOverlay(const GridData& g, const GridObj& grid, const TerrainCache& tc, MeshData& out) {
  const float cs = g.cellSize, k = cs / 256.0f, lift = 0.02f;
  struct Part {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
  } parts[2];  // positive, negative
  auto vertex = [](Part& p, const Vec3& v) {
    p.positions.insert(p.positions.end(), {v.x, v.y, v.z});
    return uint32_t(p.positions.size() / 3 - 1);
  };
  for (uint32_t c : grid.highlightCells) {
    if (c >= g.flags.size() || !(g.flags[c] & 0x4000)) continue;
    int x = int(c % uint32_t(g.width)), z = int(c / uint32_t(g.width));
    // corner (cx, cz) at the height the cell array stores for it (the last row and column repeat their neighbour)
    auto at = [&](int cx, int cz) {
      size_t hc = size_t(std::min(cz, g.length - 1)) * size_t(g.width) + size_t(std::min(cx, g.width - 1));
      return Vec3{float(cx - (g.width >> 1)) * cs, g.height[hc] * k + lift, float(cz - (g.length >> 1)) * cs};
    };
    Part& p = parts[(g.flags[c] & 0x8000) ? 1 : 0];
    uint32_t a = vertex(p, at(x, z)), b = vertex(p, at(x + 1, z)), d = vertex(p, at(x, z + 1)),
             e = vertex(p, at(x + 1, z + 1));
    p.indices.insert(p.indices.end(), {a, d, b, b, d, e});
  }
  for (uint32_t i : grid.highlightItems) {
    if (i >= g.items.size() || g.items[i].removed || !(g.items[i].flags & 0x100)) continue;
    if (g.items[i].flags & kFlagHidden) continue;  // hidden items (gave way to a tool's preview) are not drawn at all
    const GridData::Item& it = g.items[i];
    int ti = it.type < tc.typeTemplate.size() ? tc.typeTemplate[it.type] : -1;
    int division = itemDivision(g, tc, i);
    if (ti < 0 || division < 0) continue;
    const GridTemplate& tp = tc.templates[size_t(ti)];
    if (!tp.object || !tp.object->data) continue;
    const MeshData& mesh = tp.object->data->mesh;
    Mat4 world = tc.divisions[size_t(division)].world * itemPlacement(g, tc, i, division);
    Part& p = parts[(it.flags & 0x200) ? 1 : 0];
    uint32_t base = uint32_t(p.positions.size() / 3);
    for (size_t v = 0; v < mesh.vertexCount(); ++v) {
      Vec3 q = transformPoint(world, {mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]});
      vertex(p, q);
    }
    for (uint32_t idx : mesh.indices) p.indices.push_back(base + idx);
  }
  out = MeshData{};
  for (int s = 0; s < 2; ++s) {
    if (parts[s].indices.empty()) continue;
    int material = s ? tc.negativeMaterial : tc.positiveMaterial;
    if (material < 0) continue;
    Submesh sm;
    sm.firstVertex = uint32_t(out.vertexCount());
    sm.vertexCount = uint32_t(parts[s].positions.size() / 3);
    sm.firstIndex = uint32_t(out.indices.size());
    sm.indexCount = uint32_t(parts[s].indices.size());
    sm.material = material;
    for (uint32_t idx : parts[s].indices) out.indices.push_back(sm.firstVertex + idx);
    out.positions.insert(out.positions.end(), parts[s].positions.begin(), parts[s].positions.end());
    for (uint32_t v = 0; v < sm.vertexCount; ++v) {
      out.normals.insert(out.normals.end(), {0, 1, 0});
      out.uv0.insert(out.uv0.end(), {0, 0});
      out.uv1.insert(out.uv1.end(), {0, 0});
      out.tangents.insert(out.tangents.end(), {1, 0, 0, 1});
      out.colors.push_back(0xFFFFFFFFu);
    }
    out.submeshes.push_back(sm);
  }
  float r2 = 0;
  out.boundsMin = {1e30f, 1e30f, 1e30f};
  out.boundsMax = {-1e30f, -1e30f, -1e30f};
  for (size_t v = 0; v < out.vertexCount(); ++v) {
    const float* q = &out.positions[v * 3];
    for (int a = 0; a < 3; ++a) {
      out.boundsMin[size_t(a)] = std::min(out.boundsMin[size_t(a)], q[a]);
      out.boundsMax[size_t(a)] = std::max(out.boundsMax[size_t(a)], q[a]);
    }
    r2 = std::max(r2, q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
  }
  out.radius = std::sqrt(r2);
}

}  // namespace regs::engine
