// port: painted areas with exact edges (paint_edges.h)
#include "engine/paint_edges.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>
#include <functional>
#include <map>

namespace regs::engine {

namespace {

struct Seg {
  double ax, az, bx, bz;
};

// the part of segment s inside the square [x, x + 1] × [z, z + 1] (Liang–Barsky); false when it misses
bool clipToCell(const Seg& s, int x, int z, double& t0, double& t1) {
  double dx = s.bx - s.ax, dz = s.bz - s.az;
  t0 = 0, t1 = 1;
  const double p[4] = {-dx, dx, -dz, dz};
  const double q[4] = {s.ax - x, x + 1 - s.ax, s.az - z, z + 1 - s.az};
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0) {
      if (q[i] < 0) return false;
      continue;
    }
    double r = q[i] / p[i];
    if (p[i] < 0) t0 = std::max(t0, r);
    else t1 = std::min(t1, r);
    if (t0 > t1) return false;
  }
  return t1 - t0 > 1e-9;
}

bool insideRings(const std::vector<PaintRing>& rings, double px, double pz) {
  bool in = false;
  for (const PaintRing& r : rings)
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
      double ax = r[j].first, az = r[j].second, bx = r[i].first, bz = r[i].second;
      if ((az <= pz) != (bz <= pz) && px < ax + (pz - az) * (bx - ax) / (bz - az)) in = !in;
    }
  return in;
}

void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v)), o.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& o, uint32_t v) {
  for (int k = 0; k < 4; ++k) o.push_back(uint8_t(v >> (8 * k)));
}
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// the 28 bytes of an edge after its cell: u8 type, u8 lines | join << 4, u16 0, six floats
void putEdge(std::vector<uint8_t>& o, const PaintEdge& e) {
  o.push_back(e.type);
  o.push_back(uint8_t((e.lines & 0xF) | (e.join & 1) << 4));
  put16(o, 0);
  for (int k = 0; k < 2; ++k)
    for (int i = 0; i < 3; ++i) {
      uint32_t v;
      std::memcpy(&v, &e.l[k][i], 4);
      put32(o, v);
    }
}
bool getEdge(const uint8_t* p, PaintEdge& e) {
  e.type = uint8_t(p[0] & 7);
  e.lines = uint8_t(p[1] & 0xF);
  e.join = uint8_t(p[1] >> 4 & 1);
  for (int k = 0; k < 2; ++k)
    for (int i = 0; i < 3; ++i) {
      uint32_t v = get32(p + 4 + (k * 3 + i) * 4);
      std::memcpy(&e.l[k][i], &v, 4);
      if (!std::isfinite(e.l[k][i])) return false;
    }
  return e.lines == 1 || e.lines == 2;
}

constexpr int kSamples = 12;  // per side of a cell, for the fit

// the pavement paint a cell has whole (flags 0x8, no preview mark 0x40), else −1
int wholePaint(uint16_t f) { return (f & 0x48) == 0x8 ? (f & 7) : -1; }

// a candidate line of a cell's edge (the cell's own coordinates, inside on the positive side) and how much outline it
// carries (the longest are tried first)
struct Cand {
  float l[3];
  double len;
};

void addCand(std::vector<Cand>& cands, const Cand& c) {
  for (Cand& o : cands)
    if (std::fabs(o.l[0] - c.l[0]) < 1e-3f && std::fabs(o.l[1] - c.l[1]) < 1e-3f && std::fabs(o.l[2] - c.l[2]) < 1e-3f) {
      o.len += c.len;
      return;
    }
  cands.push_back(c);
}

// the best edge for the sampled truth: one line, or two joined as both or either, most samples right (fewer lines on
// a tie)
PaintEdge fitEdge(const bool* truth, std::vector<Cand> cands) {
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.len > b.len; });
  if (cands.size() > 6) cands.resize(6);
  PaintEdge best;
  int bestScore = -1;
  auto score = [&](const PaintEdge& e) {
    int n = 0;
    for (int j = 0; j < kSamples; ++j)
      for (int i = 0; i < kSamples; ++i)
        n += paintEdgeInside(e, float((i + 0.5) / kSamples), float((j + 0.5) / kSamples)) == truth[j * kSamples + i];
    return n;
  };
  for (size_t a = 0; a < cands.size(); ++a) {
    PaintEdge e;
    e.lines = 1;
    std::memcpy(e.l[0], cands[a].l, sizeof e.l[0]);
    if (int s = score(e); s > bestScore) best = e, bestScore = s;
  }
  for (size_t a = 0; a < cands.size() && bestScore < kSamples * kSamples; ++a)
    for (size_t b = a + 1; b < cands.size(); ++b)
      for (uint8_t join = 0; join < 2; ++join) {
        PaintEdge e;
        e.lines = 2, e.join = join;
        std::memcpy(e.l[0], cands[a].l, sizeof e.l[0]);
        std::memcpy(e.l[1], cands[b].l, sizeof e.l[1]);
        if (int s = score(e); s > bestScore) best = e, bestScore = s;
      }
  return best;
}

}  // namespace

