// Port: procedural buildings (see proc_buildings.h).
#include "engine/proc_buildings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "engine/display.h"
#include "render/gl.h"

namespace regs::engine {

using render::MeshData;
using render::Vec3;

namespace {

void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v)), o.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& o, uint32_t v) {
  for (int k = 0; k < 4; ++k) o.push_back(uint8_t(v >> (8 * k)));
}
void putF(std::vector<uint8_t>& o, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  put32(o, v);
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
float getF(const uint8_t* p) {
  uint32_t v = get32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// one record at p (at most n bytes); its size, 0 when it does not fit
size_t readRecord(const uint8_t* p, size_t n, GridData::ProcBuilding& b) {
  if (n < 24) return 0;
  b.roof = p[0], b.style = p[1];
  const uint16_t count = uint16_t(p[2] | p[3] << 8);
  const size_t size = 24 + size_t(count) * 8;
  if (n < size || count < 3) return 0;
  b.y = getF(p + 4), b.height = getF(p + 8), b.roofHeight = getF(p + 12), b.frontX = getF(p + 16), b.frontZ = getF(p + 20);
  b.outline.clear();
  for (uint16_t i = 0; i < count; ++i) b.outline.push_back({getF(p + 24 + i * 8), getF(p + 28 + i * 8)});
  return size;
}

uint32_t hashU(uint32_t v) {
  v ^= v >> 16, v *= 0x7feb352d, v ^= v >> 15, v *= 0x846ca68b, v ^= v >> 16;
  return v;
}
float noise(int x, int y, uint32_t seed) { return float(hashU(uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ seed) & 0xFFFF) / 65535.0f; }

// ---- the textures (256 × 256, drawn here; y = 0 is the bottom row, v = 0) ----
struct Tex {
  std::vector<uint8_t> px = std::vector<uint8_t>(256 * 256 * 4);
  void set(int x, int y, float r, float g, float b) {
    uint8_t* p = px.data() + (size_t(y) * 256 + size_t(x)) * 4;
    p[0] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255), p[1] = uint8_t(std::clamp(g, 0.0f, 1.0f) * 255);
    p[2] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255), p[3] = 255;
  }
};
struct Rgb {
  float r, g, b;
};
// a window in [x0, x1] × [y0, y1]: a light frame, glass panes with a sky reflection, a cross bar, a sill under it
bool windowAt(int x, int y, int x0, int x1, int y0, int y1, Rgb& c) {
  if (x < x0 - 3 || x > x1 + 3 || y < y0 - 5 || y > y1 + 2) return false;
  if (y < y0 && y >= y0 - 5 && x >= x0 - 3 && x <= x1 + 3) return c = {0.78f, 0.77f, 0.74f}, true;  // the sill
  if (x < x0 || x > x1 || y < y0 || y > y1) return false;
  const int f = 3;  // the frame
  if (x < x0 + f || x > x1 - f || y < y0 + f || y > y1 - f || std::abs(x - (x0 + x1) / 2) < 2 || std::abs(y - (y0 + 2 * y1) / 3) < 2)
    return c = {0.92f, 0.92f, 0.90f}, true;
  const float t = float(y - y0) / float(std::max(1, y1 - y0)), s = float(x - x0) / float(std::max(1, x1 - x0));
  const float refl = 0.12f * std::max(0.0f, 1.0f - std::fabs(s + t - 1.1f) * 3.0f);  // a diagonal sheen
  c = {0.16f + 0.10f * t + refl, 0.21f + 0.12f * t + refl, 0.27f + 0.14f * t + refl};
  return true;
}
Tex hallTex() {  // a bay of a hall: corrugated metal sheets, a band of windows under the eaves, a dark base
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float rib = 0.5f + 0.5f * std::sin(float(x) * 6.2831853f * 16.0f / 256.0f);
      float k = 0.80f + 0.10f * rib + 0.03f * noise(x / 2, y / 8, 11);
      Rgb c{0.66f * k, 0.69f * k, 0.72f * k};
      if (y < 14) c = {0.32f, 0.33f, 0.34f};                                         // the plinth
      if (y > 244) c = {0.45f, 0.47f, 0.50f};                                        // the eaves' trim
      if (y >= 170 && y <= 222) {                                                    // the window band
        const bool post = (x % 64) < 4 || y < 174 || y > 218;
        const float tt = float(y - 170) / 52.0f;
        c = post ? Rgb{0.85f, 0.86f, 0.86f} : Rgb{0.18f + 0.12f * tt, 0.24f + 0.13f * tt, 0.30f + 0.15f * tt};
      }
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex hallDoorTex() {  // a roller door: horizontal slats in a frame
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      Rgb c;
      if (x < 14 || x > 241 || y > 236) c = {0.30f, 0.32f, 0.35f};
      else {
        const float slat = 0.85f + 0.15f * std::sin(float(y) * 6.2831853f * 20.0f / 256.0f);
        c = {0.58f * slat, 0.60f * slat, 0.62f * slat};
        if (y > 150 && y < 168 && x > 40 && x < 216) c = {0.20f, 0.25f, 0.30f};  // a row of small windows
      }
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
// a bay of one floor of a house: the wall (plaster or brick) with a window in the middle
Tex houseTex(bool brick) {
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      Rgb c;
      if (brick) {
        const int row = y / 16, off = (row & 1) ? 16 : 0;
        const bool mortar = (y % 16) < 2 || ((x + off) % 32) < 2;
        const float n = 0.85f + 0.15f * noise((x + off) / 32, row, 7);
        c = mortar ? Rgb{0.70f, 0.66f, 0.60f} : Rgb{0.58f * n, 0.27f * n, 0.20f * n};
      } else {
        const float n = 0.92f + 0.08f * noise(x / 3, y / 3, 5);
        c = {0.86f * n, 0.80f * n, 0.68f * n};
        if (y < 6) c = {0.70f * n, 0.65f * n, 0.55f * n};  // the floor's cornice
      }
      Rgb w;
      if (windowAt(x, y, 78, 178, 70, 210, w)) c = w;
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex houseDoorTex(bool brick) {  // a door with a fanlight, steps and its wall around it
  Tex t = houseTex(brick);
  for (int y = 0; y < 230; ++y)
    for (int x = 70; x < 186; ++x) {
      Rgb c;
      if (y < 12) c = {0.62f, 0.62f, 0.60f};                                           // the step
      else if (x < 78 || x > 177 || y > 222) c = {0.90f, 0.89f, 0.86f};                // the frame
      else if (y > 180) c = {0.22f, 0.28f, 0.33f};                                     // the fanlight
      else {
        const bool panel = (std::abs(x - 127) < 3) || (y > 95 && y < 101);
        c = brick ? Rgb{0.24f, 0.15f, 0.10f} : Rgb{0.18f, 0.28f, 0.22f};             // wood / painted green
        if (panel) c = {c.r * 0.7f, c.g * 0.7f, c.b * 0.7f};
        if (std::abs(x - 160) < 3 && std::abs(y - 100) < 3) c = {0.8f, 0.7f, 0.3f};  // the handle
      }
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex roofTex(int kind) {  // 0 flat (gravel), 1 tiles, 2 metal sheets
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      Rgb c;
      if (kind == 0) {
        const float n = 0.75f + 0.25f * noise(x, y, 3) * noise(x / 4, y / 4, 9);
        c = {0.27f * n, 0.27f * n, 0.26f * n};
      } else if (kind == 1) {
        const int row = y / 21, off = (row & 1) ? 16 : 0;
        const float shade = 0.75f + 0.25f * float(y % 21) / 21.0f, n = 0.9f + 0.1f * noise((x + off) / 32, row, 13);
        const bool gap = ((x + off) % 32) < 2;
        c = gap ? Rgb{0.25f, 0.12f, 0.08f} : Rgb{0.62f * shade * n, 0.26f * shade * n, 0.17f * shade * n};
      } else {
        const float rib = 0.5f + 0.5f * std::sin(float(x) * 6.2831853f * 16.0f / 256.0f);
        c = {0.55f + 0.08f * rib, 0.57f + 0.08f * rib, 0.60f + 0.08f * rib};
      }
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex trimTex() {  // plain light grey (parapets, gables of halls, roof boxes)
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float n = 0.9f + 0.1f * noise(x / 4, y / 4, 21);
      t.set(x, y, 0.62f * n, 0.62f * n, 0.60f * n);
    }
  return t;
}

uint32_t uploadTex(const Tex& t) {
  uint32_t name = 0;
  gl::GenTextures(1, &name);
  gl::BindTexture(gl::TEXTURE_2D, name);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR_MIPMAP_LINEAR);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::REPEAT);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::REPEAT);
  gl::PixelStorei(gl::UNPACK_ALIGNMENT, 4);
  gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, 256, 256, 0, gl::RGBA, gl::UNSIGNED_BYTE, t.px.data());
  gl::GenerateMipmap(gl::TEXTURE_2D);
  return name;
}

// ---- the mesh ----
struct Builder {
  std::array<MeshData, kProcMaterialCount> parts;
  void quad(int mat, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, float u0, float v0, float u1, float v1) {  // a b c d: ccw seen from n
    MeshData& m = parts[size_t(mat)];
    const uint32_t base = uint32_t(m.vertexCount());
    for (auto [p, u, v] : {std::tuple{a, u0, v0}, std::tuple{b, u1, v0}, std::tuple{c, u1, v1}, std::tuple{d, u0, v1}}) {
      m.positions.insert(m.positions.end(), {p.x, p.y, p.z});
      m.normals.insert(m.normals.end(), {n.x, n.y, n.z});
      m.uv0.insert(m.uv0.end(), {u, v});
    }
    // (the renderer culls the faces wound the other way: the game's meshes are clockwise seen from the front)
    m.indices.insert(m.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
  }
  void tri(int mat, Vec3 a, Vec3 b, Vec3 c, Vec3 n, float ua, float va, float ub, float vb, float uc, float vc) {
    MeshData& m = parts[size_t(mat)];
    const uint32_t base = uint32_t(m.vertexCount());
    for (auto [p, u, v] : {std::tuple{a, ua, va}, std::tuple{b, ub, vb}, std::tuple{c, uc, vc}}) {
      m.positions.insert(m.positions.end(), {p.x, p.y, p.z});
      m.normals.insert(m.normals.end(), {n.x, n.y, n.z});
      m.uv0.insert(m.uv0.end(), {u, v});
    }
    m.indices.insert(m.indices.end(), {base, base + 2, base + 1});
  }
  // a box (roof details) from y0 to y1 over a turned rectangle centred at c
  void box(int mat, float cx, float cz, float y0, float y1, float ux, float uz, float hl, float hw) {
    const float vx = -uz, vz = ux;
    Vec3 p[4];
    for (int i = 0; i < 4; ++i) {
      const float sa = (i == 1 || i == 2) ? hl : -hl, sb = i >= 2 ? hw : -hw;
      p[i] = {cx + ux * sa + vx * sb, y0, cz + uz * sa + vz * sb};
    }
    for (int i = 0; i < 4; ++i) {
      Vec3 a = p[i], b = p[(i + 1) % 4];
      Vec3 n = render::normalize(Vec3{b.z - a.z, 0, a.x - b.x});
      quad(mat, a, b, {b.x, y1, b.z}, {a.x, y1, a.z}, n, 0, 0, 1, 0.3f);
    }
    quad(mat, {p[0].x, y1, p[0].z}, {p[3].x, y1, p[3].z}, {p[2].x, y1, p[2].z}, {p[1].x, y1, p[1].z}, {0, 1, 0}, 0, 0, 0.5f, 0.5f);
  }
};

double signedArea(const std::vector<std::pair<float, float>>& o) {
  double a = 0;
  for (size_t i = 0, j = o.size() - 1; i < o.size(); j = i++) a += double(o[j].first) * o[i].second - double(o[i].first) * o[j].second;
  return a * 0.5;
}
bool insidePoly(const std::vector<std::pair<float, float>>& p, float x, float z) {
  bool in = false;
  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
    if ((p[i].second > z) != (p[j].second > z) && x < p[i].first + (z - p[i].second) * (p[j].first - p[i].first) / (p[j].second - p[i].second))
      in = !in;
  return in;
}
// ear clipping of a simple polygon with positive signed area (x, z); false when it gets stuck (not simple)
bool triangulate(const std::vector<std::pair<float, float>>& p, std::vector<std::array<size_t, 3>>& out) {
  std::vector<size_t> idx(p.size());
  for (size_t i = 0; i < p.size(); ++i) idx[i] = i;
  auto cross = [&](size_t a, size_t b, size_t c) {
    return (double(p[b].first) - p[a].first) * (double(p[c].second) - p[a].second) -
           (double(p[b].second) - p[a].second) * (double(p[c].first) - p[a].first);
  };
  size_t guard = 0;
  while (idx.size() > 3 && guard++ < 10000) {
    bool cut = false;
    for (size_t k = 0; k < idx.size(); ++k) {
      const size_t a = idx[(k + idx.size() - 1) % idx.size()], b = idx[k], c = idx[(k + 1) % idx.size()];
      if (cross(a, b, c) <= 1e-9) continue;  // reflex
      bool empty = true;
      for (size_t q : idx) {
        if (q == a || q == b || q == c) continue;
        if (cross(a, b, q) >= 0 && cross(b, c, q) >= 0 && cross(c, a, q) >= 0) {
          empty = false;
          break;
        }
      }
      if (!empty) continue;
      out.push_back({a, b, c});
      idx.erase(idx.begin() + std::ptrdiff_t(k));
      cut = true;
      break;
    }
    if (!cut) return false;
  }
  if (idx.size() == 3) out.push_back({idx[0], idx[1], idx[2]});
  return true;
}

}  // namespace

void writeProcBuildingSection(const GridData& g, std::vector<uint8_t>& out) {
  if (g.procBuildings.empty()) return;
  std::vector<uint8_t> body;
  put32(body, uint32_t(g.procBuildings.size()));
  for (const GridData::ProcBuilding& b : g.procBuildings) {
    std::vector<uint8_t> r = procBuildingBytes(b);
    body.insert(body.end(), r.begin(), r.end());
  }
  put32(out, kProcBuildingSectionTag);
  put32(out, uint32_t(body.size()));
  out.insert(out.end(), body.begin(), body.end());
}

void parseProcBuildingSection(const std::vector<uint8_t>& d, size_t body, size_t stop, GridData& g) {
  if (body + 4 > stop) return;
  const uint32_t count = get32(d.data() + body);
  size_t p = body + 4;
  for (uint32_t i = 0; i < count && p < stop; ++i) {
    GridData::ProcBuilding b;
    const size_t n = readRecord(d.data() + p, stop - p, b);
    if (!n) break;
    g.procBuildings.push_back(std::move(b));
    p += n;
  }
}

std::vector<uint8_t> procBuildingBytes(const GridData::ProcBuilding& b) {
  std::vector<uint8_t> o;
  o.push_back(b.roof), o.push_back(b.style);
  put16(o, uint16_t(std::min<size_t>(b.outline.size(), 0xFFFF)));
  putF(o, b.y), putF(o, b.height), putF(o, b.roofHeight), putF(o, b.frontX), putF(o, b.frontZ);
  for (size_t i = 0; i < b.outline.size() && i < 0xFFFF; ++i) putF(o, b.outline[i].first), putF(o, b.outline[i].second);
  return o;
}

bool procBuildingFromBytes(const std::vector<uint8_t>& raw, GridData::ProcBuilding& b) {
  return readRecord(raw.data(), raw.size(), b) == raw.size();
}

void buildProcBuildingMesh(const GridData::ProcBuilding& src, MeshData& out) {
  if (src.outline.size() < 3) return;
  std::vector<std::pair<float, float>> o = src.outline;
  if (signedArea(o) < 0) std::reverse(o.begin(), o.end());
  Builder bd;
  const bool hall = src.style == 0, brick = src.style == 2;
  const int wallMat = hall ? kProcHall : brick ? kProcBrick : kProcPlaster;
  const int doorMat = hall ? kProcHallDoor : brick ? kProcBrickDoor : kProcPlasterDoor;
  const float y = src.y, top = src.y + src.height, skirt = src.y - 2.5f;
  const float bay = hall ? 6.0f : 3.6f, floorH = hall ? src.height : 3.2f;
  const float fl = std::hypot(src.frontX, src.frontZ);
  const float fx = fl > 1e-6f ? src.frontX / fl : 0, fz = fl > 1e-6f ? src.frontZ / fl : 0;
  uint32_t seed = hashU(uint32_t(std::lround(o[0].first * 16)) * 31u + uint32_t(std::lround(o[0].second * 16)));
  // the walls: the facade from the skirt (under the ground on a slope) to the eaves, doors on the walls facing the street
  for (size_t i = 0; i < o.size(); ++i) {
    const auto [ax, az] = o[i];
    const auto [bx, bz] = o[(i + 1) % o.size()];
    const float len = std::hypot(bx - ax, bz - az);
    if (len < 0.05f) continue;
    const float dx = (bx - ax) / len, dz = (bz - az) / len;
    const Vec3 n{dz, 0, -dx};  // outward (counter-clockwise outline)
    const float bays = std::max(1.0f, std::round(len / bay));
    const float vTop = (top - y) / floorH, vSkirt = (skirt - y) / floorH;
    bd.quad(wallMat, {ax, skirt, az}, {bx, skirt, bz}, {bx, top, bz}, {ax, top, az}, n, 0, vSkirt, bays, vTop);
    // doors: on a wall facing the street (or on the longest walls when it is not known), every few bays, in front of it
    const bool facing = fl > 1e-6f ? (n.x * fx + n.z * fz) > 0.5f : len > 12.0f;
    if (!facing || len < bay) continue;
    const float step = hall ? 3.0f * bay : 3.0f * bay;
    const int count = std::max(1, int(len / step));
    for (int k = 0; k < count; ++k) {
      const float dw = hall ? 4.8f : 1.9f, dh = hall ? 4.6f : 3.0f;
      const float c = len * (k + 0.5f) / count;
      const float s0 = c - dw * 0.5f, s1 = c + dw * 0.5f;
      if (s0 < 0.3f || s1 > len - 0.3f) continue;
      const float ox = n.x * 0.04f, oz = n.z * 0.04f;
      if (hall)
        bd.quad(doorMat, {ax + dx * s0 + ox, y, az + dz * s0 + oz}, {ax + dx * s1 + ox, y, az + dz * s1 + oz},
                {ax + dx * s1 + ox, y + dh, az + dz * s1 + oz}, {ax + dx * s0 + ox, y + dh, az + dz * s0 + oz}, n, 0, 0, 1, 1);
      else  // the door tile covers a whole bay of the ground floor
        bd.quad(doorMat, {ax + dx * (c - bay * 0.5f) + ox, y, az + dz * (c - bay * 0.5f) + oz},
                {ax + dx * (c + bay * 0.5f) + ox, y, az + dz * (c + bay * 0.5f) + oz},
                {ax + dx * (c + bay * 0.5f) + ox, y + floorH, az + dz * (c + bay * 0.5f) + oz},
                {ax + dx * (c - bay * 0.5f) + ox, y + floorH, az + dz * (c - bay * 0.5f) + oz}, n, 0, 0, 1, 1);
    }
  }
  // the roof
  std::vector<std::array<size_t, 3>> tris;
  bool flat = src.roof == 0 || o.size() != 4;
  if (!flat) {  // gabled over a four-cornered footprint: the ridge along its long sides
    const float l01 = std::hypot(o[1].first - o[0].first, o[1].second - o[0].second);
    const float l12 = std::hypot(o[2].first - o[1].first, o[2].second - o[1].second);
    const int s = l01 >= l12 ? 0 : 1;  // the long side starts at corner s (and s + 2)
    auto P = [&](int k) { return o[size_t((k + 4) % 4)]; };
    const auto a = P(s), b = P(s + 1), c = P(s + 2), d = P(s + 3);
    const float rh = src.roofHeight > 0 ? src.roofHeight : 0.4f * std::min(l01, l12);
    const Vec3 m0{(a.first + d.first) * 0.5f, top + rh, (a.second + d.second) * 0.5f};  // the ridge's ends over the short sides
    const Vec3 m1{(b.first + c.first) * 0.5f, top + rh, (b.second + c.second) * 0.5f};
    const float lr = std::max(l01, l12), half = std::min(l01, l12) * 0.5f;
    const float slope = std::hypot(half, rh);
    auto roofN = [&](Vec3 e0, Vec3 e1, Vec3 r) {  // normal of the plane through the eave e0 e1 and the ridge point r
      Vec3 u = e1 - e0, v = r - e0;
      Vec3 nn = render::normalize(Vec3{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x});
      return nn.y < 0 ? nn * -1.0f : nn;
    };
    const Vec3 A{a.first, top, a.second}, B{b.first, top, b.second}, C{c.first, top, c.second}, D{d.first, top, d.second};
    bd.quad(kProcRoofTiles, A, B, m1, m0, roofN(A, B, m1), 0, 0, lr / 3.0f, slope / 3.0f);
    bd.quad(kProcRoofTiles, C, D, m0, m1, roofN(C, D, m0), 0, 0, lr / 3.0f, slope / 3.0f);
    // the gables
    const float vTop = (top - y) / floorH;
    const float gu = std::max(1.0f, std::round(2.0f * half / bay));  // the gable: as many bays as the wall below it
    bd.tri(wallMat, B, C, m1, render::normalize(Vec3{C.z - B.z, 0, B.x - C.x}), 0, vTop, gu, vTop, gu * 0.5f, vTop + rh / floorH);
    bd.tri(wallMat, D, A, m0, render::normalize(Vec3{A.z - D.z, 0, D.x - A.x}), 0, vTop, gu, vTop, gu * 0.5f, vTop + rh / floorH);
  } else if (triangulate(o, tris)) {
    const int roofMat = hall ? kProcRoofMetal : kProcRoofFlat;
    const float ry = top + 0.02f;
    for (auto& t : tris)
      bd.tri(roofMat, {o[t[0]].first, ry, o[t[0]].second}, {o[t[1]].first, ry, o[t[1]].second}, {o[t[2]].first, ry, o[t[2]].second},
             {0, 1, 0}, o[t[0]].first / 6, o[t[0]].second / 6, o[t[1]].first / 6, o[t[1]].second / 6, o[t[2]].first / 6, o[t[2]].second / 6);
    // the parapet: a low wall around the roof, its top a little inward
    const float ph = hall ? 0.5f : 0.8f;
    for (size_t i = 0; i < o.size(); ++i) {
      const auto [ax, az] = o[i];
      const auto [bx, bz] = o[(i + 1) % o.size()];
      const float len = std::hypot(bx - ax, bz - az);
      if (len < 0.05f) continue;
      const float dx = (bx - ax) / len, dz = (bz - az) / len;
      const Vec3 n{dz, 0, -dx};
      bd.quad(kProcTrim, {ax, top, az}, {bx, top, bz}, {bx, top + ph, bz}, {ax, top + ph, az}, n, 0, 0, len / 4, 0.2f);
      const float ix = -n.x * 0.35f, iz = -n.z * 0.35f;  // the cap, 0.35 wide, and its inner face
      bd.quad(kProcTrim, {ax, top + ph, az}, {bx, top + ph, bz}, {bx + ix, top + ph, bz + iz}, {ax + ix, top + ph, az + iz}, {0, 1, 0}, 0, 0, len / 4, 0.1f);
      bd.quad(kProcTrim, {bx + ix, top, bz + iz}, {ax + ix, top, az + iz}, {ax + ix, top + ph, az + iz}, {bx + ix, top + ph, bz + iz},
              n * -1.0f, 0, 0, len / 4, 0.2f);
    }
    // roof details: skylight strips along a hall, air conditioning boxes on the others (and a few on halls), only where
    // they lie wholly inside the outline
    float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
    for (auto& q : o) minX = std::min(minX, q.first), minZ = std::min(minZ, q.second), maxX = std::max(maxX, q.first), maxZ = std::max(maxZ, q.second);
    // the longest edge's direction for the details' alignment
    float ux = 1, uz = 0, best = 0;
    for (size_t i = 0; i < o.size(); ++i) {
      const auto [ax, az] = o[i];
      const auto [bx, bz] = o[(i + 1) % o.size()];
      const float len = std::hypot(bx - ax, bz - az);
      if (len > best) best = len, ux = (bx - ax) / len, uz = (bz - az) / len;
    }
    auto fits = [&](float cx, float cz, float hl, float hw) {
      for (int k = 0; k < 4; ++k) {
        const float sa = (k & 1) ? hl + 1.0f : -hl - 1.0f, sb = (k & 2) ? hw + 1.0f : -hw - 1.0f;
        if (!insidePoly(o, cx + ux * sa - uz * sb, cz + uz * sa + ux * sb)) return false;
      }
      return true;
    };
    const float area = float(signedArea(o));
    if (hall) {  // skylight strips every 9 units across the hall, along its long side
      const float vx = -uz, vz = ux;
      float t0 = 1e30f, t1 = -1e30f, s0 = 1e30f, s1 = -1e30f;
      for (auto& q : o) {
        const float s = q.first * ux + q.second * uz, t = q.first * vx + q.second * vz;
        s0 = std::min(s0, s), s1 = std::max(s1, s), t0 = std::min(t0, t), t1 = std::max(t1, t);
      }
      for (float t = t0 + 6.0f; t < t1 - 5.0f; t += 9.0f) {
        const float sl = (s1 - s0) - 8.0f;
        if (sl < 6) break;
        const float sc = (s0 + s1) * 0.5f, cx = ux * sc + vx * t, cz = uz * sc + vz * t;
        if (fits(cx, cz, sl * 0.5f, 1.0f)) bd.box(kProcTrim, cx, cz, top, top + 0.7f, ux, uz, sl * 0.5f, 1.0f);
      }
    }
    const int boxes = std::clamp(int(area / 250.0f), 1, hall ? 3 : 6);
    for (int k = 0, tries = 0; k < boxes && tries < 40; ++tries) {
      seed = hashU(seed + 0x9e37u);
      const float cx = minX + (maxX - minX) * float(seed & 0xFFFF) / 65535.0f;
      const float cz = minZ + (maxZ - minZ) * float(seed >> 16) / 65535.0f;
      const float hl = 1.0f + float(seed % 3) * 0.4f, hw = 0.8f;
      if (!fits(cx, cz, hl, hw)) continue;
      bd.box(kProcTrim, cx, cz, top, top + 1.0f + float((seed >> 8) % 3) * 0.3f, ux, uz, hl, hw);
      ++k;
    }
  } else {
    LOG_WARN("proc buildings: an outline of %zu points did not triangulate (no roof)", o.size());
  }
  // into the output: one submesh per material
  for (int mat = 0; mat < kProcMaterialCount; ++mat) {
    const MeshData& m = bd.parts[size_t(mat)];
    if (m.indices.empty()) continue;
    const uint32_t vbase = uint32_t(out.vertexCount());
    render::Submesh sm;
    sm.material = mat;
    sm.firstVertex = vbase;
    sm.vertexCount = uint32_t(m.vertexCount());
    sm.firstIndex = uint32_t(out.indices.size());
    sm.indexCount = uint32_t(m.indices.size());
    out.positions.insert(out.positions.end(), m.positions.begin(), m.positions.end());
    out.normals.insert(out.normals.end(), m.normals.begin(), m.normals.end());
    out.uv0.insert(out.uv0.end(), m.uv0.begin(), m.uv0.end());
    for (uint32_t i : m.indices) out.indices.push_back(i + vbase);
    out.submeshes.push_back(sm);
  }
}

void collectProcBuildings(Display& d, GridObj* grid, std::vector<render::DrawItem>& items) {
  struct State {
    std::vector<render::GpuMaterial> materials;
    render::GpuMesh mesh;
    bool built = false;
    uint64_t version = ~0ull, generation = ~0ull;
  };
  static State st;
  const GridData& g = *grid->data;
  if (g.procBuildings.empty()) return;
  if (st.materials.empty()) {  // the textures, drawn once
    const Tex texs[kProcMaterialCount] = {hallTex(), hallDoorTex(), houseTex(false), houseDoorTex(false), houseTex(true), houseDoorTex(true),
                                          roofTex(0), roofTex(1), roofTex(2), trimTex()};
    for (int k = 0; k < kProcMaterialCount; ++k) {
      render::GpuMaterial m;
      m.flags = render::kMatDiffuseMap;
      m.textures = {uploadTex(texs[k])};
      for (int c = 0; c < 3; ++c) m.ambient[c] = 0.6f, m.diffuse[c] = 0.55f;
      m.surfaceClass = render::kSurfaceOther;  // (buildings)
      st.materials.push_back(m);
    }
  }
  if (!st.built || st.version != grid->procVersion || st.generation != grid->generation) {
    if (st.built) d.r3d().destroy(st.mesh);
    st.built = false;
    MeshData m;
    for (const GridData::ProcBuilding& b : g.procBuildings) buildProcBuildingMesh(b, m);
    if (m.vertexCount()) {
      m.radius = 1e6f;
      render::computeTangents(m);
      st.mesh = d.r3d().upload(m);
      st.built = true;
    }
    st.version = grid->procVersion, st.generation = grid->generation;
  }
  if (!st.built) return;
  render::DrawItem it;
  it.mesh = &st.mesh;
  it.materials = &st.materials;
  items.push_back(it);
}

}  // namespace regs::engine
