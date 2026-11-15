// Port: procedural buildings (see proc_buildings.h).
#include "engine/proc_buildings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <tuple>

#include "core/log.h"
#include "engine/display.h"
#include "engine/night_lights.h"
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
float smooth01(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// ---- the textures (256 × 256, drawn here; y = 0 is the bottom row, v = 0) ----
struct Tex {
  std::vector<uint8_t> px = std::vector<uint8_t>(256 * 256 * 4);
  void set(int x, int y, float r, float g, float b) {
    uint8_t* p = px.data() + (size_t(y & 255) * 256 + size_t(x & 255)) * 4;
    p[0] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255), p[1] = uint8_t(std::clamp(g, 0.0f, 1.0f) * 255);
    p[2] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255), p[3] = 255;
  }
};
struct Rgb {
  float r, g, b;
};
Tex hallWallTex() {  // corrugated metal sheets (one tile: 6 × 6 units)
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float rib = 0.5f + 0.5f * std::sin(float(x) * 6.2831853f * 16.0f / 256.0f);
      const float k = 0.80f + 0.10f * rib + 0.03f * noise(x / 2, y / 8, 11) - 0.05f * noise(x / 64, y / 32, 4);
      t.set(x, y, 0.66f * k, 0.69f * k, 0.72f * k);
    }
  return t;
}
Tex rollerDoorTex() {  // a roller door: slats, a row of small windows
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float slat = 0.85f + 0.15f * std::sin(float(y) * 6.2831853f * 20.0f / 256.0f);
      Rgb c{0.50f * slat, 0.53f * slat, 0.57f * slat};
      if (y > 150 && y < 168 && (x % 40) > 6) c = {0.16f, 0.20f, 0.25f};
      if (y < 6) c = {0.25f, 0.25f, 0.25f};
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex plasterTex(Rgb k) {  // plaster of a tint (one tile: 3.6 × 3.2 units, a floor's cornice at the bottom)
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float n = 0.92f + 0.08f * noise(x / 3, y / 3, 5) - 0.04f * noise(x / 40, y / 40, 8);
      const float c = y < 8 ? 0.86f : 1.0f;
      t.set(x, y, k.r * n * c, k.g * n * c, k.b * n * c);
    }
  return t;
}
Tex brickTex(Rgb k) {
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const int row = y / 16, off = (row & 1) ? 16 : 0;
      const bool mortar = (y % 16) < 2 || ((x + off) % 32) < 2;
      const float n = 0.85f + 0.15f * noise((x + off) / 32, row, 7);
      Rgb c = mortar ? Rgb{0.70f, 0.66f, 0.60f} : Rgb{k.r * n, k.g * n, k.b * n};
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex panelTex(Rgb k) {  // cladding panels with open joints (a panel 1.8 × 1.6 units)
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const bool joint = (x % 128) < 3 || (y % 128) < 3;
      const float n = 0.95f + 0.05f * noise(x / 128, y / 128, 61) - 0.03f * noise(x / 8, y / 8, 62);
      Rgb c = joint ? Rgb{k.r * 0.55f, k.g * 0.55f, k.b * 0.55f} : Rgb{k.r * n, k.g * n, k.b * n};
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex spandrelTex(Rgb k) {  // an office's spandrel: smooth metal sheets with fine horizontal lines
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float n = 0.94f + 0.06f * noise(x / 64, y / 2, 71);
      const float line = (y % 32) < 1 ? 0.8f : 1.0f;
      t.set(x, y, k.r * n * line, k.g * n * line, k.b * n * line);
    }
  return t;
}
Tex signTex(Rgb k) {  // a shop's fascia: its colour, a light edge, blocky white lettering in the middle
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      Rgb c = k;
      if (y < 12 || y > 243) c = {0.85f, 0.85f, 0.85f};
      const int col = x / 14;
      const bool letter = y > 80 && y < 176 && (x % 14) < 10 && (x / 8) % 32 < 22 && (x / 8) % 32 > 3 &&
                          noise(col, (y - 80) / 24, 81) > 0.35f;
      if (letter) c = {0.97f, 0.96f, 0.92f};
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex doorLeafTex() {  // a wooden door with panels and a handle
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float grain = 0.9f + 0.1f * noise(x / 2, y / 24, 17);
      Rgb c{0.30f * grain, 0.19f * grain, 0.12f * grain};
      const bool panel = (x > 30 && x < 120 && ((y > 20 && y < 110) || (y > 140 && y < 236))) ||
                         (x > 136 && x < 226 && ((y > 20 && y < 110) || (y > 140 && y < 236)));
      if (panel) c = {c.r * 0.82f, c.g * 0.82f, c.b * 0.82f};
      if (std::abs(x - 128) < 3) c = {0.12f, 0.08f, 0.05f};
      if (std::hypot(float(x - 108), float(y - 125)) < 5) c = {0.80f, 0.70f, 0.35f};
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex gravelTex() {  // a flat roof: gravel, tar seams, a drain, weathering and moss (one tile: 8 × 8 units)
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float grain = noise(x, y, 3), pebble = noise(x / 2, y / 2, 9);
      const float patch = noise(x / 48, y / 48, 23);
      const float k = 0.62f + 0.22f * grain * pebble + 0.10f * patch;
      Rgb c{0.42f * k, 0.41f * k, 0.38f * k};
      if (pebble > 0.93f) c = {0.55f, 0.53f, 0.50f};                   // light pebbles
      if ((x % 128) < 2 || (y % 128) < 2) c = {c.r * 0.6f, c.g * 0.6f, c.b * 0.6f};  // tar seams (every 4 units)
      const float dr = std::hypot(float(x - 192), float(y - 64));
      if (dr < 9) c = dr < 5 ? Rgb{0.08f, 0.08f, 0.08f} : Rgb{0.30f, 0.30f, 0.30f};  // a drain
      if (noise(x / 32, y / 32, 31) > 0.92f) c = {c.r * 0.88f, c.g * 0.95f, c.b * 0.84f};  // moss
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex tilesTex() {
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const int row = y / 21, off = (row & 1) ? 16 : 0;
      const float shade = 0.75f + 0.25f * float(y % 21) / 21.0f, n = 0.85f + 0.15f * noise((x + off) / 32, row, 13);
      const bool gap = ((x + off) % 32) < 2;
      Rgb c = gap ? Rgb{0.25f, 0.12f, 0.08f} : Rgb{0.62f * shade * n, 0.26f * shade * n, 0.17f * shade * n};
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex metalRoofTex() {
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float rib = 0.5f + 0.5f * std::sin(float(x) * 6.2831853f * 16.0f / 256.0f);
      const float k = 0.95f - 0.08f * noise(x / 32, y / 64, 41);
      t.set(x, y, (0.50f + 0.08f * rib) * k, (0.53f + 0.08f * rib) * k, (0.57f + 0.08f * rib) * k);
    }
  return t;
}
Tex plainTex(float r, float g, float b, uint32_t seed) {  // trim (grey), frames (white): a little noise
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float n = 0.92f + 0.08f * noise(x / 4, y / 4, seed);
      t.set(x, y, r * n, g * n, b * n);
    }
  return t;
}
Tex glassTex() {  // glass: dark with a sky sheen, curtains at the sides (lit panes glow through the emission)
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float s = float(x) / 255, v = float(y) / 255;
      const float sheen = 0.10f * std::max(0.0f, 1.0f - std::fabs(s + v - 1.1f) * 3.0f);
      Rgb c{0.14f + 0.08f * v + sheen, 0.18f + 0.10f * v + sheen, 0.24f + 0.12f * v + sheen};
      if (s < 0.14f || s > 0.86f) c = {c.r * 1.6f + 0.05f, c.g * 1.5f + 0.04f, c.b * 1.3f};  // curtains
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex solarTex() {  // solar cells in a grid
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const bool line = (x % 32) < 2 || (y % 32) < 2;
      const bool frame = x < 6 || x > 249 || y < 6 || y > 249;
      Rgb c = frame ? Rgb{0.70f, 0.72f, 0.74f} : line ? Rgb{0.45f, 0.50f, 0.58f} : Rgb{0.08f, 0.12f, 0.24f + 0.06f * noise(x, y, 2)};
      t.set(x, y, c.r, c.g, c.b);
    }
  return t;
}
Tex grilleTex() {  // an air conditioner's top: a round fan behind a grille
  Tex t;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const float r = std::hypot(float(x - 128), float(y - 128));
      Rgb c{0.62f, 0.63f, 0.64f};
      if (r < 100) c = (int(r) % 12) < 3 ? Rgb{0.70f, 0.71f, 0.72f} : Rgb{0.12f, 0.12f, 0.13f};
      if (r < 18) c = {0.40f, 0.40f, 0.42f};
      t.set(x, y, c.r, c.g, c.b);
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
Vec3 cross3(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
struct Builder {
  std::array<MeshData, kProcMaterialCount> parts;
  // a quad facing n (corners a b c d around it in either order), uv (u0, v0) at a … (u0, v1) at d
  void quad(int mat, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, float u0, float v0, float u1, float v1) {
    MeshData& m = parts[size_t(mat)];
    const uint32_t base = uint32_t(m.vertexCount());
    for (auto [p, u, v] : {std::tuple{a, u0, v0}, std::tuple{b, u1, v0}, std::tuple{c, u1, v1}, std::tuple{d, u0, v1}}) {
      m.positions.insert(m.positions.end(), {p.x, p.y, p.z});
      m.normals.insert(m.normals.end(), {n.x, n.y, n.z});
      m.uv0.insert(m.uv0.end(), {u, v});
    }
    // front faces are wound counter-clockwise as seen from their front (the renderer culls the others)
    if (dot3(cross3(b - a, c - a), n) > 0) m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    else m.indices.insert(m.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
  }
  void tri(int mat, Vec3 a, Vec3 b, Vec3 c, Vec3 n, float ua, float va, float ub, float vb, float uc, float vc) {
    MeshData& m = parts[size_t(mat)];
    const uint32_t base = uint32_t(m.vertexCount());
    for (auto [p, u, v] : {std::tuple{a, ua, va}, std::tuple{b, ub, vb}, std::tuple{c, uc, vc}}) {
      m.positions.insert(m.positions.end(), {p.x, p.y, p.z});
      m.normals.insert(m.normals.end(), {n.x, n.y, n.z});
      m.uv0.insert(m.uv0.end(), {u, v});
    }
    if (dot3(cross3(b - a, c - a), n) > 0) m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    else m.indices.insert(m.indices.end(), {base, base + 2, base + 1});
  }
  // a box from y0 to y1 over a turned rectangle (half sizes hl along u, hw across), its top in topMat
  void box(int mat, int topMat, float cx, float cz, float y0, float y1, float ux, float uz, float hl, float hw) {
    const float vx = -uz, vz = ux;
    Vec3 p[4];
    for (int i = 0; i < 4; ++i) {
      const float sa = (i == 1 || i == 2) ? hl : -hl, sb = i >= 2 ? hw : -hw;
      p[i] = {cx + ux * sa + vx * sb, y0, cz + uz * sa + vz * sb};
    }
    for (int i = 0; i < 4; ++i) {
      Vec3 a = p[i], b = p[(i + 1) % 4];
      Vec3 mid{(a.x + b.x) * 0.5f - cx, 0, (a.z + b.z) * 0.5f - cz};
      quad(mat, a, b, {b.x, y1, b.z}, {a.x, y1, a.z}, render::normalize(mid), 0, 0, 1, 0.3f);
    }
    quad(topMat, {p[0].x, y1, p[0].z}, {p[1].x, y1, p[1].z}, {p[2].x, y1, p[2].z}, {p[3].x, y1, p[3].z}, {0, 1, 0}, 0, 0, 1, 1);
  }
  // an upright prism of `sides` around (cx, cz) (vent pipes)
  void pipe(int mat, float cx, float cz, float y0, float y1, float r, int sides) {
    for (int k = 0; k < sides; ++k) {
      const float a0 = 6.2831853f * float(k) / float(sides), a1 = 6.2831853f * float(k + 1) / float(sides);
      Vec3 p0{cx + r * std::cos(a0), y0, cz + r * std::sin(a0)}, p1{cx + r * std::cos(a1), y0, cz + r * std::sin(a1)};
      const float am = (a0 + a1) * 0.5f;
      quad(mat, p0, p1, {p1.x, y1, p1.z}, {p0.x, y1, p0.z}, {std::cos(am), 0, std::sin(am)}, 0, 0, 0.2f, 0.3f);
      tri(mat, {cx, y1, cz}, {p0.x, y1, p0.z}, {p1.x, y1, p1.z}, {0, 1, 0}, 0, 0, 0.1f, 0, 0, 0.1f);
    }
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

// a wall's frame: the point at s along it from (ax, az), height y, depth t into the building
struct WallFrame {
  float ax, az, dx, dz;
  Vec3 n;  // outward
  Vec3 at(float s, float y, float t) const { return {ax + dx * s - n.x * t, y, az + dz * s - n.z * t}; }
};

// a bay of a floor with an opening: the wall around it, the reveals (frameMat), the pane or door at the back (depth),
// mullions just in front of the pane, a sill under it
void opening(Builder& bd, const WallFrame& w, int wallMat, float uScale, float vScale, float yBase, float bay0, float bayW, float y0,
             float fh, float s0, float s1, float o0, float o1, float depth, int paneMat, int frameMat, bool sill, int mullionsV,
             int mullionsH, float paneU1 = 1, float paneV1 = 1) {
  auto wq = [&](float a0, float a1, float b0, float b1) {  // bay-local [a0, a1] × [b0, b1] of the wall
    if (a1 - a0 < 1e-3f || b1 - b0 < 1e-3f) return;
    bd.quad(wallMat, w.at(bay0 + a0, y0 + b0, 0), w.at(bay0 + a1, y0 + b0, 0), w.at(bay0 + a1, y0 + b1, 0), w.at(bay0 + a0, y0 + b1, 0), w.n,
            (bay0 + a0) / uScale, (y0 + b0 - yBase) / vScale, (bay0 + a1) / uScale, (y0 + b1 - yBase) / vScale);
  };
  wq(0, s0, 0, fh), wq(s1, bayW, 0, fh), wq(s0, s1, 0, o0), wq(s0, s1, o1, fh);
  const Vec3 along{w.dx, 0, w.dz};
  const float A = bay0 + s0, B = bay0 + s1, Y0 = y0 + o0, Y1 = y0 + o1;
  bd.quad(frameMat, w.at(A, Y0, 0), w.at(A, Y0, depth), w.at(A, Y1, depth), w.at(A, Y1, 0), along, 0, 0, 0.1f, 1);
  bd.quad(frameMat, w.at(B, Y0, 0), w.at(B, Y0, depth), w.at(B, Y1, depth), w.at(B, Y1, 0), along * -1.0f, 0, 0, 0.1f, 1);
  bd.quad(frameMat, w.at(A, Y1, 0), w.at(B, Y1, 0), w.at(B, Y1, depth), w.at(A, Y1, depth), {0, -1, 0}, 0, 0, 1, 0.1f);
  bd.quad(frameMat, w.at(A, Y0, 0), w.at(B, Y0, 0), w.at(B, Y0, depth), w.at(A, Y0, depth), {0, 1, 0}, 0, 0, 1, 0.1f);
  bd.quad(paneMat, w.at(A, Y0, depth), w.at(B, Y0, depth), w.at(B, Y1, depth), w.at(A, Y1, depth), w.n, 0, 0, paneU1, paneV1);
  const float md = depth - 0.03f, mw = 0.05f;
  for (int k = 1; k <= mullionsV; ++k) {
    const float s = A + (B - A) * float(k) / float(mullionsV + 1);
    bd.quad(frameMat, w.at(s - mw, Y0, md), w.at(s + mw, Y0, md), w.at(s + mw, Y1, md), w.at(s - mw, Y1, md), w.n, 0, 0, 0.1f, 1);
  }
  for (int k = 1; k <= mullionsH; ++k) {
    const float yy = Y0 + (Y1 - Y0) * (mullionsH == 1 ? 0.66f : float(k) / float(mullionsH + 1));
    bd.quad(frameMat, w.at(A, yy - mw, md), w.at(B, yy - mw, md), w.at(B, yy + mw, md), w.at(A, yy + mw, md), w.n, 0, 0, 1, 0.1f);
  }
  if (sill) {
    const float e = 0.10f, out = 0.14f, th = 0.07f;
    bd.quad(frameMat, w.at(A - e, Y0, -out), w.at(B + e, Y0, -out), w.at(B + e, Y0, 0), w.at(A - e, Y0, 0), {0, 1, 0}, 0, 0, 1, 0.1f);
    bd.quad(frameMat, w.at(A - e, Y0 - th, -out), w.at(B + e, Y0 - th, -out), w.at(B + e, Y0, -out), w.at(A - e, Y0, -out), w.n, 0, 0, 1, 0.1f);
  }
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
  const int type = src.style;  // 0 hall, 1 plaster, 2 brick, 3 office, 4 shop
  const bool hall = type == 0, brick = type == 2, office = type == 3, shop = type == 4;
  uint32_t seed = hashU(uint32_t(std::lround(o[0].first * 16)) * 31u + uint32_t(std::lround(o[0].second * 16)));
  const uint32_t var = hashU(seed ^ 0xFACADEu);  // the facade variant
  const bool bigBox = shop && src.height <= 7.5f;  // a big low store: one tall floor, an entrance front, its sign
  const bool punched = office && ((var >> 5) & 1);  // an office of panels with single windows, else ribbons and fins
  const int winStyle = int((var >> 6) % 4);         // houses: cross windows, tall ones, wide ones, pairs
  const bool cornice = !hall && !office && !bigBox && ((var >> 8) & 1);
  int wallMat, groundMat, frameMat = (var >> 4) % 3 == 0 ? kProcFrameDark : kProcFrame;
  const int signMat = kProcSign + int((var >> 9) % 4);
  if (hall) wallMat = groundMat = kProcHall;
  else if (office) wallMat = groundMat = punched ? kProcPanel + int((var >> 2) % 3) : kProcSpandrel + int(var % 3), frameMat = kProcFrameDark;
  else if (shop) wallMat = groundMat = ((var >> 5) & 1) || bigBox ? kProcPanel + int((var >> 2) % 3) : kProcPlaster + int(var % 4), frameMat = kProcFrameDark;
  else if (brick) wallMat = groundMat = kProcBrick + int(var % 3);
  else wallMat = kProcPlaster + int(var % 4), groundMat = ((var >> 3) & 1) ? kProcTrim : wallMat;
  // (the skirt reaches 12 down: on a slope the walls go into the ground, no floating wing)
  const float y = src.y, top = src.y + src.height, skirt = src.y - 12.0f;
  const float bay = hall ? 6.0f : 3.6f;
  const float uScale = hall ? 6.0f : office ? 3.2f : 3.6f, vScale = hall ? 6.0f : office ? 3.6f : 3.2f;
  const float fl = std::hypot(src.frontX, src.frontZ);
  const float fx = fl > 1e-6f ? src.frontX / fl : 0, fz = fl > 1e-6f ? src.frontZ / fl : 0;
  // the floors: halls and big stores one; offices and shops a tall ground floor (4.2) and floors of 3.6 / 3.2; houses 3.2
  std::vector<std::pair<float, float>> floors;  // bottom, height
  if (hall || bigBox) {
    floors.push_back({y, src.height});
  } else {
    const float first = office || shop ? 4.2f : 3.2f, rest = office ? 3.6f : 3.2f;
    for (float y0 = y; floors.size() < 60;) {
      const float hgt = floors.empty() ? first : rest;
      if (y0 + hgt > top + 0.3f) break;
      floors.push_back({y0, std::min(hgt, top - y0)});
      y0 += hgt;
    }
    if (floors.empty()) floors.push_back({y, src.height});
  }
  const float floorsTop = floors.back().first + floors.back().second;
  uint32_t windowNo = 0;
  auto paneFor = [&]() {  // dark (a third) or one of the three lit groups (they switch on one after another at dusk)
    const uint32_t hh = hashU(seed ^ (windowNo++ * 0x9e3779b9u));
    return (hh % 100) < 32 ? int(kProcGlass) : int(kProcGlassLit0) + int((hh >> 8) % 3);
  };
  // a band standing out of the wall over its whole length (cornices, shop fascias): front, top and underside
  auto band = [&](const WallFrame& w, float len, float s0, float s1, float yy, float hgt, float out, int frontMat, int edgeMat) {
    bd.quad(frontMat, w.at(s0, yy, -out), w.at(s1, yy, -out), w.at(s1, yy + hgt, -out), w.at(s0, yy + hgt, -out), w.n, s0 / 4.0f, 0, s1 / 4.0f, 1);
    bd.quad(edgeMat, w.at(s0, yy + hgt, -out), w.at(s1, yy + hgt, -out), w.at(s1, yy + hgt, 0), w.at(s0, yy + hgt, 0), {0, 1, 0}, 0, 0, len / 4, 0.1f);
    bd.quad(edgeMat, w.at(s0, yy, 0), w.at(s1, yy, 0), w.at(s1, yy, -out), w.at(s0, yy, -out), {0, -1, 0}, 0, 0, len / 4, 0.1f);
    const Vec3 al{w.dx, 0, w.dz};
    bd.quad(edgeMat, w.at(s0, yy, 0), w.at(s0, yy, -out), w.at(s0, yy + hgt, -out), w.at(s0, yy + hgt, 0), al * -1.0f, 0, 0, 0.1f, 0.1f);
    bd.quad(edgeMat, w.at(s1, yy, 0), w.at(s1, yy, -out), w.at(s1, yy + hgt, -out), w.at(s1, yy + hgt, 0), al, 0, 0, 0.1f, 0.1f);
  };
  // ---- the walls: per floor and bay an opening in a recess, doors on the walls facing the street ----
  for (size_t i = 0; i < o.size(); ++i) {
    auto [ax, az] = o[i];
    auto [bx, bz] = o[(i + 1) % o.size()];
    const float len = std::hypot(bx - ax, bz - az);
    if (len < 0.05f) continue;
    {  // the outward side must lie outside (a part of an outline winding the other way: the wall turned round)
      const float mx = (ax + bx) * 0.5f + (bz - az) / len * 0.25f, mz = (az + bz) * 0.5f - (bx - ax) / len * 0.25f;
      if (insidePoly(o, mx, mz)) std::swap(ax, bx), std::swap(az, bz);
    }
    const WallFrame w{ax, az, (bx - ax) / len, (bz - az) / len, {(bz - az) / len, 0, -(bx - ax) / len}};
    bd.quad(groundMat, w.at(0, skirt, 0), w.at(len, skirt, 0), w.at(len, y, 0), w.at(0, y, 0), w.n, 0, (skirt - y) / vScale, len / uScale, 0);
    if (top - floorsTop > 0.01f)
      bd.quad(wallMat, w.at(0, floorsTop, 0), w.at(len, floorsTop, 0), w.at(len, top, 0), w.at(0, top, 0), w.n, 0, (floorsTop - y) / vScale,
              len / uScale, (top - y) / vScale);
    const bool facing = fl > 1e-6f ? (w.n.x * fx + w.n.z * fz) > 0.5f : len > 12.0f;
    for (size_t f = 0; f < floors.size(); ++f) {
      const float y0 = floors[f].first, fh = floors[f].second;
      const bool ground = f == 0;
      const int mat = ground ? groundMat : wallMat;
      const float bayT = hall ? 6.0f : bigBox ? 3.0f : office ? (punched ? 2.0f : 1.6f) : (shop && ground) ? 3.0f : bay;
      const int bays = std::max(1, int(std::round(len / bayT)));
      const float bw = len / float(bays);
      const int doors = facing && ground ? std::max(1, bays / (office ? 7 : shop ? 3 : 4)) : 0;
      auto isDoorBay = [&](int k) {
        for (int dd = 0; dd < doors; ++dd)
          if (k == int((float(dd) + 0.5f) * float(bays) / float(doors))) return true;
        return false;
      };
      for (int k = 0; k < bays; ++k) {
        const float b0 = float(k) * bw;
        auto plain = [&]() {
          bd.quad(mat, w.at(b0, y0, 0), w.at(b0 + bw, y0, 0), w.at(b0 + bw, y0 + fh, 0), w.at(b0, y0 + fh, 0), w.n, b0 / uScale,
                  (y0 - y) / vScale, (b0 + bw) / uScale, (y0 - y + fh) / vScale);
        };
        // a window centred in the bay (or in each half of it), its mullions and sill
        auto window = [&](float ww, float o0, float o1, float depth, int mV, int mH, bool sill, int pane) {
          if (ww > bw - 0.3f || o1 > fh - 0.1f) return plain();
          const float s0 = (bw - ww) * 0.5f;
          opening(bd, w, mat, uScale, vScale, y, b0, bw, y0, fh, s0, s0 + ww, o0, o1, depth, pane, frameMat, sill, mV, mH);
        };
        if (bw < 1.2f) {  // too narrow for an opening
          plain();
          continue;
        }
        if (hall) {
          if (isDoorBay(k) && bw >= 5.4f && fh >= 5.5f) {  // a roller door in its recess, a lamp over it
            const float dw = std::min(4.8f, bw - 0.6f), s0 = (bw - dw) * 0.5f;
            opening(bd, w, mat, uScale, vScale, y, b0, bw, y0, fh, s0, s0 + dw, 0, 4.6f, 0.35f, kProcHallDoor, kProcTrim, false, 0, 0);
            const Vec3 c = w.at(b0 + bw * 0.5f, y0, -0.14f);
            bd.box(kProcTrim, kProcGlassLit0, c.x, c.z, y0 + 4.8f, y0 + 5.0f, w.dx, w.dz, 0.25f, 0.12f);
          } else if (fh >= 5.0f) {  // the window band under the eaves
            const float o1 = fh - 0.9f, o0 = o1 - 1.6f;
            opening(bd, w, mat, uScale, vScale, y, b0, bw, y0, fh, 0.3f, bw - 0.3f, o0, o1, 0.14f, paneFor(), kProcTrim, false,
                    std::max(1, int(bw / 1.6f)), 0, (bw - 0.6f) / 2.0f, 1);
          } else {
            plain();
          }
          continue;
        }
        if (bigBox) {  // the store's front: an entrance of glass around the door bays, else cladding
          const bool front = facing && (isDoorBay(k) || isDoorBay(k - 1) || isDoorBay(k + 1));
          if (front && fh >= 4.5f)
            opening(bd, w, mat, uScale, vScale, y, b0, bw, y0, fh, 0.1f, bw - 0.1f, 0.0f, std::min(3.8f, fh - 1.9f), 0.3f, kProcGlassLit0,
                    kProcFrameDark, false, isDoorBay(k) ? 1 : 2, 1);
          else
            plain();
          continue;
        }
        if (shop && ground) {
          if (facing) {  // shop windows (lit at dusk) and their doors
            const bool door = isDoorBay(k);
            opening(bd, w, mat, uScale, vScale, y, b0, bw, y0, fh, 0.15f, bw - 0.15f, door ? 0.0f : 0.35f, 3.05f, 0.25f, kProcGlassLit0,
                    kProcFrameDark, false, door ? 1 : 1, door ? 1 : 0);
          } else {
            window(1.4f, 1.0f, 2.6f, 0.2f, 1, 0, false, paneFor());
          }
          continue;
        }
        if (office) {
          if (ground) {  // the lobby: glass from floor to ceiling, its doors lit
            opening(bd, w, mat, uScale, vScale, y, b0, bw, y0, fh, 0.08f, bw - 0.08f, 0.15f, std::min(3.7f, fh - 0.3f), 0.25f,
                    isDoorBay(k) ? kProcGlassLit0 : paneFor(), kProcFrameDark, false, 0, 1);
          } else if (punched) {
            window(1.2f, 1.0f, 2.8f, 0.2f, 0, 0, false, paneFor());
          } else {  // ribbon windows between fins
            opening(bd, w, mat, uScale, vScale, y, b0, bw, y0, fh, 0.08f, bw - 0.08f, 0.9f, fh - 0.2f, 0.18f, paneFor(), kProcFrameDark, false, 0,
                    0);
          }
          continue;
        }
        if (shop) {  // the shop's upper floors: single windows without sills
          window(1.4f, 0.8f, 2.5f, 0.18f, 1, 0, false, paneFor());
          continue;
        }
        if (ground && isDoorBay(k)) {  // a front door in its recess, a lit fanlight over it, a lamp beside it, a step
          const float dw = 1.4f, s0 = (bw - dw) * 0.5f;
          opening(bd, w, mat, uScale, vScale, y, b0, bw, y0, fh, s0, s0 + dw, 0.0f, 2.45f, 0.30f, kProcDoor, frameMat, false, 0, 0);
          bd.quad(kProcGlassLit0, w.at(b0 + s0 + 0.1f, y0 + 2.55f, -0.01f), w.at(b0 + s0 + dw - 0.1f, y0 + 2.55f, -0.01f),
                  w.at(b0 + s0 + dw - 0.1f, y0 + 2.9f, -0.01f), w.at(b0 + s0 + 0.1f, y0 + 2.9f, -0.01f), w.n, 0, 0, 1, 0.3f);
          const Vec3 c = w.at(b0 + s0 + dw + 0.35f, y0, -0.1f);
          bd.box(kProcTrim, kProcGlassLit0, c.x, c.z, y0 + 2.05f, y0 + 2.4f, w.dx, w.dz, 0.09f, 0.09f);
          bd.quad(kProcTrim, w.at(b0 + s0 - 0.15f, y0 + 0.15f, -0.45f), w.at(b0 + s0 + dw + 0.15f, y0 + 0.15f, -0.45f),
                  w.at(b0 + s0 + dw + 0.15f, y0 + 0.15f, 0.3f), w.at(b0 + s0 - 0.15f, y0 + 0.15f, 0.3f), {0, 1, 0}, 0, 0, 1, 0.3f);
          bd.quad(kProcTrim, w.at(b0 + s0 - 0.15f, y0 - 0.3f, -0.45f), w.at(b0 + s0 + dw + 0.15f, y0 - 0.3f, -0.45f),
                  w.at(b0 + s0 + dw + 0.15f, y0 + 0.15f, -0.45f), w.at(b0 + s0 - 0.15f, y0 + 0.15f, -0.45f), w.n, 0, 0, 1, 0.1f);
          continue;
        }
        switch (winStyle) {  // the house's windows
          case 0: window(1.5f, 0.85f, 2.65f, 0.22f, 1, 1, true, paneFor()); break;  // cross windows
          case 1: window(1.15f, 0.6f, 2.8f, 0.24f, 1, 1, true, paneFor()); break;    // tall ones with a transom
          case 2: window(1.9f, 0.95f, 2.55f, 0.2f, 2, 0, true, paneFor()); break;    // wide ones
          default:                                                                    // pairs
            if (bw < 3.0f) {
              window(1.2f, 0.8f, 2.7f, 0.22f, 0, 1, true, paneFor());
            } else {
              const float hb = bw * 0.5f;
              for (int p = 0; p < 2; ++p)
                opening(bd, w, mat, uScale, vScale, y, b0 + hb * float(p), hb, y0, fh, (hb - 0.75f) * 0.5f, (hb + 0.75f) * 0.5f, 0.8f, 2.7f,
                        0.22f, paneFor(), frameMat, true, 0, 1);
            }
        }
      }
      if (cornice && f + 1 < floors.size()) band(w, len, -0.15f, len + 0.15f, y0 + fh - 0.12f, 0.22f, 0.15f, kProcFrame, kProcFrame);
    }
    // the shop's fascia over its windows (its sign), the big store's sign high on its front
    if (shop && facing && !bigBox) band(w, len, 0.1f, len - 0.1f, y + 3.3f, 0.75f, 0.18f, signMat, kProcTrim);
    if (bigBox && facing && len > 8) band(w, len, len * 0.25f, len * 0.75f, top - 1.7f, 1.1f, 0.15f, signMat, kProcTrim);
  }
  // ---- the roof ----
  float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
  for (auto& q : o) minX = std::min(minX, q.first), minZ = std::min(minZ, q.second), maxX = std::max(maxX, q.first), maxZ = std::max(maxZ, q.second);
  float ux = 1, uz = 0, best = 0;  // the longest edge's direction: the roof details line up with it
  for (size_t i = 0; i < o.size(); ++i) {
    const auto [ax, az] = o[i];
    const auto [bx, bz] = o[(i + 1) % o.size()];
    const float len = std::hypot(bx - ax, bz - az);
    if (len > best) best = len, ux = (bx - ax) / len, uz = (bz - az) / len;
  }
  const float vx = -uz, vz = ux;
  auto fits = [&](float cx, float cz, float hl, float hw, float margin) {
    for (int k = 0; k < 4; ++k) {
      const float sa = (k & 1) ? hl + margin : -hl - margin, sb = (k & 2) ? hw + margin : -hw - margin;
      if (!insidePoly(o, cx + ux * sa + vx * sb, cz + uz * sa + vz * sb)) return false;
    }
    return true;
  };
  std::vector<std::array<float, 3>> taken;  // roof details placed (x, z, radius): they keep apart
  auto freeAt = [&](float cx, float cz, float r) {
    for (auto& t : taken)
      if (std::hypot(cx - t[0], cz - t[1]) < r + t[2]) return false;
    return true;
  };
  auto rnd = [&]() {
    seed = hashU(seed + 0x9e37u);
    return float(seed & 0xFFFF) / 65535.0f;
  };
  std::vector<std::array<size_t, 3>> tris;
  if (src.roof == 1 && o.size() == 4) {  // gabled: the ridge along the long sides, roof windows, chimneys
    const float l01 = std::hypot(o[1].first - o[0].first, o[1].second - o[0].second);
    const float l12 = std::hypot(o[2].first - o[1].first, o[2].second - o[1].second);
    const int s = l01 >= l12 ? 0 : 1;
    auto P = [&](int k) { return o[size_t((k + 4) % 4)]; };
    const auto a = P(s), b = P(s + 1), c = P(s + 2), d = P(s + 3);
    const float half = std::min(l01, l12) * 0.5f, lr = std::max(l01, l12);
    const float rh = src.roofHeight > 0 ? src.roofHeight : 0.8f * half;
    const Vec3 m0{(a.first + d.first) * 0.5f, top + rh, (a.second + d.second) * 0.5f};
    const Vec3 m1{(b.first + c.first) * 0.5f, top + rh, (b.second + c.second) * 0.5f};
    const float slope = std::hypot(half, rh);
    const Vec3 A{a.first, top, a.second}, B{b.first, top, b.second}, C{c.first, top, c.second}, D{d.first, top, d.second};
    auto planeN = [&](Vec3 e0, Vec3 e1, Vec3 r) {
      Vec3 nn = render::normalize(cross3(e1 - e0, r - e0));
      return nn.y < 0 ? nn * -1.0f : nn;
    };
    const Vec3 nAB = planeN(A, B, m1), nCD = planeN(C, D, m0);
    bd.quad(kProcRoofTiles, A, B, m1, m0, nAB, 0, 0, lr / 3.0f, slope / 3.0f);
    bd.quad(kProcRoofTiles, C, D, m0, m1, nCD, 0, 0, lr / 3.0f, slope / 3.0f);
    const float vTop = (top - y) / vScale, gu = std::max(1.0f, std::round(2.0f * half / bay));
    bd.tri(wallMat, B, C, m1, render::normalize(Vec3{C.z - B.z, 0, B.x - C.x}), 0, vTop, gu, vTop, gu * 0.5f, vTop + rh / vScale);
    bd.tri(wallMat, D, A, m0, render::normalize(Vec3{A.z - D.z, 0, D.x - A.x}), 0, vTop, gu, vTop, gu * 0.5f, vTop + rh / vScale);
    // solar panels on the south-facing slope of some houses (flush over the tiles), no roof windows there
    const int solarSide = (hashU(seed ^ 0x50a7u) % 100) < 40 ? (nAB.z >= nCD.z ? 0 : 1) : -1;
    if (solarSide >= 0) {
      const Vec3 e0 = solarSide ? C : A, e1 = solarSide ? D : B, nn = solarSide ? nCD : nAB;
      const Vec3 along = render::normalize(e1 - e0);
      Vec3 up = render::normalize(cross3(nn, along));
      if (up.y < 0) up = up * -1.0f;
      for (float sl = slope * 0.2f; sl + 1.6f < slope * 0.7f; sl += 1.7f)
        for (float al = lr * 0.25f; al + 1.0f < lr * 0.75f; al += 1.05f) {
          const Vec3 p0 = e0 + along * al + up * sl + nn * 0.05f;
          bd.quad(kProcSolar, p0, p0 + along * 1.0f, p0 + along * 1.0f + up * 1.6f, p0 + up * 1.6f, nn, 0, 0, 1, 1);
        }
    }
    for (int side = 0; side < 2; ++side) {  // roof windows: framed panes just over the tiles, lit at night like windows
      if (side == solarSide) continue;
      const Vec3 e0 = side ? C : A, e1 = side ? D : B, nn = side ? nCD : nAB;
      const Vec3 along = render::normalize(e1 - e0);
      Vec3 toRidge = render::normalize(cross3(nn, along));
      if (toRidge.y < 0) toRidge = toRidge * -1.0f;
      const int count = std::max(1, int(lr / 6.0f));
      for (int k = 0; k < count; ++k) {
        if (rnd() < 0.25f) continue;
        const Vec3 c0 = e0 + (e1 - e0) * ((float(k) + 0.5f) / float(count)) + toRidge * (slope * 0.35f) + nn * 0.04f;
        const float hw = 0.45f, hh = 0.6f, fr = 0.08f;
        const Vec3 p0 = c0 - along * hw - toRidge * hh, p1 = c0 + along * hw - toRidge * hh, p2 = c0 + along * hw + toRidge * hh,
                   p3 = c0 - along * hw + toRidge * hh;
        bd.quad(kProcFrame, p0 - along * fr - toRidge * fr, p1 + along * fr - toRidge * fr, p2 + along * fr + toRidge * fr,
                p3 - along * fr + toRidge * fr, nn, 0, 0, 1, 1);
        bd.quad(paneFor(), p0 + nn * 0.01f, p1 + nn * 0.01f, p2 + nn * 0.01f, p3 + nn * 0.01f, nn, 0, 0, 1, 1);
      }
    }
    for (int k = 0, n = 1 + int(rnd() * 2.0f); k < n; ++k) {  // brick chimneys on the ridge
      const Vec3 c0 = m0 + (m1 - m0) * (0.2f + 0.6f * rnd());
      bd.box(kProcBrick, kProcTrim, c0.x, c0.z, top + rh * 0.4f, top + rh + 0.9f, (m1.x - m0.x) / lr, (m1.z - m0.z) / lr, 0.35f, 0.35f);
    }
  } else if (triangulate(o, tris)) {
    const int roofMat = hall ? kProcRoofMetal : kProcRoofFlat;
    const float ry = top + 0.02f, us = hall ? 6.0f : 8.0f;
    // the roof's texture along the building (owner 2026-11-15: it did not follow the building's angle)
    auto ru = [&](size_t k) { return (o[k].first * ux + o[k].second * uz) / us; };
    auto rv = [&](size_t k) { return (o[k].first * vx + o[k].second * vz) / us; };
    for (auto& t : tris)
      bd.tri(roofMat, {o[t[0]].first, ry, o[t[0]].second}, {o[t[1]].first, ry, o[t[1]].second}, {o[t[2]].first, ry, o[t[2]].second}, {0, 1, 0},
             ru(t[0]), rv(t[0]), ru(t[1]), rv(t[1]), ru(t[2]), rv(t[2]));
    const float ph = hall ? 0.5f : 0.8f;  // the parapet, its cap and inner face
    for (size_t i = 0; i < o.size(); ++i) {
      const auto [ax, az] = o[i];
      const auto [bx, bz] = o[(i + 1) % o.size()];
      const float len = std::hypot(bx - ax, bz - az);
      if (len < 0.05f) continue;
      const Vec3 n{(bz - az) / len, 0, -(bx - ax) / len};
      bd.quad(kProcTrim, {ax, top, az}, {bx, top, bz}, {bx, top + ph, bz}, {ax, top + ph, az}, n, 0, 0, len / 4, 0.2f);
      const float ix = -n.x * 0.35f, iz = -n.z * 0.35f;
      bd.quad(kProcTrim, {ax, top + ph, az}, {bx, top + ph, bz}, {bx + ix, top + ph, bz + iz}, {ax + ix, top + ph, az + iz}, {0, 1, 0}, 0, 0, len / 4, 0.1f);
      bd.quad(kProcTrim, {bx + ix, top, bz + iz}, {ax + ix, top, az + iz}, {ax + ix, top + ph, az + iz}, {bx + ix, top + ph, bz + iz}, n * -1.0f, 0,
              0, len / 4, 0.2f);
    }
    float s0 = 1e30f, s1 = -1e30f, t0 = 1e30f, t1 = -1e30f;
    for (auto& q : o) {
      const float s = q.first * ux + q.second * uz, t = q.first * vx + q.second * vz;
      s0 = std::min(s0, s), s1 = std::max(s1, s), t0 = std::min(t0, t), t1 = std::max(t1, t);
    }
    const float area = float(signedArea(o));
    // solar panels in rows along the building, each tilted toward the side facing south (+z)
    auto solarRows = [&](int maxPanels) {
      const float sv = vz >= 0 ? 1.0f : -1.0f;  // the panels' low edge toward +v·sv (the south-facing side)
      int panels = 0;
      for (float t = t0 + 2.0f; t < t1 - 1.5f && panels < maxPanels; t += 2.4f)
        for (float s = s0 + 1.5f; s < s1 - 1.5f && panels < maxPanels; s += 1.9f) {
          const float px = ux * s + vx * t, pz = uz * s + vz * t;
          if (!fits(px, pz, 0.95f, 0.65f, 0.5f) || !freeAt(px, pz, 1.1f)) continue;
          const float h0 = top + 0.25f, h1 = top + 0.9f;
          auto P3 = [&](float a, float b, float hh) { return Vec3{px + ux * a + vx * b, hh, pz + uz * a + vz * b}; };
          const Vec3 a = P3(-0.85f, -0.55f * sv, h1), b = P3(0.85f, -0.55f * sv, h1), c = P3(0.85f, 0.55f * sv, h0), e = P3(-0.85f, 0.55f * sv, h0);
          Vec3 nn = render::normalize(cross3(b - a, c - a));
          if (nn.y < 0) nn = nn * -1.0f;
          bd.quad(kProcSolar, a, b, c, e, nn, 0, 0, 1, 1);
          bd.quad(kProcTrim, {a.x, top, a.z}, {b.x, top, b.z}, b, a, render::normalize(Vec3{-vx * sv, 0, -vz * sv}), 0, 0, 1, 0.2f);
          bd.tri(kProcTrim, {a.x, top, a.z}, a, {e.x, top, e.z}, {-ux, 0, -uz}, 0, 0, 0, 0.2f, 0.2f, 0);
          bd.tri(kProcTrim, {b.x, top, b.z}, b, {c.x, top, c.z}, {ux, 0, uz}, 0, 0, 0, 0.2f, 0.2f, 0);
          taken.push_back({px, pz, 0.9f});
          ++panels;
        }
    };
    const bool hallSolar = hall && ((var >> 12) & 1);
    if (hallSolar) solarRows(400);
    if (hall && !hallSolar) {  // roof lanterns: glazed ridges every 9 units across the hall, along its long side
      for (float t = t0 + 6.0f; t < t1 - 5.0f; t += 9.0f) {
        const float sl = (s1 - s0) - 8.0f;
        if (sl < 6) break;
        const float sc = (s0 + s1) * 0.5f, cx = ux * sc + vx * t, cz = uz * sc + vz * t, hw = 1.2f, hl = sl * 0.5f;
        if (!fits(cx, cz, hl, hw, 1.0f)) continue;
        const float yk = top + 0.5f, yr = top + 1.3f;
        Vec3 c[4];
        for (int k = 0; k < 4; ++k) {
          const float sa = (k == 1 || k == 2) ? hl : -hl, sb = k >= 2 ? hw : -hw;
          c[k] = {cx + ux * sa + vx * sb, top, cz + uz * sa + vz * sb};
        }
        for (int k = 0; k < 4; ++k) {  // the kerb
          const Vec3 p0 = c[k], p1 = c[(k + 1) % 4];
          const Vec3 mid{(p0.x + p1.x) * 0.5f - cx, 0, (p0.z + p1.z) * 0.5f - cz};
          bd.quad(kProcTrim, p0, p1, {p1.x, yk, p1.z}, {p0.x, yk, p0.z}, render::normalize(mid), 0, 0, 1, 0.2f);
        }
        const Vec3 r0{cx - ux * hl, yr, cz - uz * hl}, r1{cx + ux * hl, yr, cz + uz * hl};
        for (int side = 0; side < 2; ++side) {  // the glazed slopes and their glazing bars
          const float sb = side ? hw : -hw;
          const Vec3 e0{cx - ux * hl + vx * sb, yk, cz - uz * hl + vz * sb}, e1{cx + ux * hl + vx * sb, yk, cz + uz * hl + vz * sb};
          Vec3 nn = render::normalize(cross3(e1 - e0, r1 - e0));
          if (nn.y < 0) nn = nn * -1.0f;
          bd.quad(kProcGlassLit2, e0, e1, r1, r0, nn, 0, 0, hl / 1.5f, 1);
          for (float bs = -hl + 1.5f; bs < hl; bs += 1.5f) {
            const Vec3 q0{cx + ux * bs + vx * sb, yk, cz + uz * bs + vz * sb}, q1{cx + ux * bs, yr, cz + uz * bs};
            const Vec3 off = Vec3{ux, 0, uz} * 0.05f, lift = nn * 0.02f;
            bd.quad(kProcTrim, q0 - off + lift, q0 + off + lift, q1 + off + lift, q1 - off + lift, nn, 0, 0, 0.1f, 1);
          }
        }
        for (int e = 0; e < 2; ++e) {  // the gable ends
          const float sa = e ? hl : -hl, sg = e ? 1.0f : -1.0f;
          bd.tri(kProcTrim, {cx + ux * sa - vx * hw, yk, cz + uz * sa - vz * hw}, {cx + ux * sa + vx * hw, yk, cz + uz * sa + vz * hw},
                 {cx + ux * sa, yr, cz + uz * sa}, {ux * sg, 0, uz * sg}, 0, 0, 1, 0, 0.5f, 0.3f);
        }
        for (float bs = -hl; bs <= hl; bs += 2.0f) taken.push_back({cx + ux * bs, cz + uz * bs, hw + 0.3f});
      }
    } else {
      for (int k = 0, n = std::clamp(int(area / 180.0f), 1, 5), tries = 0; k < n && tries < 60; ++tries) {  // skylights
        const float cx = minX + (maxX - minX) * rnd(), cz = minZ + (maxZ - minZ) * rnd();
        if (!fits(cx, cz, 1.0f, 0.75f, 0.8f) || !freeAt(cx, cz, 1.6f)) continue;
        bd.box(kProcFrame, kProcGlassLit2, cx, cz, top, top + 0.45f, ux, uz, 1.0f, 0.75f);
        taken.push_back({cx, cz, 1.4f});
        ++k;
      }
      if (area > 300.0f)  // a stair house with its door
        for (int tries = 0; tries < 30; ++tries) {
          const float cx = minX + (maxX - minX) * rnd(), cz = minZ + (maxZ - minZ) * rnd();
          if (!fits(cx, cz, 1.6f, 1.3f, 0.8f) || !freeAt(cx, cz, 2.3f)) continue;
          bd.box(wallMat, kProcRoofFlat, cx, cz, top, top + 2.5f, ux, uz, 1.6f, 1.3f);
          const Vec3 dc{cx + vx * 1.31f, top, cz + vz * 1.31f}, au{ux * 0.5f, 0, uz * 0.5f}, up{0, 2.1f, 0};
          bd.quad(kProcDoor, dc - au, dc + au, dc + au + up, dc - au + up, {vx, 0, vz}, 0, 0, 1, 1);
          taken.push_back({cx, cz, 2.1f});
          break;
        }
      if ((hashU(seed ^ 0x5017u) % 100) < 45) solarRows(120);
      if (false) {  // (the old world-aligned rows)
        int panels = 0;
        for (float pz = minZ + 2.0f; pz < maxZ - 1.5f && panels < 60; pz += 2.4f)
          for (float px = minX + 1.5f; px < maxX - 1.5f && panels < 60; px += 1.9f) {
            bool inside = true;
            for (int k = 0; k < 4 && inside; ++k) inside = insidePoly(o, px + ((k & 1) ? 1.5f : -1.5f), pz + ((k & 2) ? 1.3f : -1.3f));
            if (!inside || !freeAt(px, pz, 1.1f)) continue;
            const float hw = 0.85f, h0 = top + 0.25f, h1 = top + 0.9f;
            const Vec3 a{px - hw, h1, pz - 0.55f}, b{px + hw, h1, pz - 0.55f}, c{px + hw, h0, pz + 0.55f}, e{px - hw, h0, pz + 0.55f};
            Vec3 nn = render::normalize(cross3(b - a, c - a));
            if (nn.y < 0) nn = nn * -1.0f;
            bd.quad(kProcSolar, a, b, c, e, nn, 0, 0, 1, 1);
            bd.quad(kProcTrim, {a.x, top, a.z}, {b.x, top, b.z}, b, a, {0, 0, -1}, 0, 0, 1, 0.2f);
            bd.tri(kProcTrim, {a.x, top, a.z}, a, {e.x, top, e.z}, {-1, 0, 0}, 0, 0, 0, 0.2f, 0.2f, 0);
            bd.tri(kProcTrim, {b.x, top, b.z}, b, {c.x, top, c.z}, {1, 0, 0}, 0, 0, 0, 0.2f, 0.2f, 0);
            taken.push_back({px, pz, 0.9f});
            ++panels;
          }
      }
    }
    for (int k = 0, n = std::clamp(int(area / 200.0f), 1, hall ? 4 : 6), tries = 0; k < n && tries < 60; ++tries) {  // air conditioners
      const float cx = minX + (maxX - minX) * rnd(), cz = minZ + (maxZ - minZ) * rnd();
      if (!fits(cx, cz, 0.8f, 0.8f, 0.8f) || !freeAt(cx, cz, 1.2f)) continue;
      bd.box(kProcTrim, kProcGrille, cx, cz, top, top + 0.9f, ux, uz, 0.8f, 0.8f);
      taken.push_back({cx, cz, 1.1f});
      ++k;
    }
    for (int k = 0, n = 2 + int(area / 300.0f), tries = 0; k < n && tries < 60; ++tries) {  // vent pipes
      const float cx = minX + (maxX - minX) * rnd(), cz = minZ + (maxZ - minZ) * rnd();
      if (!fits(cx, cz, 0.2f, 0.2f, 0.6f) || !freeAt(cx, cz, 0.5f)) continue;
      bd.pipe(kProcTrim, cx, cz, top, top + 0.7f + 0.5f * rnd(), 0.13f + 0.08f * rnd(), 8);
      taken.push_back({cx, cz, 0.4f});
      ++k;
    }
  } else {
    LOG_WARN("proc buildings: an outline of %zu points did not triangulate (no roof)", o.size());
  }
  for (int mat = 0; mat < kProcMaterialCount; ++mat) {  // into the output: one submesh per material
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
  if (st.materials.empty()) {  // the textures, drawn once (in ProcMaterial order)
    const Tex glass = glassTex();
    const Tex texs[kProcMaterialCount] = {
        hallWallTex(), rollerDoorTex(),
        plasterTex({0.86f, 0.80f, 0.68f}), plasterTex({0.86f, 0.71f, 0.46f}), plasterTex({0.74f, 0.77f, 0.80f}), plasterTex({0.86f, 0.66f, 0.58f}),
        brickTex({0.58f, 0.27f, 0.20f}), brickTex({0.38f, 0.23f, 0.17f}), brickTex({0.78f, 0.65f, 0.44f}),
        doorLeafTex(), gravelTex(), tilesTex(), metalRoofTex(), plainTex(0.62f, 0.62f, 0.60f, 21), plainTex(0.90f, 0.89f, 0.86f, 22),
        plainTex(0.20f, 0.21f, 0.22f, 23), glass, glass, glass, glass, solarTex(), grilleTex(),
        panelTex({0.80f, 0.80f, 0.78f}), panelTex({0.30f, 0.31f, 0.33f}), panelTex({0.74f, 0.66f, 0.54f}),
        spandrelTex({0.24f, 0.25f, 0.27f}), spandrelTex({0.32f, 0.38f, 0.46f}), spandrelTex({0.42f, 0.34f, 0.24f}),
        signTex({0.70f, 0.12f, 0.10f}), signTex({0.10f, 0.25f, 0.60f}), signTex({0.08f, 0.45f, 0.20f}), signTex({0.85f, 0.62f, 0.08f})};
    for (int k = 0; k < kProcMaterialCount; ++k) {
      render::GpuMaterial m;
      m.flags = render::kMatDiffuseMap;
      m.textures = {uploadTex(texs[k])};
      for (int c = 0; c < 3; ++c) m.ambient[c] = 0.6f, m.diffuse[c] = 0.55f;
      if (k >= kProcGlass && k <= kProcGlassLit2) {  // glass: a little shine
        m.flags |= render::kMatSpecular;
        for (int c = 0; c < 3; ++c) m.specular[c] = 0.5f;
        m.shininess = 40;
      }
      m.surfaceClass = render::kSurfaceOther;  // (buildings)
      st.materials.push_back(m);
    }
  }
  // the lit windows: three groups switching on one after another at dusk (the cycle's night01, night_lights.h), warm light
  const float night = vehicleNight01();
  static const float kOn[3] = {0.15f, 0.30f, 0.45f};
  for (int k = 0; k < 3; ++k) {
    const float e = smooth01(kOn[k], kOn[k] + 0.2f, night) * (k == 2 ? 0.40f : 0.52f);  // (× 2.5 at night: the scene's emission scale)
    render::GpuMaterial& m = st.materials[size_t(kProcGlassLit0 + k)];
    m.emission[0] = 1.0f * e, m.emission[1] = 0.78f * e, m.emission[2] = 0.45f * e;
  }
  for (int k = 0; k < 4; ++k) {  // the shop signs lit too
    const float e = smooth01(0.12f, 0.32f, night) * 0.35f;
    render::GpuMaterial& m = st.materials[size_t(kProcSign + k)];
    for (int c = 0; c < 3; ++c) m.emission[c] = e;
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
