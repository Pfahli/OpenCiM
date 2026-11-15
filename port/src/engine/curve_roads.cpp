// Port: curved roads (engine/curve_roads.h).
#include "engine/curve_roads.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

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
  float from = 0, to = 1e30f;  // the stretch bands are drawn on (arc length)
  std::function<float(float)> shift;  // moves bands sideways along the way (a one-way track meeting a two-way road)

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
      float sa = std::max(std::max(0.0f, from), w0), sb = std::min(std::min(length, to), w0 + drawn);
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
        float u = uStart + uRate * (s - w0), sh = shift ? shift(s) : 0.0f;
        uint32_t L = vertex(c.p + c.right * (o0 + sh) + Vec3{0, dy, 0}, up, u, v0);
        uint32_t R = vertex(c.p + c.right * (o1 + sh) + Vec3{0, dy, 0}, up, u, v1);
        if (!first) tri(prevL, prevR, L, up), tri(prevR, R, L, up);
        prevL = L, prevR = R, first = false;
      }
    });
  }

  // a vertical face along offset o from dyBottom to dyTop, facing right (+1) or left (−1); u fixed, v bottom → top
  void wall(float o, float dyBottom, float dyTop, float facing, float u, float vBottom, float vTop) {
    uint32_t pb = 0, pt = 0;
    bool first = true;
    for (float s : rows(std::max(0.0f, from), std::min(length, to))) {
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

bool curveEndOffMap(const GridData& g, const CurveRoad& c, int end) {
  const float hx = float(g.width) * g.cellSize * 0.5f, hz = float(g.length) * g.cellSize * 0.5f;
  const float x = end ? c.x1 : c.x0, z = end ? c.z1 : c.z0;
  const float ox = end ? c.dx1 : -c.dx0, oz = end ? c.dz1 : -c.dz0;  // outward along the end
  if (hx <= 0 || hz <= 0) return false;
  if (std::fabs(x) >= hx - 0.5f && ox * x > 0 && std::fabs(ox) > 0.95f) return true;
  return std::fabs(z) >= hz - 0.5f && oz * z > 0 && std::fabs(oz) > 0.95f;
}

bool curveKindLeavesMap(int kind) { return kind == 0 || kind == 1 || kind == 2 || kind == 6 || kind == 7; }

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
  // an end on a grass tram track keeps its turning circle too (cars turn there; the tracks run on through it)
  // an end on the map's edge leads off the map (its lanes the traffic's way in and out, sim_graph.cpp)
  e.free0 = ((!(c.ends & 1) && !joined(c.x0, c.z0)) || (c.ends & 8)) && !curveEndOffMap(g, c, 0);
  e.free1 = ((!(c.ends & 2) && !joined(c.x1, c.z1)) || (c.ends & 16)) && !curveEndOffMap(g, c, 1);
  // a ramp's end on an express road's entry or exit
  if (curveIsRamp(c.kind)) e.free0 = e.free0 && !curveEndOnRampPort(g, c, 0), e.free1 = e.free1 && !curveEndOnRampPort(g, c, 1);
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

// the bridge span bent along a bridge curve and its pillar ("" none: the plain piece, no pillars)
const char* curveBridgePiece(int kind) {
  switch (kind) {
    case 0: return "bridge_rsmall_gap";
    case 1: return "bridge_rlarge_gap";
    case 2: return "bridge_ravenue_gap";
    case 6: return "bridge_tdouble_gap";
    case 7: return "bridge_rexpress_gap";
    case 8: return "ramp_elevated_basic";  // the elevated ramp
    default: return "";
  }
}
const char* curveBridgeEnd(int kind) {
  switch (kind) {
    case 0: return "bridge_rsmall_end";
    case 1: return "bridge_rlarge_end";
    case 2: return "bridge_ravenue_end";
    case 6: return "bridge_tdouble_end";
    case 7: return "bridge_rexpress_end";
    default: return "";
  }
}
const char* curveTunnelPiece(int kind) {
  switch (kind) {
    case 0: return "tunnel_rsmall_gap";
    case 1: return "tunnel_rlarge_gap";
    case 2: return "tunnel_ravenue_gap";
    case 6: return "tunnel_tdouble_gap";
    case 7: return "tunnel_rexpress_gap";
    default: return "";
  }
}
const char* curveTunnelEnd(int kind) {
  switch (kind) {
    case 0: return "tunnel_rsmall_end";
    case 1: return "tunnel_rlarge_end";
    case 2: return "tunnel_ravenue_end";
    case 6: return "tunnel_tdouble_end";
    case 7: return "tunnel_rexpress_end";
    default: return "";
  }
}
bool curveTunnelOpen(const GridData& g, const CurveRoad& c, float s, CurveEnds caps, float margin) {
  if (c.bridge != 2) return true;
  if (caps.free0) s -= curveProfile(c.kind).capReach;  // a capped shape's arc lengths start at the cap's tip
  const float L = curveLength(c), portal = curveProfile(c.kind).piece * curvePieces(c).stretch + margin;
  return (s <= portal && !curveEndContinues(g, c, 0)) || (s >= L - portal && !curveEndContinues(g, c, 1));
}
const char* expressTransitionPiece(int kind) {
  return kind == 1 ? "road_express_large_basic" : kind == 2 ? "road_express_avenue_basic" : "";
}
int curveExpressTransition(const GridData& g, const CurveRoad& c, int end) {
  if (!curveIsExpress(c.kind)) return -1;
  const float x = end ? c.x1 : c.x0, z = end ? c.z1 : c.z0;
  const float dx = end ? c.dx1 : c.dx0, dz = end ? c.dz1 : c.dz0;
  for (const CurveRoad& k : g.curves) {
    if (k.kind != 1 && k.kind != 2) continue;
    for (int ke = 0; ke < 2; ++ke) {
      float kx = ke ? k.x1 : k.x0, kz = ke ? k.z1 : k.z0, kdx = ke ? k.dx1 : k.dx0, kdz = ke ? k.dz1 : k.dz0;
      if (std::fabs(kx - x) < 0.2f && std::fabs(kz - z) < 0.2f && std::fabs(kdx * dx + kdz * dz) > 0.985f) return k.kind;
    }
  }
  return -1;
}
bool curveEndContinues(const GridData& g, const CurveRoad& c, int end) {
  float x = end ? c.x1 : c.x0, z = end ? c.z1 : c.z0;
  for (const CurveRoad& k : g.curves) {
    if (sameCurve(k, c) || k.bridge != c.bridge) continue;
    if ((std::fabs(k.x0 - x) < 0.2f && std::fabs(k.z0 - z) < 0.2f) || (std::fabs(k.x1 - x) < 0.2f && std::fabs(k.z1 - z) < 0.2f))
      return true;
  }
  return false;
}
const char* curveTramDeadend(int kind) {
  switch (kind) {
    case 1: return "tram_large_deadend";
    case 2: return "tram_avenue_deadend";
    default: return "";
  }
}
const char* curveBridgePillar(int kind) {
  switch (kind) {
    case 0: return "bridge_rsmall2_pillar";
    case 1: return "bridge_rlarge_pillar";
    case 2: return "bridge_ravenue_pillar";
    case 7: return "bridge_rexpress_pillar";
    case 8: return "ramp_elevated_pillar";
    default: return "";
  }
}

// ---- express ramps ----
const char* rampPortPiece(bool enter) { return enter ? "road_express_enter" : "road_express_exit"; }

void rampPortBend(const RampPort& rp, float& side, float& across) {
  // the entry piece's road runs 3 to -z of its origin (lanes -16..+10), the exit piece's 3 to +z; an entry on the right
  // side as it is, an exit on the right turned round (its slip lane then leaves the right side's traffic)
  side = rp.enter ? float(rp.side) : float(-rp.side);
  across = rp.enter ? 3.0f : -3.0f;
}

std::vector<RampPort> expressRampPorts(const GridData& g, int ei) {
  std::vector<RampPort> out;
  if (ei < 0 || size_t(ei) >= g.curves.size() || !curveIsExpress(g.curves[size_t(ei)].kind)) return out;
  const CurveRoad& e = g.curves[size_t(ei)];
  const CurveProfile& pf = curveProfile(e.kind);
  CurvePieces cp = curvePieces(e);
  std::vector<CurveSample> shape = curveShape(e);
  for (int i = 0; i < cp.count; ++i) {
    const float sc = (float(i) + 0.5f) * pf.piece * cp.stretch;
    for (int side : {1, -1})
      for (bool enter : {true, false}) {
        RampPort rp;
        rp.express = ei, rp.piece = i, rp.side = side, rp.enter = enter, rp.sCentre = sc;
        float sg, across;
        rampPortBend(rp, sg, across);
        // the slip lane's end in the template: (-6, +15) on the entry piece, (-6, -15) on the exit piece, at 45 degrees
        const float lx = -6.0f, lz = enter ? 15.0f : -15.0f;
        CurveSample k = curveAt(shape, sc + sg * lx * cp.stretch);
        Vec3 dir = render::normalize(Vec3{k.dir.x, 0, k.dir.z}), right = k.right;
        rp.p = k.p + right * (sg * (lz + across));
        // the way into the ramp: an entry's ramp comes in forward and inward (the template's (0.71, -0.71)), so out of
        // the junction it runs back and outward; an exit's leaves forward and outward ((-0.71, -0.71) in its template)
        Vec3 travel = enter ? dir * (sg * 0.7071f) + right * (sg * -0.7071f) : dir * (sg * -0.7071f) + right * (sg * -0.7071f);
        rp.out = render::normalize(enter ? travel * -1.0f : travel);
        out.push_back(rp);
      }
  }
  return out;
}

std::vector<RampPort> usedRampPorts(const GridData& g) {
  std::vector<RampPort> used;
  bool anyRamp = false, anyExpress = false;
  for (const CurveRoad& c : g.curves) anyRamp = anyRamp || curveIsRamp(c.kind), anyExpress = anyExpress || curveIsExpress(c.kind);
  if (!anyRamp || !anyExpress) return used;
  for (size_t ei = 0; ei < g.curves.size(); ++ei) {
    if (!curveIsExpress(g.curves[ei].kind)) continue;
    std::vector<RampPort> ports = expressRampPorts(g, int(ei));
    for (size_t ri = 0; ri < g.curves.size(); ++ri) {
      const CurveRoad& r = g.curves[ri];
      if (!curveIsRamp(r.kind)) continue;
      for (int end = 0; end < 2; ++end) {
        float x = end ? r.x1 : r.x0, z = end ? r.z1 : r.z0;
        for (const RampPort& p : ports)
          if (std::fabs(p.p.x - x) < 0.3f && std::fabs(p.p.z - z) < 0.3f) {
            RampPort u = p;
            u.ramp = int(ri), u.rampEnd = end;
            used.push_back(u);
            break;
          }
      }
    }
  }
  return used;
}

bool curveEndOnRampPort(const GridData& g, const CurveRoad& c, int end) {
  if (!curveIsRamp(c.kind)) return false;
  float x = end ? c.x1 : c.x0, z = end ? c.z1 : c.z0;
  for (size_t ei = 0; ei < g.curves.size(); ++ei) {
    const CurveRoad& e = g.curves[ei];
    if (!curveIsExpress(e.kind)) continue;
    // near the express curve at all (its ports lie 18 out)
    std::vector<CurveSample> shape = curveShape(e);
    float side, s;
    curveNearest(shape, x, z, side, s);
    if (std::fabs(std::fabs(side) - 18.0f) > 2.0f) continue;
    for (const RampPort& p : expressRampPorts(g, int(ei)))
      if (std::fabs(p.p.x - x) < 0.3f && std::fabs(p.p.z - z) < 0.3f) return true;
  }
  return false;
}

const CurveProfile& curveProfile(int kind) {
  // sizes from the templates (REGS_DUMP_TEMPLATES): large and avenue pieces are 24 units long (two blocks), the large
  // road 24 wide with asphalt ±6, the avenue 30 wide with asphalt ±10.5 around its median, the footpath 3 × 3
  static const CurveProfile k[kCurveKinds] = {
      {2, 0, 12.0f, 6.0f, 3.0f, kCurveCapReach, 6.0f, "road_small_basic", "road_small_deadend", "tram_small_basic", 6.0f, ""},
      {5, 8, 24.0f, 12.0f, 6.0f, 12.0f, 15.0f, "road_large_basic", "road_large_deadend", "tram_large_basic", 12.0f, ""},
      {6, 16, 24.0f, 15.0f, 10.5f, 9.0f, 18.0f, "road_avenue_basic", "road_avenue_deadend", "tram_avenue_basic", 12.0f, ""},
      {1, 0, 3.0f, 1.5f, 0.0f, 1.5f, 3.0f, "alley_footpath_basic", "alley_footpath_deadend", "", 1.5f, ""},
      // tracks: pieces of one block (12 long, the bed 6 wide), the dead end three cells beyond the block's edge
      {0xd, 24, 12.0f, 3.0f, 3.0f, 9.0f, 20.0f, "metro_ground_basic", "metro_ground_deadend", "", 4.5f, ""},
      {0xf, 24, 12.0f, 3.0f, 3.0f, 9.0f, 20.0f, "metro_elevated_basic", "metro_elevated_deadend", "", 4.5f, "metro_elevated_pillar"},
      {9, 24, 12.0f, 3.0f, 3.0f, 9.0f, 20.0f, "track_double_basic", "track_double_deadend", "", 4.5f, ""},
      // the express road: 30 wide like the avenue (three lanes each way at 5, 9, 13 around a 6-wide median), no pavement
      {7, 24, 24.0f, 15.0f, 15.0f, 9.0f, 30.0f, "road_express_basic", "road_express_deadend", "", 12.0f, ""},
      // the express road's ramp: 6 wide, pieces of 12, its dead end 9 long (centred 4.5 beyond the end)
      {3, 0, 12.0f, 3.0f, 3.0f, 7.5f, 12.0f, "road_ramp_basic", "road_ramp_deadend", "", 4.5f, ""},
  };
  return k[std::clamp(kind, 0, kCurveKinds - 1)];
}

CurvePieces curvePieces(const CurveRoad& c, float length) {
  const CurveProfile& pf = curveProfile(c.kind);
  float L = length >= 0 ? std::max(length, 0.01f) : curveLength(c);
  CurvePieces r;
  r.count = std::max(1, int(std::lround(L / pf.piece)));
  r.stretch = L / (float(r.count) * pf.piece);
  return r;
}

void curveCapFrame(const CurveRoad& c, int end, bool openPlusX, Vec3& o, Vec3& ax, Vec3& az) {
  const CurveProfile& pf = curveProfile(c.kind);
  // outward along the end, the piece's centre half a piece beyond it; its open side faces the curve
  Vec3 out = end ? Vec3{c.dx1, 0, c.dz1} : Vec3{-c.dx0, 0, -c.dz0};
  Vec3 e = end ? Vec3{c.x1, c.y1, c.z1} : Vec3{c.x0, c.y0, c.z0};
  o = e + out * pf.capOffset;
  ax = openPlusX ? out * -1.0f : out;  // local +x toward the open side (or away)
  az = rightOf(ax);
}

std::vector<CurveSample> curveShape(const CurveRoad& c, CurveEnds caps) {
  std::vector<CurveSample> d = denseOf(c);
  const float reach = curveProfile(c.kind).capReach;
  if (caps.free1) {
    CurveSample e = d.back();
    e.p = e.p + Vec3{c.dx1, 0, c.dz1} * reach;
    e.s += reach;
    d.push_back(e);
  }
  if (caps.free0) {
    CurveSample b = d.front();
    b.p = b.p - Vec3{c.dx0, 0, c.dz0} * reach;
    for (CurveSample& k : d) k.s += reach;
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

std::vector<CurveSample> sampleCurveRange(const CurveRoad& c, float s0, float s1, float step) {
  std::vector<CurveSample> d = denseOf(c);
  float L = d.back().s;
  s0 = std::clamp(s0, 0.0f, L), s1 = std::clamp(s1, s0, L);
  int n = std::max(1, int(std::ceil((s1 - s0) / step)));
  std::vector<CurveSample> out;
  for (int i = 0; i <= n; ++i) {
    float s = s0 + (s1 - s0) * float(i) / float(n);
    out.push_back(s <= 0 ? d.front() : s >= L ? d.back() : curveAt(d, s));
  }
  return out;
}

float curveLength(const CurveRoad& c) { return denseOf(c).back().s; }

float curveTramOffset(int kind) { return kind == 1 ? 4.5f : 1.5f; }

void curveOneWayMeets(const GridData& g, const CurveRoad& c, float trim0, float trim1, bool& meet0, bool& meet1) {
  auto oneWayAt = [&](float x, float z, bool atStart) {
    for (const CurveRoad& k : g.curves)
      if ((k.ends & 0x20) && !sameCurve(k, c) &&
          (atStart ? std::fabs(k.x1 - x) < 0.2f && std::fabs(k.z1 - z) < 0.2f : std::fabs(k.x0 - x) < 0.2f && std::fabs(k.z0 - z) < 0.2f))
        return true;
    return false;
  };
  const bool oneWay = (c.ends & 0x20) != 0;
  meet0 = oneWay && trim0 <= 0.0f && !oneWayAt(c.x0, c.z0, true);
  meet1 = oneWay && trim1 <= 0.0f && !oneWayAt(c.x1, c.z1, false);
}

bool junctionTramEnd(const CurveJunction& j, const CurveArm& a, bool incoming, Vec3& p) {
  Vec3 r = rightOf(a.out);
  if (a.flow == 0) {
    float off = curveTramOffset(a.kind);
    p = a.mouth + r * (incoming ? -off : off);
    return true;
  }
  if ((a.flow == 1) != incoming) return false;
  const float off = a.kind == 0 ? 1.5f : curveTramOffset(a.kind);
  p = a.single ? a.mouth : a.mouth + r * (incoming ? -off : off);
  return true;
}

const CurveLanes& curveLanes(int kind) {
  static const CurveLanes k[kCurveKinds] = {
      {1, {1.5f, 0}, 0x300}, {2, {1.5f, 4.5f}, 0x500}, {2, {6.0f, 9.0f}, 0x500}, {0, {0, 0}, 0},
      {0, {0, 0}, 0},         {0, {0, 0}, 0},           {0, {0, 0}, 0}, {0, {0, 0}, 0},
      {1, {0, 0}, 0x700}};  // tracks and express roads: their pieces' paths only; ramps: one lane on the centre line
  return k[std::clamp(kind, 0, kCurveKinds - 1)];
}

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
  float m = curveProfile(c.kind).halfWidth + 3.0f, cs = g.cellSize;
  x0 = std::clamp(int(std::floor((lx - m) / cs + float(g.width) * 0.5f)), 0, g.width - 1);
  z0 = std::clamp(int(std::floor((lz - m) / cs + float(g.length) * 0.5f)), 0, g.length - 1);
  x1 = std::clamp(int(std::floor((hx + m) / cs + float(g.width) * 0.5f)), 0, g.width - 1);
  z1 = std::clamp(int(std::floor((hz + m) / cs + float(g.length) * 0.5f)), 0, g.length - 1);
}

void buildCurveMesh(const CurveRoad& c, CurveEnds caps, int matAsphalt, int matMarks, int matWalk, render::MeshData& out,
                    int matTram, float trim0, float trim1, bool meet0, bool meet1) {
  std::vector<CurveSample> d = curveShape(c, caps);
  Builder b{out, d, d.back().s, {}};
  // a trimmed end (a junction) has no cap: the shape's arc lengths are the curve's there
  const float keep0 = caps.free0 ? 0.0f : trim0, keep1 = d.back().s - (caps.free1 ? 0.0f : trim1);
  b.from = keep0, b.to = keep1;
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
  if (!(c.ends & 0x40))  // centre dashes (a single-lane one-way road has none)
    b.band(-0.19f, 0.10f, -0.19f, 6.0f, 2.0f, 2.27f, 0.4317f, -0.03546f, 0.1099f, 0.0764f);
  if (c.ends & 0x20) {  // one-way: road-marks.dds' straight arrow on every lane every 25 units, pointing the way to drive
    const float u0 = 0.027f, u1 = 0.070f, vTail = 0.977f, vTip = 0.596f, len = 4.0f, half = 0.5f;
    const Vec3 up{0, 1, 0};
    const float a0 = std::max(keep0, caps.free0 ? kCurveCapReach : 0.0f) + 6.0f;
    const float a1 = std::min(keep1, d.back().s - (caps.free1 ? kCurveCapReach : 0.0f)) - 6.0f - len;
    const int count = a1 >= a0 ? 1 + int((a1 - a0) / 25.0f) : 0;
    const float first = count > 0 ? a0 + (a1 - a0 - float(count - 1) * 25.0f) * 0.5f : 0.0f;  // centred along the road
    for (int i = 0; i < count; ++i)
      for (float lane : (c.ends & 0x40) ? std::vector<float>{0.0f} : std::vector<float>{-1.5f, 1.5f}) {
        uint32_t pl = 0, pr = 0;
        for (int k = 0; k <= 4; ++k) {
          float t = float(k) / 4.0f;
          CurveSample q = curveAt(d, first + float(i) * 25.0f + t * len);
          float v = vTail + (vTip - vTail) * t;
          uint32_t l = b.vertex(q.p + q.right * (lane - half) + Vec3{0, -0.19f, 0}, up, u0, v);
          uint32_t r = b.vertex(q.p + q.right * (lane + half) + Vec3{0, -0.19f, 0}, up, u1, v);
          if (k > 0) b.tri(pl, pr, l, up), b.tri(pr, r, l, up);
          pl = l, pr = r;
        }
      }
  }
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
  if ((c.ends & 4) && matTram >= 0) {  // tram tracks: tracks/small_basic.object's rail strip (both lanes), every 6 units
    b.begin(matTram);
    // the curve itself, not its turning circles' reach (a grass track's own rails run on from an end on its block)
    b.from = caps.free0 ? kCurveCapReach : keep0;
    b.to = caps.free1 ? d.back().s - kCurveCapReach : keep1;
    if (c.ends & 0x20) {  // one-way (a roundabout): one track, in the outer lane or (one lane) on the centre line
      if (c.ends & 0x40) {
        // where it meets a two-way road the track swings over to that road's right-hand track on the piece next to the end
        // (as the tram lane does: the lanes are cut into pieces of at most 5 units)
        const float span = keep1 - keep0, piece = span / std::max(1.0f, std::ceil(span / 5.0f));
        auto ease = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); };
        b.shift = [=](float s) {
          float o = 0;
          if (meet0) o = std::max(o, 1.5f * (1 - ease((s - keep0) / piece)));
          if (meet1) o = std::max(o, 1.5f * (1 - ease((keep1 - s) / piece)));
          return o;
        };
        b.band(-0.7f, 0.7f, -0.18f, 6.0f, 0.0f, 6.0f, 1.0f, -1.0f / 6.0f, 0.1875f, 0.0625f);
        b.shift = nullptr;
      }
      else b.band(0.8f, 2.2f, -0.18f, 6.0f, 0.0f, 6.0f, 1.0f, -1.0f / 6.0f, 0.0625f, 0.1875f);
    } else {
      b.band(-2.2f, -0.8f, -0.18f, 6.0f, 0.0f, 6.0f, 1.0f, -1.0f / 6.0f, 0.1875f, 0.0625f);
      b.band(-0.8f, 0.8f, -0.18f, 6.0f, 0.0f, 6.0f, 1.0f, -1.0f / 6.0f, 0.0625f, 0.0625f);
      b.band(0.8f, 2.2f, -0.18f, 6.0f, 0.0f, 6.0f, 1.0f, -1.0f / 6.0f, 0.0625f, 0.1875f);
    }
    b.end();
  }
}

render::MeshData sliceMeshAlongX(const render::MeshData& src, float step, int axis) {
  struct V {
    float a[10];  // position, normal, uv0, uv1
  };
  auto get = [&](uint32_t i) {
    V v{};
    for (int k = 0; k < 3; ++k) v.a[k] = src.positions[i * 3 + k];
    for (int k = 0; k < 3; ++k) v.a[3 + k] = src.normals.size() > i * 3 + 2 ? src.normals[i * 3 + k] : (k == 1 ? 1.0f : 0.0f);
    for (int k = 0; k < 2; ++k) v.a[6 + k] = src.uv0.size() > i * 2 + 1 ? src.uv0[i * 2 + k] : 0.0f;
    for (int k = 0; k < 2; ++k) v.a[8 + k] = src.uv1.size() > i * 2 + 1 ? src.uv1[i * 2 + k] : v.a[6 + k];
    return v;
  };
  auto lerp = [](const V& p, const V& q, float t) {
    V r;
    for (int k = 0; k < 10; ++k) r.a[k] = p.a[k] + (q.a[k] - p.a[k]) * t;
    return r;
  };
  // keeps the part of a polygon on one side of x = c (keepAbove: x ≥ c)
  auto clip = [&](const std::vector<V>& in, float c, bool keepAbove) {
    std::vector<V> out;
    for (size_t i = 0; i < in.size(); ++i) {
      const V &p = in[i], &q = in[(i + 1) % in.size()];
      bool pin = keepAbove ? p.a[axis] >= c : p.a[axis] <= c, qin = keepAbove ? q.a[axis] >= c : q.a[axis] <= c;
      if (pin) out.push_back(p);
      if (pin != qin) out.push_back(lerp(p, q, (c - p.a[axis]) / (q.a[axis] - p.a[axis])));
    }
    return out;
  };
  render::MeshData m;
  auto put = [&](const V& v) {
    m.positions.insert(m.positions.end(), {v.a[0], v.a[1], v.a[2]});
    m.normals.insert(m.normals.end(), {v.a[3], v.a[4], v.a[5]});
    m.uv0.insert(m.uv0.end(), {v.a[6], v.a[7]});
    m.uv1.insert(m.uv1.end(), {v.a[8], v.a[9]});
    return uint32_t(m.positions.size() / 3 - 1);
  };
  for (const render::Submesh& sm : src.submeshes) {
    render::Submesh o = sm;
    o.firstVertex = uint32_t(m.vertexCount());
    o.firstIndex = uint32_t(m.indices.size());
    for (uint32_t t = 0; t + 2 < sm.indexCount; t += 3) {
      std::vector<V> tri = {get(src.indices[sm.firstIndex + t]), get(src.indices[sm.firstIndex + t + 1]),
                            get(src.indices[sm.firstIndex + t + 2])};
      float lo = std::min({tri[0].a[axis], tri[1].a[axis], tri[2].a[axis]}), hi = std::max({tri[0].a[axis], tri[1].a[axis], tri[2].a[axis]});
      int k0 = int(std::floor(lo / step)), k1 = int(std::ceil(hi / step));
      for (int k = k0; k < k1; ++k) {
        std::vector<V> poly = clip(clip(tri, float(k) * step, true), float(k + 1) * step, false);
        if (poly.size() < 3) continue;
        uint32_t first = put(poly[0]), prev = put(poly[1]);
        for (size_t i = 2; i < poly.size(); ++i) {
          uint32_t cur = put(poly[i]);
          m.indices.insert(m.indices.end(), {first, prev, cur});
          prev = cur;
        }
      }
    }
    o.vertexCount = uint32_t(m.vertexCount()) - o.firstVertex;
    o.indexCount = uint32_t(m.indices.size()) - o.firstIndex;
    m.submeshes.push_back(o);
  }
  return m;
}

void appendPlacedMesh(const render::MeshData& src, int materialBase, const Vec3& o, const Vec3& ax, const Vec3& az,
                      render::MeshData& out) {
  if (src.positions.empty() || materialBase < 0) return;
  const uint32_t base = uint32_t(out.vertexCount());
  const Vec3 up{0, 1, 0};
  for (size_t i = 0; i < src.vertexCount(); ++i) {
    const float* p = &src.positions[i * 3];
    Vec3 w = o + ax * p[0] + up * p[1] + az * p[2];
    Vec3 nl{src.normals.size() > i * 3 + 2 ? src.normals[i * 3] : 0.0f, src.normals.size() > i * 3 + 2 ? src.normals[i * 3 + 1] : 1.0f,
            src.normals.size() > i * 3 + 2 ? src.normals[i * 3 + 2] : 0.0f};
    Vec3 nw = ax * nl.x + up * nl.y + az * nl.z;
    out.positions.insert(out.positions.end(), {w.x, w.y, w.z});
    out.normals.insert(out.normals.end(), {nw.x, nw.y, nw.z});
    float u = src.uv0.size() > i * 2 + 1 ? src.uv0[i * 2] : 0, v = src.uv0.size() > i * 2 + 1 ? src.uv0[i * 2 + 1] : 0;
    out.uv0.insert(out.uv0.end(), {u, v});
    if (src.uv1.size() > i * 2 + 1) out.uv1.insert(out.uv1.end(), {src.uv1[i * 2], src.uv1[i * 2 + 1]});
    else out.uv1.insert(out.uv1.end(), {u, v});
  }
  for (const render::Submesh& sm : src.submeshes) {
    render::Submesh s;
    s.material = materialBase + sm.material;
    s.firstVertex = base + sm.firstVertex;
    s.vertexCount = sm.vertexCount;
    s.firstIndex = uint32_t(out.indices.size());
    for (uint32_t k = 0; k < sm.indexCount; ++k) out.indices.push_back(base + src.indices[sm.firstIndex + k]);
    s.indexCount = sm.indexCount;
    out.submeshes.push_back(s);
  }
}

void appendBentMesh(const render::MeshData& src, int materialBase, const std::vector<CurveSample>& shape, float s0,
                    float side, float across, render::MeshData& out, float scale, float dy) {
  if (src.positions.empty() || materialBase < 0) return;
  const uint32_t base = uint32_t(out.vertexCount()), first = uint32_t(out.indices.size());
  const size_t n = src.vertexCount();
  const float sg = side >= 0 ? 1.0f : -1.0f;
  for (size_t i = 0; i < n; ++i) {
    float x = src.positions[i * 3], y = src.positions[i * 3 + 1], z = src.positions[i * 3 + 2];
    CurveSample k = curveAt(shape, s0 + sg * x * scale);
    Vec3 dir = render::normalize(Vec3{k.dir.x, 0, k.dir.z}) * sg, right = k.right * sg, up{0, 1, 0};
    Vec3 p = k.p + right * (z + across) + up * (y + dy);
    Vec3 nl{src.normals.size() > i * 3 + 2 ? src.normals[i * 3] : 0.0f, src.normals.size() > i * 3 + 2 ? src.normals[i * 3 + 1] : 1.0f,
            src.normals.size() > i * 3 + 2 ? src.normals[i * 3 + 2] : 0.0f};
    Vec3 nw = dir * nl.x + up * nl.y + right * nl.z;
    out.positions.insert(out.positions.end(), {p.x, p.y, p.z});
    out.normals.insert(out.normals.end(), {nw.x, nw.y, nw.z});
    float u = src.uv0.size() > i * 2 + 1 ? src.uv0[i * 2] : 0, v = src.uv0.size() > i * 2 + 1 ? src.uv0[i * 2 + 1] : 0;
    out.uv0.insert(out.uv0.end(), {u, v});
    if (src.uv1.size() > i * 2 + 1) out.uv1.insert(out.uv1.end(), {src.uv1[i * 2], src.uv1[i * 2 + 1]});
    else out.uv1.insert(out.uv1.end(), {u, v});
  }
  for (const render::Submesh& sm : src.submeshes) {
    render::Submesh o;
    o.material = materialBase + sm.material;
    o.firstVertex = base + sm.firstVertex;
    o.vertexCount = sm.vertexCount;
    o.firstIndex = uint32_t(out.indices.size());
    for (uint32_t k = 0; k < sm.indexCount; ++k) out.indices.push_back(base + src.indices[sm.firstIndex + k]);
    o.indexCount = sm.indexCount;
    out.submeshes.push_back(o);
  }
  (void)first;
}

render::MeshData rightHalfOf(const render::MeshData& src) {
  render::MeshData out = src;
  out.indices.clear();
  out.submeshes.clear();
  for (const render::Submesh& sm : src.submeshes) {
    render::Submesh o = sm;
    o.firstIndex = uint32_t(out.indices.size());
    o.indexCount = 0;
    for (uint32_t k = 0; k + 2 < sm.indexCount; k += 3) {
      float z = 0;
      for (int j = 0; j < 3; ++j) z += src.positions[size_t(src.indices[sm.firstIndex + k + uint32_t(j)]) * 3 + 2];
      if (z < -0.03f) continue;
      for (int j = 0; j < 3; ++j) out.indices.push_back(src.indices[sm.firstIndex + k + uint32_t(j)]);
      o.indexCount += 3;
    }
    if (o.indexCount) out.submeshes.push_back(o);
  }
  return out;
}

std::string stopTemplateName(const CurveRoad::Stop& st, int kind) {
  const char* road = kind == 1 ? "_large" : kind == 2 ? "_avenue" : "_small";
  if (st.kind == 1)  // trams: small roads keep design 01 (as before); large roads and avenues their design
    return kind == 0 ? "stop_tram01_small" : "stop_tram0" + std::to_string(std::clamp(int(st.design), 1, 3)) + road;
  return "stop_bus0" + std::to_string(std::clamp(int(st.design), 1, 3)) + road;
}

float curveStopOffset(int kind, int stopKind) {
  if (kind == 2 && stopKind == 1) return 4.5f;  // an avenue's tram stop: in the middle strip
  return curveProfile(kind).carriageway;
}

void stopAnchor(const GridData& g, const CurveRoad& c, const CurveRoad::Stop& st, int& x, int& z, int& orient) {
  std::vector<CurveSample> d = curveShape(c);
  CurveSample k = curveAt(d, st.s);
  // the pavement's middle (small road 4.5, large road 9, avenue 12.75 out; an avenue's tram stop on its platform, 3.75
  // out in the middle strip); the stop items' footprints along x and across: small 4 × 2, large 8 × 4, avenue 8 × 3
  const CurveProfile& pf = curveProfile(c.kind);
  const float out = c.kind == 2 && st.kind == 1 ? 3.75f : (pf.carriageway + pf.halfWidth) * 0.5f;
  Vec3 p = k.p + k.right * (float(st.side) * out);
  orient = std::fabs(k.dir.x) >= std::fabs(k.dir.z) ? 0 : 1;
  const int along = c.kind == 1 || c.kind == 2 ? 8 : 4, across = c.kind == 1 ? 4 : c.kind == 2 ? 3 : 2;
  int sx = orient ? across : along, sz = orient ? along : across;
  const float cs = g.cellSize;
  x = int(std::lround(p.x / cs + float(g.width) * 0.5f - float(sx) * 0.5f));
  z = int(std::lround(p.z / cs + float(g.length) * 0.5f - float(sz) * 0.5f));
}

void refreshCurveAnchors(GridData& g) {
  g.curveAnchors.clear();
  for (const CurveRoad& c : g.curves)
    for (const CurveRoad::Stop& st : c.stops) {
      int x, z, o;
      stopAnchor(g, c, st, x, z, o);
      std::string name = stopTemplateName(st, c.kind);
      for (size_t i = 0; i < g.items.size(); ++i) {
        const GridData::Item& it = g.items[i];
        if (it.removed || it.x != x || it.z != z || it.type >= g.types.size() || g.types[it.type] != name) continue;
        g.curveAnchors.insert(uint32_t(i));
        break;
      }
    }
}

// a point past an end on a block (the block's own ground; a grass tram track's block keeps its grass and height)
static bool beyondBlockEnd(const CurveRoad& c, float x, float z) {
  // (an end on a grass tram track has its turning circle over the block: not this)
  if ((c.ends & 9) == 1 && (x - c.x0) * c.dx0 + (z - c.z0) * c.dz0 < -0.05f) return true;
  if ((c.ends & 18) == 2 && (x - c.x1) * c.dx1 + (z - c.z1) * c.dz1 > 0.05f) return true;
  return false;
}

// a point past an end of the curve that is not free (it meets a junction or another curve): that end's ground belongs to
// what it meets (an avenue ring's junction lies on the island's edge: the road ending there must not claim the island)
static bool pastJoinedEnd(const CurveRoad& c, CurveEnds caps, float x, float z) {
  if (!caps.free0 && !(c.ends & 1) && (x - c.x0) * c.dx0 + (z - c.z0) * c.dz0 < -0.05f) return true;
  if (!caps.free1 && !(c.ends & 2) && (x - c.x1) * c.dx1 + (z - c.z1) * c.dz1 > 0.05f) return true;
  return false;
}

void levelTerrainUnderCurve(GridObj* grid, const CurveRoad& c) {
  GridData& g = *grid->data;
  if (g.height.empty() || curveIsElevated(c.kind) || c.bridge == 1) return;  // elevated tracks and bridges leave the ground
  const bool tunnel = c.bridge == 2;  // tunnels: only their portals cut the ground (no rim: the walls hold it)
  const CurveEnds caps = curveFreeEnds(g, c);
  std::vector<CurveSample> d = curveShape(c, caps);
  int x0, z0, x1, z1;
  curveCells(g, c, x0, z0, x1, z1);
  const float cs = g.cellSize, hw = curveProfile(c.kind).halfWidth;
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
      if (beyondBlockEnd(c, wx, wz) || pastJoinedEnd(c, caps, wx, wz)) continue;
      float side, s;
      curveNearest(d, wx, wz, side, s);
      float ad = curveAcross(c, side);
      if (ad > hw + 3.0f || (underItem && ad > hw)) continue;
      if (tunnel && (ad > hw + 1.5f || !curveTunnelOpen(g, c, s, caps, 3.0f))) continue;
      float target = (curveAt(d, s).p.y - 0.25f) * 256.0f / cs;  // just under the asphalt (−0.2)
      // a tunnel: a cell behind its mouth and beside the road the ground comes down to the portal's top (6 over the road),
      // so the portal's frame stands clear in front of the hill; only lowered
      if (tunnel && (ad > hw || !curveTunnelOpen(g, c, s, caps))) {
        target = (curveAt(d, s).p.y + 5.8f) * 256.0f / cs;
        if (target >= float(g.height[ci])) continue;
      }
      if (underItem && target >= float(g.height[ci])) continue;
      float w = ad <= hw + 0.3f ? 1.0f : 1.0f - smoother((ad - hw - 0.3f) / 2.7f);
      float h = float(g.height[ci]) + (target - float(g.height[ci])) * w;
      int16_t nh = int16_t(std::clamp(int(std::lround(h)), -0x7fff, 0x7fff));
      if (nh == g.height[ci]) continue;
      g.height[ci] = nh;
      cx0 = std::min(cx0, x), cz0 = std::min(cz0, z), cx1 = std::max(cx1, x), cz1 = std::max(cz1, z);
    }
  if (cx1 >= 0) gridEditCellsChanged(grid, std::max(0, cx0 - 1), std::max(0, cz0 - 1), cx1, cz1, true);
}

