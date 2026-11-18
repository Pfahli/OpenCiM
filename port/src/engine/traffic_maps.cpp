// Traffic maps for the mods' shader hooks (engine/traffic_maps.h).
#include "engine/traffic_maps.h"
#include "core/env.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "engine/grid.h"
#include "engine/grid_render.h"
#include "engine/surface_map.h"
#include "engine/sim.h"
#include "engine/sim_graph.h"
#include "engine/sim_state.h"
#include "render/gpu.h"
#include "render/glsl.h"

namespace regs::engine {

using namespace regs::engine::sim;
using render::Vec3;

namespace {

constexpr float kTexel = 1.0f;           // metres per texel (lane and trodden maps)
constexpr float kListCell = 2.0f;        // m, the lane lists' cells (the shader uses the same)
constexpr float kListNear = 4.0f;        // m from a cell centre: lanes that can touch a point in the cell (regs_laneSoft)
constexpr float kListFar = 6.0f;         // m: and their kerb heap (regs_lanePlowed)
constexpr int kListMax = 40;             // lanes per cell at most
constexpr uint32_t kTunnel = 0x800000;

void paintLanes(TrafficMaps& t, const LaneGraph& g) {
  // not the lane-change pieces: they cross diagonally from one lane to the next, and as the nearest lane at a point
  // they bent the tyre tracks and the plowed strips into zig-zags
  const uint32_t want = kPathRoad | kPathTram, skip = kPathPark | kPathWalk | kPathDeleted | kTunnel | kPathLaneChange;
  // the lanes get numbers of their own (the lists and the lane texture use them): large maps have far more graph nodes in
  // all (Vienna about 125,000)
  t.laneId.assign(g.nodes.size(), -1);
  t.laneNodes.clear();
  // lane changes without the flag: a straight piece that kinks away from a predecessor and back into a successor
  // while those two run parallel (more than 6° each way, the two within 3°): left out as well
  auto dirOf = [&](const LaneNode& n) {
    float dx = n.p1.x - n.p0.x, dz = n.p1.z - n.p0.z, l = std::sqrt(dx * dx + dz * dz);
    return l > 0.05f ? Vec3{dx / l, 0, dz / l} : Vec3{0, 0, 0};
  };
  std::vector<std::vector<int>> preds(g.nodes.size());
  for (size_t i = 0; i < g.nodes.size(); ++i)
    for (int n : g.nodes[i].next)
      if (n >= 0 && size_t(n) < g.nodes.size()) preds[size_t(n)].push_back(int(i));
  auto unflaggedChange = [&](size_t i) {
    Vec3 d = dirOf(g.nodes[i]);
    if (d.x == 0 && d.z == 0) return false;
    for (int p : preds[i]) {
      Vec3 a = dirOf(g.nodes[size_t(p)]);
      if (a.x * d.x + a.z * d.z > 0.9945f) continue;  // within 6°: not kinked
      for (int q : g.nodes[i].next) {
        Vec3 b = dirOf(g.nodes[size_t(q)]);
        if (b.x * d.x + b.z * d.z <= 0.9945f && a.x * b.x + a.z * b.z > 0.9986f) return true;
      }
    }
    return false;
  };
  // longer ones (several pieces): a branch forking off a lane at more than 6° while another successor runs straight
  // on, followed until it merges into a lane (a node with two predecessors), at most 20 pieces, junctions excluded
  std::vector<uint8_t> branch(g.nodes.size(), 0);
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    const LaneNode& n = g.nodes[i];
    if (n.next.size() < 2 || !(n.flags & want) || (n.flags & kPathIntersection)) continue;
    Vec3 d = dirOf(n);
    bool straight = false;
    for (int q : n.next) straight |= dirOf(g.nodes[size_t(q)]).x * d.x + dirOf(g.nodes[size_t(q)]).z * d.z > 0.9945f;
    if (!straight) continue;
    for (int q : n.next) {
      Vec3 b = dirOf(g.nodes[size_t(q)]);
      if (b.x * d.x + b.z * d.z > 0.9945f) continue;
      int k = q;
      for (int steps = 0; steps < 20 && k >= 0; ++steps) {
        const LaneNode& m = g.nodes[size_t(k)];
        if ((m.flags & kPathIntersection) || preds[size_t(k)].size() >= 2) break;
        branch[size_t(k)] = 1;
        k = m.next.size() == 1 ? m.next[0] : -1;
      }
    }
  }
  size_t unflagged = 0;
  for (size_t i = 0; i < g.nodes.size() && t.laneNodes.size() < 262143; ++i) {
    const LaneNode& n = g.nodes[i];
    if (!(n.flags & want) || (n.flags & skip)) continue;
    if (!(n.flags & kPathIntersection) && (branch[i] || unflaggedChange(i))) {
      ++unflagged;
      continue;
    }
    t.laneId[i] = int32_t(t.laneNodes.size());
    t.laneNodes.push_back(int(i));
  }
  // The lane lists (vector lanes, regs_lanesAround): per 2 m cell every lane piece whose chord comes near enough to
  // matter anywhere in the cell, nearest first: first those within kListNear of the cell centre (a lane's band 1.35 m
  // and 1.5 m past its ends
  // with its soft edge, 1.7 m, plus the cell's half diagonal and the curve's bulge; tracks, slush, plowed strips), then
  // those within kListFar (the kerb heap up to 2.5 m beyond a lane's band). A shader only reads its own cell's list,
  // so no lane drops out at a cell edge while it still has an effect there (junctions: the turning lanes too).
  t.listW = int(std::ceil(float(t.laneW) * kTexel / kListCell));
  t.listH = int(std::ceil(float(t.laneH) * kTexel / kListCell));
  struct Entry {
    uint32_t cell;
    float d;
    int lane;
  };
  std::vector<Entry> entries;
  for (size_t l = 0; l < t.laneNodes.size(); ++l) {
    const LaneNode& n = g.nodes[size_t(t.laneNodes[l])];
    float ax = n.p0.x, az = n.p0.z, ex = n.p1.x - ax, ez = n.p1.z - az, len2 = ex * ex + ez * ez;
    int x0 = std::max(0, int((std::min(n.p0.x, n.p1.x) - kListFar - t.originX) / kListCell));
    int x1 = std::min(t.listW - 1, int((std::max(n.p0.x, n.p1.x) + kListFar - t.originX) / kListCell));
    int z0 = std::max(0, int((std::min(n.p0.z, n.p1.z) - kListFar - t.originZ) / kListCell));
    int z1 = std::min(t.listH - 1, int((std::max(n.p0.z, n.p1.z) + kListFar - t.originZ) / kListCell));
    for (int z = z0; z <= z1; ++z)
      for (int x = x0; x <= x1; ++x) {
        float cx = t.originX + (float(x) + 0.5f) * kListCell - ax, cz = t.originZ + (float(z) + 0.5f) * kListCell - az;
        float s = len2 > 1e-6f ? std::clamp((cx * ex + cz * ez) / len2, 0.0f, 1.0f) : 0.0f;
        float dx = cx - s * ex, dz = cz - s * ez, d = std::sqrt(dx * dx + dz * dz);
        if (d <= kListFar) entries.push_back({uint32_t(z) * uint32_t(t.listW) + uint32_t(x), d, int(l)});
      }
  }
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.cell != b.cell ? a.cell < b.cell : a.d < b.d; });
  t.lanes.assign(size_t(t.listW) * size_t(t.listH) * 4, 0);
  t.listIds.clear();
  size_t cut = 0, maxCount = 0;
  for (size_t a = 0; a < entries.size();) {
    size_t b = a;
    while (b < entries.size() && entries[b].cell == entries[a].cell) ++b;
    size_t count = std::min(b - a, size_t(kListMax)), near = 0;
    cut += (b - a) - count;
    maxCount = std::max(maxCount, b - a);
    uint32_t start = uint32_t(t.listIds.size());
    for (size_t k = a; k < a + count; ++k) {
      t.listIds.push_back(float(entries[k].lane));
      if (entries[k].d <= kListNear) ++near;
    }
    uint16_t* c = &t.lanes[size_t(entries[a].cell) * 4];
    c[0] = uint16_t(start & 0xffff), c[1] = uint16_t(start >> 16), c[2] = uint16_t(near), c[3] = uint16_t(count);
    a = b;
  }
  t.laneVersion = g.laneVersion;
  t.lanesBuilt = true;
  t.lanesUploaded = false;
  t.laneCurvesDirty = true;
  LOG_INFO("traffic maps: %zu lanes of %zu nodes (%zu unflagged lane changes left out), lane lists %dx%d cells, %zu "
           "entries (most in a cell %zu, %zu beyond %d left out)", t.laneNodes.size(), g.nodes.size(), unflagged,
           t.listW, t.listH, t.listIds.size(), maxCount, cut, kListMax);
}

