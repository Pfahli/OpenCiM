// Port: curved roads (engine/curve_roads.h).
#include "engine/curve_roads.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/grid_edit.h"

namespace regs::engine {

using render::Vec3;

namespace {

constexpr float kPi = 3.14159265f;

// the curve at t: x, z from the Bézier, y eased from the start's height to the end's (flat where it meets the blocks)
Vec3 bezier(const CurveRoad& c, float t) {
  float u = 1 - t, b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
  float x1 = c.x0 + c.dx0 * c.a, z1 = c.z0 + c.dz0 * c.a, x2 = c.x1 - c.dx1 * c.b, z2 = c.z1 - c.dz1 * c.b;
  float ease = t * t * (3 - 2 * t);
  return {b0 * c.x0 + b1 * x1 + b2 * x2 + b3 * c.x1, c.y0 + (c.y1 - c.y0) * ease, b0 * c.z0 + b1 * z1 + b2 * z2 + b3 * c.z1};
}

Vec3 bezierDerivative(const CurveRoad& c, float t) {
  float u = 1 - t;
  float x1 = c.x0 + c.dx0 * c.a, z1 = c.z0 + c.dz0 * c.a, x2 = c.x1 - c.dx1 * c.b, z2 = c.z1 - c.dz1 * c.b;
  return {3 * u * u * (x1 - c.x0) + 6 * u * t * (x2 - x1) + 3 * t * t * (c.x1 - x2), (c.y1 - c.y0) * 6 * t * u,
          3 * u * u * (z1 - c.z0) + 6 * u * t * (z2 - z1) + 3 * t * t * (c.z1 - z2)};
}

Vec3 rightOf(const Vec3& dir) { return render::normalize(Vec3{-dir.z, 0, dir.x}); }

// 257 samples evenly in t with their arc length
std::vector<CurveSample> denseOf(const CurveRoad& c) {
  const int n = 256;
  std::vector<CurveSample> out(n + 1);
  float s = 0;
  for (int i = 0; i <= n; ++i) {
    float t = float(i) / float(n);
    CurveSample& k = out[size_t(i)];
    k.p = bezier(c, t);
    Vec3 d = bezierDerivative(c, t);
    if (i == 0) d = {c.dx0, 0, c.dz0};  // exact at the ends: the blocks' directions
    if (i == n) d = {c.dx1, 0, c.dz1};
    k.dir = render::normalize(d);
    k.right = rightOf(k.dir);
    if (i > 0) {
      Vec3 e = k.p - out[size_t(i) - 1].p;
      s += std::sqrt(dot(e, e));
    }
    k.s = s;
  }
  return out;
}

float smoother(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * x * (x * (x * 6 - 15) + 10);
}

// ---- the mesh ----

struct Builder {
  render::MeshData& m;
  const std::vector<CurveSample>& d;
  float length;
  render::Submesh sub;

  void begin(int material) {
    sub = render::Submesh();
    sub.material = material;
    sub.firstVertex = uint32_t(m.vertexCount());
    sub.firstIndex = uint32_t(m.indices.size());
  }
  void end() {
    sub.vertexCount = uint32_t(m.vertexCount()) - sub.firstVertex;
    sub.indexCount = uint32_t(m.indices.size()) - sub.firstIndex;
    if (sub.indexCount) m.submeshes.push_back(sub);
  }
  uint32_t vertex(const Vec3& p, const Vec3& n, float u, float v) {
    m.positions.insert(m.positions.end(), {p.x, p.y, p.z});
    m.normals.insert(m.normals.end(), {n.x, n.y, n.z});
    m.uv0.insert(m.uv0.end(), {u, v});
    m.uv1.insert(m.uv1.end(), {u, v});
    return uint32_t(m.vertexCount() - 1);
  }
  Vec3 pos(uint32_t i) const { return {m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]}; }
  // a triangle whose front (counter-clockwise) faces `facing`
  void tri(uint32_t a, uint32_t b, uint32_t c, const Vec3& facing) {
    Vec3 n = render::cross(pos(b) - pos(a), pos(c) - pos(a));
    if (dot(n, facing) < 0) std::swap(b, c);
    m.indices.insert(m.indices.end(), {a, b, c});
  }

