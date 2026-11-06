// The surface map for the mods' shader hooks (engine/surface_map.h).
#include "engine/surface_map.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "render/gl.h"

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
  m.topX0 = 0, m.topZ0 = 0, m.topX1 = m.topW - 1, m.topZ1 = m.topH - 1;
  m.dirtyX0 = 0, m.dirtyZ0 = 0, m.dirtyX1 = m.w - 1, m.dirtyZ1 = m.h - 1;  // the empty map once
  LOG_INFO("surface map: %dx%d texels of %.1f m", m.w, m.h, m.texel);
}

// the top map: every face of an item mesh with an area seen from above raises the cells under it to its height
void paintTop(SurfaceMap& m, const MeshData& mesh, const Mat4& world, const std::vector<GpuMaterial>& materials) {
  const float inv = 1.0f / m.topTexel;
  for (const Submesh& sm : mesh.submeshes) {
    if (sm.material >= 0 && size_t(sm.material) < materials.size() && materials[size_t(sm.material)].surfaceClass == kSurfaceOverlay)
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
          if (m.topX1 < m.topX0) m.topX0 = m.topX1 = x, m.topZ0 = m.topZ1 = z;
          m.topX0 = std::min(m.topX0, x), m.topX1 = std::max(m.topX1, x);
          m.topZ0 = std::min(m.topZ0, z), m.topZ1 = std::max(m.topZ1, z);
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
    if (c == kSurfaceOther || c == kSurfaceWater) continue;
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
  if (m.topW <= 0 || m.topX1 < m.topX0) return;
  gl::PixelStorei(gl::UNPACK_ALIGNMENT, 2);
  if (!m.topTexture) {
    gl::GenTextures(1, &m.topTexture);
    gl::BindTexture(gl::TEXTURE_2D, m.topTexture);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::TexImage2D(gl::TEXTURE_2D, 0, 0x822A /* GL_R16 */, m.topW, m.topH, 0, gl::RED, 0x1403 /* GL_UNSIGNED_SHORT */,
                   m.top.data());
  } else {
    int w = m.topX1 - m.topX0 + 1, h = m.topZ1 - m.topZ0 + 1;
    std::vector<uint16_t> rect(size_t(w) * size_t(h));
    for (int z = 0; z < h; ++z)
      std::copy_n(&m.top[size_t(m.topZ0 + z) * size_t(m.topW) + size_t(m.topX0)], size_t(w), &rect[size_t(z) * size_t(w)]);
    gl::BindTexture(gl::TEXTURE_2D, m.topTexture);
    gl::TexSubImage2D(gl::TEXTURE_2D, 0, m.topX0, m.topZ0, w, h, gl::RED, 0x1403, rect.data());
  }
  gl::PixelStorei(gl::UNPACK_ALIGNMENT, 4);
  gl::BindTexture(gl::TEXTURE_2D, 0);
  m.topX0 = 0, m.topZ0 = 0, m.topX1 = -1, m.topZ1 = -1;
}

void surfaceMapUpload(SurfaceMap& m) {
  uploadTop(m);
  if (m.w <= 0 || m.dirtyX1 < m.dirtyX0) return;
  if (!m.texture) {
    gl::GenTextures(1, &m.texture);
    gl::BindTexture(gl::TEXTURE_2D, m.texture);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
    gl::TexImage2D(gl::TEXTURE_2D, 0, 0x8229 /* GL_R8 */, m.w, m.h, 0, gl::RED, gl::UNSIGNED_BYTE, m.cls.data());
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 4);
  } else {
    int w = m.dirtyX1 - m.dirtyX0 + 1, h = m.dirtyZ1 - m.dirtyZ0 + 1;
    std::vector<uint8_t> rect(size_t(w) * size_t(h));
    for (int z = 0; z < h; ++z)
      std::copy_n(&m.cls[size_t(m.dirtyZ0 + z) * size_t(m.w) + size_t(m.dirtyX0)], size_t(w), &rect[size_t(z) * size_t(w)]);
    gl::BindTexture(gl::TEXTURE_2D, m.texture);
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
    gl::TexSubImage2D(gl::TEXTURE_2D, 0, m.dirtyX0, m.dirtyZ0, w, h, gl::RED, gl::UNSIGNED_BYTE, rect.data());
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 4);
  }
  gl::BindTexture(gl::TEXTURE_2D, 0);
  m.dirtyX0 = 0, m.dirtyZ0 = 0, m.dirtyX1 = -1, m.dirtyZ1 = -1;
}

}  // namespace regs::engine