void applyCurveCover(GridData& g, int x0, int z0, int x1, int z1) {
  if (g.flags.empty()) return;
  if (g.curveCover.size() != g.flags.size()) g.curveCover.assign(g.flags.size(), 0);
  x0 = std::max(x0, 0), z0 = std::max(z0, 0), x1 = std::min(x1, g.width - 1), z1 = std::min(z1, g.length - 1);
  for (int z = z0; z <= z1; ++z)  // the rect's marks again (the cover itself was cleared and re-stamped by the caller)
    for (int x = x0; x <= x1; ++x) g.curveCover[size_t(z) * size_t(g.width) + size_t(x)] = 0;
  if (g.curves.empty()) return;
  const float cs = g.cellSize;
  // a cell whose ground drops more than a unit under the road (a bank, a river channel) stays uncovered: the terrain
  // draws its slope under the road's edge; covered, it left a hole down to the background (a flat brown face beside a
  // bridge at a junction on a river bank, OSM import)
  auto dropsAway = [&](int x, int z, float roadY) {
    if (g.height.empty()) return false;
    int lowest = 0x7fff;
    for (int dz = 0; dz <= 1; ++dz)
      for (int dx = 0; dx <= 1; ++dx) {
        int hx = std::min(x + dx, g.width - 1), hz = std::min(z + dz, g.length - 1);
        lowest = std::min(lowest, int(g.height[size_t(hz) * size_t(g.width) + size_t(hx)]));
      }
    return float(lowest) * cs / 256.0f < roadY - 1.0f;
  };
  for (const CurveRoad& c : g.curves) {
    if (curveIsElevated(c.kind) || c.bridge == 1) continue;  // no ground cover under elevated tracks and bridges
    int cx0, cz0, cx1, cz1;
    curveCells(g, c, cx0, cz0, cx1, cz1);
    cx0 = std::max(cx0, x0), cz0 = std::max(cz0, z0), cx1 = std::min(cx1, x1), cz1 = std::min(cz1, z1);
    if (cx0 > cx1 || cz0 > cz1) continue;
    const CurveEnds caps = curveFreeEnds(g, c);
    std::vector<CurveSample> d = curveShape(c, caps);
    for (int z = cz0; z <= cz1; ++z)
      for (int x = cx0; x <= cx1; ++x) {
        bool inside = true;
        for (int k = 0; k < 5 && inside; ++k) {  // the four corners and the centre
          float fx = k == 4 ? 0.5f : float(k & 1), fz = k == 4 ? 0.5f : float(k >> 1);
          float side, s, px = (float(x) + fx - float(g.width) * 0.5f) * cs, pz = (float(z) + fz - float(g.length) * 0.5f) * cs;
          curveNearest(d, px, pz, side, s);
          inside = curveAcross(c, side) <= curveProfile(c.kind).halfWidth - 0.2f && !beyondBlockEnd(c, px, pz) &&
                   !pastJoinedEnd(c, caps, px, pz) &&
                   curveTunnelOpen(g, c, s, caps, 3.0f);  // a tunnel's tube: the ground over it stays (but behind the mouth)
        }
        if (!inside) continue;
        {
          float side, s, px = (float(x) + 0.5f - float(g.width) * 0.5f) * cs, pz = (float(z) + 0.5f - float(g.length) * 0.5f) * cs;
          curveNearest(d, px, pz, side, s);
          if (dropsAway(x, z, curveAt(d, s).p.y)) continue;
        }
        size_t ci = size_t(z) * size_t(g.width) + size_t(x);
        if ((g.flags[ci] & 0x0F00) != 0x0F00) g.curveCover[ci] = 1;  // covered by the curve, not by an item
        g.flags[ci] |= 0x0F00;
      }
  }
  // the junctions' cells wholly inside their outlines
  for (const CurveJunction& j : findCurveJunctions(g)) {
    int cx0, cz0, cx1, cz1;
    junctionCells(g, j, cx0, cz0, cx1, cz1);
    cx0 = std::max(cx0, x0), cz0 = std::max(cz0, z0), cx1 = std::min(cx1, x1), cz1 = std::min(cz1, z1);
    if (cx0 > cx1 || cz0 > cz1) continue;
    std::vector<Vec3> outer = junctionOutline(j, 1, -0.2f, 6);
    // not over water, a bridge arm's part of the junction or ground more than a cell under it: covered cells there
    // hide the ground and the terrain hangs its base walls from them into the gap (a flat brown box beside a bridge
    // that ends at a junction, OSM import)
    std::vector<std::pair<std::vector<CurveSample>, float>> bridges;
    float core = 0;
    for (const CurveArm& a : j.arms) {
      const CurveRoad& ac = g.curves[size_t(a.curve)];
      if (ac.bridge == 1) bridges.push_back({curveShape(ac), curveProfile(ac.kind).halfWidth});
      else core = std::max(core, curveProfile(ac.kind).halfWidth);
    }
    for (int z = cz0; z <= cz1; ++z)
      for (int x = cx0; x <= cx1; ++x) {
        bool inside = true;
        for (int k = 0; k < 5 && inside; ++k) {
          float fx = k == 4 ? 0.5f : float(k & 1), fz = k == 4 ? 0.5f : float(k >> 1);
          inside = insideOutline(outer, (float(x) + fx - float(g.width) * 0.5f) * cs, (float(z) + fz - float(g.length) * 0.5f) * cs);
        }
        if (!inside) continue;
        size_t ci = size_t(z) * size_t(g.width) + size_t(x);
        if (g.flags[ci] & 0x2000) continue;  // water
        if (dropsAway(x, z, j.centre.y)) continue;  // the ground falling away under the junction
        if (!bridges.empty()) {
          float wx = (float(x) + 0.5f - float(g.width) * 0.5f) * cs, wz = (float(z) + 0.5f - float(g.length) * 0.5f) * cs;
          float fromCentre = std::sqrt((wx - j.centre.x) * (wx - j.centre.x) + (wz - j.centre.z) * (wz - j.centre.z));
          bool onBridge = false;
          for (const auto& [shape, hw] : bridges) {
            float side, s;
            curveNearest(shape, wx, wz, side, s);
            if (std::fabs(side) <= hw + 1.5f && fromCentre > core) onBridge = true;
          }
          if (onBridge) continue;
        }
        if ((g.flags[ci] & 0x0F00) != 0x0F00) g.curveCover[ci] = 1;
        g.flags[ci] |= 0x0F00;
      }
  }
}