  // the rows of one window [sa, sb]: at most 1.5 units apart, both ends included
  std::vector<float> rows(float sa, float sb) const {
    int n = std::max(1, int(std::ceil((sb - sa) / 1.5f)));
    std::vector<float> r;
    for (int i = 0; i <= n; ++i) r.push_back(sa + (sb - sa) * float(i) / float(n));
    return r;
  }

  // the windows of a repeating pattern: period, phase (start of window 0), drawn part of each window (≤ period)
  template <typename F>
  void windows(float period, float phase, float drawn, F fn) const {
    int k0 = int(std::floor((0 - phase) / period)) - 1, k1 = int(std::ceil((length - phase) / period)) + 1;
    for (int k = k0; k <= k1; ++k) {
      float w0 = phase + float(k) * period;
      float sa = std::max(0.0f, w0), sb = std::min(length, w0 + drawn);
      if (sb - sa > 0.01f) fn(w0, sa, sb);
    }
  }

  // a flat band from offset o0 to o1 (left to right of the centre line), dy above the item height; u runs uStart +
  // uRate·(s − window start), v from v0 at o0 to v1 at o1
  void band(float o0, float o1, float dy, float period, float phase, float drawn, float uStart, float uRate, float v0,
            float v1) {
    const Vec3 up{0, 1, 0};
    windows(period, phase, drawn, [&](float w0, float sa, float sb) {
      uint32_t prevL = 0, prevR = 0;
      bool first = true;
      for (float s : rows(sa, sb)) {
        CurveSample c = curveAt(d, s);
        float u = uStart + uRate * (s - w0);
        uint32_t L = vertex(c.p + c.right * o0 + Vec3{0, dy, 0}, up, u, v0);
        uint32_t R = vertex(c.p + c.right * o1 + Vec3{0, dy, 0}, up, u, v1);
        if (!first) tri(prevL, prevR, L, up), tri(prevR, R, L, up);
        prevL = L, prevR = R, first = false;
      }
    });
  }

  // a vertical face along offset o from dyBottom to dyTop, facing right (+1) or left (−1); u fixed, v bottom → top
  void wall(float o, float dyBottom, float dyTop, float facing, float u, float vBottom, float vTop) {
    uint32_t pb = 0, pt = 0;
    bool first = true;
    for (float s : rows(0, length)) {
      CurveSample c = curveAt(d, s);
      Vec3 n = c.right * facing;
      uint32_t B = vertex(c.p + c.right * o + Vec3{0, dyBottom, 0}, n, u, vBottom);
      uint32_t T = vertex(c.p + c.right * o + Vec3{0, dyTop, 0}, n, u, vTop);
      if (!first) tri(pb, B, pt, n), tri(B, T, pt, n);
      pb = B, pt = T, first = false;
    }
  }

  // a free end's round end: around q (the item height) a half disc facing out along u, asphalt to radius 3, kerb,
  // pavement to 6, the outer edge down into the ground; part 0 asphalt, 1 walkways (the caller's submeshes)
  void cap(const Vec3& q, const Vec3& u, int part) {
    const Vec3 up{0, 1, 0}, r = rightOf(u);
    const int n = 16;
    auto dirAt = [&](int i) {
      float th = -kPi * 0.5f + kPi * float(i) / float(n);
      return u * std::cos(th) + r * std::sin(th);
    };
    if (part == 0) {
      Vec3 c = q + Vec3{0, -0.2f, 0};
      uint32_t centre = vertex(c, up, 0.16f, 0.9612f);
      uint32_t prev = 0;
      for (int i = 0; i <= n; ++i) {
        Vec3 off = dirAt(i) * 3.0f;
        uint32_t v = vertex(c + off, up, 0.16f - 0.008375f * dot(off, u), 0.9612f - 0.00813f * dot(off, r));
        if (i > 0) tri(centre, prev, v, up);
        prev = v;
      }
      return;
    }
    uint32_t pi = 0, po = 0, kb = 0, kt = 0, ob = 0, ot = 0;
    for (int i = 0; i <= n; ++i) {
      Vec3 dir = dirAt(i);
      float arc = 4.5f * kPi * float(i) / float(n);
      uint32_t in = vertex(q + dir * 3.0f, up, 0.8597f - 0.008933f * arc, 0.0190f);
      uint32_t out = vertex(q + dir * 6.0f, up, 0.8597f - 0.008933f * arc, 0.0458f);
      uint32_t kB = vertex(q + dir * 3.0f + Vec3{0, -0.2f, 0}, dir * -1.0f, 0.1921f, 0.9970f);
      uint32_t kT = vertex(q + dir * 3.0f, dir * -1.0f, 0.1921f, 0.9952f);
      uint32_t oB = vertex(q + dir * 6.0f + Vec3{0, -0.6f, 0}, dir, 0.1921f, 0.9970f);
      uint32_t oT = vertex(q + dir * 6.0f, dir, 0.1921f, 0.9952f);
      if (i > 0) {
        tri(pi, po, in, up), tri(po, out, in, up);
        Vec3 inward = dir * -1.0f;
        tri(kb, kB, kt, inward), tri(kB, kT, kt, inward);
        tri(ob, oB, ot, dir), tri(oB, oT, ot, dir);
      }
      pi = in, po = out, kb = kB, kt = kT, ob = oB, ot = oT;
    }
  }
};

}  // namespace

