// .object and .material loading (docs/asset-format/mesh-material.md). Mirrors gsObject's loader chain: root header,
// render mesh and lod1, vertex list with skin/uv2 children, indices, material records with external .material files.
#include "render/mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace regs::render {

namespace {

struct Bytes {
  const std::vector<uint8_t>& d;
  uint32_t u32(size_t at) const {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  }
  uint16_t u16(size_t at) const {
    uint16_t v = 0;
    if (at + 2 <= d.size()) std::memcpy(&v, d.data() + at, 2);
    return v;
  }
  float f32(size_t at) const {
    float v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  }
  std::string str(size_t at, size_t n) const {
    std::string s;
    for (size_t i = 0; i < n && at + i < d.size() && d[at + i]; ++i) s += char(d[at + i]);
    return s;
  }
  // children of a chunk body [from, end): calls fn(tag, bodyStart, bodyEnd)
  template <typename F>
  void children(size_t from, size_t end, F fn) const {
    end = std::min(end, d.size());
    for (size_t at = from; at + 8 <= end;) {
      uint32_t tag = u32(at), size = u32(at + 4);
      size_t body = at + 8, stop = std::min(end, body + size);
      fn(tag, body, stop);
      at = body + size;
    }
  }
};

std::string dirOf(const std::string& path) {
  size_t k = path.find_last_of("/\\");
  return k == std::string::npos ? std::string() : path.substr(0, k + 1);
}

// texture names resolve like script paths against the asset's folder; '/' or '~/' means the VFS root
std::string resolve(const std::string& dir, const std::string& name) {
  if (name.empty()) return name;
  if (name[0] == '/' || name[0] == '\\') return name.substr(1);
  if (name.size() > 1 && name[0] == '~' && (name[1] == '/' || name[1] == '\\')) return name.substr(2);
  return dir + name;
}

std::array<float, 4> rgba(const Bytes& b, size_t at) {
  return {b.d[at] / 255.0f, b.d[at + 1] / 255.0f, b.d[at + 2] / 255.0f, b.d[at + 3] / 255.0f};
}

// FUN_1008ec70: appearance fields of a 72-byte header plus the slot chunks in [slotsFrom, slotsEnd)
MaterialDef parseMaterial(const Bytes& b, size_t hdr, size_t slotsFrom, size_t slotsEnd, const std::string& dir) {
  MaterialDef m;
  m.name = b.str(hdr, 32);
  uint8_t b20 = b.d[hdr + 0x20], b21 = b.d[hdr + 0x21];
  m.opacity = b.d[hdr + 0x23];
  m.specularLevel = b.d[hdr + 0x24];
  m.glossiness = b.d[hdr + 0x25];
  m.selfIllumination = b.d[hdr + 0x26];
  m.reflection = b.d[hdr + 0x27];
  m.ambient = rgba(b, hdr + 0x28);
  m.diffuse = rgba(b, hdr + 0x2c);
  m.specular = rgba(b, hdr + 0x30);
  m.emissive = rgba(b, hdr + 0x34);
  if (b20 == 0x1F) m.flags |= 0x08000000;
  if (b21 & 2) m.flags |= kMatDecal;
  if (b21 & 4) m.flags |= kMatUv2Primary;
  if (b.d[hdr + 0x22]) m.flags |= kMatTwoSided;
  if (m.opacity != 255) m.flags |= kMatAlphaBlend;
  if (m.specularLevel > 4 && (m.specular[0] > 0 || m.specular[1] > 0 || m.specular[2] > 0)) m.flags |= kMatSpecular;
  if (m.reflection) m.flags |= kMatReflection;
  std::string tex[6];
  b.children(slotsFrom, slotsEnd, [&](uint32_t tag, size_t body, size_t) {
    if (tag < 0x02013110 || tag > 0x02013150 || (tag & 0xF) || b.d[body]) return;  // disabled slots are ignored
    uint8_t usage = b.d[body + 2], bits = b.d[body + 3];
    std::string name = resolve(dir, b.str(body + 8, 32));
    switch (tag) {
      case 0x02013110:
        m.flags |= kMatDiffuseMap;
        if (usage == 0 && bits == 1) m.flags |= kMatAlphaTest;
        else if (usage == 0 && bits > 1) m.flags |= kMatAlphaBlend;
        else if (usage == 1 && bits) m.flags |= kMatColorize;
        tex[0] = name;
        break;
      case 0x02013130: m.flags |= kMatSpecularMap; tex[1] = name; break;
      case 0x02013120:
        m.flags |= kMatNormalMap;
        if (usage == 1 && bits) m.flags |= kMatColorize;
        tex[2] = name;
        break;
      case 0x02013140: m.flags |= 0x20000; tex[3] = name; break;
      case 0x02013150: m.flags |= 0x100000; tex[4] = name; break;
    }
  });
  if (m.flags & kMatReflection) tex[5] = "textures/envmap.dds";
  if (m.flags & kMatDecal) m.flags &= ~kMatAlphaBlend;
  for (auto& t : tex)  // units are packed in key order
    if (!t.empty()) m.textures.push_back(t);
  return m;
}

void parseMesh(const Bytes& b, size_t from, size_t end, const std::string& dir,
               const std::function<bool(const std::string&, std::vector<uint8_t>&)>& read, MeshData& mesh,
               std::vector<MaterialDef>& materials) {
  b.children(from, end, [&](uint32_t tag, size_t body, size_t stop) {
    if (tag == 0x02011000) {
      uint32_t n = b.u32(body);
      size_t v0 = body + 4;
      if (v0 + size_t(n) * 36 > stop) return;
      mesh.positions.resize(size_t(n) * 3);
      mesh.normals.resize(size_t(n) * 3);
      mesh.uv0.resize(size_t(n) * 2);
      mesh.colors.resize(n);
      float r = 0;
      for (uint32_t i = 0; i < n; ++i) {
        size_t a = v0 + size_t(i) * 36;
        for (int k = 0; k < 3; ++k) {
          mesh.positions[i * 3 + k] = b.f32(a + 4 * k);
          mesh.normals[i * 3 + k] = b.f32(a + 12 + 4 * k);
          float p = mesh.positions[i * 3 + k];
          mesh.boundsMin[k] = i ? std::min(mesh.boundsMin[k], p) : p;
          mesh.boundsMax[k] = i ? std::max(mesh.boundsMax[k], p) : p;
        }
        mesh.colors[i] = b.u32(a + 24);
        mesh.uv0[i * 2] = b.f32(a + 28);
        mesh.uv0[i * 2 + 1] = b.f32(a + 32);
        const float* p = &mesh.positions[i * 3];
        r = std::max(r, std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
      }
      mesh.radius = r;
      b.children(v0 + size_t(n) * 36, stop, [&](uint32_t t2, size_t b2, size_t) {
        if (t2 == 0x02011200 && b.u16(b2) == 2) {  // second UV set
          mesh.uv1.resize(size_t(n) * 2);
          for (size_t i = 0; i < size_t(n) * 2; ++i) mesh.uv1[i] = b.f32(b2 + 4 + 4 * i);
        } else if (t2 == 0x02011100) {  // skin (0x10087ce0): n influences per vertex, the first min(n, 4) used
          uint32_t per = b.u16(b2);
          if (!per || b2 + 4 + size_t(n) * per * 8 > b.d.size()) return;
          mesh.skinBones.assign(size_t(n) * 4, 0);
          mesh.skinWeights.assign(size_t(n) * 4, 0.0f);
          for (uint32_t i = 0; i < n; ++i) {
            float sum = 0;
            for (uint32_t k = 0; k < std::min<uint32_t>(per, 4); ++k) {
              size_t at = b2 + 4 + (size_t(i) * per + k) * 8;
              mesh.skinBones[i * 4 + k] = b.u16(at);
              mesh.skinWeights[i * 4 + k] = b.f32(at + 4);
              sum += mesh.skinWeights[i * 4 + k];
            }
            if (sum != 0)  // renormalised to sum 1; a zero sum stays (0x10087ce0.c:45-56)
              for (int k = 0; k < 4; ++k) mesh.skinWeights[i * 4 + k] /= sum;
          }
        }
      });
    } else if (tag == 0x02012000) {
      uint32_t n = b.u32(body);
      if (body + 4 + size_t(n) * 4 > stop) return;
      mesh.indices.resize(n);
      std::memcpy(mesh.indices.data(), b.d.data() + body + 4, size_t(n) * 4);
    } else if (tag == 0x02013000) {
      b.children(body + 4, stop, [&](uint32_t t2, size_t rec, size_t recEnd) {
        if (t2 != 0x02013100 || mesh.submeshes.size() >= b.u32(body)) return;
        Submesh s;
        s.firstVertex = b.u32(rec + 0x38);
        s.vertexCount = b.u32(rec + 0x3c);
        s.firstIndex = b.u32(rec + 0x40);
        s.indexCount = b.u32(rec + 0x44);
        // appearance from <dir>/<name>.material when it exists, else the embedded record
        std::string name = b.str(rec, 32);
        auto it = std::find_if(materials.begin(), materials.end(), [&](const MaterialDef& m) { return m.file == name; });
        if (it != materials.end()) {
          s.material = int(it - materials.begin());
        } else {
          std::vector<uint8_t> ext;
          MaterialDef m;
          if (read(dir + name + ".material", ext) && ext.size() >= 8 + 72 && Bytes{ext}.u32(0) == 0x02013100) {
            Bytes e{ext};
            m = parseMaterial(e, 8, 8 + 72, std::min<size_t>(ext.size(), 8 + e.u32(4)), dir);
          } else {
            m = parseMaterial(b, rec, rec + 72, recEnd, dir);
          }
          // keep the header's name: getMaterial("foliage") must find tree01's "foliage-final.material", whose
          // header says "foliage" (the engine's lookup has no error there, the port's had)
          m.file = name;
          if (m.name.empty()) m.name = name;
          s.material = int(materials.size());
          materials.push_back(std::move(m));
        }
        mesh.submeshes.push_back(s);
      });
    }
  });
}

}  // namespace

bool loadMaterialFile(const std::string& path, const std::function<bool(const std::string&, std::vector<uint8_t>&)>& read,
                      MaterialDef& out) {
  std::vector<uint8_t> d;
  if (!read(path, d) || d.size() < 8 + 72) return false;
  Bytes b{d};
  if (b.u32(0) != 0x02013100) return false;
  out = parseMaterial(b, 8, 8 + 72, std::min<size_t>(d.size(), 8 + b.u32(4)), dirOf(path));
  return true;
}

bool loadObject(const std::string& path, const std::function<bool(const std::string&, std::vector<uint8_t>&)>& read,
                ObjectData& out, std::string* error) {
  std::vector<uint8_t> d;
  if (!read(path, d)) {
    if (error) *error = "file not found";
    return false;
  }
  Bytes b{d};
  if (d.size() < 8 + 60 || b.u32(0) != 0x02000000) {
    if (error) *error = "not an .object file";
    return false;
  }
  std::string dir = dirOf(path);
  out.name = b.str(8, 32);
  for (int k = 0; k < 3; ++k) out.nodePosition[k] = b.f32(8 + 0x20 + 4 * k);
  for (int k = 0; k < 4; ++k) out.nodeRotation[k] = b.f32(8 + 0x2c + 4 * k);
  size_t end = std::min(d.size(), size_t(8) + b.u32(4));
  b.children(8 + 60, end, [&](uint32_t tag, size_t body, size_t stop) {
    if (tag == 0x02010000) parseMesh(b, body, stop, dir, read, out.mesh, out.materials);
    else if (tag == 0x02040000) {
      parseMesh(b, body, stop, dir, read, out.lod1, out.materials);
      out.hasLod1 = true;
    }
  });
  if (out.mesh.positions.empty()) {
    if (error) *error = "no render mesh";
    return false;
  }
  computeTangents(out.mesh);
  if (out.hasLod1) computeTangents(out.lod1);
  swapPrimaryUv(out.mesh, out.materials);
  if (out.hasLod1) swapPrimaryUv(out.lod1, out.materials);
  return true;
}

void swapPrimaryUv(MeshData& m, const std::vector<MaterialDef>& materials) {
  // gsSubMesh_OGL upload (0x10049710.c:139-147, 264-288): with flag 0x01000000 and a second set, uv1 goes to texcoord 0
  // and uv0 to texcoord 1; without a second set nothing changes
  if (m.uv1.size() < m.uv0.size()) return;
  std::vector<bool> done(m.uv0.size() / 2, false);
  for (const Submesh& s : m.submeshes) {
    if (size_t(s.material) >= materials.size() || !(materials[size_t(s.material)].flags & kMatUv2Primary)) continue;
    for (size_t v = s.firstVertex; v < size_t(s.firstVertex) + s.vertexCount && v < done.size(); ++v) {
      if (done[v]) continue;
      done[v] = true;
      std::swap(m.uv0[v * 2], m.uv1[v * 2]);
      std::swap(m.uv0[v * 2 + 1], m.uv1[v * 2 + 1]);
    }
  }
}

void computeTangents(MeshData& m) {
  // gsCalculateTextureTangents (0x10048890, docs/renderer/scene-basics.md §6.6): per-triangle dP/du and dP/dv from
  // the file uvs, a handedness vote per corner, then t = normalize((N × T) × N) and w = sign of the votes (−1/0/+1)
  size_t n = m.vertexCount();
  std::vector<float> acc(n * 4, 0.0f);
  auto inv = [](float x) { return x != 0 ? 1.0f / x : 0.0f; };
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    uint32_t v0 = m.indices[i], v1 = m.indices[i + 1], v2 = m.indices[i + 2];
    if (v0 >= n || v1 >= n || v2 >= n) continue;
    const float *p0 = &m.positions[v0 * 3], *p1 = &m.positions[v1 * 3], *p2 = &m.positions[v2 * 3];
    const float *t0 = &m.uv0[v0 * 2], *t1 = &m.uv0[v1 * 2], *t2 = &m.uv0[v2 * 2];
    float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    float du1 = t1[0] - t0[0], dv1 = t1[1] - t0[1], du2 = t2[0] - t0[0], dv2 = t2[1] - t0[1];
    float a = dv1 != 0 ? inv(du2 - dv2 * du1 / dv1) : 0, b = dv2 != 0 ? inv(du1 - dv1 * du2 / dv2) : 0;
    float c = du1 != 0 ? inv(dv2 - dv1 * du2 / du1) : 0, d = du2 != 0 ? inv(dv1 - dv2 * du1 / du2) : 0;
    float T[3], B[3];
    for (int k = 0; k < 3; ++k) {
      T[k] = a * e2[k] + b * e1[k];
      B[k] = c * e2[k] + d * e1[k];
    }
    float f[3] = {e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0]};  // e2 × e1
    float g[3] = {f[1] * T[2] - f[2] * T[1], f[2] * T[0] - f[0] * T[2], f[0] * T[1] - f[1] * T[0]};          // (e2 × e1) × T
    float s = g[0] * B[0] + g[1] * B[1] + g[2] * B[2];
    float w = s > 0 ? 1.0f : s < 0 ? -1.0f : 0.0f;
    for (uint32_t v : {v0, v1, v2}) {
      for (int k = 0; k < 3; ++k) acc[v * 4 + k] += T[k];
      acc[v * 4 + 3] += w;
    }
  }
  m.tangents.assign(n * 4, 0.0f);
  for (size_t v = 0; v < n; ++v) {
    const float* N = &m.normals[v * 3];
    const float* t = &acc[v * 4];
    float x[3] = {N[1] * t[2] - N[2] * t[1], N[2] * t[0] - N[0] * t[2], N[0] * t[1] - N[1] * t[0]};  // N × T
    float o[3] = {x[1] * N[2] - x[2] * N[1], x[2] * N[0] - x[0] * N[2], x[0] * N[1] - x[1] * N[0]};  // (N × T) × N
    float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    for (int k = 0; k < 3; ++k) m.tangents[v * 4 + k] = len > 0 ? o[k] / len : 0.0f;
    m.tangents[v * 4 + 3] = t[3] > 0 ? 1.0f : t[3] < 0 ? -1.0f : 0.0f;
  }
}

}  // namespace regs::render
