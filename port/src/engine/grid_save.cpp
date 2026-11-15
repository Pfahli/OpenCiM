// Saving the map state: gsGrid.saveToFile writes map.grid section by section in the loaded file's order: the terrain
// and network from the live map (city growth and other live edits change them; removed items are dropped and every
// item reference renumbered, savedItemRefs), the sections the simulation keeps live (vehicles, characters, lines,
// citizens) by its GridSectionWriter, anything else as loaded. The timeline has its own natives (engine/timeline.cpp).
#include "engine/grid_save.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/curve_roads.h"
#include "engine/paint_edges.h"
#include "engine/proc_buildings.h"
#include "engine/grid.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

namespace {

GridSectionWriter g_sectionWriter;

uint32_t u32(const std::vector<uint8_t>& d, size_t at) {
  uint32_t v = 0;
  if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
  return v;
}

void putU32(std::vector<uint8_t>& d, size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); }

void put32(std::vector<uint8_t>& d, uint32_t v) { d.insert(d.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); }
void put16(std::vector<uint8_t>& d, uint16_t v) { d.insert(d.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 2); }
size_t beginChunk(std::vector<uint8_t>& d, uint32_t tag) {
  put32(d, tag);
  put32(d, 0);
  return d.size();
}
void endChunk(std::vector<uint8_t>& d, size_t body) { putU32(d, body - 4, uint32_t(d.size() - body)); }

// 0x07010000 (save-format README §3.1): per cell i16 height in the file's base (the loader adds 0x200 − heightBase)
// and u16 flags without the selection highlight
void writeTerrain(const GridData& g, std::vector<uint8_t>& out) {
  size_t body = beginChunk(out, 0x07010000);
  int shift = 0x200 - int(g.heightBase);
  for (size_t i = 0; i < g.height.size(); ++i) {
    put16(out, uint16_t(int16_t(g.height[i] - shift)));
    put16(out, uint16_t(g.flags[i] & 0x3FFF));
  }
  endChunk(out, body);
}

// 0x07020000 (§3.2): {typeCount, itemCount}; the type names in use (in their loaded order), the 16-byte item records
// with parents renumbered, and one padded UTF-8 name per item (writer FUN_100c3840)
void writeNetwork(const GridData& g, std::vector<uint8_t>& out) {
  std::vector<uint32_t> refs = savedItemRefs(g);
  std::vector<int> typeMap(g.types.size(), -1);
  for (const GridData::Item& it : g.items)
    if (!it.removed && it.type < typeMap.size()) typeMap[it.type] = 0;
  uint32_t typeCount = 0, itemCount = 0;
  for (int& t : typeMap)
    if (t == 0) t = int(typeCount++);
  for (const GridData::Item& it : g.items) itemCount += it.removed ? 0 : 1;
  size_t body = beginChunk(out, 0x07020000);
  put32(out, typeCount);
  put32(out, itemCount);
  size_t types = beginChunk(out, 0x07020100);
  for (size_t t = 0; t < g.types.size(); ++t)
    if (typeMap[t] >= 0) {
      out.insert(out.end(), g.types[t].begin(), g.types[t].end());
      out.push_back(0);
    }
  while ((out.size() - types) % 4) out.push_back(0);  // padded once at the end
  endChunk(out, types);
  size_t items = beginChunk(out, 0x07020210);
  for (const GridData::Item& it : g.items) {
    if (it.removed) continue;
    put16(out, uint16_t((uint32_t(typeMap[it.type]) & 0x3FFF) | uint32_t(it.orient & 3) << 14));
    put16(out, uint16_t(it.x));
    put16(out, uint16_t(it.y));
    put16(out, uint16_t(it.z));
    put32(out, it.parent && it.parent <= refs.size() ? refs[it.parent - 1] : 0);
    put32(out, (it.flags & 0x72500000u) | (it.variation & 0xFFu));
  }
  endChunk(out, items);
  size_t names = beginChunk(out, 0x07020310);
  for (size_t i = 0; i < g.items.size(); ++i) {
    if (g.items[i].removed) continue;
    size_t start = out.size();
    if (i < g.names.size()) out.insert(out.end(), g.names[i].begin(), g.names[i].end());
    out.push_back(0);
    while ((out.size() - start) % 4) out.push_back(0);
  }
  endChunk(out, names);
  endChunk(out, body);
  LOG_INFO("grid save: %u items of %u types, %zu removed since load", itemCount, typeCount, g.items.size() - itemCount);
}