CurveEnds curveFreeEnds(const GridData& g, const CurveRoad& c) {
  auto joined = [&](float x, float z) {
    for (const CurveRoad& k : g.curves) {
      if (sameCurve(k, c)) continue;
      if (std::fabs(k.x0 - x) < 0.2f && std::fabs(k.z0 - z) < 0.2f) return true;
      if (std::fabs(k.x1 - x) < 0.2f && std::fabs(k.z1 - z) < 0.2f) return true;
    }
    return false;
  };
  CurveEnds e;
  e.free0 = !(c.ends & 1) && !joined(c.x0, c.z0);
  e.free1 = !(c.ends & 2) && !joined(c.x1, c.z1);
  return e;
}

CurveSample curveAt(const std::vector<CurveSample>& d, float s) {
  if (d.empty()) return {};
  if (s <= d.front().s) return d.front();
  if (s >= d.back().s) return d.back();
  auto it = std::lower_bound(d.begin(), d.end(), s, [](const CurveSample& k, float v) { return k.s < v; });
  const CurveSample& b = *it;
  const CurveSample& a = *(it - 1);
  float f = b.s > a.s ? (s - a.s) / (b.s - a.s) : 0;
  CurveSample r;
  r.p = a.p + (b.p - a.p) * f;
  r.dir = render::normalize(a.dir + (b.dir - a.dir) * f);
  r.right = rightOf(r.dir);
  r.s = s;
  return r;
}

std::vector<CurveSample> curveShape(const CurveRoad& c, CurveEnds caps) {
  std::vector<CurveSample> d = denseOf(c);
  if (caps.free1) {
    CurveSample e = d.back();
    e.p = e.p + Vec3{c.dx1, 0, c.dz1} * kCurveCapReach;
    e.s += kCurveCapReach;
    d.push_back(e);
  }
  if (caps.free0) {
    CurveSample b = d.front();
    b.p = b.p - Vec3{c.dx0, 0, c.dz0} * kCurveCapReach;
    for (CurveSample& k : d) k.s += kCurveCapReach;
    b.s = 0;
    d.insert(d.begin(), b);
  }
  return d;
}

std::vector<CurveSample> sampleCurve(const CurveRoad& c, float step) {
  std::vector<CurveSample> d = denseOf(c);
  float L = d.back().s;
  int n = std::max(1, int(std::ceil(L / step)));
  std::vector<CurveSample> out;
  for (int i = 0; i <= n; ++i) out.push_back(i == 0 ? d.front() : i == n ? d.back() : curveAt(d, L * float(i) / float(n)));
  return out;
}

float curveLength(const CurveRoad& c) { return denseOf(c).back().s; }

float curveMinRadius(const CurveRoad& c) {
  std::vector<CurveSample> d = denseOf(c);
  float best = 1e30f;
  for (size_t i = 0; i + 1 < d.size(); ++i) {
    float ds = d[i + 1].s - d[i].s;
    float cosA = std::clamp(d[i].right.x * d[i + 1].right.x + d[i].right.z * d[i + 1].right.z, -1.0f, 1.0f);
    float a = std::acos(cosA);
    if (a > 1e-5f) best = std::min(best, ds / a);
  }
  return best;
}