// one curve: 13 floats (the 13th the end flags), i32 kind, u32 stop count, per stop f32 s, i8 side, u8 kind, u8 design,
// u8 0, then u16 length + the name's UTF-8 bytes (format 3: the count's bits 31 and 30 set; format 2 (bit 31) had no
// name, format 1 no stop count). The kind's second byte: the bridge flag.
static void writeCurve(const CurveRoad& c, std::vector<uint8_t>& out) {
  auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
  float f[13] = {c.x0, c.y0, c.z0, c.dx0, c.dz0, c.x1, c.y1, c.z1, c.dx1, c.dz1, c.a, c.b, float(c.ends)};
  int32_t kind = (c.kind & 0xff) | (int32_t(c.bridge) << 8);
  uint32_t n = uint32_t(c.stops.size());
  put(f, sizeof(f)), put(&kind, 4), put(&n, 4);
  for (const CurveRoad::Stop& st : c.stops) {
    uint8_t b[4] = {uint8_t(st.side), st.kind, st.design, 0};
    put(&st.s, 4), put(b, 4);
  }
  uint16_t len = uint16_t(std::min<size_t>(c.name.size(), 0xFFFF));
  put(&len, 2), put(c.name.data(), len);
}

// reads one curve at p; false when it does not fit before stop
static bool readCurve(const std::vector<uint8_t>& d, size_t& p, size_t stop, bool v2, bool v3, CurveRoad& c) {
  if (p + 56 > stop) return false;
  float f[13];
  int32_t kind;
  std::memcpy(f, d.data() + p, 52);
  std::memcpy(&kind, d.data() + p + 52, 4);
  p += 56;
  c = CurveRoad();
  c.x0 = f[0], c.y0 = f[1], c.z0 = f[2], c.dx0 = f[3], c.dz0 = f[4];
  c.x1 = f[5], c.y1 = f[6], c.z1 = f[7], c.dx1 = f[8], c.dz1 = f[9];
  c.a = f[10], c.b = f[11], c.kind = kind & 0xff, c.ends = uint8_t(int(f[12]) & 255);
  c.bridge = uint8_t((kind >> 8) & 0xff);
  if (!v2) return true;
  if (p + 4 > stop) return false;
  uint32_t n;
  std::memcpy(&n, d.data() + p, 4);
  p += 4;
  for (uint32_t k = 0; k < n && p + 8 <= stop; ++k, p += 8) {
    CurveRoad::Stop st;
    std::memcpy(&st.s, d.data() + p, 4);
    st.side = int8_t(d[p + 4]) < 0 ? -1 : 1;
    st.kind = d[p + 5];
    st.design = d[p + 6];
    c.stops.push_back(st);
  }
  if (v3 && p + 2 <= stop) {
    uint16_t len;
    std::memcpy(&len, d.data() + p, 2);
    p += 2;
    if (p + len > stop) return false;
    c.name.assign((const char*)d.data() + p, len);
    p += len;
  }
  return true;
}