// a data texture without mipmaps, clamped, nearest or linear; created on first use, then (re)defined with the data
void upload(uint32_t& tex, int w, int h, render::gpu::Format format, render::gpu::Pixels layout, const void* data,
            bool linear) {
  using namespace render::gpu;
  if (!tex) {
    tex = dev().createTexture(TexKind::Tex2D, 1);
    SamplerDesc s;
    s.minFilter = s.magFilter = linear ? Filter::Linear : Filter::Nearest;
    s.wrapS = s.wrapT = Wrap::ClampToEdge;
    dev().setSampler(tex, s);
  }
  dev().texImage(tex, 0, 0, format, w, h, layout, data);
}

// ---- footprints ----
constexpr float kFootTexel = 0.05f;   // m
constexpr int kFootTile = 32;         // texels per tile side (1.6 m)
constexpr int kAtlasTiles = 64;       // tiles per atlas side
constexpr int kAtlasSize = kFootTile * kAtlasTiles;
constexpr int kSlots = kAtlasTiles * kAtlasTiles;
constexpr float kStride = 0.7f;       // m between two steps
constexpr float kFootRange = 400.0f;  // m from the camera: walkers further away leave no steps

void footInit(TrafficMaps& t) {
  if (t.tilesX > 0) return;
  t.tilesX = int(std::ceil(float(t.laneW) * kTexel / (kFootTexel * kFootTile)));
  t.tilesZ = int(std::ceil(float(t.laneH) * kTexel / (kFootTexel * kFootTile)));
  t.tileIndex.assign(size_t(t.tilesX) * size_t(t.tilesZ), 0);
  t.atlas.assign(size_t(kAtlasSize) * size_t(kAtlasSize), 0);
  t.slotTile.assign(kSlots, -1);
  t.slotStamp.assign(kSlots, 0.0f);
  t.slotDirty.assign(kSlots, 0);
  t.freeSlots = kSlots;
}