std::vector<PaintAreaCell> paintAreaCells(int W, int L, const std::vector<PaintRing>& rings) {
  std::vector<PaintAreaCell> out;
  std::vector<Seg> segs;
  double minX = 1e30, minZ = 1e30, maxX = -1e30, maxZ = -1e30;
  for (const PaintRing& r : rings) {
    if (r.size() < 3) continue;
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
      segs.push_back({r[j].first, r[j].second, r[i].first, r[i].second});
      minX = std::min(minX, r[i].first), maxX = std::max(maxX, r[i].first);
      minZ = std::min(minZ, r[i].second), maxZ = std::max(maxZ, r[i].second);
    }
  }
  if (segs.empty()) return out;
  int x0 = std::max(0, int(std::floor(minX))), x1 = std::min(W - 1, int(std::floor(maxX)));
  int z0 = std::max(0, int(std::floor(minZ))), z1 = std::min(L - 1, int(std::floor(maxZ)));
  if (x0 > x1 || z0 > z1) return out;
  // the outline's segments per border cell
  std::map<uint32_t, std::vector<size_t>> border;
  for (size_t s = 0; s < segs.size(); ++s) {
    const Seg& g = segs[s];
    int sx0 = std::max(x0, int(std::floor(std::min(g.ax, g.bx)))), sx1 = std::min(x1, int(std::floor(std::max(g.ax, g.bx))));
    int sz0 = std::max(z0, int(std::floor(std::min(g.az, g.bz)))), sz1 = std::min(z1, int(std::floor(std::max(g.az, g.bz))));
    for (int z = sz0; z <= sz1; ++z)
      for (int x = sx0; x <= sx1; ++x) {
        double t0, t1;
        if (clipToCell(g, x, z, t0, t1)) border[paintEdgeKey(x, z)].push_back(s);
      }
  }
  for (int z = z0; z <= z1; ++z)
    for (int x = x0; x <= x1; ++x) {
      auto it = border.find(paintEdgeKey(x, z));
      if (it == border.end()) {
        if (insideRings(rings, x + 0.5, z + 0.5)) out.push_back({x, z, true, {}});
        continue;
      }
      // the outline sampled in the cell (every crossed cell is cut, slivers and nearly whole ones too: no notches)
      bool truth[kSamples * kSamples];
      for (int j = 0; j < kSamples; ++j)
        for (int i = 0; i < kSamples; ++i)
          truth[j * kSamples + i] = insideRings(rings, x + (i + 0.5) / kSamples, z + (j + 0.5) / kSamples);
      // candidate lines: the segments crossing the cell, in the cell's own coordinates, inside on the positive side
      std::vector<Cand> cands;
      for (size_t s : it->second) {
        const Seg& g = segs[s];
        double t0, t1;
        clipToCell(g, x, z, t0, t1);
        double dx = g.bx - g.ax, dz = g.bz - g.az, len = std::sqrt(dx * dx + dz * dz);
        if (len < 1e-9) continue;
        double nx = -dz / len, nz = dx / len;  // left of a → b
        double mt = (t0 + t1) * 0.5, mx = g.ax + dx * mt, mz = g.az + dz * mt;
        if (!insideRings(rings, mx + nx * 1e-4, mz + nz * 1e-4)) nx = -nx, nz = -nz;
        Cand c;
        c.l[0] = float(nx), c.l[1] = float(nz), c.l[2] = float(nx * (x - g.ax) + nz * (z - g.az));
        c.len = (t1 - t0) * len;
        addCand(cands, c);
      }
      if (cands.empty()) continue;
      out.push_back({x, z, false, fitEdge(truth, cands)});
    }
  return out;
}

