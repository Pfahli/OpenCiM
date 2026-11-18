// The surface map for the mods' shader hooks (engine/surface_map.h).
#include "engine/surface_map.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "render/gpu.h"

namespace regs::engine {

using namespace regs::render;

void surfaceMapInit(SurfaceMap& m, float originX, float originZ, float sizeX, float sizeZ) {
  m.originX = originX;
  m.originZ = originZ;
  m.w = std::max(1, int(std::ceil(sizeX / m.texel)));
  m.h = std::max(1, int(std::ceil(sizeZ / m.texel)));
  m.cls.assign(size_t(m.w) * size_t(m.h), 0);
  m.topW = std::max(1, int(std::ceil(sizeX / m.topTexel)));
  m.topH = std::max(1, int(std::ceil(sizeZ / m.topTexel)));
  m.top.assign(size_t(m.topW) * size_t(m.topH), 0);
  m.trodden.assign(size_t(m.topW) * size_t(m.topH), 0);
  m.topTilesW = (m.topW + SurfaceMap::kTopTile - 1) / SurfaceMap::kTopTile;
  m.topTilesH = (m.topH + SurfaceMap::kTopTile - 1) / SurfaceMap::kTopTile;
  m.topDirty.assign(size_t(m.topTilesW) * size_t(m.topTilesH), 0);
  surfaceMarkTop(m, 0, 0, m.topW - 1, m.topH - 1);
  m.dirtyX0 = 0, m.dirtyZ0 = 0, m.dirtyX1 = m.w - 1, m.dirtyZ1 = m.h - 1;  // the empty map once
  LOG_INFO("surface map: %dx%d texels of %.1f m", m.w, m.h, m.texel);
}

void surfaceMarkTop(SurfaceMap& m, int x0, int z0, int x1, int z1) {
  x0 = std::max(0, x0), z0 = std::max(0, z0), x1 = std::min(m.topW - 1, x1), z1 = std::min(m.topH - 1, z1);
  if (x1 < x0 || z1 < z0) return;
  const int T = SurfaceMap::kTopTile;
  for (int tz = z0 / T; tz <= z1 / T; ++tz)
    for (int tx = x0 / T; tx <= x1 / T; ++tx) m.topDirty[size_t(tz) * size_t(m.topTilesW) + size_t(tx)] = 1;
  m.topAnyDirty = true;
}

// the top map: every face of an item mesh with an area seen from above raises the cells under it to its height
void paintTop(SurfaceMap& m, const MeshData& mesh, const Mat4& world, const std::vector<GpuMaterial>& materials) {
  const float inv = 1.0f / m.topTexel;
  for (const Submesh& sm : mesh.submeshes) {
    // overlays are not there; tree crowns are no roof (snow and rain fall through them onto the ground)
    if (sm.material >= 0 && size_t(sm.material) < materials.size() &&
        (materials[size_t(sm.material)].surfaceClass == kSurfaceOverlay ||
         materials[size_t(sm.material)].surfaceClass == kSurfaceFoliage))
      continue;
    for (uint32_t k = 0; k + 2 < sm.indexCount; k += 3) {
      Vec3 p[3];
      for (int j = 0; j < 3; ++j) {
        uint32_t v = mesh.indices[sm.firstIndex + k + uint32_t(j)];
        p[j] = transformPoint(world, {mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]});
      }
      float ax = (p[0].x - m.originX) * inv, az = (p[0].z - m.originZ) * inv;
      float bx = (p[1].x - m.originX) * inv, bz = (p[1].z - m.originZ) * inv;
      float cx = (p[2].x - m.originX) * inv, cz = (p[2].z - m.originZ) * inv;
      float det = (bx - ax) * (cz - az) - (cx - ax) * (bz - az);
      if (std::fabs(det) < 0.02f) continue;  // edge-on from above (walls, tree cards)
      int x0 = std::max(0, int(std::floor(std::min({ax, bx, cx})))), x1 = std::min(m.topW - 1, int(std::ceil(std::max({ax, bx, cx}))));
      int z0 = std::max(0, int(std::floor(std::min({az, bz, cz})))), z1 = std::min(m.topH - 1, int(std::ceil(std::max({az, bz, cz}))));
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
          float px = float(x) + 0.5f, pz = float(z) + 0.5f;
          float l1 = ((px - ax) * (cz - az) - (cx - ax) * (pz - az)) / det;
          float l2 = ((bx - ax) * (pz - az) - (px - ax) * (bz - az)) / det;
          float l0 = 1 - l1 - l2;
          if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
          float y = p[0].y * l0 + p[1].y * l1 + p[2].y * l2;
          uint16_t v = uint16_t(std::clamp((y + 100.0f) * 100.0f, 1.0f, 65535.0f));
          uint16_t& t = m.top[size_t(z) * size_t(m.topW) + size_t(x)];
          if (v <= t) continue;
          t = v;
          m.topDirty[size_t(z / SurfaceMap::kTopTile) * size_t(m.topTilesW) + size_t(x / SurfaceMap::kTopTile)] = 1;
          m.topAnyDirty = true;
        }
    }
  }
}