void footIndexDirty(TrafficMaps& t, int x, int z) {
  if (t.indexX1 < t.indexX0) t.indexX0 = t.indexX1 = x, t.indexZ0 = t.indexZ1 = z;
  t.indexX0 = std::min(t.indexX0, x), t.indexX1 = std::max(t.indexX1, x);
  t.indexZ0 = std::min(t.indexZ0, z), t.indexZ1 = std::max(t.indexZ1, z);
}

void footFreeSlot(TrafficMaps& t, int slot) {
  int tile = t.slotTile[size_t(slot)];
  if (tile < 0) return;
  t.tileIndex[size_t(tile)] = 0;
  footIndexDirty(t, tile % t.tilesX, tile / t.tilesX);
  t.slotTile[size_t(slot)] = -1;
  int bx = (slot % kAtlasTiles) * kFootTile, bz = (slot / kAtlasTiles) * kFootTile;
  for (int z = 0; z < kFootTile; ++z)
    std::fill_n(&t.atlas[size_t(bz + z) * kAtlasSize + size_t(bx)], kFootTile, uint8_t(0));
  t.slotDirty[size_t(slot)] = 1;
  ++t.freeSlots;
}

// the atlas slot of a tile, taken on first use (the least recently stepped tile is reused when all are taken)
int footSlot(TrafficMaps& t, int tile) {
  if (uint16_t v = t.tileIndex[size_t(tile)]) return int(v) - 1;
  int slot = -1;
  if (t.freeSlots > 0) {
    for (int i = 0; i < kSlots; ++i)
      if (t.slotTile[size_t(i)] < 0) { slot = i; break; }
  } else {
    float oldest = 1e30f;
    for (int i = 0; i < kSlots; ++i)
      if (t.slotStamp[size_t(i)] < oldest) oldest = t.slotStamp[size_t(i)], slot = i;
    footFreeSlot(t, slot);
  }
  --t.freeSlots;
  t.slotTile[size_t(slot)] = tile;
  t.tileIndex[size_t(tile)] = uint16_t(slot + 1);
  footIndexDirty(t, tile % t.tilesX, tile / t.tilesX);
  return slot;
}

// one shoe print at (x, z) pointing along (dx, dz): a sole of about 26 × 10 cm with a softer heel, added to what is
// there, so steps on the same path add up to a trodden lane
void footStamp(TrafficMaps& t, float x, float z, float dx, float dz) {
  const float halfLen = 0.13f, halfWid = 0.05f;
  float px = (x - t.originX) / kFootTexel, pz = (z - t.originZ) / kFootTexel;
  int r = int(std::ceil(halfLen / kFootTexel)) + 1;
  int maxX = t.tilesX * kFootTile, maxZ = t.tilesZ * kFootTile;
  for (int gz = int(pz) - r; gz <= int(pz) + r; ++gz)
    for (int gx = int(px) - r; gx <= int(px) + r; ++gx) {
      if (gx < 0 || gz < 0 || gx >= maxX || gz >= maxZ) continue;
      float ox = (float(gx) + 0.5f - px) * kFootTexel, oz = (float(gz) + 0.5f - pz) * kFootTexel;
      float along = ox * dx + oz * dz, side = -ox * dz + oz * dx;
      // toe wider than the heel, with a waist
      float wid = halfWid * (along > 0 ? 1.0f : 0.8f) * (std::fabs(along / halfLen + 0.15f) < 0.2f ? 0.85f : 1.0f);
      float e = std::sqrt((along / halfLen) * (along / halfLen) + (side / wid) * (side / wid));
      if (e >= 1.0f) continue;
      float v = 1.0f - std::max(0.0f, (e - 0.55f) / 0.45f);  // soft rim
      int tile = (gz / kFootTile) * t.tilesX + gx / kFootTile;
      int slot = footSlot(t, tile);
      t.slotStamp[size_t(slot)] = t.age;
      size_t k = size_t((slot / kAtlasTiles) * kFootTile + gz % kFootTile) * kAtlasSize +
                 size_t((slot % kAtlasTiles) * kFootTile + gx % kFootTile);
      t.atlas[k] = uint8_t(std::min(255, int(t.atlas[k]) + int(v * 200.0f)));
      t.slotDirty[size_t(slot)] = 1;
    }
}