std::vector<PaintAreaCell> paintArea(GridData& g, const std::vector<PaintRing>& rings, int type, int& x0, int& z0,
                                     int& x1, int& z1) {
  x0 = z0 = 1 << 30, x1 = z1 = -1;
  std::vector<PaintAreaCell> cells = paintAreaCells(g.width, g.length, rings);
  type &= 7;
  for (PaintAreaCell& c : cells) {
    uint16_t& f = g.flags[size_t(c.z) * size_t(g.width) + size_t(c.x)];
    if (f & 0x2000) continue;
    const uint32_t key = paintEdgeKey(c.x, c.z);
    auto it = g.paintEdges.find(key);
    const bool paintedSame = (f & 0x48) == 0x8 && (f & 7) == type;
    if (!c.full && !paintedSame && it != g.paintEdges.end()) {
      // a border cell another area cut already: the same paint joins both cuts (refitted from both edges' lines);
      // another paint takes the rest of the cell when it covered most of that rest
      const PaintEdge old = it->second;
      bool truth[kSamples * kSamples];
      int count = 0, other = 0;
      for (int j = 0; j < kSamples; ++j)
        for (int i = 0; i < kSamples; ++i) {
          float u = float((i + 0.5) / kSamples), v = float((j + 0.5) / kSamples);
          bool mine = paintEdgeInside(c.edge, u, v), was = paintEdgeInside(old, u, v);
          bool in = mine || (old.type == type && was);
          truth[j * kSamples + i] = in;
          count += in;
          other += !in && was && old.type != type;
        }
      if (old.type == type) {
        std::vector<Cand> cands;
        for (const PaintEdge* e : {static_cast<const PaintEdge*>(&c.edge), &old})
          for (int k = 0; k < e->lines; ++k) addCand(cands, {{e->l[k][0], e->l[k][1], e->l[k][2]}, 1.0});
        if (count == kSamples * kSamples) c.full = true;
        else c.edge = fitEdge(truth, cands);
      } else if (wholePaint(f) < 0 && 2 * other >= kSamples * kSamples - count) {
        f = uint16_t((f & 0xFF98) | 0x8 | old.type);
      }
    }
    if (c.full) {
      if (it != g.paintEdges.end()) g.paintEdges.erase(it);
      f = uint16_t((f & 0xFF98) | 0x8 | type);
    } else if (!paintedSame) {
      c.edge.type = uint8_t(type);
      g.paintEdges[key] = c.edge;
    }
    x0 = std::min(x0, c.x), z0 = std::min(z0, c.z), x1 = std::max(x1, c.x), z1 = std::max(z1, c.z);
  }
  return cells;
}

namespace {
int g_brushShape = kBrushCircle;
}
int brushShape() { return g_brushShape; }
void setBrushShape(int shape) { g_brushShape = std::clamp(shape, 0, 3); }

BrushShape BrushShape::sweptFrom(double ax, double az) const {
  BrushShape at = *this;
  at.cx = ax, at.cz = az;
  PaintRing pts = outline(), more = at.outline();
  pts.insert(pts.end(), more.begin(), more.end());
  // the convex hull (Andrew's monotone chain), counter-clockwise
  std::sort(pts.begin(), pts.end());
  pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
  auto cross = [](const std::pair<double, double>& o, const std::pair<double, double>& a, const std::pair<double, double>& b) {
    return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first);
  };
  PaintRing hull(2 * pts.size());
  size_t k = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 1e-12) --k;
    hull[k++] = pts[i];
  }
  for (size_t i = pts.size() - 1, t = k + 1; i-- > 0;) {
    while (k >= t && cross(hull[k - 2], hull[k - 1], pts[i]) <= 1e-12) --k;
    hull[k++] = pts[i];
  }
  hull.resize(k > 1 ? k - 1 : k);
  BrushShape s;
  s.kind = kBrushSweep;
  s.cx = (ax + cx) * 0.5, s.cz = (az + cz) * 0.5;
  s.R = R + 0.5 * std::sqrt((cx - ax) * (cx - ax) + (cz - az) * (cz - az));
  s.poly = std::move(hull);
  return s;
}

void BrushShape::bounds(double& x0, double& z0, double& x1, double& z1) const {
  if (kind != kBrushSweep) {
    x0 = cx - R, z0 = cz - R, x1 = cx + R, z1 = cz + R;
    return;
  }
  x0 = z0 = 1e30, x1 = z1 = -1e30;
  for (const auto& [px, pz] : poly) x0 = std::min(x0, px), x1 = std::max(x1, px), z0 = std::min(z0, pz), z1 = std::max(z1, pz);
}