void surfaceMapPaint(SurfaceMap& m, const MeshData& mesh, const Mat4& world, const std::vector<GpuMaterial>& materials,
                     bool items) {
  if (m.w <= 0) return;
  if (items) paintTop(m, mesh, world, materials);
  const float inv = 1.0f / m.texel;
  for (const Submesh& sm : mesh.submeshes) {
    if (sm.material < 0 || size_t(sm.material) >= materials.size()) continue;
    uint8_t c = materials[size_t(sm.material)].surfaceClass;
    if (c == kSurfaceOther || c == kSurfaceWater || c == kSurfaceFoliage) continue;
    for (uint32_t k = 0; k + 2 < sm.indexCount; k += 3) {
      Vec3 p[3];
      for (int j = 0; j < 3; ++j) {
        uint32_t v = mesh.indices[sm.firstIndex + k + uint32_t(j)];
        p[j] = transformPoint(world, {mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]});
      }
      Vec3 n = cross(p[1] - p[0], p[2] - p[0]);
      float len = std::sqrt(dot(n, n));
      if (len < 1e-6f || std::fabs(n.y) < 0.7f * len) continue;  // walkable faces only (either winding)
      // texel bounds of the triangle in x/z
      float ax = (p[0].x - m.originX) * inv, az = (p[0].z - m.originZ) * inv;
      float bx = (p[1].x - m.originX) * inv, bz = (p[1].z - m.originZ) * inv;
      float cx = (p[2].x - m.originX) * inv, cz = (p[2].z - m.originZ) * inv;
      int x0 = std::max(0, int(std::floor(std::min({ax, bx, cx})))), x1 = std::min(m.w - 1, int(std::ceil(std::max({ax, bx, cx}))));
      int z0 = std::max(0, int(std::floor(std::min({az, bz, cz})))), z1 = std::min(m.h - 1, int(std::ceil(std::max({az, bz, cz}))));
      if (x0 > x1 || z0 > z1) continue;
      float det = (bx - ax) * (cz - az) - (cx - ax) * (bz - az);
      if (std::fabs(det) < 1e-9f) continue;
      bool any = false;
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
          float px = float(x) + 0.5f, pz = float(z) + 0.5f;  // texel centre
          float l1 = ((px - ax) * (cz - az) - (cx - ax) * (pz - az)) / det;
          float l2 = ((bx - ax) * (pz - az) - (px - ax) * (bz - az)) / det;
          float l0 = 1 - l1 - l2;
          if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
          size_t i = size_t(z) * size_t(m.w) + size_t(x);
          uint8_t old = m.cls[i];
          if (old & 0x80) {
            if (!items) continue;  // terrain under an item
            uint8_t oc = old & 0x7F;
            if (c == kSurfaceRoad && (oc == kSurfacePavement || oc == kSurfacePlatform)) continue;  // the kerb texel
          }
          m.cls[i] = uint8_t(c | (items ? 0x80 : 0));
          any = true;
        }
      if (!any) continue;
      if (m.dirtyX1 < m.dirtyX0) m.dirtyX0 = x0, m.dirtyX1 = x1, m.dirtyZ0 = z0, m.dirtyZ1 = z1;
      else m.dirtyX0 = std::min(m.dirtyX0, x0), m.dirtyX1 = std::max(m.dirtyX1, x1), m.dirtyZ0 = std::min(m.dirtyZ0, z0),
      m.dirtyZ1 = std::max(m.dirtyZ1, z1);
    }
  }
}