void footClear(TrafficMaps& t) {
  if (t.freeSlots == kSlots) return;
  for (int i = 0; i < kSlots; ++i) footFreeSlot(t, i);
  t.atlasAllDirty = true;
}

// walkers near the camera step every kStride, left and right of their line; steps fill in with footFill
void footTick(TrafficMaps& t, SimState& s, float dt) {
  footInit(t);
  if (t.footFill <= 0) {
    footClear(t);
    for (auto& w : t.walkers) w.valid = false;
    return;
  }
  if (t.walkers.size() < s.characters.size()) t.walkers.resize(s.characters.size());
  const float range2 = kFootRange * kFootRange;
  for (size_t i = 0; i < s.characters.size(); ++i) {
    const Character& c = s.characters[i];
    TrafficMaps::Walker& w = t.walkers[i];
    float ex = c.pos.x - s.eye.x, ez = c.pos.z - s.eye.z;
    if (!c.alive || c.animal || c.virtualLeft >= 0 || ex * ex + ez * ez > range2) {
      w.valid = false;
      continue;
    }
    if (!w.valid) {
      w.valid = true, w.last = c.pos, w.stride = 0;
      continue;
    }
    float dx = c.pos.x - w.last.x, dz = c.pos.z - w.last.z, d = std::sqrt(dx * dx + dz * dz);
    if (d > 5.0f) {  // a jump (teleport, boarding): start again
      w.last = c.pos, w.stride = 0;
      continue;
    }
    if (d < 1e-4f) continue;
    float ux = dx / d, uz = dz / d;
    float at = kStride - w.stride;  // distance along this move to the next step
    while (at <= d) {
      float sx = w.last.x + ux * at, sz = w.last.z + uz * at;
      float off = w.side ? 0.09f : -0.09f;  // the feet about 18 cm apart
      footStamp(t, sx - uz * off, sz + ux * off, ux, uz);
      w.side ^= 1;
      at += kStride;
    }
    w.stride = d - (at - kStride);
    w.last = c.pos;
  }
  // filling in: once a second, all steps fade with the time constant; empty tiles are given back
  t.fadeClock += dt;
  if (t.fadeClock >= 1.0f && t.footFill < 1e6f) {
    float keep = std::exp(-t.fadeClock / t.footFill);
    t.fadeClock = 0;
    for (int slot = 0; slot < kSlots; ++slot) {
      if (t.slotTile[size_t(slot)] < 0) continue;
      int bx = (slot % kAtlasTiles) * kFootTile, bz = (slot / kAtlasTiles) * kFootTile;
      bool any = false;
      for (int z = 0; z < kFootTile; ++z) {
        uint8_t* row = &t.atlas[size_t(bz + z) * kAtlasSize + size_t(bx)];
        for (int x = 0; x < kFootTile; ++x) {
          row[x] = uint8_t(float(row[x]) * keep);
          any |= row[x] > 0;
        }
      }
      if (any) t.slotDirty[size_t(slot)] = 1;
      else footFreeSlot(t, slot);
    }
  } else if (t.footFill >= 1e6f) {
    t.fadeClock = 0;
  }
}

}  // namespace