bool BrushShape::polyContains(double px, double pz) const {
  if (poly.size() < 3) return false;
  // convex, either way round: the point on the same side of every side as the outline turns
  double area = 0;
  for (size_t i = 0; i < poly.size(); ++i) {
    const auto& a = poly[i];
    const auto& b = poly[(i + 1) % poly.size()];
    area += a.first * b.second - b.first * a.second;
  }
  const double sign = area >= 0 ? 1.0 : -1.0;
  for (size_t i = 0; i < poly.size(); ++i) {
    const auto& a = poly[i];
    const auto& b = poly[(i + 1) % poly.size()];
    if (sign * ((b.first - a.first) * (pz - a.second) - (b.second - a.second) * (px - a.first)) < -1e-9) return false;
  }
  return true;
}

bool BrushShape::polyTouches(int x, int z) const {
  if (poly.size() < 3) return false;
  double x0, z0, x1, z1;
  bounds(x0, z0, x1, z1);
  if (x1 <= x || x0 >= x + 1 || z1 <= z || z0 >= z + 1) return false;
  for (int k = 0; k < 4; ++k)  // a cell corner inside
    if (polyContains(x + (k & 1), z + (k >> 1))) return true;
  for (const auto& [px, pz] : poly)  // a corner of the outline inside the cell
    if (px > x && px < x + 1 && pz > z && pz < z + 1) return true;
  for (size_t i = 0; i < poly.size(); ++i) {  // a side crossing the cell
    const auto& a = poly[i];
    const auto& b = poly[(i + 1) % poly.size()];
    double t0, t1;
    if (clipToCell(Seg{a.first, a.second, b.first, b.second}, x, z, t0, t1)) return true;
  }
  return false;
}

PaintRing BrushShape::outline() const {
  PaintRing r;
  if (kind == kBrushSweep) {
    r = poly;
  } else if (kind == kBrushSquare) {
    r = {{cx - R, cz - R}, {cx + R, cz - R}, {cx + R, cz + R}, {cx - R, cz + R}};
  } else if (kind == kBrushDiamond) {
    r = {{cx, cz - R}, {cx + R, cz}, {cx, cz + R}, {cx - R, cz}};
  } else {
    for (int k = 0; k < 64; ++k) {
      double a = k * 6.283185307179586 / 64;
      r.push_back({cx + R * std::cos(a), cz + R * std::sin(a)});
    }
  }
  return r;
}