void uploadTop(SurfaceMap& m) {
  if (m.topW <= 0 || !m.topAnyDirty) return;
  // r = top height, g = trodden (0..255 → 0..65535), into the kept buffer
  auto pack = [&](int x0, int z0, int w, int h) {
    size_t n = size_t(w) * size_t(h) * 2;
    if (m.topPack.size() < n) m.topPack.resize(n);
    uint16_t* rect = m.topPack.data();
    for (int z = 0; z < h; ++z) {
      size_t i = size_t(z0 + z) * size_t(m.topW) + size_t(x0), o = size_t(z) * size_t(w) * 2;
      for (int x = 0; x < w; ++x, ++i, o += 2) {
        rect[o] = m.top[i];
        rect[o + 1] = uint16_t(m.trodden[i] * 257u);
      }
    }
    return rect;
  };
  using namespace render::gpu;
  if (!m.topTexture) {
    m.topTexture = dev().createTexture(TexKind::Tex2D, 1);
    // linear: regs_shelterDepth's quick "anything overhead nearby?" test and the trodden map; the exact reads use
    // texelFetch
    SamplerDesc s;
    s.wrapS = s.wrapT = Wrap::ClampToEdge;
    dev().setSampler(m.topTexture, s);
    dev().texImage(m.topTexture, 0, 0, Format::RG16, m.topW, m.topH, Pixels::RG16, pack(0, 0, m.topW, m.topH));
  } else {
    // the dirty tiles as runs along each row of tiles; with many runs one rectangle around them all instead (each
    // upload is a copy and a barrier on Vulkan)
    struct Run {
      int tz, tx0, tx1;
    };
    static thread_local std::vector<Run> runs;
    runs.clear();
    const int T = SurfaceMap::kTopTile;
    int bx0 = m.topTilesW, bx1 = -1, bz0 = m.topTilesH, bz1 = -1;
    for (int tz = 0; tz < m.topTilesH; ++tz) {
      const uint8_t* row = &m.topDirty[size_t(tz) * size_t(m.topTilesW)];
      for (int tx = 0; tx < m.topTilesW; ++tx) {
        if (!row[tx]) continue;
        int tx1 = tx;
        while (tx1 + 1 < m.topTilesW && row[tx1 + 1]) ++tx1;
        runs.push_back({tz, tx, tx1});
        bx0 = std::min(bx0, tx), bx1 = std::max(bx1, tx1), bz0 = std::min(bz0, tz), bz1 = std::max(bz1, tz);
        tx = tx1;
      }
    }
    auto send = [&](int tx0, int tz0, int tx1, int tz1) {
      int x0 = tx0 * T, z0 = tz0 * T, x1 = std::min(m.topW, (tx1 + 1) * T), z1 = std::min(m.topH, (tz1 + 1) * T);
      dev().texSubImage(m.topTexture, 0, x0, z0, x1 - x0, z1 - z0, Pixels::RG16, pack(x0, z0, x1 - x0, z1 - z0));
    };
    constexpr size_t kMaxRuns = 32;
    if (runs.size() > kMaxRuns) send(bx0, bz0, bx1, bz1);
    else
      for (const Run& r : runs) send(r.tx0, r.tz, r.tx1, r.tz);
  }
  std::fill(m.topDirty.begin(), m.topDirty.end(), uint8_t(0));
  m.topAnyDirty = false;
}

void surfaceMapTrodden(SurfaceMap& m, const uint8_t* bytes, int w, float originX, float originZ, int x0, int z0, int x1,
                       int z1) {
  if (m.topW <= 0 || x1 < x0 || z1 < z0) return;
  const int dx = int(std::lround((originX - m.originX) / m.topTexel)), dz = int(std::lround((originZ - m.originZ) / m.topTexel));
  int tx0 = std::max(0, x0 + dx), tx1 = std::min(m.topW - 1, x1 + dx);
  int tz0 = std::max(0, z0 + dz), tz1 = std::min(m.topH - 1, z1 + dz);
  if (tx1 < tx0 || tz1 < tz0) return;
  const int T = SurfaceMap::kTopTile;
  for (int z = tz0; z <= tz1; ++z) {
    uint8_t* dst = &m.trodden[size_t(z) * size_t(m.topW)];
    const uint8_t* src = bytes + ptrdiff_t(z - dz) * ptrdiff_t(w);
    uint8_t* flags = &m.topDirty[size_t(z / T) * size_t(m.topTilesW)];
    for (int x = tx0; x <= tx1; ++x)
      if (dst[x] != src[x - dx]) {
        dst[x] = src[x - dx];
        flags[x / T] = 1;
        m.topAnyDirty = true;
      }
  }
}

void surfaceMapUpload(SurfaceMap& m) {
  uploadTop(m);
  if (m.w <= 0 || m.dirtyX1 < m.dirtyX0) return;
  // RG8: R the class byte (read exactly with texelFetch), G 255 on the carriageway (kSurfaceRoad) else 0, filtered
  // linearly for a smooth road edge (regs_roadEdge)
  auto rg = [&](int x0, int z0, int w, int h) {
    std::vector<uint8_t> rect(size_t(w) * size_t(h) * 2);
    for (int z = 0; z < h; ++z)
      for (int x = 0; x < w; ++x) {
        uint8_t c = m.cls[size_t(z0 + z) * size_t(m.w) + size_t(x0 + x)];
        size_t k = (size_t(z) * size_t(w) + size_t(x)) * 2;
        rect[k] = c;
        rect[k + 1] = (c & 0x7F) == render::kSurfaceRoad ? 255 : 0;
      }
    return rect;
  };
  using namespace render::gpu;
  if (!m.texture) {
    m.texture = dev().createTexture(TexKind::Tex2D, 1);
    SamplerDesc s;
    s.wrapS = s.wrapT = Wrap::ClampToEdge;
    dev().setSampler(m.texture, s);
    std::vector<uint8_t> all = rg(0, 0, m.w, m.h);
    dev().texImage(m.texture, 0, 0, Format::RG8, m.w, m.h, Pixels::RG8, all.data());
  } else {
    int w = m.dirtyX1 - m.dirtyX0 + 1, h = m.dirtyZ1 - m.dirtyZ0 + 1;
    std::vector<uint8_t> rect = rg(m.dirtyX0, m.dirtyZ0, w, h);
    dev().texSubImage(m.texture, 0, m.dirtyX0, m.dirtyZ0, w, h, Pixels::RG8, rect.data());
  }
  m.dirtyX0 = 0, m.dirtyZ0 = 0, m.dirtyX1 = -1, m.dirtyZ1 = -1;
}

}  // namespace regs::engine