void simTrafficMapsTick(script::VM& vm, GridObj* grid, SimState& s, float dt) {
  if (render::shaderHooks().empty() || dt <= 0) return;
  if (!s.trafficMaps) {
    s.trafficMaps = std::make_shared<TrafficMaps>();
    TrafficMaps& t = *s.trafficMaps;
    float cs = s.graph.cellSize;
    t.originX = -float(s.graph.width) * cs * 0.5f;
    t.originZ = -float(s.graph.length) * cs * 0.5f;
    t.laneW = std::max(1, int(std::ceil(float(s.graph.width) * cs / kTexel)));
    t.laneH = std::max(1, int(std::ceil(float(s.graph.length) * cs / kTexel)));
    t.troddenBytes.assign(size_t(t.laneW) * size_t(t.laneH), 0);
    t.troddenTilesW = (t.laneW + TrafficMaps::kTroddenTile - 1) / TrafficMaps::kTroddenTile;
    t.troddenTilesH = (t.laneH + TrafficMaps::kTroddenTile - 1) / TrafficMaps::kTroddenTile;
    t.troddenDirty.assign(size_t(t.troddenTilesW) * size_t(t.troddenTilesH), 0);
  }
  TrafficMaps& t = *s.trafficMaps;
  // the lanes again when they changed (an aircraft's or boat's inline node is none: the node count alone would repaint
  // for each of them)
  if (!t.lanesBuilt || t.laneVersion != s.graph.laneVersion) {
    paintLanes(t, s.graph);
    if (regs::envVar("REGS_DEBUG_TRAFFIC_MAPS")) {  // where the parked cars are (for close-up checks)
      int parked = 0;
      for (const Mover& m : s.movers)
        if (m.live && m.parked && !m.track.nodes.empty() && ++parked <= 12) {
          const LaneNode& n = s.graph.nodes[size_t(m.track.nodes[m.track.at(m.dist)])];
          LOG_INFO("traffic maps: parked car at (%.0f, %.0f)", n.p0.x, n.p0.z);
        }
      LOG_INFO("traffic maps: %d parked cars", parked);
    }
  }
  t.age += dt;
  // lane traffic: occupancy time per node, decaying (fast for the first 20 s, so a loaded city shows its traffic)
  if (t.laneCount.size() < s.graph.nodes.size()) t.laneCount.resize(s.graph.nodes.size(), 0.0f);
  float tau = t.age < 20.0f ? 10.0f : 60.0f;
  float keep = std::exp(-dt / tau);
  for (float& c : t.laneCount) c *= keep;
  for (const Mover& m : s.movers) {
    if (!m.live || m.parked || m.boat || m.aircraft || m.track.nodes.empty()) continue;
    int node = m.track.nodes[m.track.at(m.dist)];
    if (node >= 0 && size_t(node) < t.laneCount.size()) t.laneCount[size_t(node)] += dt;
  }
  t.laneTrafficDirty = true;
  // trodden cells: visible pedestrians (not animals, not off-screen walks)
  float keepT = std::exp(-dt / 300.0f);
  for (size_t i = 0; i < t.troddenCells.size();) {
    float& v = t.troddenTime[i];
    v *= keepT;
    if (v >= 0.05f) {
      ++i;
      continue;
    }
    uint32_t cell = t.troddenCells[i], x = cell & 0xffff, z = cell >> 16;
    uint8_t& b = t.troddenBytes[size_t(z) * size_t(t.laneW) + x];
    if (b) b = 0, t.markTrodden(int(x), int(z));
    t.troddenIndex.erase(cell);
    if (i + 1 < t.troddenCells.size()) {  // the last entry (not decayed yet) takes this place
      t.troddenCells[i] = t.troddenCells.back();
      t.troddenTime[i] = t.troddenTime.back();
      t.troddenIndex[t.troddenCells[i]] = uint32_t(i);
    }
    t.troddenCells.pop_back();
    t.troddenTime.pop_back();
  }
  int walkers = 0;
  for (const Character& c : s.characters) {
    if (!c.alive || c.animal || c.virtualLeft >= 0) continue;
    int x = int((c.pos.x - t.originX) / kTexel), z = int((c.pos.z - t.originZ) / kTexel);
    if (x < 0 || z < 0 || x >= t.laneW || z >= t.laneH) continue;
    uint32_t cell = uint32_t(z) << 16 | uint32_t(x);
    auto [it, fresh] = t.troddenIndex.try_emplace(cell, uint32_t(t.troddenCells.size()));
    if (fresh) t.troddenCells.push_back(cell), t.troddenTime.push_back(0.0f);
    t.troddenTime[it->second] += dt;
    ++walkers;
  }
  // footprints (gsGrid.regsFootprints)
  {
    float fill = 0;
    if (grid) {
      auto f = grid->fieldValues.find(u"regsFootprints");
      if (f != grid->fieldValues.end()) fill = vm.toFloat(vm.resolve(f->second));
    }
    t.footFill = fill;
    footTick(t, s, dt);
  }
  // snowplows: the lane under a plow's rear (all behind its blade) is cleared; the first pass over a lane with snow on
  // it again adds to its kerb heap
  if (t.lanePlow.size() < s.graph.nodes.size()) {
    t.lanePlow.resize(s.graph.nodes.size(), 0.0f);
    t.laneHeap.resize(s.graph.nodes.size(), 0.0f);
  }
  if (t.footFill <= 0) {
    if (t.plowAny) {
      std::fill(t.lanePlow.begin(), t.lanePlow.end(), 0.0f);
      std::fill(t.laneHeap.begin(), t.laneHeap.end(), 0.0f);
      t.plowAny = false;
    }
  } else {
    if (t.plowAny && t.footFill < 1e6f) {
      float k = std::exp(-dt / t.footFill);
      for (float& p : t.lanePlow) p *= k;
    }
    for (const Mover& m : s.movers) {
      if (!m.live || !m.plow || m.track.nodes.empty()) continue;
      int node = m.track.nodes[m.track.at(m.dist)];
      if (node < 0 || size_t(node) >= t.lanePlow.size()) continue;
      float& p = t.lanePlow[size_t(node)];
      if (p < 0.9f) t.laneHeap[size_t(node)] = std::min(1.0f, t.laneHeap[size_t(node)] + 0.6f * (1.0f - p));
      p = 1.0f;
      t.plowAny = true;
    }
  }
  static const bool debug = regs::envVar("REGS_DEBUG_TRAFFIC_MAPS") != nullptr;
  static float logClock = 0;
  if (debug && (logClock += dt) > 10.0f) {
    logClock = 0;
    float maxT = 0, maxL = 0;
    for (float v : t.troddenTime) maxT = std::max(maxT, v);
    int plowed = 0;
    for (float p : t.lanePlow) plowed += p > 0.5f;
    for (float c : t.laneCount) maxL = std::max(maxL, c);
    LOG_INFO("traffic maps: %d walkers, %zu trodden cells (max %.1f s), busiest lane %.1f s; footprints fill %.0f, %d tiles; %d plows, %d lanes plowed",
             walkers, t.troddenCells.size(), maxT, maxL, t.footFill, kSlots - t.freeSlots, s.plowsLive, plowed);
  }
}