void paintShape(GridData& g, int brushType, const BrushShape& shape) {
  const double cx = shape.cx, cz = shape.cz, R = shape.R;
  if (R <= 0 || brushType < 1 || brushType > 9) return;
  const bool erase = brushType == 9;
  const int t = brushType - 1;
  double bx0, bz0, bx1, bz1;
  shape.bounds(bx0, bz0, bx1, bz1);
  int x0 = std::max(0, int(std::floor(bx0))), x1 = std::min(g.width - 1, int(std::floor(bx1)));
  int z0 = std::max(0, int(std::floor(bz0))), z1 = std::min(g.length - 1, int(std::floor(bz1)));
  auto inCircle = [&](double px, double pz) { return shape.contains(px, pz); };
  // the square's, the diamond's and a sweep's sides as cut lines, inward (the paint) or outward (the eraser)
  std::vector<std::array<double, 6>> sides;  // the side from (0, 1), its inward normal (2, 3), to (4, 5)
  if (shape.kind != kBrushCircle) {
    PaintRing o = shape.outline();
    for (size_t i = 0; i < o.size(); ++i) {
      auto a = o[i], b = o[(i + 1) % o.size()];
      double nx = -(b.second - a.second), nz = b.first - a.first, len = std::sqrt(nx * nx + nz * nz);
      if (len < 1e-9) continue;
      nx /= len, nz /= len;
      if (nx * (cx - a.first) + nz * (cz - a.second) < 0) nx = -nx, nz = -nz;
      sides.push_back({a.first, a.second, nx, nz, b.first, b.second});
    }
  }
  for (int z = z0; z <= z1; ++z)
    for (int x = x0; x <= x1; ++x) {
      if (!shape.touches(x, z)) continue;  // the brush misses the cell
      uint16_t& f = g.flags[size_t(z) * size_t(g.width) + size_t(x)];
      const uint32_t key = paintEdgeKey(x, z);
      auto it = g.paintEdges.find(key);
      // wholly inside (all four corners: the shapes are convex): painted or bare, without an edge
      if (shape.contains(x, z) && shape.contains(x + 1, z) && shape.contains(x, z + 1) && shape.contains(x + 1, z + 1)) {
        if (it != g.paintEdges.end()) g.paintEdges.erase(it);
        f = erase ? uint16_t(f & 0xFFF0) : uint16_t((f & 0xFF98) | 0x8 | t);
        continue;
      }
      const int own = (f & 0x48) == 0x8 ? (f & 7) : -1;  // the cell's whole paint
      const PaintEdge* old = it != g.paintEdges.end() ? &it->second : nullptr;
      int type;
      std::function<bool(double, double)> was;  // the paint of `type` in the cell before the dab
      if (!erase) {
        if (own == t) continue;  // painted whole already
        type = t;
        was = [&](double u, double v) { return old && old->type == t && paintEdgeInside(*old, float(u), float(v)); };
      } else if (own >= 0) {
        type = own;
        was = [](double, double) { return true; };
      } else if (old) {
        type = old->type;
        was = [&](double u, double v) { return paintEdgeInside(*old, float(u), float(v)); };
      } else {
        continue;  // nothing to erase
      }
      bool truth[kSamples * kSamples];
      int count = 0;
      for (int j = 0; j < kSamples; ++j)
        for (int i = 0; i < kSamples; ++i) {
          double u = (i + 0.5) / kSamples, v = (j + 0.5) / kSamples;
          bool in = erase ? was(u, v) && !inCircle(x + u, z + v) : was(u, v) || inCircle(x + u, z + v);
          truth[j * kSamples + i] = in;
          count += in;
        }
      // candidates: the old edge's lines, the circle's tangents where it crosses the cell (inward for paint)
      std::vector<Cand> cands;
      if (old && old->type == type)
        for (int k = 0; k < old->lines; ++k) addCand(cands, {{old->l[k][0], old->l[k][1], old->l[k][2]}, 5.0});  // first in line
      if (shape.kind == kBrushCircle) {
        double a0 = std::atan2(z + 0.5 - cz, x + 0.5 - cx), spread = std::min(0.6, 0.6 / R);
        for (double a : {a0, a0 - spread, a0 + spread}) {
          double px = cx + R * std::cos(a), pz = cz + R * std::sin(a), ux = -std::cos(a), uz = -std::sin(a);
          if (erase) ux = -ux, uz = -uz;
          addCand(cands, {{float(ux), float(uz), float(ux * (x - px) + uz * (z - pz))}, a == a0 ? 0.9 : 0.8});
        }
      } else {  // the sides crossing the cell first (by the length inside it), then the nearer ones
        for (const auto& s : sides) {
          double ux = erase ? -s[2] : s[2], uz = erase ? -s[3] : s[3], t0, t1, weight;
          Seg sg{s[0], s[1], s[4], s[5]};
          if (clipToCell(sg, x, z, t0, t1)) {
            weight = 2.0 + (t1 - t0) * std::sqrt((s[4] - s[0]) * (s[4] - s[0]) + (s[5] - s[1]) * (s[5] - s[1]));
          } else {
            double d = std::fabs(s[2] * (x + 0.5 - s[0]) + s[3] * (z + 0.5 - s[1]));
            weight = 0.9 - std::min(0.5, d * 0.01);
          }
          addCand(cands, {{float(ux), float(uz), float(ux * (x - s[0]) + uz * (z - s[1]))}, weight});
        }
      }
      if (erase && own >= 0) f = uint16_t(f & 0xFFF0);  // the cell keeps its paint only inside the new edge
      // paint over another paint's edge: a cell holds one edge, so the other paint takes the rest of the cell when it
      // covered most of that rest (else the rest goes bare); no notch where strokes of two paints meet
      if (!erase && old && old->type != t && own < 0 && count > 0 && count < kSamples * kSamples) {
        int other = 0;
        for (int j = 0; j < kSamples; ++j)
          for (int i = 0; i < kSamples; ++i)
            other += !truth[j * kSamples + i] && paintEdgeInside(*old, float((i + 0.5) / kSamples), float((j + 0.5) / kSamples));
        if (2 * other >= kSamples * kSamples - count) f = uint16_t((f & 0xFF98) | 0x8 | old->type);
      }
      if (count == 0) {
        if (it != g.paintEdges.end()) g.paintEdges.erase(it);
      } else if (count == kSamples * kSamples) {
        if (it != g.paintEdges.end()) g.paintEdges.erase(it);
        f = uint16_t((f & 0xFF98) | 0x8 | type);
      } else {
        PaintEdge e = fitEdge(truth, cands);
        e.type = uint8_t(type);
        g.paintEdges[key] = e;
      }
    }
}