void writeCurveSection(const GridData& g, std::vector<uint8_t>& out) {
  std::vector<uint8_t> body;
  uint32_t count = uint32_t(g.curves.size()) | 0xC0000000u;
  body.insert(body.end(), (const uint8_t*)&count, (const uint8_t*)&count + 4);
  for (const CurveRoad& c : g.curves) writeCurve(c, body);
  uint32_t tag = kCurveSectionTag, size = uint32_t(body.size());
  out.insert(out.end(), (const uint8_t*)&tag, (const uint8_t*)&tag + 4);
  out.insert(out.end(), (const uint8_t*)&size, (const uint8_t*)&size + 4);
  out.insert(out.end(), body.begin(), body.end());
}

void parseCurveSection(const std::vector<uint8_t>& d, size_t body, size_t stop, GridData& g) {
  g.curves.clear();
  if (stop > d.size() || body + 4 > stop) return;
  uint32_t count;
  std::memcpy(&count, d.data() + body, 4);
  bool v2 = (count & 0x80000000u) != 0, v3 = v2 && (count & 0x40000000u) != 0;
  count &= 0x3fffffffu;
  size_t p = body + 4;
  CurveRoad c;
  for (uint32_t i = 0; i < count && readCurve(d, p, stop, v2, v3, c); ++i) g.curves.push_back(c);
  refreshCurveAnchors(g);
}