void curveNearest(const std::vector<CurveSample>& d, float x, float z, float& side, float& s) {
  float best = 1e30f;
  side = 1e30f, s = 0;
  for (size_t i = 0; i + 1 < d.size(); ++i) {
    float ax = d[i].p.x, az = d[i].p.z, bx = d[i + 1].p.x, bz = d[i + 1].p.z;
    float ex = bx - ax, ez = bz - az, l2 = ex * ex + ez * ez;
    float f = l2 > 0 ? std::clamp(((x - ax) * ex + (z - az) * ez) / l2, 0.0f, 1.0f) : 0;
    float px = ax + ex * f, pz = az + ez * f, dx = x - px, dz = z - pz, q = dx * dx + dz * dz;
    if (q >= best) continue;
    best = q;
    s = d[i].s + (d[i + 1].s - d[i].s) * f;
    Vec3 r = d[i].right;
    float dist = std::sqrt(q);
    side = dx * r.x + dz * r.z >= 0 ? dist : -dist;
  }
}

void curveCells(const GridData& g, const CurveRoad& c, int& x0, int& z0, int& x1, int& z1) {
  float lx = 1e30f, lz = 1e30f, hx = -1e30f, hz = -1e30f;
  for (const CurveSample& k : curveShape(c, {true, true}))
    lx = std::min(lx, k.p.x), lz = std::min(lz, k.p.z), hx = std::max(hx, k.p.x), hz = std::max(hz, k.p.z);
  float m = kCurveHalfWidth + 3.0f, cs = g.cellSize;
  x0 = std::clamp(int(std::floor((lx - m) / cs + float(g.width) * 0.5f)), 0, g.width - 1);
  z0 = std::clamp(int(std::floor((lz - m) / cs + float(g.length) * 0.5f)), 0, g.length - 1);
  x1 = std::clamp(int(std::floor((hx + m) / cs + float(g.width) * 0.5f)), 0, g.width - 1);
  z1 = std::clamp(int(std::floor((hz + m) / cs + float(g.length) * 0.5f)), 0, g.length - 1);
}

void buildCurveMesh(const CurveRoad& c, CurveEnds caps, int matAsphalt, int matMarks, int matWalk, render::MeshData& out) {
  std::vector<CurveSample> d = curveShape(c, caps);
  Builder b{out, d, d.back().s, {}};
  Vec3 q0 = d.front().p, q1 = d.back().p, u0 = Vec3{-c.dx0, 0, -c.dz0}, u1 = Vec3{c.dx1, 0, c.dz1};
  // the texture regions of small_basic.object, which repeat per 12-unit block (edge lines every 9, dashes every 6)
  b.begin(matAsphalt);
  b.band(-3.0f, 3.0f, -0.2f, 12.0f, 0.0f, 12.0f, 0.2101f, -0.008375f, 0.9856f, 0.9368f);
  if (caps.free0) b.cap(q0, u0, 0);
  if (caps.free1) b.cap(q1, u1, 0);
  b.end();
  b.begin(matMarks);
  b.band(-2.81f, -2.52f, -0.19f, 9.0f, 0.0f, 9.0f, 0.9916f, -0.1099f, 0.0372f, 0.0052f);  // edge lines
  b.band(2.52f, 2.81f, -0.19f, 9.0f, 0.0f, 9.0f, 0.9916f, -0.1099f, 0.0722f, 0.0401f);
  b.band(-0.19f, 0.10f, -0.19f, 6.0f, 2.0f, 2.27f, 0.4317f, -0.03546f, 0.1099f, 0.0764f);  // centre dashes
  b.end();
  b.begin(matWalk);
  b.band(-6.0f, -3.0f, 0.0f, 12.0f, 0.0f, 12.0f, 0.5383f, -0.008925f, 0.0458f, 0.0190f);  // pavements
  b.band(3.0f, 6.0f, 0.0f, 12.0f, 0.0f, 12.0f, 0.8597f, -0.008933f, 0.0190f, 0.0458f);
  b.wall(-3.0f, -0.2f, 0.0f, 1.0f, 0.2457f, 0.9970f, 0.9952f);   // kerbs, facing the carriageway
  b.wall(3.0f, -0.2f, 0.0f, -1.0f, 0.1921f, 0.9970f, 0.9952f);
  b.wall(-6.0f, -0.6f, 0.0f, -1.0f, 0.2457f, 0.9970f, 0.9952f);  // the outer edges down into the ground
  b.wall(6.0f, -0.6f, 0.0f, 1.0f, 0.1921f, 0.9970f, 0.9952f);
  if (caps.free0) b.cap(q0, u0, 1);
  if (caps.free1) b.cap(q1, u1, 1);
  b.end();
}