void writePaintEdgeSection(const GridData& g, std::vector<uint8_t>& out) {
  if (g.paintEdges.empty()) return;
  std::vector<uint8_t> body;
  put32(body, uint32_t(g.paintEdges.size()));
  for (const auto& [key, e] : g.paintEdges) {
    put16(body, uint16_t(key & 0xFFFF));
    put16(body, uint16_t(key >> 16));
    putEdge(body, e);
  }
  put32(out, kPaintEdgeSectionTag);
  put32(out, uint32_t(body.size()));
  out.insert(out.end(), body.begin(), body.end());
}

void parsePaintEdgeSection(const std::vector<uint8_t>& d, size_t body, size_t stop, GridData& g) {
  if (body + 4 > stop) return;
  uint32_t count = get32(d.data() + body);
  for (uint32_t i = 0; i < count && body + 4 + size_t(i + 1) * 32 <= stop; ++i) {
    const uint8_t* p = d.data() + body + 4 + size_t(i) * 32;
    int x = get16(p), z = get16(p + 2);
    PaintEdge e;
    if (x < g.width && z < g.length && getEdge(p + 4, e)) g.paintEdges[paintEdgeKey(x, z)] = e;
  }
}

std::vector<uint8_t> paintEdgeActionBytes(const GridData& g, int x0, int z0, int w, int l) {
  std::vector<uint8_t> o;
  put16(o, uint16_t(int16_t(x0)));
  put16(o, uint16_t(int16_t(z0)));
  put16(o, uint16_t(w));
  put16(o, uint16_t(l));
  put32(o, 0);
  uint32_t count = 0;
  for (int z = std::max(0, z0); z < std::min(g.length, z0 + l); ++z) {
    auto it = g.paintEdges.lower_bound(paintEdgeKey(std::max(0, x0), z));
    for (; it != g.paintEdges.end() && it->first <= paintEdgeKey(std::min(g.width, x0 + w) - 1, z); ++it) {
      put16(o, uint16_t(int(it->first & 0xFFFF) - x0));
      put16(o, uint16_t(z - z0));
      putEdge(o, it->second);
      ++count;
    }
  }
  for (int k = 0; k < 4; ++k) o[8 + k] = uint8_t(count >> (8 * k));
  return o;
}

bool applyPaintEdgeAction(GridData& g, const std::vector<uint8_t>& raw, int& x0, int& z0, int& w, int& l) {
  if (raw.size() < 12) return false;
  x0 = int16_t(get16(raw.data())), z0 = int16_t(get16(raw.data() + 2));
  w = get16(raw.data() + 4), l = get16(raw.data() + 6);
  uint32_t count = get32(raw.data() + 8);
  for (int z = std::max(0, z0); z < std::min(g.length, z0 + l); ++z)
    for (int x = std::max(0, x0); x < std::min(g.width, x0 + w); ++x) g.paintEdges.erase(paintEdgeKey(x, z));
  for (uint32_t i = 0; i < count && 12 + size_t(i + 1) * 32 <= raw.size(); ++i) {
    const uint8_t* p = raw.data() + 12 + size_t(i) * 32;
    int x = x0 + get16(p), z = z0 + get16(p + 2);
    PaintEdge e;
    if (x >= 0 && z >= 0 && x < g.width && z < g.length && getEdge(p + 4, e)) g.paintEdges[paintEdgeKey(x, z)] = e;
  }
  return true;
}

void shiftPaintEdgeAction(std::vector<uint8_t>& raw, int dx, int dz) {
  if (raw.size() < 4) return;
  int x = int16_t(get16(raw.data())) + dx, z = int16_t(get16(raw.data() + 2)) + dz;
  raw[0] = uint8_t(x), raw[1] = uint8_t(x >> 8), raw[2] = uint8_t(z), raw[3] = uint8_t(z >> 8);
}

}  // namespace regs::engine