bool sameCurve(const CurveRoad& a, const CurveRoad& b) {
  auto near = [](float p, float q) { return std::fabs(p - q) < 0.01f; };
  return near(a.x0, b.x0) && near(a.z0, b.z0) && near(a.x1, b.x1) && near(a.z1, b.z1) && near(a.dx0, b.dx0) &&
         near(a.dz0, b.dz0) && near(a.dx1, b.dx1) && near(a.dz1, b.dz1) && near(a.a, b.a) && near(a.b, b.b) &&
         a.kind == b.kind;
}

static std::vector<CurveJunction> junctionsAt(const GridData& g, const CurveRoad& c);

void applyCurveEdit(script::VM& vm, GridObj* grid, const CurveRoad& c, bool add) {
  if (!grid || !grid->data) return;
  GridData& g = *grid->data;
  gridEditBegin(vm, grid);
  bool changed = false;
  // the junctions at its ends before and after (their areas change with it)
  std::vector<std::array<int, 4>> areas;
  auto junctionAreas = [&]() {
    for (const CurveJunction& j : junctionsAt(g, c)) {
      int x0, z0, x1, z1;
      junctionCells(g, j, x0, z0, x1, z1);
      areas.push_back({x0, z0, x1, z1});
    }
  };
  junctionAreas();
  if (add) {
    bool known = false;
    for (const CurveRoad& k : g.curves) known = known || sameCurve(k, c);
    if (!known) {
      g.curves.push_back(c);
      levelTerrainUnderCurve(grid, c);
      levelTerrainUnderJunctions(grid, c);
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
    refreshCurveAnchors(g);
    int x0, z0, x1, z1;
    curveCells(g, c, x0, z0, x1, z1);
    grid->curveRects.push_back({x0, z0, x1, z1});
    grid->curvesDirty = true;
    ++grid->curvesVersion;
    gridEditCellsChanged(grid, x0, z0, x1, z1, true);  // the cover under it again (applyCurveCover), the terrain mesh
    junctionAreas();
    for (const auto& a : areas) {
      grid->curveRects.push_back(a);
      gridEditCellsChanged(grid, a[0], a[1], a[2], a[3], true);
    }
  }
  gridEditEnd(vm, grid);
}

std::vector<uint8_t> curveActionBytes(const CurveRoad& c, bool add) {
  std::vector<uint8_t> out{uint8_t(add ? 1 : 0)};
  writeCurve(c, out);
  return out;
}

bool curveFromActionBytes(const std::vector<uint8_t>& raw, CurveRoad& c, bool& add) {
  if (raw.size() < 1 + 60) return false;
  add = raw[0] != 0;
  size_t p = 1;
  return readCurve(raw, p, raw.size(), true, true, c);  // older records end before the name
}

// ---- junctions ----

std::vector<CurveJunction> findCurveJunctions(const GridData& g) {
  struct End {
    int curve, end, kind;
    Vec3 p, out;
  };
  std::vector<End> ends;
  for (size_t i = 0; i < g.curves.size(); ++i) {
    const CurveRoad& c = g.curves[i];
    ends.push_back({int(i), 0, c.kind, {c.x0, c.y0, c.z0}, render::normalize(Vec3{c.dx0, 0, c.dz0})});
    ends.push_back({int(i), 1, c.kind, {c.x1, c.y1, c.z1}, render::normalize(Vec3{-c.dx1, 0, -c.dz1})});
  }
  std::vector<uint8_t> used(ends.size(), 0);
  std::vector<CurveJunction> out;
  for (size_t a = 0; a < ends.size(); ++a) {
    if (used[a]) continue;
    std::vector<size_t> group{a};
    for (size_t b = a + 1; b < ends.size(); ++b)
      if (!used[b] && curveJoinClass(ends[b].kind) == curveJoinClass(ends[a].kind) && std::fabs(ends[b].p.x - ends[a].p.x) < 0.2f &&
          std::fabs(ends[b].p.z - ends[a].p.z) < 0.2f)
        group.push_back(b);
    for (size_t k : group) used[k] = 1;
    if (group.size() < 2) continue;
    if (curveNoJunctions(ends[a].kind)) continue;  // tracks and express roads only join smoothly (no junction pieces)
    if (group.size() == 2 && dot(ends[group[0]].out, ends[group[1]].out) < -0.985f) continue;  // a smooth join
    CurveJunction j;
    j.kind = ends[a].kind;
    for (size_t k : group) j.centre = j.centre + ends[k].p * (1.0f / float(group.size()));
    // the radius: two neighbouring arms' edges (half a width out, the wider of the two road kinds) meet hw / tan(angle / 2)
    // from the centre (road kinds may mix: a large road into a roundabout)
    std::vector<std::pair<float, float>> ang;  // angle, half width
    float R = 0;
    for (size_t k : group) {
      float hw = curveProfile(ends[k].kind).halfWidth;
      ang.push_back({std::atan2(ends[k].out.z, ends[k].out.x), hw});
      R = std::max(R, hw);
    }
    std::sort(ang.begin(), ang.end());
    for (size_t k = 0; k < ang.size(); ++k) {
      const auto &p = ang[k], &q = ang[(k + 1) % ang.size()];
      float gap = k + 1 < ang.size() ? q.first - p.first : q.first + 2 * kPi - p.first;
      if (gap < kPi - 0.05f && gap > 0.01f) R = std::max(R, std::max(p.second, q.second) / std::tan(gap * 0.5f));
    }
    j.radius = std::min(R + (j.kind == 3 ? 0.5f : 1.0f), 40.0f);
    for (size_t k : group) {
      const CurveRoad& c = g.curves[size_t(ends[k].curve)];
      std::vector<CurveSample> d = denseOf(c);
      const float L = d.back().s;
      CurveArm arm;
      arm.curve = ends[k].curve;
      arm.end = ends[k].end;
      arm.tram = (c.ends & 4) != 0;
      arm.kind = c.kind;
      if (c.ends & 0x20) arm.flow = arm.end ? 1 : 2, arm.single = (c.ends & 0x40) != 0, arm.ring = (c.ends & 0x80) != 0;
      size_t n = d.size(), hit = n;  // the first sample from the junction's end at the radius
      for (size_t i = 0; i < n; ++i) {
        size_t at = arm.end ? n - 1 - i : i;
        float dx = d[at].p.x - j.centre.x, dz = d[at].p.z - j.centre.z;
        if (dx * dx + dz * dz >= j.radius * j.radius) {
          hit = at;
          break;
        }
      }
      const CurveSample& q = hit < n ? d[hit] : (arm.end ? d.front() : d.back());
      arm.trim = arm.end ? L - q.s : q.s;
      arm.mouth = q.p;
      Vec3 dir = render::normalize(Vec3{q.dir.x, 0, q.dir.z});
      arm.out = arm.end ? dir * -1.0f : dir;
      j.arms.push_back(arm);
    }
    const Vec3 c = j.centre;
    std::sort(j.arms.begin(), j.arms.end(), [&](const CurveArm& x, const CurveArm& y) {
      return std::atan2(x.mouth.z - c.z, x.mouth.x - c.x) < std::atan2(y.mouth.z - c.z, y.mouth.x - c.x);
    });
    out.push_back(j);
  }
  return out;
}

Vec3 splitCurve(const CurveRoad& c, float s, CurveRoad& first, CurveRoad& second) {
  std::vector<CurveSample> d = denseOf(c);
  const float L = d.back().s;
  s = std::clamp(s, 0.01f, L - 0.01f);
  // t at arc length s (the dense samples are evenly spaced in t)
  size_t i = 1;
  while (i + 1 < d.size() && d[i].s < s) ++i;
  float f = d[i].s > d[i - 1].s ? (s - d[i - 1].s) / (d[i].s - d[i - 1].s) : 0;
  float t = (float(i - 1) + f) / float(d.size() - 1);
  // de Casteljau on the plan (x, z); the heights at the cut from the curve's own easing
  Vec3 P0{c.x0, 0, c.z0}, P1{c.x0 + c.dx0 * c.a, 0, c.z0 + c.dz0 * c.a}, P3{c.x1, 0, c.z1};
  Vec3 P2{c.x1 - c.dx1 * c.b, 0, c.z1 - c.dz1 * c.b};
  auto lerp = [](const Vec3& a, const Vec3& b, float k) { return a + (b - a) * k; };
  Vec3 Q0 = lerp(P0, P1, t), Q1 = lerp(P1, P2, t), Q2 = lerp(P2, P3, t), R0 = lerp(Q0, Q1, t), R1 = lerp(Q1, Q2, t);
  Vec3 S = lerp(R0, R1, t);
  S.y = bezier(c, t).y;
  auto len = [](const Vec3& v) { return std::sqrt(v.x * v.x + v.z * v.z); };
  Vec3 dm = render::normalize(Vec3{R1.x - R0.x, 0, R1.z - R0.z});
  first = c, second = c;
  first.x1 = S.x, first.y1 = S.y, first.z1 = S.z, first.dx1 = dm.x, first.dz1 = dm.z;
  first.a = std::max(0.5f, len(Q0 - P0)), first.b = std::max(0.5f, len(S - R0));
  second.x0 = S.x, second.y0 = S.y, second.z0 = S.z, second.dx0 = dm.x, second.dz0 = dm.z;
  second.a = std::max(0.5f, len(R1 - S)), second.b = std::max(0.5f, len(P3 - Q2));
  first.ends = uint8_t(c.ends & (1 | 4 | 8 | 32 | 64 | 128));
  second.ends = uint8_t(c.ends & (2 | 4 | 16 | 32 | 64 | 128));
  first.stops.clear(), second.stops.clear();
  for (CurveRoad::Stop st : c.stops) {
    if (st.s < s) {
      first.stops.push_back(st);
    } else {
      st.s -= s;
      second.stops.push_back(st);
    }
  }
  return S;
}

void curveTrims(const std::vector<CurveJunction>& js, int curve, float& trim0, float& trim1) {
  trim0 = trim1 = 0;
  for (const CurveJunction& j : js)
    for (const CurveArm& a : j.arms)
      if (a.curve == curve) (a.end ? trim1 : trim0) = a.trim;
}

std::vector<Vec3> junctionOutline(const CurveJunction& j, int layer, float delta, int perGap) {
  // side +1: right of the arm's way out, -1: left. An avenue ring's arm (its outer half only) has nothing on the island
  // side: its outer edge on the centre line, its kerb 4.5 beyond it (the right-hand carriageway's inner edge)
  auto widthOf = [&](const CurveArm& a, int side = 1) {
    const CurveProfile& pf = curveProfile(a.kind);
    if (a.ring && a.kind == 2 && side == (a.end == 0 ? -1 : 1)) return (layer ? 0.0f : -4.5f) + delta;
    return (layer ? pf.halfWidth : pf.carriageway) + delta;
  };
  std::vector<Vec3> pts;
  const size_t n = j.arms.size();
  const Vec3 c = j.centre;
  auto angleOf = [&](const Vec3& p) { return std::atan2(p.z - c.z, p.x - c.x); };
  auto atY = [&](Vec3 p) {
    p.y = c.y;
    return p;
  };
  for (size_t i = 0; i < n; ++i) {
    const CurveArm &A = j.arms[i], &B = j.arms[(i + 1) % n];
    Vec3 ra = rightOf(A.out), rb = rightOf(B.out);
    const float wa = widthOf(A, 1), wb = widthOf(B, -1);
    Vec3 Ea = atY(A.mouth + ra * wa), Eb = atY(B.mouth - rb * wb);
    pts.push_back(atY(A.mouth - ra * widthOf(A, -1)));  // the mouth edge
    pts.push_back(Ea);
    // between two arms of an avenue ring on the island side: along the island's edge (through the centre, which lies on
    // the ring's centre line), not across the island
    auto islandSide = [&](const CurveArm& a, int side) { return a.ring && a.kind == 2 && side == (a.end == 0 ? -1 : 1); };
    if (islandSide(A, 1) && islandSide(B, -1)) {
      Vec3 P = c * 2.0f - (Ea + Eb) * 0.5f;
      for (int k = 1; k <= perGap; ++k) {
        float t = float(k) / float(perGap + 1), u = 1 - t;
        pts.push_back(atY(Ea * (u * u) + P * (2 * u * t) + Eb * (t * t)));
      }
      continue;
    }
    float gap = angleOf(B.mouth) - angleOf(A.mouth);
    while (gap <= 0.0f) gap += 2 * kPi;
    if (gap > kPi + 0.09f) {  // around the back of the centre
      Vec3 ua = render::normalize(Vec3{A.mouth.x - c.x, 0, A.mouth.z - c.z});
      Vec3 ub = render::normalize(Vec3{B.mouth.x - c.x, 0, B.mouth.z - c.z});
      Vec3 Ba = c + rightOf(ua) * wa, Bb = c - rightOf(ub) * wb;
      float a0 = std::atan2(Ba.z - c.z, Ba.x - c.x), a1 = std::atan2(Bb.z - c.z, Bb.x - c.x);
      while (a1 <= a0) a1 += 2 * kPi;
      pts.push_back(atY(Ba));
      for (int k = 1; k <= perGap - 2; ++k) {
        float f = float(k) / float(perGap - 1), a = a0 + (a1 - a0) * f, w = wa + (wb - wa) * f;
        pts.push_back(atY(c + Vec3{std::cos(a) * w, 0, std::sin(a) * w}));
      }
      pts.push_back(atY(Bb));
      continue;
    }
    bool straight = gap > kPi - 0.09f;
    Vec3 X;
    if (!straight) {  // where the two edges meet
      float den = A.out.x * B.out.z - A.out.z * B.out.x;
      if (std::fabs(den) < 1e-4f) {
        straight = true;
      } else {
        Vec3 dd = Eb - Ea;
        float t = (dd.x * B.out.z - dd.z * B.out.x) / den;
        X = Ea + A.out * t;
        // edges that meet only beyond the mouths (arms curving toward each other, as a roundabout's ring on its island
        // side): no rounded corner, a straight side
        if (t > -0.1f) straight = true;
      }
    }
    Vec3 c1 = Ea + (X - Ea) * 0.55f, c2 = Eb + (X - Eb) * 0.55f;
    for (int k = 1; k <= perGap; ++k) {
      float t = float(k) / float(perGap + 1), u = 1 - t;
      Vec3 p = straight ? Ea + (Eb - Ea) * t
                        : Ea * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + Eb * (t * t * t);
      pts.push_back(atY(p));
    }
  }
  return pts;
}

bool insideOutline(const std::vector<Vec3>& poly, float x, float z) {
  bool in = false;
  for (size_t i = 0, k = poly.size() - 1; i < poly.size(); k = i++) {
    const Vec3 &a = poly[i], &b = poly[k];
    if ((a.z > z) != (b.z > z) && x < (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x) in = !in;
  }
  return in;
}

PlanarUV fitPlanarUV(const render::MeshData& m, int material) {
  PlanarUV r;
  double S[3][3] = {}, U[3] = {}, V[3] = {};
  float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
  int count = 0;
  for (const render::Submesh& sm : m.submeshes) {
    if (sm.material != material) continue;
    for (uint32_t k = 0; k < sm.indexCount; ++k) {
      uint32_t v = m.indices[sm.firstIndex + k];
      if (m.uv0.size() <= size_t(v) * 2 + 1) continue;
      double f[3] = {1.0, m.positions[v * 3], m.positions[v * 3 + 2]};
      for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) S[a][b] += f[a] * f[b];
        U[a] += f[a] * m.uv0[v * 2];
        V[a] += f[a] * m.uv0[v * 2 + 1];
      }
      x0 = std::min(x0, float(f[1])), x1 = std::max(x1, float(f[1]));
      z0 = std::min(z0, float(f[2])), z1 = std::max(z1, float(f[2]));
      ++count;
    }
  }
  auto det3 = [](double M[3][3]) {
    return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
           M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
  };
  double D = det3(S);
  if (count < 3 || std::fabs(D) < 1e-9 || x1 - x0 < 0.1f || z1 - z0 < 0.1f) return r;
  for (int col = 0; col < 3; ++col) {  // Cramer's rule for both fits
    double Mu[3][3], Mv[3][3];
    for (int a = 0; a < 3; ++a)
      for (int b = 0; b < 3; ++b) Mu[a][b] = b == col ? U[a] : S[a][b], Mv[a][b] = b == col ? V[a] : S[a][b];
    r.a[col] = float(det3(Mu) / D);
    r.b[col] = float(det3(Mv) / D);
  }
  r.x0 = x0, r.x1 = x1, r.z0 = z0, r.z1 = z1;
  r.ok = true;
  return r;
}

