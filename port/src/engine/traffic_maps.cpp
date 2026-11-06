// Traffic maps for the mods' shader hooks (engine/traffic_maps.h).
#include "engine/traffic_maps.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "engine/grid.h"
#include "engine/sim.h"
#include "engine/sim_graph.h"
#include "engine/sim_state.h"
#include "render/gl.h"
#include "render/glsl.h"

namespace regs::engine {

using namespace regs::engine::sim;
using render::Vec3;

namespace {

constexpr float kTexel = 1.0f;           // metres per texel (lane and trodden maps)
constexpr float kLaneHalfWidth = 1.35f;  // the painted band of a lane
constexpr float kPi = 3.14159265f;
constexpr uint32_t kTunnel = 0x800000;

void paintLanes(TrafficMaps& t, const LaneGraph& g) {
  const uint32_t want = kPathRoad | kPathTram, skip = kPathPark | kPathWalk | kPathDeleted | kTunnel;
  std::fill(t.lanes.begin(), t.lanes.end(), uint8_t(0));
  std::vector<float> best(size_t(t.laneW) * size_t(t.laneH), 1e9f);
  size_t painted = 0;
  for (int pass = 0; pass < 2; ++pass)  // lanes first, then the intersections' lanes where nothing is yet
    for (size_t i = 0; i < g.nodes.size() && i < 65535; ++i) {
      const LaneNode& n = g.nodes[i];
      if (!(n.flags & want) || (n.flags & skip)) continue;
      bool cross = (n.flags & kPathIntersection) != 0;
      if ((pass == 0) == cross) continue;
      float dx = n.p1.x - n.p0.x, dz = n.p1.z - n.p0.z, len = std::sqrt(dx * dx + dz * dz);
      if (len < 0.05f) continue;
      float a = std::atan2(dz, dx);
      if (a < 0) a += kPi;
      if (a >= kPi) a -= kPi;
      uint8_t A = uint8_t(int(std::lround(a / kPi * 256.0f)) & 255);
      float ca = std::cos(float(A) / 256.0f * kPi), sa = std::sin(float(A) / 256.0f * kPi);
      float ux = dx / len, uz = dz / len;  // along the lane
      float nx = -sa, nz = ca;            // the canonical normal (from the stored angle)
      float minX = std::min(n.p0.x, n.p1.x) - 2, maxX = std::max(n.p0.x, n.p1.x) + 2;
      float minZ = std::min(n.p0.z, n.p1.z) - 2, maxZ = std::max(n.p0.z, n.p1.z) + 2;
      int x0 = std::max(0, int((minX - t.originX) / kTexel)), x1 = std::min(t.laneW - 1, int((maxX - t.originX) / kTexel));
      int z0 = std::max(0, int((minZ - t.originZ) / kTexel)), z1 = std::min(t.laneH - 1, int((maxZ - t.originZ) / kTexel));
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
          float cx = t.originX + (float(x) + 0.5f) * kTexel - n.p0.x, cz = t.originZ + (float(z) + 0.5f) * kTexel - n.p0.z;
          float along = cx * ux + cz * uz;
          if (along < -0.2f || along > len + 0.2f) continue;
          float lat = cx * nx + cz * nz;
          float side = std::fabs(cx * -uz + cz * ux);
          if (side > kLaneHalfWidth) continue;
          size_t k = size_t(z) * size_t(t.laneW) + size_t(x);
          if (pass == 1 && (t.lanes[k * 4] | t.lanes[k * 4 + 1])) continue;
          if (side >= best[k]) continue;  // the nearest lane centre wins
          best[k] = side;
          uint32_t idx = uint32_t(i + 1);
          t.lanes[k * 4] = uint8_t(idx & 255);
          t.lanes[k * 4 + 1] = uint8_t(idx >> 8);
          t.lanes[k * 4 + 2] = uint8_t(std::clamp(int(std::lround(lat / 0.02f)) + 128, 0, 255));
          t.lanes[k * 4 + 3] = A;
          ++painted;
        }
    }
  t.graphNodes = g.nodes.size();
  t.lanesBuilt = true;
  t.lanesUploaded = false;
  LOG_INFO("traffic maps: %dx%d texels, %zu lane texels painted from %zu nodes", t.laneW, t.laneH, painted, g.nodes.size());
}

void upload(uint32_t& tex, int w, int h, uint32_t format, uint32_t layout, const uint8_t* data, bool linear) {
  if (!tex) {
    gl::GenTextures(1, &tex);
    gl::BindTexture(gl::TEXTURE_2D, tex);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, linear ? gl::LINEAR : gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, linear ? gl::LINEAR : gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
  } else {
    gl::BindTexture(gl::TEXTURE_2D, tex);
  }
  gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
  gl::TexImage2D(gl::TEXTURE_2D, 0, int(format), w, h, 0, layout, gl::UNSIGNED_BYTE, data);
  gl::PixelStorei(gl::UNPACK_ALIGNMENT, 4);
  gl::BindTexture(gl::TEXTURE_2D, 0);
}

}  // namespace