bool gridTrafficMaps(GridObj* grid, uint32_t& laneTraffic, float rect[4], float cells[4]) {
  SimState* s = simOf(grid);
  if (!s || !s->trafficMaps || !s->trafficMaps->lanesBuilt) return false;
  TrafficMaps& t = *s->trafficMaps;
  double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const bool full = t.laneCurvesDirty || !t.laneTrafficTexture;  // the lanes were (re)built: everything at once
  if (full || t.lastUpload < 0 || now - t.lastUpload >= 0.2) {  // at most 5 times a second
    t.lastUpload = now;
    if (t.laneTrafficDirty || full) {
      int rows = std::max(1, int((t.laneNodes.size() + 255) / 256));
      // RGBA32F, 1024 wide: per row of 256 lanes columns 0-255 their values (traffic, plowed, heap, 0), 256-511 and 512-767 their
      // curves (p0.xz, b.xz) and (c.xz, p1.xz), the cubic Bézier control points (buildStrip's b = p0 + f0·L/3,
      // c = p1 − f1·L/3); the curves are uploaded with the lane map, the values every time
      std::vector<float> values(size_t(rows) * 256 * 4, 0.0f);
      // the traffic blurred along the lanes (two passes over each node and its successors and predecessors, about
      // ±6 m): counted per 3 m node, a track would otherwise start and stop in steps at the node ends
      const LaneGraph& G = simOf(grid)->graph;
      std::vector<float> smooth(t.laneCount), sum(t.laneCount.size()), weight(t.laneCount.size());
      for (int pass = 0; pass < 2; ++pass) {
        std::fill(sum.begin(), sum.end(), 0.0f);
        std::fill(weight.begin(), weight.end(), 0.0f);
        for (size_t i = 0; i < smooth.size() && i < G.nodes.size(); ++i)
          for (int n : G.nodes[i].next)
            if (n >= 0 && size_t(n) < smooth.size()) {
              sum[i] += smooth[size_t(n)], weight[i] += 1.0f;
              sum[size_t(n)] += smooth[i], weight[size_t(n)] += 1.0f;
            }
        for (size_t i = 0; i < smooth.size(); ++i)
          if (weight[i] > 0) smooth[i] = 0.5f * smooth[i] + 0.5f * sum[i] / weight[i];
      }
      for (size_t l = 0; l < t.laneNodes.size(); ++l) {  // by lane number (paintLanes)
        size_t i = size_t(t.laneNodes[l]);
        if (i < smooth.size()) values[l * 4] = 1.0f - std::exp(-smooth[i] / 4.0f);
        if (i < t.lanePlow.size()) values[l * 4 + 1] = t.lanePlow[i], values[l * 4 + 2] = t.laneHeap[i];
      }
      if (full) {
        // one RGBA32F texture, 1024 texels wide (a texture unit fewer for the shader hooks): the lanes' rows (values,
        // curve, curve), then the lists (4 lane numbers per texel), then the cells (2 per texel: the list's first
        // float counted from the texture's first float, and near + 64 · all)
        int listRows = std::max(1, int((t.listIds.size() + 4095) / 4096));
        uint32_t base = uint32_t(rows) * 1024u * 4u;
        size_t cellCount = size_t(t.listW) * size_t(t.listH);
        int cellRows = std::max(1, int((cellCount + 2047) / 2048));
        t.cellBase = uint32_t(rows + listRows) * 1024u;  // the cells' first texel
        t.lanesUploaded = true;
        std::vector<float> all(size_t(rows + listRows + cellRows) * 1024 * 4, 0.0f);
        std::copy(t.listIds.begin(), t.listIds.end(), all.begin() + base);
        for (size_t k = 0; k < cellCount; ++k) {
          const uint16_t* c = &t.lanes[k * 4];
          if (!c[3]) continue;
          float* o = &all[size_t(t.cellBase) * 4 + k * 2];
          o[0] = float((uint32_t(c[0]) | uint32_t(c[1]) << 16) + base);
          o[1] = float(c[2] + 64 * c[3]);
        }
        for (size_t l = 0; l < t.laneNodes.size(); ++l) {
          const LaneNode& n = G.nodes[size_t(t.laneNodes[l])];
          float L = n.length / 3.0f;
          size_t r = l / 256, c = l % 256;
          float* a = &all[(r * 1024 + 256 + c) * 4];
          float* b = &all[(r * 1024 + 512 + c) * 4];
          a[0] = n.p0.x, a[1] = n.p0.z, a[2] = n.p0.x + n.f0.x * L, a[3] = n.p0.z + n.f0.z * L;
          b[0] = n.p1.x - n.f1.x * L, b[1] = n.p1.z - n.f1.z * L, b[2] = n.p1.x, b[3] = n.p1.z;
        }
        for (int r = 0; r < rows; ++r) std::copy_n(&values[size_t(r) * 256 * 4], 256 * 4, &all[size_t(r) * 1024 * 4]);
        upload(t.laneTrafficTexture, 1024, rows + listRows + cellRows, render::gpu::Format::RGBA32F,
               render::gpu::Pixels::RGBA32F, all.data(), false);
        t.laneTrafficRows = rows;
        t.laneCurvesDirty = false;
      } else {
        render::gpu::dev().texSubImage(t.laneTrafficTexture, 0, 0, 0, 256, rows, render::gpu::Pixels::RGBA32F, values.data());
      }
      t.laneTrafficDirty = false;
    }
    for (size_t i = 0; i < t.troddenCells.size(); ++i) {
      uint32_t cell = t.troddenCells[i];
      float v = t.troddenTime[i];
      int x = int(cell & 0xffff), z = int(cell >> 16);
      uint8_t b = uint8_t(std::lround(255.0f * (1.0f - std::exp(-v / 1.5f))));
      uint8_t& old = t.troddenBytes[size_t(z) * size_t(t.laneW) + size_t(x)];
      if (old != b) old = b, t.markTrodden(x, z);
    }
    // the trodden map goes into the top map's texture (engine/surface_map.h, its second channel): first all of it,
    // then only the tiles whose bytes changed; kept for later while the map has no surface map yet
    SurfaceMap* sm = grid->terrain ? grid->terrain->surfaceMap.get() : nullptr;
    if (sm != t.troddenTarget) t.troddenSynced = false, t.troddenTarget = sm;  // a new surface map: all again
    if (sm && sm->topW > 0) {
      if (!t.troddenSynced) {
        surfaceMapTrodden(*sm, t.troddenBytes.data(), t.laneW, t.originX, t.originZ, 0, 0, t.laneW - 1, t.laneH - 1);
        t.troddenSynced = true;
      } else if (t.troddenAnyDirty) {
        const int T = TrafficMaps::kTroddenTile;
        for (int tz = 0; tz < t.troddenTilesH; ++tz)
          for (int tx = 0; tx < t.troddenTilesW; ++tx)
            if (t.troddenDirty[size_t(tz) * size_t(t.troddenTilesW) + size_t(tx)])
              surfaceMapTrodden(*sm, t.troddenBytes.data(), t.laneW, t.originX, t.originZ, tx * T, tz * T,
                                std::min(t.laneW - 1, tx * T + T - 1), std::min(t.laneH - 1, tz * T + T - 1));
      }
      std::fill(t.troddenDirty.begin(), t.troddenDirty.end(), uint8_t(0));
      t.troddenAnyDirty = false;
    }
  }
  laneTraffic = t.laneTrafficTexture;
  cells[0] = float(t.listW), cells[1] = float(t.listH), cells[2] = float(t.cellBase), cells[3] = 0;
  rect[0] = t.originX;
  rect[1] = t.originZ;
  rect[2] = 1.0f / (float(t.laneW) * kTexel);
  rect[3] = 1.0f / (float(t.laneH) * kTexel);
  return true;
}