void buildJunctionMesh(const CurveJunction& j, int matFill, const PlanarUV& uv, int matWalk, render::MeshData& out,
                       int matMarks, int matTram) {
  const CurveProfile& pf = curveProfile(j.kind);
  const Vec3 up{0, 1, 0};
  const bool footpath = pf.carriageway <= 0;
  const int perGap = 12;
  const float fillY = footpath ? 0.0f : -0.2f;
  std::vector<Vec3> inner = junctionOutline(j, footpath ? 1 : 0, 0.0f, perGap);
  const size_t N = inner.size();
  if (N < 3) return;
  // the fill: a fan from the centre, cut into the texture's tiles (the template's own UV plane per tile)
  if (matFill >= 0 && uv.ok) {
    render::MeshData fan;
    auto put = [&](const Vec3& p) {
      fan.positions.insert(fan.positions.end(), {p.x, p.y + fillY, p.z});
      fan.normals.insert(fan.normals.end(), {0.0f, 1.0f, 0.0f});
      fan.uv0.insert(fan.uv0.end(), {0.0f, 0.0f});
      fan.uv1.insert(fan.uv1.end(), {0.0f, 0.0f});
    };
    put(j.centre);
    for (const Vec3& p : inner) put(p);
    render::Submesh sm;
    for (uint32_t i = 0; i < uint32_t(N); ++i) {
      uint32_t a = 0, b = 1 + i, c = 1 + (i + 1) % uint32_t(N);
      Vec3 pa = j.centre, pb = inner[i], pc = inner[(i + 1) % N];
      if (render::cross(pb - pa, pc - pa).y < 0) std::swap(b, c);
      fan.indices.insert(fan.indices.end(), {a, b, c});
    }
    sm.vertexCount = uint32_t(fan.vertexCount());
    sm.indexCount = uint32_t(fan.indices.size());
    fan.submeshes.push_back(sm);
    const float W = uv.x1 - uv.x0, H = uv.z1 - uv.z0;
    render::MeshData cut = sliceMeshAlongX(sliceMeshAlongX(fan, W, 0), H, 2);
    for (size_t t = 0; t + 2 < cut.indices.size(); t += 3) {
      uint32_t v[3] = {cut.indices[t], cut.indices[t + 1], cut.indices[t + 2]};
      float cx = 0, cz = 0;
      for (uint32_t k : v) cx += cut.positions[k * 3] / 3.0f, cz += cut.positions[k * 3 + 2] / 3.0f;
      float ix = std::floor(cx / W), iz = std::floor(cz / H);
      for (uint32_t k : v) {
        float lx = cut.positions[k * 3] - ix * W + uv.x0, lz = cut.positions[k * 3 + 2] - iz * H + uv.z0;
        cut.uv0[k * 2] = uv.a[0] + uv.a[1] * lx + uv.a[2] * lz;
        cut.uv0[k * 2 + 1] = uv.b[0] + uv.b[1] * lx + uv.b[2] * lz;
        cut.uv1[k * 2] = cut.uv0[k * 2], cut.uv1[k * 2 + 1] = cut.uv0[k * 2 + 1];
      }
    }
    appendPlacedMesh(cut, matFill, Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}, out);
  }
  if (footpath || matWalk < 0) return;
  // the pavement corners between the arms (the small road's pavement texture region) with their kerbs and outer walls
  std::vector<Vec3> outer = junctionOutline(j, 1, 0.0f, perGap);
  render::Submesh sub;
  sub.material = matWalk;
  sub.firstVertex = uint32_t(out.vertexCount());
  sub.firstIndex = uint32_t(out.indices.size());
  auto vertex = [&](const Vec3& p, const Vec3& n, float u, float v) {
    out.positions.insert(out.positions.end(), {p.x, p.y, p.z});
    out.normals.insert(out.normals.end(), {n.x, n.y, n.z});
    out.uv0.insert(out.uv0.end(), {u, v});
    out.uv1.insert(out.uv1.end(), {u, v});
    return uint32_t(out.vertexCount() - 1);
  };
  auto pos = [&](uint32_t i) { return Vec3{out.positions[i * 3], out.positions[i * 3 + 1], out.positions[i * 3 + 2]}; };
  auto tri = [&](uint32_t a, uint32_t b, uint32_t c, const Vec3& facing) {
    if (dot(render::cross(pos(b) - pos(a), pos(c) - pos(a)), facing) < 0) std::swap(b, c);
    out.indices.insert(out.indices.end(), {a, b, c});
  };
  const size_t n = j.arms.size(), per = size_t(perGap) + 2;
  for (size_t i = 0; i < n; ++i) {
    std::vector<size_t> line;  // the corner after arm i: its mouth edge's far point, the gap, the next arm's near point
    for (size_t k = 1; k <= size_t(perGap) + 1; ++k) line.push_back(i * per + k);
    line.push_back(((i + 1) % n) * per);
    float total = 0;
    for (size_t k = 1; k < line.size(); ++k) {
      Vec3 e = inner[line[k]] - inner[line[k - 1]];
      total += std::sqrt(dot(e, e));
    }
    const float scale = total > 12.0f ? 12.0f / total : 1.0f;
    float arc = 0;
    uint32_t pI = 0, pO = 0, kb = 0, kt = 0, ob = 0, ot = 0;
    for (size_t k = 0; k < line.size(); ++k) {
      const Vec3 &pi = inner[line[k]], &po = outer[line[k]];
      if (k > 0) {
        Vec3 e = pi - inner[line[k - 1]];
        arc += std::sqrt(dot(e, e));
      }
      float u = 0.8597f - 0.008933f * arc * scale;
      Vec3 toC = render::normalize(Vec3{j.centre.x - pi.x, 0, j.centre.z - pi.z});
      Vec3 away = render::normalize(Vec3{po.x - j.centre.x, 0, po.z - j.centre.z});
      uint32_t I = vertex(pi, up, u, 0.0190f), O = vertex(po, up, u, 0.0458f);
      uint32_t KB = vertex(pi + Vec3{0, -0.2f, 0}, toC, 0.1921f, 0.9970f), KT = vertex(pi, toC, 0.1921f, 0.9952f);
      uint32_t OB = vertex(po + Vec3{0, -0.6f, 0}, away, 0.1921f, 0.9970f), OT = vertex(po, away, 0.1921f, 0.9952f);
      if (k > 0) {
        tri(pI, pO, I, up), tri(pO, O, I, up);
        tri(kb, KB, kt, toC), tri(KB, KT, kt, toC);
        tri(ob, OB, ot, away), tri(OB, OT, ot, away);
      }
      pI = I, pO = O, kb = KB, kt = KT, ob = OB, ot = OT;
    }
  }
  sub.vertexCount = uint32_t(out.vertexCount()) - sub.firstVertex;
  sub.indexCount = uint32_t(out.indices.size()) - sub.firstIndex;
  if (sub.indexCount) out.submeshes.push_back(sub);
  // tram tracks: between every two tracked arms, along the tram lane's own Bézier (the street tram piece's rail strip,
  // tracks/small_basic: 1.4 wide, the texture repeating every 6 units)
  if (matTram >= 0) {
    render::Submesh tr;
    tr.material = matTram;
    tr.firstVertex = uint32_t(out.vertexCount());
    tr.firstIndex = uint32_t(out.indices.size());
    const float ry = j.centre.y - 0.18f;
    for (const CurveArm& A : j.arms)
      for (const CurveArm& B : j.arms) {
        Vec3 p0, p3;
        if (&A == &B || !A.tram || !B.tram || !junctionTramEnd(j, A, true, p0) || !junctionTramEnd(j, B, false, p3)) continue;
        Vec3 f0 = A.out * -1.0f, f1 = B.out, e = p3 - p0;
        float L = std::sqrt(dot(e, e));
        Vec3 p1 = p0 + f0 * (L / 3), p2 = p3 - f1 * (L / 3);
        const int steps = 16;
        std::vector<Vec3> pts;
        for (int k = 0; k <= steps; ++k) {
          float t = float(k) / float(steps), u = 1 - t;
          Vec3 p = p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
          p.y = ry;
          pts.push_back(p);
        }
        float arc = 0, w0 = 0;
        auto side = [&](size_t k) {
          Vec3 d = pts[std::min(k + 1, pts.size() - 1)] - pts[k > 0 ? k - 1 : 0];
          return rightOf(d);
        };
        uint32_t pL = vertex(pts[0] - side(0) * 0.7f, up, 1.0f, 0.1875f), pR = vertex(pts[0] + side(0) * 0.7f, up, 1.0f, 0.0625f);
        for (size_t k = 1; k < pts.size(); ++k) {
          Vec3 d = pts[k] - pts[k - 1];
          float seg = std::sqrt(dot(d, d)), segStart = arc;
          arc += seg;
          Vec3 sa = side(k - 1), sb = side(k);
          while (true) {
            float nextB = w0 + 6.0f;
            if (arc <= nextB + 1e-4f || seg < 1e-4f) {
              float u = 1.0f - (arc - w0) / 6.0f;
              uint32_t Lv = vertex(pts[k] - sb * 0.7f, up, u, 0.1875f), Rv = vertex(pts[k] + sb * 0.7f, up, u, 0.0625f);
              tri(pL, pR, Lv, up), tri(pR, Rv, Lv, up);
              pL = Lv, pR = Rv;
              break;
            }
            float t = (nextB - segStart) / seg;
            Vec3 m = pts[k - 1] + d * t, sm = render::normalize(sa + (sb - sa) * t);
            uint32_t Lv = vertex(m - sm * 0.7f, up, 0.0f, 0.1875f), Rv = vertex(m + sm * 0.7f, up, 0.0f, 0.0625f);
            tri(pL, pR, Lv, up), tri(pR, Rv, Lv, up);
            w0 = nextB;
            pL = vertex(m - sm * 0.7f, up, 1.0f, 0.1875f), pR = vertex(m + sm * 0.7f, up, 1.0f, 0.0625f);
          }
        }
      }
    tr.vertexCount = uint32_t(out.vertexCount()) - tr.firstVertex;
    tr.indexCount = uint32_t(out.indices.size()) - tr.firstIndex;
    if (tr.indexCount) out.submeshes.push_back(tr);
  }
  if (matMarks < 0) return;
  // markings (road-marks.dds regions of small_junction4.object, v as the loader gives them: raw + 1)
  render::Submesh mk;
  mk.material = matMarks;
  mk.firstVertex = uint32_t(out.vertexCount());
  mk.firstIndex = uint32_t(out.indices.size());
  const float my = j.centre.y - 0.19f;
  auto at = [&](Vec3 p) {
    p.y = my;
    return p;
  };
  // zebra crossings: bars across each arm just inside its mouth, cycling through the atlas' bar variants
  for (const CurveArm& A : j.arms) {
    if (A.ring || curveIsRamp(A.kind)) continue;  // no crossing over a roundabout's ring or a ramp
    Vec3 in = A.out * -1.0f, r = rightOf(A.out), m0 = at(A.mouth);
    int bars = std::max(1, int((2.0f * curveProfile(A.kind).carriageway - 0.32f) / 0.93f));
    float first = -(float(bars) * 0.93f - 0.32f) * 0.5f;
    for (int k = 0; k < bars; ++k) {
      float z0 = first + float(k) * 0.93f, z1 = z0 + 0.61f;
      float u0 = 0.1587f + 0.033f * float(k % 5), u1 = u0 + 0.0215f;
      uint32_t a = vertex(m0 + in * 0.17f + r * z0, up, u0, 0.5466f), b = vertex(m0 + in * 0.17f + r * z1, up, u1, 0.5466f);
      uint32_t c = vertex(m0 + in * 2.88f + r * z1, up, u1, 0.3492f), d = vertex(m0 + in * 2.88f + r * z0, up, u0, 0.3492f);
      tri(a, b, c, up), tri(a, c, d, up);
    }
  }
  // edge lines along the kerbs (0.19..0.48 inside), the texture repeating every 9 units as on the straight road
  std::vector<Vec3> e0 = junctionOutline(j, 0, -0.19f, perGap), e1 = junctionOutline(j, 0, -0.48f, perGap);
  const float uA = 0.9916f, uRate = -0.1099f, vOut = 0.0722f, vIn = 0.0401f;
  for (size_t i = 0; i < n; ++i) {
    std::vector<size_t> line;
    for (size_t k = 1; k <= size_t(perGap) + 1; ++k) line.push_back(i * per + k);
    line.push_back(((i + 1) % n) * per);
    float arc = 0, w0 = 0;
    uint32_t pO = vertex(at(e0[line[0]]), up, uA, vOut), pI = vertex(at(e1[line[0]]), up, uA, vIn);
    for (size_t k = 1; k < line.size(); ++k) {
      Vec3 a = at(e0[line[k - 1]]), b = at(e0[line[k]]), ai = at(e1[line[k - 1]]), bi = at(e1[line[k]]);
      Vec3 e = b - a;
      float L = std::sqrt(dot(e, e)), segStart = arc;
      arc += L;
      while (true) {
        float nextB = w0 + 9.0f;
        if (arc <= nextB + 1e-4f || L < 1e-4f) {
          uint32_t O = vertex(b, up, uA + uRate * (arc - w0), vOut), I = vertex(bi, up, uA + uRate * (arc - w0), vIn);
          tri(pO, pI, O, up), tri(pI, I, O, up);
          pO = O, pI = I;
          break;
        }
        float t = (nextB - segStart) / L;
        Vec3 mo = a + (b - a) * t, mi = ai + (bi - ai) * t;
        uint32_t O = vertex(mo, up, uA + uRate * 9.0f, vOut), I = vertex(mi, up, uA + uRate * 9.0f, vIn);
        tri(pO, pI, O, up), tri(pI, I, O, up);
        w0 = nextB;
        pO = vertex(mo, up, uA, vOut), pI = vertex(mi, up, uA, vIn);
      }
    }
  }
  mk.vertexCount = uint32_t(out.vertexCount()) - mk.firstVertex;
  mk.indexCount = uint32_t(out.indices.size()) - mk.firstIndex;
  if (mk.indexCount) out.submeshes.push_back(mk);
}