void levelTerrainUnderCurve(GridObj* grid, const CurveRoad& c) {
  GridData& g = *grid->data;
  if (g.height.empty()) return;
  std::vector<CurveSample> d = curveShape(c, curveFreeEnds(g, c));
  int x0, z0, x1, z1;
  curveCells(g, c, x0, z0, x1, z1);
  const float cs = g.cellSize;
  auto covered = [&](int x, int z) {
    if (x < 0 || z < 0 || x >= g.width || z >= g.length) return false;
    return (g.flags[size_t(z) * size_t(g.width) + size_t(x)] & 0x0F00) != 0;
  };
  int cx0 = 1 << 30, cz0 = 1 << 30, cx1 = -1, cz1 = -1;
  for (int z = z0; z <= std::min(z1 + 1, g.length - 1); ++z)
    for (int x = x0; x <= std::min(x1 + 1, g.width - 1); ++x) {
      size_t ci = size_t(z) * size_t(g.width) + size_t(x);
      if (g.flags[ci] & 0x1000) continue;  // a locked corner
      // a corner of a covered cell (a road block's edge where the curve meets it) only goes down, under the road:
      // the block hides it, and the curve's first cells would otherwise show grass through the asphalt
      bool underItem = covered(x - 1, z - 1) || covered(x, z - 1) || covered(x - 1, z) || covered(x, z);
      float wx = (float(x) - float(g.width) * 0.5f) * cs, wz = (float(z) - float(g.length) * 0.5f) * cs;
      float side, s;
      curveNearest(d, wx, wz, side, s);
      float ad = std::fabs(side);
      if (ad > kCurveHalfWidth + 3.0f || (underItem && ad > kCurveHalfWidth)) continue;
      float target = (curveAt(d, s).p.y - 0.25f) * 256.0f / cs;  // just under the asphalt (−0.2)
      if (underItem && target >= float(g.height[ci])) continue;
      float w = ad <= kCurveHalfWidth + 0.3f ? 1.0f : 1.0f - smoother((ad - kCurveHalfWidth - 0.3f) / 2.7f);
      float h = float(g.height[ci]) + (target - float(g.height[ci])) * w;
      int16_t nh = int16_t(std::clamp(int(std::lround(h)), -0x7fff, 0x7fff));
      if (nh == g.height[ci]) continue;
      g.height[ci] = nh;
      cx0 = std::min(cx0, x), cz0 = std::min(cz0, z), cx1 = std::max(cx1, x), cz1 = std::max(cz1, z);
    }
  if (cx1 >= 0) gridEditCellsChanged(grid, std::max(0, cx0 - 1), std::max(0, cz0 - 1), cx1, cz1, true);
}

void applyCurveCover(GridData& g, int x0, int z0, int x1, int z1) {
  if (g.curves.empty() || g.flags.empty()) return;
  const float cs = g.cellSize;
  for (const CurveRoad& c : g.curves) {
    int cx0, cz0, cx1, cz1;
    curveCells(g, c, cx0, cz0, cx1, cz1);
    cx0 = std::max(cx0, x0), cz0 = std::max(cz0, z0), cx1 = std::min(cx1, x1), cz1 = std::min(cz1, z1);
    if (cx0 > cx1 || cz0 > cz1) continue;
    std::vector<CurveSample> d = curveShape(c, curveFreeEnds(g, c));
    for (int z = cz0; z <= cz1; ++z)
      for (int x = cx0; x <= cx1; ++x) {
        bool inside = true;
        for (int k = 0; k < 5 && inside; ++k) {  // the four corners and the centre
          float fx = k == 4 ? 0.5f : float(k & 1), fz = k == 4 ? 0.5f : float(k >> 1);
          float side, s;
          curveNearest(d, (float(x) + fx - float(g.width) * 0.5f) * cs, (float(z) + fz - float(g.length) * 0.5f) * cs, side, s);
          inside = std::fabs(side) <= kCurveHalfWidth - 0.2f;
        }
        if (inside) g.flags[size_t(z) * size_t(g.width) + size_t(x)] |= 0x0F00;
      }
  }
}