void simTrafficMapsTick(script::VM&, GridObj* grid, SimState& s, float dt) {
  if (render::shaderHooks().empty() || dt <= 0) return;
  if (!s.trafficMaps) {
    s.trafficMaps = std::make_shared<TrafficMaps>();
    TrafficMaps& t = *s.trafficMaps;
    float cs = s.graph.cellSize;
    t.originX = -float(s.graph.width) * cs * 0.5f;
    t.originZ = -float(s.graph.length) * cs * 0.5f;
    t.laneW = std::max(1, int(std::ceil(float(s.graph.width) * cs / kTexel)));
    t.laneH = std::max(1, int(std::ceil(float(s.graph.length) * cs / kTexel)));
    t.lanes.assign(size_t(t.laneW) * size_t(t.laneH) * 4, 0);
    t.troddenBytes.assign(size_t(t.laneW) * size_t(t.laneH), 0);
  }
  TrafficMaps& t = *s.trafficMaps;
  if (!t.lanesBuilt || t.graphNodes != s.graph.nodes.size()) {
    paintLanes(t, s.graph);
    if (std::getenv("REGS_DEBUG_TRAFFIC_MAPS")) {  // where the parked cars are (for close-up checks)
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
  for (auto it = t.trodden.begin(); it != t.trodden.end();) {
    it->second *= keepT;
    uint32_t x = it->first & 0xffff, z = it->first >> 16;
    if (it->second < 0.05f) {
      t.troddenBytes[size_t(z) * size_t(t.laneW) + x] = 0;
      if (t.dirtyX1 < t.dirtyX0) t.dirtyX0 = t.dirtyX1 = int(x), t.dirtyZ0 = t.dirtyZ1 = int(z);
      t.dirtyX0 = std::min(t.dirtyX0, int(x)), t.dirtyX1 = std::max(t.dirtyX1, int(x));
      t.dirtyZ0 = std::min(t.dirtyZ0, int(z)), t.dirtyZ1 = std::max(t.dirtyZ1, int(z));
      it = t.trodden.erase(it);
    } else {
      ++it;
    }
  }
  int walkers = 0;
  for (const Character& c : s.characters) {
    if (!c.alive || c.animal || c.virtualLeft >= 0) continue;
    int x = int((c.pos.x - t.originX) / kTexel), z = int((c.pos.z - t.originZ) / kTexel);
    if (x < 0 || z < 0 || x >= t.laneW || z >= t.laneH) continue;
    t.trodden[uint32_t(z) << 16 | uint32_t(x)] += dt;
    ++walkers;
  }
  static const bool debug = std::getenv("REGS_DEBUG_TRAFFIC_MAPS") != nullptr;
  static float logClock = 0;
  if (debug && (logClock += dt) > 10.0f) {
    logClock = 0;
    float maxT = 0, maxL = 0;
    for (const auto& [cell, v] : t.trodden) maxT = std::max(maxT, v);
    for (float c : t.laneCount) maxL = std::max(maxL, c);
    LOG_INFO("traffic maps: %d walkers, %zu trodden cells (max %.1f s), busiest lane %.1f s", walkers, t.trodden.size(),
             maxT, maxL);
  }
}

bool gridTrafficMaps(GridObj* grid, uint32_t& lanes, uint32_t& laneTraffic, uint32_t& trodden, float rect[4]) {
  SimState* s = simOf(grid);
  if (!s || !s->trafficMaps || !s->trafficMaps->lanesBuilt) return false;
  TrafficMaps& t = *s->trafficMaps;
  double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (!t.lanesUploaded) {
    upload(t.laneTexture, t.laneW, t.laneH, 0x8058 /* GL_RGBA8 */, 0x1908 /* GL_RGBA */, t.lanes.data(), false);
    t.lanesUploaded = true;
  }
  if (t.lastUpload < 0 || now - t.lastUpload >= 0.2) {  // at most 5 times a second
    t.lastUpload = now;
    if (t.laneTrafficDirty) {
      int rows = int((t.laneCount.size() + 255) / 256);
      t.laneValue.assign(size_t(std::max(rows, 1)) * 256, 0);
      for (size_t i = 0; i < t.laneCount.size(); ++i)
        t.laneValue[i] = uint8_t(std::lround(255.0f * (1.0f - std::exp(-t.laneCount[i] / 12.0f))));
      upload(t.laneTrafficTexture, 256, std::max(rows, 1), 0x8229 /* GL_R8 */, gl::RED, t.laneValue.data(), false);
      t.laneTrafficDirty = false;
    }
    for (const auto& [cell, v] : t.trodden) {
      int x = int(cell & 0xffff), z = int(cell >> 16);
      t.troddenBytes[size_t(z) * size_t(t.laneW) + size_t(x)] = uint8_t(std::lround(255.0f * (1.0f - std::exp(-v / 1.5f))));
      if (t.dirtyX1 < t.dirtyX0) t.dirtyX0 = t.dirtyX1 = x, t.dirtyZ0 = t.dirtyZ1 = z;
      t.dirtyX0 = std::min(t.dirtyX0, x), t.dirtyX1 = std::max(t.dirtyX1, x);
      t.dirtyZ0 = std::min(t.dirtyZ0, z), t.dirtyZ1 = std::max(t.dirtyZ1, z);
    }
    if (!t.troddenTexture) {
      upload(t.troddenTexture, t.laneW, t.laneH, 0x8229, gl::RED, t.troddenBytes.data(), true);
    } else if (t.dirtyX1 >= t.dirtyX0) {  // only the rectangle the walkers touched
      int w = t.dirtyX1 - t.dirtyX0 + 1, h = t.dirtyZ1 - t.dirtyZ0 + 1;
      std::vector<uint8_t> rectBytes(size_t(w) * size_t(h));
      for (int z = 0; z < h; ++z)
        std::copy_n(&t.troddenBytes[size_t(t.dirtyZ0 + z) * size_t(t.laneW) + size_t(t.dirtyX0)], size_t(w),
                    &rectBytes[size_t(z) * size_t(w)]);
      gl::BindTexture(gl::TEXTURE_2D, t.troddenTexture);
      gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
      gl::TexSubImage2D(gl::TEXTURE_2D, 0, t.dirtyX0, t.dirtyZ0, w, h, gl::RED, gl::UNSIGNED_BYTE, rectBytes.data());
      gl::PixelStorei(gl::UNPACK_ALIGNMENT, 4);
      gl::BindTexture(gl::TEXTURE_2D, 0);
    }
    t.dirtyX0 = 0, t.dirtyZ0 = 0, t.dirtyX1 = -1, t.dirtyZ1 = -1;
  }
  lanes = t.laneTexture;
  laneTraffic = t.laneTrafficTexture;
  trodden = t.troddenTexture;
  rect[0] = t.originX;
  rect[1] = t.originZ;
  rect[2] = 1.0f / (float(t.laneW) * kTexel);
  rect[3] = 1.0f / (float(t.laneH) * kTexel);
  return true;
}

}  // namespace regs::engine