void junctionCells(const GridData& g, const CurveJunction& j, int& x0, int& z0, int& x1, int& z1) {
  float lx = 1e30f, lz = 1e30f, hx = -1e30f, hz = -1e30f;
  for (const Vec3& p : junctionOutline(j, 1, 0.0f, 4))
    lx = std::min(lx, p.x), lz = std::min(lz, p.z), hx = std::max(hx, p.x), hz = std::max(hz, p.z);
  const float m = 3.0f, cs = g.cellSize;
  x0 = std::clamp(int(std::floor((lx - m) / cs + float(g.width) * 0.5f)), 0, g.width - 1);
  z0 = std::clamp(int(std::floor((lz - m) / cs + float(g.length) * 0.5f)), 0, g.length - 1);
  x1 = std::clamp(int(std::floor((hx + m) / cs + float(g.width) * 0.5f)), 0, g.width - 1);
  z1 = std::clamp(int(std::floor((hz + m) / cs + float(g.length) * 0.5f)), 0, g.length - 1);
}

// the junctions with an arm ending where one of c's ends is
static std::vector<CurveJunction> junctionsAt(const GridData& g, const CurveRoad& c) {
  std::vector<CurveJunction> r;
  for (const CurveJunction& j : findCurveJunctions(g))
    for (const Vec3& e : {Vec3{c.x0, 0, c.z0}, Vec3{c.x1, 0, c.z1}})
      if ((j.kind == 3) == (c.kind == 3) && std::fabs(j.centre.x - e.x) < 0.3f && std::fabs(j.centre.z - e.z) < 0.3f) {
        r.push_back(j);
        break;
      }
  return r;
}