void writeCurveSection(const GridData& g, std::vector<uint8_t>& out) {
  auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
  uint32_t tag = kCurveSectionTag, size = uint32_t(4 + g.curves.size() * 14 * 4), count = uint32_t(g.curves.size());
  put(&tag, 4), put(&size, 4), put(&count, 4);
  for (const CurveRoad& c : g.curves) {
    float f[13] = {c.x0, c.y0, c.z0, c.dx0, c.dz0, c.x1, c.y1, c.z1, c.dx1, c.dz1, c.a, c.b, float(c.ends)};
    int32_t kind = c.kind;
    put(f, sizeof(f)), put(&kind, 4);
  }
}

void parseCurveSection(const std::vector<uint8_t>& d, size_t body, size_t stop, GridData& g) {
  g.curves.clear();
  if (stop > d.size() || body + 4 > stop) return;
  uint32_t count;
  std::memcpy(&count, d.data() + body, 4);
  size_t p = body + 4;
  for (uint32_t i = 0; i < count && p + 56 <= stop; ++i, p += 56) {
    float f[13];
    int32_t kind;
    std::memcpy(f, d.data() + p, 52);
    std::memcpy(&kind, d.data() + p + 52, 4);
    CurveRoad c;
    c.x0 = f[0], c.y0 = f[1], c.z0 = f[2], c.dx0 = f[3], c.dz0 = f[4];
    c.x1 = f[5], c.y1 = f[6], c.z1 = f[7], c.dx1 = f[8], c.dz1 = f[9];
    c.a = f[10], c.b = f[11], c.kind = kind, c.ends = uint8_t(int(f[12]) & 3);
    g.curves.push_back(c);
  }
}

bool sameCurve(const CurveRoad& a, const CurveRoad& b) {
  auto near = [](float p, float q) { return std::fabs(p - q) < 0.01f; };
  return near(a.x0, b.x0) && near(a.z0, b.z0) && near(a.x1, b.x1) && near(a.z1, b.z1) && near(a.dx0, b.dx0) &&
         near(a.dz0, b.dz0) && near(a.dx1, b.dx1) && near(a.dz1, b.dz1) && near(a.a, b.a) && near(a.b, b.b) &&
         a.kind == b.kind;
}

void applyCurveEdit(script::VM& vm, GridObj* grid, const CurveRoad& c, bool add) {
  if (!grid || !grid->data) return;
  GridData& g = *grid->data;
  gridEditBegin(vm, grid);
  bool changed = false;
  if (add) {
    bool known = false;
    for (const CurveRoad& k : g.curves) known = known || sameCurve(k, c);
    if (!known) {
      g.curves.push_back(c);
      levelTerrainUnderCurve(grid, c);
      changed = true;
    }
  } else {
    for (size_t i = 0; i < g.curves.size(); ++i)
      if (sameCurve(g.curves[i], c)) {
        g.curves.erase(g.curves.begin() + std::ptrdiff_t(i));
        changed = true;
        break;
      }
  }
  if (changed) {
    int x0, z0, x1, z1;
    curveCells(g, c, x0, z0, x1, z1);
    grid->curveRects.push_back({x0, z0, x1, z1});
    grid->curvesDirty = true;
    ++grid->curvesVersion;
    gridEditCellsChanged(grid, x0, z0, x1, z1, true);  // the cover under it again (applyCurveCover), the terrain mesh
  }
  gridEditEnd(vm, grid);
}

std::vector<uint8_t> curveActionBytes(const CurveRoad& c, bool add) {
  GridData one;
  one.curves.push_back(c);
  std::vector<uint8_t> section;
  writeCurveSection(one, section);  // tag, size, count, then the curve
  std::vector<uint8_t> out{uint8_t(add ? 1 : 0)};
  out.insert(out.end(), section.begin() + 12, section.end());
  return out;
}

bool curveFromActionBytes(const std::vector<uint8_t>& raw, CurveRoad& c, bool& add) {
  if (raw.size() < 1 + 56) return false;
  add = raw[0] != 0;
  std::vector<uint8_t> body{1, 0, 0, 0};  // count 1, then the curve
  body.insert(body.end(), raw.begin() + 1, raw.begin() + 57);
  GridData one;
  parseCurveSection(body, 0, body.size(), one);
  if (one.curves.empty()) return false;
  c = one.curves[0];
  return true;
}

}  // namespace regs::engine