// One R8 texture (a texture unit fewer for the shader hooks): the tile atlas at the top (kAtlasSize²), below it the
// tile index, two texels per tile (slot + 1, low and high byte; 0 where nobody stepped); the width is the larger of
// the two, the rest zero.
bool gridFootprints(GridObj* grid, uint32_t& atlas, float rect[4]) {
  SimState* s = simOf(grid);
  if (!s || !s->trafficMaps || s->trafficMaps->tilesX <= 0) return false;
  TrafficMaps& t = *s->trafficMaps;
  // the upload follows the traffic maps' pace (gridTrafficMaps runs first and sets lastUpload when it uploads)
  bool create = !t.footAtlasTexture;
  if (create) {
    const int w = std::max(kAtlasSize, 2 * t.tilesX), h = kAtlasSize + t.tilesZ;
    std::vector<uint8_t> zero(size_t(w) * size_t(h), 0);
    upload(t.footAtlasTexture, w, h, render::gpu::Format::R8, render::gpu::Pixels::R8, zero.data(), false);
    t.indexX0 = 0, t.indexZ0 = 0, t.indexX1 = t.tilesX - 1, t.indexZ1 = t.tilesZ - 1;
    t.atlasAllDirty = true;
  }
  if (t.indexX1 >= t.indexX0) {
    int w = t.indexX1 - t.indexX0 + 1, h = t.indexZ1 - t.indexZ0 + 1;
    std::vector<uint8_t> rectData(size_t(w) * 2 * size_t(h));
    for (int z = 0; z < h; ++z)
      for (int x = 0; x < w; ++x) {
        uint16_t v = t.tileIndex[size_t(t.indexZ0 + z) * size_t(t.tilesX) + size_t(t.indexX0 + x)];
        rectData[(size_t(z) * size_t(w) + size_t(x)) * 2] = uint8_t(v & 0xFF);
        rectData[(size_t(z) * size_t(w) + size_t(x)) * 2 + 1] = uint8_t(v >> 8);
      }
    render::gpu::dev().texSubImage(t.footAtlasTexture, 0, 2 * t.indexX0, kAtlasSize + t.indexZ0, 2 * w, h,
                                   render::gpu::Pixels::R8, rectData.data());
    t.indexX0 = 0, t.indexZ0 = 0, t.indexX1 = -1, t.indexZ1 = -1;
  }
  int dirty = 0;
  for (uint8_t d : t.slotDirty) dirty += d;
  if (t.atlasAllDirty || dirty > kSlots / 4) {
    render::gpu::dev().texSubImage(t.footAtlasTexture, 0, 0, 0, kAtlasSize, kAtlasSize, render::gpu::Pixels::R8, t.atlas.data());
    std::fill(t.slotDirty.begin(), t.slotDirty.end(), uint8_t(0));
    t.atlasAllDirty = false;
  } else if (dirty > 0) {
    std::vector<uint8_t> tile(size_t(kFootTile) * kFootTile);
    for (int slot = 0; slot < kSlots; ++slot) {
      if (!t.slotDirty[size_t(slot)]) continue;
      t.slotDirty[size_t(slot)] = 0;
      int bx = (slot % kAtlasTiles) * kFootTile, bz = (slot / kAtlasTiles) * kFootTile;
      for (int z = 0; z < kFootTile; ++z)
        std::copy_n(&t.atlas[size_t(bz + z) * kAtlasSize + size_t(bx)], kFootTile, &tile[size_t(z) * kFootTile]);
      render::gpu::dev().texSubImage(t.footAtlasTexture, 0, bx, bz, kFootTile, kFootTile, render::gpu::Pixels::R8, tile.data());
    }
  }
  atlas = t.footAtlasTexture;
  rect[0] = t.originX;
  rect[1] = t.originZ;
  rect[2] = kFootTexel;
  rect[3] = float(kAtlasTiles);
  return true;
}

void registerTrafficMapNatives(script::VM& vm) {
  using namespace regs::script;
  if (ClassInfo* g = vm.findClass("gsGrid")) {
    FieldInfo& f = g->fields[u"regsFootprints"];
    f.name = u"regsFootprints";
    f.get = [](script::VM&, script::Object* o) {
      auto it = o->fieldValues.find(u"regsFootprints");
      return it == o->fieldValues.end() ? script::Value::real(0.0) : it->second;
    };
    f.set = [](script::VM& v, script::Object* o, const script::Value& x) { o->fieldValues[u"regsFootprints"] = v.resolve(x); };
  }
}

}  // namespace regs::engine