void levelTerrainUnderJunctions(GridObj* grid, const CurveRoad& c) {
  GridData& g = *grid->data;
  if (g.height.empty()) return;
  const float cs = g.cellSize;
  for (const CurveJunction& j : junctionsAt(g, c)) {
    std::vector<Vec3> outer = junctionOutline(j, 1, 0.0f, 6);
    int x0, z0, x1, z1;
    junctionCells(g, j, x0, z0, x1, z1);
    int16_t target = int16_t(std::lround((j.centre.y - 0.25f) * 256.0f / cs));
    // bridge arms: their part of the junction (farther from the centre than the other arms' half width) stays over the
    // ground as it is, and so does water (raised riverbed showed as a flat box beside a bridge, OSM import)
    std::vector<std::pair<std::vector<CurveSample>, float>> bridges;  // shape, half width
    float core = 0;
    for (const CurveArm& a : j.arms) {
      const CurveRoad& ac = g.curves[size_t(a.curve)];
      if (ac.bridge) bridges.push_back({curveShape(ac), curveProfile(ac.kind).halfWidth});
      else core = std::max(core, curveProfile(ac.kind).halfWidth);
    }
    auto water = [&](int x, int z) {
      for (int dz = -1; dz <= 0; ++dz)
        for (int dx = -1; dx <= 0; ++dx) {
          int cx = x + dx, cz = z + dz;
          if (cx >= 0 && cz >= 0 && cx < g.width && cz < g.length && (g.flags[size_t(cz) * size_t(g.width) + size_t(cx)] & 0x2000))
            return true;
        }
      return false;
    };
    for (int z = z0; z <= std::min(z1 + 1, g.length - 1); ++z)
      for (int x = x0; x <= std::min(x1 + 1, g.width - 1); ++x) {
        size_t ci = size_t(z) * size_t(g.width) + size_t(x);
        if (g.flags[ci] & 0x1000) continue;  // a locked corner
        float wx = (float(x) - float(g.width) * 0.5f) * cs, wz = (float(z) - float(g.length) * 0.5f) * cs;
        if (!insideOutline(outer, wx, wz)) continue;
        if (water(x, z)) continue;
        bool onBridge = false;
        float fromCentre = std::sqrt((wx - j.centre.x) * (wx - j.centre.x) + (wz - j.centre.z) * (wz - j.centre.z));
        for (const auto& [shape, hw] : bridges) {
          float side, s;
          curveNearest(shape, wx, wz, side, s);
          if (std::fabs(side) <= hw + 1.5f && fromCentre > core) onBridge = true;
        }
        if (onBridge) continue;
        g.height[ci] = target;
      }
    gridEditCellsChanged(grid, std::max(0, x0 - 1), std::max(0, z0 - 1), x1, z1, true);
  }
}

}  // namespace regs::engine