// port: the section of items off the grid (GridData::Item::yaw, offX, offZ; kItemTurnSectionTag), with the shifts
// (count bit 31); none when every item is on the grid
void writeItemTurns(const GridData& g, std::vector<uint8_t>& out) {
  std::vector<uint32_t> refs = savedItemRefs(g);
  std::vector<uint8_t> body;
  put32(body, 0);
  uint32_t count = 0;
  auto putF = [&](float f) {
    uint32_t a;
    std::memcpy(&a, &f, 4);
    put32(body, a);
  };
  for (size_t i = 0; i < g.items.size(); ++i)
    if (refs[i] && itemOffGrid(g.items[i])) {
      put32(body, refs[i]);
      putF(g.items[i].yaw);
      putF(g.items[i].offX);
      putF(g.items[i].offZ);
      ++count;
    }
  if (!count) return;
  putU32(body, 0, count | 0x80000000u);
  put32(out, kItemTurnSectionTag);
  put32(out, uint32_t(body.size()));
  out.insert(out.end(), body.begin(), body.end());
}

}  // namespace

std::vector<uint8_t> gridFileOf(const GridData& g) {
  std::vector<uint8_t> out;
  put32(out, 0x07000000);
  put32(out, 0);
  put32(out, uint32_t(g.width));
  put32(out, uint32_t(g.length));
  uint32_t cs;
  std::memcpy(&cs, &g.cellSize, 4);
  put32(out, cs);
  put32(out, g.bucketShift);
  put16(out, g.heightBase);
  put16(out, 0);
  put32(out, 0);
  writeTerrain(g, out);
  writeNetwork(g, out);
  writeItemTurns(g, out);
  writePaintEdgeSection(g, out);
  putU32(out, 4, uint32_t(out.size() - 8));
  return out;
}

void setGridSectionWriter(GridSectionWriter writer) { g_sectionWriter = std::move(writer); }

void registerSaveNatives(VM& vm) {
  // saveToFile(path): root chunk 0x07000000 with its 24-byte header, then the sections (save-format README §3)
  implement(vm, "gsGrid", u"saveToFile", [](VM& v, Object* s, int, const Value* a) {
    auto* g = dynamic_cast<GridObj*>(s);
    std::string path = utf16ToUtf8(v.toStr(a[0]));
    if (!g || !g->sourceFile || g->sourceFile->size() < 32) {
      LOG_ERROR("grid saveToFile %s: no loaded map", path.c_str());
      return Value::boolean(false);
    }
    const std::vector<uint8_t>& src = *g->sourceFile;
    size_t rootEnd = std::min(src.size(), size_t(8) + u32(src, 4));
    std::vector<uint8_t> out(src.begin(), src.begin() + 32);  // root tag, size, header
    std::vector<uint32_t> seen;
    for (size_t p = 32; p + 8 <= rootEnd;) {
      uint32_t tag = u32(src, p);
      size_t end = std::min(rootEnd, p + 8 + u32(src, p + 4));
      std::vector<uint8_t> chunk;
      if (tag == 0x07010000 && g->data) writeTerrain(*g->data, out);
      else if (tag == 0x07020000 && g->data) writeNetwork(*g->data, out);
      else if (tag == kCurveSectionTag && g->data) writeCurveSection(*g->data, out);  // port: curved roads
      else if (tag == kItemTurnSectionTag && g->data) writeItemTurns(*g->data, out);  // port: free item turns
      else if (tag == kPaintEdgeSectionTag && g->data) writePaintEdgeSection(*g->data, out);  // port: paint edges
      else if (tag == kProcBuildingSectionTag && g->data) writeProcBuildingSection(*g->data, out);  // port: procedural buildings
      else if (g_sectionWriter && g_sectionWriter(v, g, tag, chunk)) out.insert(out.end(), chunk.begin(), chunk.end());
      else out.insert(out.end(), src.begin() + std::ptrdiff_t(p), src.begin() + std::ptrdiff_t(end));
      seen.push_back(tag);
      p = end;
    }
    for (uint32_t tag : {0x07040000u, 0x07050000u, 0x07060000u, 0x07070000u}) {
      std::vector<uint8_t> chunk;
      if (std::find(seen.begin(), seen.end(), tag) == seen.end() && g_sectionWriter && g_sectionWriter(v, g, tag, chunk))
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    // port: the curved roads' section when the loaded file had none (new maps, maps from the original)
    if (g->data && !g->data->curves.empty() && std::find(seen.begin(), seen.end(), kCurveSectionTag) == seen.end())
      writeCurveSection(*g->data, out);
    if (g->data && std::find(seen.begin(), seen.end(), kItemTurnSectionTag) == seen.end()) writeItemTurns(*g->data, out);
    if (g->data && std::find(seen.begin(), seen.end(), kPaintEdgeSectionTag) == seen.end()) writePaintEdgeSection(*g->data, out);
    if (g->data && std::find(seen.begin(), seen.end(), kProcBuildingSectionTag) == seen.end()) writeProcBuildingSection(*g->data, out);
    putU32(out, 4, uint32_t(out.size() - 8));
    return Value::boolean(v.vfs().write(path, out));
  });

}

}  // namespace regs::engine
