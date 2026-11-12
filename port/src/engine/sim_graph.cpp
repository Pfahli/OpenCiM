// The lane graph (docs/simulation/pathfinding.md): template paths from the .object locators (gsGridObject::loaded,
// L:0x081808b0) and setPathFlags / addPath, one node per path of every placed item (gsGridObject::addPaths,
// L:0x08186df0), successor links by endpoint distance and direction (FUN_100e4dd0) and the lane search (FUN_100e5fe0).
#include "engine/sim_graph.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <queue>

#include "core/log.h"
#include "core/text.h"
#include "engine/curve_roads.h"
#include "engine/api.h"
#include "engine/scene3d.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using render::Quat;
using render::Vec3;

namespace {

// A named path of a template in its object space (after the load-time rotation): the 0x3c-byte record of the
// template's path map (+0x29c).
struct PathRecord {
  uint32_t flags = 0;
  Vec3 p0, p1, f0, f1;
};

// The template's path map and the script calls that edit it, replayed in order when the map first needs it
// (setPathFlags runs in onLoaded, before any item exists).
class PathSetObj : public Object {
 public:
  using Object::Object;
  struct Op {
    enum Kind { Locators, Flags, Add } kind = Flags;
    std::string name;
    uint32_t flags = 0;
    PathRecord path;
  };
  std::vector<Op> ops;
  bool built = false;
  std::map<std::string, PathRecord> paths;  // ascending byte order, as the engine's std::map iterates
};

int fieldInt(const Object* o, const char16_t* name, int def, VM& vm) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toInt(it->second);
}

PathSetObj* pathSet(VM& vm, Object* tpl, bool create) {
  auto it = tpl->fieldValues.find(u"<paths>");
  if (it != tpl->fieldValues.end())
    if (auto* p = dynamic_cast<PathSetObj*>(it->second.object())) return p;
  if (!create) return nullptr;
  ClassInfo* c = vm.findClass("gsObjectVariant");
  if (!c) return nullptr;
  Ref<Object> o(new PathSetObj(c));
  tpl->fieldValues[u"<paths>"] = Value::heap(VT::Object, o.get());
  return static_cast<PathSetObj*>(o.get());
}

// the medium of a template's locator paths by item type (gsGridObject::loaded switch on +0x25c)
uint32_t classOfType(int type) {
  switch (type) {
    case 2: return 0x300;
    case 3: case 4: case 7: return 0x700;
    case 5: case 6: return 0x500;
    case 8: case 9: return 0x1000;
    case 10: case 11: case 12: return 0x800;
    case 13: case 14: case 15: case 16: return 0x2000;
    case 0x11: return 0x8000;
    case 0x12: return 0x4000;
    default: return 0;
  }
}

// a path direction from a stored rotation: q·(0,0,−1) in the engine's convention is the conjugate rotation in ours;
// locator rotations carry the quarter turn about −Y the loader multiplies in (checked on straight and curved tracks:
// the result runs along the chord, and along the diagonal at the 45° locator of a curve)
Vec3 locatorForward(const Quat& q) { return render::rotate(render::conjugate(q), Vec3{-1, 0, 0}); }
Vec3 pathForward(const Quat& q) { return render::rotate(render::conjugate(q), Vec3{0, 0, -1}); }

void applyFlags(PathSetObj* set, const std::string& name, uint32_t flags) {
  auto it = set->paths.find(name);
  if (it == set->paths.end()) {
    LOG_DEBUG("Path not found: %s", name.c_str());
    return;
  }
  // flags without a medium keep the medium (method_setPathFlags L:0x08180460)
  it->second.flags = (flags & kPathClassMask) == 0 ? (it->second.flags & kPathClassMask) | flags : flags;
}

// the locator pairs of the template's mesh: consecutive names "x-…n", "x-…m" that differ only in the last character
void addLocatorPaths(VM& vm, ObjectObj* tpl, PathSetObj* set) {
  std::vector<uint8_t> d;
  if (tpl->pendingPath.empty() || !vm.vfs().read(tpl->pendingPath, d) || d.size() < 8 + 60) return;
  auto u32 = [&](size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  };
  auto f32 = [&](size_t at) {
    float v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  };
  if (u32(0) != 0x02000000) return;
  struct Locator {
    Vec3 pos;
    Quat rot;
  };
  std::map<std::string, Locator> locators;  // gsObject+0x134, by name
  size_t end = std::min(d.size(), size_t(8) + u32(4));
  for (size_t at = 8 + 60; at + 8 <= end;) {
    uint32_t tag = u32(at), size = u32(at + 4);
    size_t b = at + 8;
    if (tag == 0x02600000 && b + 64 <= end) {
      std::string name;
      for (size_t i = 0; i < 31 && d[b + 4 + i]; ++i) name += char(d[b + 4 + i]);
      locators[name] = {{f32(b + 0x24), f32(b + 0x28), f32(b + 0x2c)},
                        {f32(b + 0x30), f32(b + 0x34), f32(b + 0x38), f32(b + 0x3c)}};
    }
    at = b + size;
  }
  // the load rotation of the diagonal "b" variants (+0x1b8): positions and directions turn with the mesh
  float cb = tpl->loadYaw != 0 ? std::cos(tpl->loadYaw) : 1, sb = tpl->loadYaw != 0 ? std::sin(tpl->loadYaw) : 0;
  auto bake = [&](const Vec3& v) { return Vec3{v.x * cb + v.z * sb, v.y, -v.x * sb + v.z * cb}; };
  uint32_t medium = classOfType(fieldInt(tpl, u"type", 0, vm));
  for (auto a = locators.begin(); a != locators.end(); ++a) {
    auto b = std::next(a);
    if (b == locators.end()) break;
    const std::string &s1 = a->first, &s2 = b->first;
    if (s1.size() <= 3 || s2.size() <= 3 || s1[1] != '-' || s2[1] != '-') continue;
    if (s1.compare(0, s1.size() - 1, s2, 0, std::min(s2.size(), s1.size() - 1)) != 0) continue;
    PathRecord r;
    r.flags = medium;
    r.p0 = bake(a->second.pos);
    r.p1 = bake(b->second.pos);
    r.f0 = bake(locatorForward(a->second.rot));
    r.f1 = bake(locatorForward(b->second.rot));
    set->paths[s1] = r;
  }
}

void buildPaths(VM& vm, ObjectObj* tpl, PathSetObj* set) {
  if (set->built) return;
  set->built = true;
  bool locators = false;
  for (const auto& op : set->ops) {
    if (op.kind == PathSetObj::Op::Locators) {
      addLocatorPaths(vm, tpl, set);
      locators = true;
    } else if (op.kind == PathSetObj::Op::Add) {
      set->paths[op.name] = op.path;
    } else {
      applyFlags(set, op.name, op.flags);
    }
  }
  if (!locators) addLocatorPaths(vm, tpl, set);
  set->ops.clear();
}

float bezierDeviation(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, float t) {
  // gsBezierCurve: the second derivative
  Vec3 v = a * (6 - 6 * t) + b * (18 * t - 12) + c * (6 - 18 * t) + d * (6 * t);
  return dot(v, v);
}

bool compatible(const LaneNode& a, const LaneNode& b) { return ((a.flags ^ b.flags) & 0x1000f900) == 0; }
bool isAir(const LaneNode& n) { return (n.flags & 0x1000f900) == kPathAir; }
float dist2(const Vec3& a, const Vec3& b) {
  Vec3 d = a - b;
  return dot(d, d);
}

void insertNode(LaneGraph& G, int ni, std::vector<int>& scratch) {
  LaneNode& n = G.nodes[size_t(ni)];
  float cs = G.cellSize;
  // the bucket key: the cell of the chord midpoint (bytes 0x100e4df5-0x100e4e4b)
  n.cellX = int((n.p0.x + n.p1.x) / cs + float(G.width) + 0.5f) >> 1;
  n.cellZ = int((n.p0.z + n.p1.z) / cs + float(G.length) + 0.5f) >> 1;
  bool air = isAir(n);
  int r = air ? 12 : 0, o = air ? -6 : 0;
  // predecessors: candidates around p0
  G.query(int(n.p0.x / cs) + G.width / 2 + o, int(n.p0.z / cs) + G.length / 2 + o, r, r, 0, scratch);
  for (int ci : scratch) {
    LaneNode& c = G.nodes[size_t(ci)];
    if (!compatible(c, n) || dot(n.f0, c.f1) <= 0.1f) continue;
    float t = (c.flags & kPathConnectFrom) || (n.flags & kPathConnectTo) ? (air ? 500.0f : 20.0f) : 0.01f;
    if (dist2(c.p1, n.p0) >= t) continue;
    if (std::find(c.next.begin(), c.next.end(), ni) != c.next.end()) continue;
    c.next.push_back(ni);
    ++G.nodes[size_t(ni)].inDegree;
  }
  // successors: candidates around p1
  LaneNode& m = G.nodes[size_t(ni)];
  G.query(int(m.p1.x / cs) + G.width / 2 + o, int(m.p1.z / cs) + G.length / 2 + o, r, r, 0, scratch);
  for (int ci : scratch) {
    LaneNode& c = G.nodes[size_t(ci)];
    if (!compatible(c, m) || dot(c.f0, m.f1) <= 0.1f) continue;
    float t = (m.flags & kPathConnectFrom) || (c.flags & kPathConnectTo) ? (air ? 500.0f : 20.0f) : 0.01f;
    if (dist2(m.p1, c.p0) >= t) continue;
    if (std::find(m.next.begin(), m.next.end(), ci) != m.next.end()) continue;
    m.next.push_back(ci);
    ++c.inDegree;
  }
  // pushed at the head of its bucket
  int bx = std::clamp(m.cellX >> 3, 0, G.bucketsX - 1), bz = std::clamp(m.cellZ >> 3, 0, G.bucketsZ - 1);
  int& head = G.buckets[size_t(bz * G.bucketsX + bx)];
  m.bucketNext = head;
  head = ni;
}

// FUN_10085240: squared distance between the segments a0-a1 and b0-b1
float segmentDistance2(const Vec3& a0, const Vec3& a1, const Vec3& b0, const Vec3& b1) {
  Vec3 d1 = a1 - a0, d2 = b1 - b0, r = a0 - b0;
  float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r), s = 0, t = 0;
  if (a <= 1e-9f && e <= 1e-9f) return dot(r, r);
  if (a <= 1e-9f) {
    t = std::clamp(f / e, 0.0f, 1.0f);
  } else {
    float c = dot(d1, r);
    if (e <= 1e-9f) {
      s = std::clamp(-c / a, 0.0f, 1.0f);
    } else {
      float b = dot(d1, d2), denom = a * e - b * b;
      s = denom != 0 ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
      t = (b * s + f) / e;
      if (t < 0) t = 0, s = std::clamp(-c / a, 0.0f, 1.0f);
      else if (t > 1) t = 1, s = std::clamp((b - c) / a, 0.0f, 1.0f);
    }
  }
  Vec3 p = a0 + d1 * s, q = b0 + d2 * t, d = p - q;
  return dot(d, d);
}

// §2.4: each path is two chords through its midpoint; paths closer than 0.5 units (2.24 with CONNECT flags) cross,
// unless one continues the other or either is a parking spot
size_t linkCrossings(LaneGraph& G, size_t first = 0) {
  std::vector<int> found;
  std::vector<Vec3> mid(G.nodes.size());
  for (size_t i = 0; i < G.nodes.size(); ++i) mid[i] = laneBezier(G.nodes[i], 0.5f);
  size_t pairs = 0;
  for (size_t ni = first; ni < G.nodes.size(); ++ni) {
    LaneNode& n = G.nodes[ni];
    if (n.flags & (kPathPark | kPathDeleted)) continue;
    G.query(n.cellX - 1, n.cellZ - 1, 2, 2, 0, found);
    for (int ci : found) {
      if (size_t(ci) >= ni) continue;  // each pair once, as the engine links a new node to the older ones
      LaneNode& c = G.nodes[size_t(ci)];
      if (c.flags & kPathPark) continue;
      if (std::find(n.next.begin(), n.next.end(), ci) != n.next.end() ||
          std::find(c.next.begin(), c.next.end(), int(ni)) != c.next.end())
        continue;
      const Vec3 &m1 = mid[ni], &m2 = mid[size_t(ci)];
      float d = std::min({segmentDistance2(n.p0, m1, c.p0, m2), segmentDistance2(n.p0, m1, m2, c.p1),
                          segmentDistance2(m1, n.p1, c.p0, m2), segmentDistance2(m1, n.p1, m2, c.p1)});
      bool connect = ((n.flags | c.flags) & (kPathConnectTo | kPathConnectFrom)) != 0;
      if (d > (connect ? 5.0f : 0.25f)) continue;
      n.crossings.push_back(ci);
      c.crossings.push_back(int(ni));
      ++pairs;
    }
  }
  return pairs;
}

}  // namespace

Vec3 laneBezier(const LaneNode& n, float t) {
  Vec3 b = n.p0 + n.f0 * (n.length / 3), c = n.p1 - n.f1 * (n.length / 3);
  float u = 1 - t;
  return n.p0 * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + n.p1 * (t * t * t);
}

void LaneGraph::query(int x, int z, int w, int l, uint32_t mask, std::vector<int>& out) const {
  out.clear();
  if (buckets.empty()) return;
  int x0 = x - 3, x1 = x + w + 3, z0 = z - 3, z1 = z + l + 3;
  int bx0 = std::clamp(x0 >> 3, 0, bucketsX - 1), bx1 = std::clamp(x1 >> 3, 0, bucketsX - 1);
  int bz0 = std::clamp(z0 >> 3, 0, bucketsZ - 1), bz1 = std::clamp(z1 >> 3, 0, bucketsZ - 1);
  for (int bz = bz0; bz <= bz1; ++bz)
    for (int bx = bx0; bx <= bx1; ++bx)
      for (int i = buckets[size_t(bz * bucketsX + bx)]; i >= 0; i = nodes[size_t(i)].bucketNext) {
        const LaneNode& n = nodes[size_t(i)];
        if (n.cellX >= x0 && n.cellX <= x1 && n.cellZ >= z0 && n.cellZ <= z1 && (n.flags & mask) == mask) out.push_back(i);
      }
}

int LaneGraph::nearest(const Vec3& p, uint32_t mask) const {
  int cx = int(p.x / cellSize + float(width) * 0.5f), cz = int(p.z / cellSize + float(length) * 0.5f);
  std::vector<int> found;
  for (int s = 0; s <= 128; s += 8) {
    query(cx - (s >> 1), cz - (s >> 1), s, s, mask, found);
    if (!found.empty()) break;
  }
  int best = -1;
  float bestD = 0;
  for (int i : found) {
    const LaneNode& n = nodes[size_t(i)];
    Vec3 m = (n.p0 + n.p1) - p * 2.0f;
    float d = m.x * m.x + 16 * m.y * m.y + m.z * m.z;
    if (best < 0 || d < bestD) best = i, bestD = d;
  }
  return best;
}

int LaneGraph::resolve(uint32_t ref) const {
  if ((ref >> 24) == 0x7f) {  // the i-th generated water lane from the head of the engine's list (prepended)
    uint32_t i = ref & 0xffffff;
    return i < uint32_t(latticeCount) ? latticeCount - 1 - int(i) : -1;
  }
  if (ref == 0 || (ref & 0x80000000u)) return -1;
  uint32_t item = ref >> 8, lane = ref & 0xff;
  if (item == 0 || item > itemNodes.size()) return -1;
  const auto& list = itemNodes[item - 1];
  return lane < list.size() ? list[lane] : -1;
}

bool LaneGraph::search(int start, int goal, bool ignoreTraffic, std::vector<int>& path, float& cost) const {
  path.clear();
  cost = 0;
  if (start < 0 || goal < 0) return false;
  if (start == goal) return true;
  size_t n = nodes.size();
  // scratch reused between searches: a node counts as discovered when its stamp is the current search's
  static thread_local std::vector<uint32_t> stamp;
  static thread_local std::vector<int> parent;
  static thread_local uint32_t serial = 0;
  if (stamp.size() != n) stamp.assign(n, 0), parent.assign(n, -1), serial = 0;
  if (++serial == 0) std::fill(stamp.begin(), stamp.end(), 0u), serial = 1;
  struct Discovered {
    std::vector<uint32_t>& s;
    uint32_t v;
    bool operator[](size_t i) const { return s[i] == v; }
    void set(size_t i) const { s[i] = v; }
  } discovered{stamp, serial};
  using Entry = std::pair<float, int>;  // g, node
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
  discovered.set(size_t(start));
  open.push({0.0f, start});
  const LaneNode& gl = nodes[size_t(goal)];
  auto viable = [&](int s) { return s == goal || !nodes[size_t(s)].next.empty(); };
  bool found = false;
  while (!open.empty()) {
    auto [g, pi] = open.top();
    open.pop();
    if (pi == goal) {
      found = true;
      break;
    }
    const LaneNode& p = nodes[size_t(pi)];
    for (int si : p.next) {
      const LaneNode& s = nodes[size_t(si)];
      if (discovered[size_t(si)] || (s.flags & 0x4000000) || !viable(si)) continue;
      // §4.2: travel time, cheaper towards the goal and away from the map centre
      float dGoal = std::max(std::fabs(s.p0.x - gl.p0.x), std::fabs(s.p0.z - gl.p0.z));
      float dCentre = std::max(std::fabs(s.p0.x), std::fabs(s.p0.z));
      float c = s.length / s.speedLimit * (2.0f + 0.002f * dGoal - 0.001f * dCentre);
      if ((s.flags & kPathClassMask) == 0x700) c *= 0.4f;
      if ((s.flags & kPathIntersection) && !(p.flags & kPathIntersection)) c += 4.0f;
      if (s.flags & kPathLaneChange) c += 6.0f;
      (void)ignoreTraffic;  // no reservations yet: the traffic terms are 0
      discovered.set(size_t(si));
      parent[size_t(si)] = pi;
      // §4.3 lookahead: a dead-end chain whose only way on is already taken is discovered but not expanded
      if (si != goal) {
        int live = 0, last = -1;
        for (int k : s.next)
          if (viable(k)) ++live, last = k;
        if ((s.next.size() == 1 || (s.next.size() == 2 && live == 1)) && last >= 0 &&
            (discovered[size_t(last)] || (nodes[size_t(last)].flags & 0x4000000)) && last != goal)
          continue;
      }
      open.push({g + c, si});
    }
  }
  if (!found) return false;
  for (int k = goal; k != start; k = parent[size_t(k)]) {
    path.push_back(k);
    cost += nodes[size_t(k)].length / nodes[size_t(k)].speedLimit;
  }
  std::reverse(path.begin(), path.end());
  return true;
}

int itemHeight(const GridData& g, const GridData::Item& it, uint32_t templateFlags, int templateType) {
  int y = it.y + (0x200 - int(g.heightBase));
  if (((it.flags | templateFlags) & 0x8000000) || templateType == 0x10) y = (y + 0x7f) & ~0x7f;  // ALIGN4, station
  return y;
}

namespace {

// gsGrid::updateSpecialSegments (FUN_100c1840, boats-aircraft.md §2): one-way water lanes over every 4×4-cell block
// that is all water with water strips on its sides, at the water surface −1.25 cells; created block by block (z
// outer) in the table order of §2.4 and linked as they are made, before any item
}  // namespace

void waterLattice(const GridData& g, const std::vector<uint16_t>& flags, std::vector<LaneNode>& out) {
  out.clear();
  const float cs = g.cellSize, y = -1.25f * cs;
  const int W = g.width, L = g.length, BX = W >> 2, BZ = L >> 2;
  auto water = [&](int x, int z) { return (flags[size_t(z) * size_t(L) + size_t(x)] & 0x2000) != 0; };  // stride L (Q1)
  auto strip = [&](int x, int z, int dx, int dz) {
    for (int k = 0; k < 4; ++k)
      if (!water(x + dx * k, z + dz * k)) return false;
    return true;
  };
  auto emit = [&](LaneNode n) {
    n.length = std::sqrt(dist2(n.p0, n.p1));
    n.speedLimit = 6;
    n.curveFactor = 20;
    out.push_back(n);
  };
  for (int bz = 0; bz < BZ; ++bz)
    for (int bx = 0; bx < BX; ++bx) {
      int x0 = 4 * bx, z0 = 4 * bz;
      bool open = true;
      for (int k = 0; k < 4 && open; ++k) open = strip(x0, z0 + k, 1, 0);
      if (open && bz > 0) open = strip(x0, z0 - 1, 1, 0);
      if (open && bx > 0) open = strip(x0 - 1, z0, 0, 1);
      if (open && bz < BZ - 1) open = strip(x0, z0 + 4, 1, 0);
      if (open && bx < BX - 1) open = strip(x0 + 4, z0, 0, 1);
      if (!open) continue;
      auto at = [&](int lx, int lz) { return Vec3{(float(x0 + lx) - float(W) * 0.5f) * cs, y, (float(z0 + lz) - float(L) * 0.5f) * cs}; };
      auto lane = [&](Vec3 p0, Vec3 p1, uint32_t flags) {
        LaneNode n;
        n.flags = flags;
        n.p0 = p0, n.p1 = p1;
        n.f0 = n.f1 = render::normalize(p1 - p0);
        return n;
      };
      auto combine = [&](const LaneNode& a, const LaneNode& b, uint32_t flags) {  // FUN_100e1730: from a's start to b's end
        LaneNode n;
        n.flags = flags;
        n.p0 = a.p0, n.f0 = a.f0, n.p1 = b.p1, n.f1 = b.f1;
        return n;
      };
      int p = bx & 2, q = bz & 2, r = (bx + bz) & 2, r2 = (bx - bz) & 2;
      LaneNode A = lane(at(2, 2 * p), at(2, 4 - 2 * p), kPathWater);
      if (bz == 0) A.flags |= p == 0 ? kPathStart : kPathEnd;
      else if (bz == BZ - 1) A.flags |= p == 0 ? kPathEnd : kPathStart;
      LaneNode B = lane(at(2 * q, 2), at(4 - 2 * q, 2), kPathWater);
      if (bx == 0) B.flags |= q == 0 ? kPathStart : kPathEnd;
      else if (bx == BX - 1) B.flags |= q == 0 ? kPathEnd : kPathStart;
      emit(A);
      emit(B);
      emit(combine(A, B, 0x8030));
      emit(combine(B, A, 0x8030));
      if (((bx ^ bz) & 1) != 0) continue;
      LaneNode E = lane(at(4 - 2 * r, 2 * r), at(2 * r, 4 - 2 * r), kPathWater);
      emit(E);
      if (p == r) emit(combine(A, E, 0x8010)), emit(combine(E, A, 0x8010));
      if (q != r) emit(combine(B, E, 0x8010)), emit(combine(E, B, 0x8010));
      LaneNode F = lane(at(4 - 2 * r2, 4 - 2 * r2), at(2 * r2, 2 * r2), kPathWater);
      emit(F);
      if (p != r2) emit(combine(A, F, 0x8010)), emit(combine(F, A, 0x8010));
      if (q != r2) emit(combine(B, F, 0x8010)), emit(combine(F, B, 0x8010));
    }
}

namespace {

void addWaterLattice(LaneGraph& G, const GridData& g, const std::vector<uint16_t>& flags) {
  std::vector<LaneNode> lattice;
  waterLattice(g, flags, lattice);
  std::vector<int> scratch;
  for (LaneNode& n : lattice) {
    int ni = int(G.nodes.size());
    G.nodes.push_back(std::move(n));
    insertNode(G, ni, scratch);
  }
  G.latticeCount = int(G.nodes.size());
}

// gsGridItem::fixGrid (0x100e9730.c:441-453, boats-aircraft.md §2.6): an item reaching the water surface hides the
// water lanes in its footprint from the searches (0x4000000, never cleared)
void hideWaterUnder(LaneGraph& G, const GridData& g, const GridData::Item& it, int sx, int sz, int y, int height) {
  if (!(y < 0 && y + height * 256 > -0x200)) return;
  std::vector<int> found;
  G.query(it.x, it.z, sx, sz, kPathWater, found);
  for (int n : found) {
    LaneNode& nd = G.nodes[size_t(n)];
    if (nd.cellX >= it.x && nd.cellX <= it.x + sx && nd.cellZ >= it.z && nd.cellZ <= it.z + sz) nd.flags |= 0x4000000;
  }
  (void)g;
}

struct Tpl {
  ObjectObj* object = nullptr;
  PathSetObj* paths = nullptr;
  int width = 1, length = 1, type = 0, height = 1;
  uint32_t flags = 0;
};

Tpl templateOf(VM& vm, GridObj* grid, const GridData& g, size_t type) {
  Tpl tp;
  if (type >= g.types.size()) return tp;
  auto it = grid->templates.find(g.types[type]);
  if (it == grid->templates.end()) return tp;
  auto* o = dynamic_cast<ObjectObj*>(it->second.get());
  if (!o) return tp;
  tp.object = o;
  tp.width = std::max(1, fieldInt(o, u"width", 1, vm));
  tp.length = std::max(1, fieldInt(o, u"length", 1, vm));
  tp.type = fieldInt(o, u"type", 0, vm);
  tp.flags = uint32_t(fieldInt(o, u"flags", 0, vm));
  tp.height = fieldInt(o, u"height", 1, vm);
  tp.paths = pathSet(vm, o, true);
  if (tp.paths) buildPaths(vm, o, tp.paths);
  return tp;
}

}  // namespace

void finishLaneNode(LaneNode& n) {
  n.length = std::sqrt(dist2(n.p0, n.p1));
  // §2.1 steps 4-6: the curve's bend sets curveFactor and with it the speed limit
  Vec3 b = n.p0 + n.f0 * (n.length / 3), cc = n.p1 - n.f1 * (n.length / 3);
  float k = std::sqrt(std::max({bezierDeviation(n.p0, b, cc, n.p1, 0.1f), bezierDeviation(n.p0, b, cc, n.p1, 0.5f),
                                bezierDeviation(n.p0, b, cc, n.p1, 0.9f)}));
  n.curveFactor = k >= 8 ? 2.5f : k > 1 ? 20.0f / k : 20.0f;
  if (n.flags & kPathClassMask) n.curveFactor *= 1.5f;
  float limit = std::max(2 * n.curveFactor, 1.5f);
  uint32_t medium = n.flags & kPathClassMask;
  float cap = medium == 0x1000 || medium == 0x2000 || medium == 0x700 ? 10.0f
              : medium == 0x500                                       ? 8.0f
              : medium == kPathAir                                    ? ((n.flags & kPathIgnoreTraffic) ? 6.0f : 3.0f)
                                                                      : 6.0f;
  n.speedLimit = std::min(limit, cap);
}

namespace {

// gsGridObject::addPaths: one node per path of a placed item, inserted and linked (§2.1-2.3)
void addPaths(const GridData& g, LaneGraph& G, size_t i, const Tpl& tp, std::vector<int>& scratch) {
  const GridData::Item& it = g.items[i];
  if (it.removed || !tp.object) return;
  int sx = (it.orient & 1) ? tp.length : tp.width, sz = (it.orient & 1) ? tp.width : tp.length;
  if (!(it.flags & 0x500000)) hideWaterUnder(G, g, it, sx, sz, itemHeight(g, it, tp.flags, tp.type), tp.height);
  if (!tp.paths || tp.paths->paths.empty()) return;
  // FUN_100ecbf0: no paths for temporary or hidden items and markers
  if ((it.flags & 0x500000) || tp.type == 0x15) return;
  const float cs = g.cellSize;
  // the item placement of the renderer (grid-items.md §2.2): turn by (orient + 2)·90°, then to the item centre
  Vec3 T{(2.0f * float(it.x) + float(sx - g.width)) * cs * 0.5f, float(itemHeight(g, it, tp.flags, tp.type)) * cs / 256.0f,
         (2.0f * float(it.z) + float(sz - g.length)) * cs * 0.5f};
  T.x += it.offX, T.z += it.offZ;  // port: snapped off the grid
  float c, s;
  itemTurn(it, c, s);  // with the port's free turn
  auto place = [&](const Vec3& v) { return Vec3{v.x * c + v.z * s, v.y, -v.x * s + v.z * c}; };
  auto& list = G.itemNodes[i];
  for (const auto& [name, path] : tp.paths->paths) {
    LaneNode n;
    n.flags = path.flags | ((tp.flags & 0x8000) ? 0x800000u : 0u);
    n.item = int(i);
    n.p0 = place(path.p0) + T;
    n.p1 = place(path.p1) + T;
    n.f0 = render::normalize(place(path.f0));
    n.f1 = render::normalize(place(path.f1));
    finishLaneNode(n);
    int ni = int(G.nodes.size());
    G.nodes.push_back(n);
    insertNode(G, ni, scratch);
    list.push_back(ni);
  }
  std::reverse(list.begin(), list.end());  // prepended: descending path names
}

}  // namespace

// REGS_DUMP_TEMPLATES=a,b,...: size, mesh bounds per material and paths of the named templates (curved-road research)
void dumpTemplates(VM& vm, GridObj* grid) {
  const char* env = std::getenv("REGS_DUMP_TEMPLATES");
  if (!env) return;
  std::string list = env;
  size_t at = 0;
  while (at <= list.size()) {
    size_t e = list.find(',', at);
    std::string name = list.substr(at, e == std::string::npos ? std::string::npos : e - at);
    at = e == std::string::npos ? list.size() + 1 : e + 1;
    auto it = grid->templates.find(name);
    auto* o = it == grid->templates.end() ? nullptr : dynamic_cast<ObjectObj*>(it->second.get());
    if (!o) {
      LOG_INFO("dump %s: no template", name.c_str());
      continue;
    }
    LOG_INFO("dump %s: %s width %d length %d type %d conn %x offset %d,%d yaw %.2f", name.c_str(), o->pendingPath.c_str(),
             fieldInt(o, u"width", 1, vm), fieldInt(o, u"length", 1, vm), fieldInt(o, u"type", 0, vm),
             fieldInt(o, u"connections", 0, vm), fieldInt(o, u"offsetx", 0, vm), fieldInt(o, u"offsetz", 0, vm), o->loadYaw);
    auto data = o->pendingPath.empty() ? o->data : loadObjectCached(vm, o->pendingPath);
    if (data) {
      const render::MeshData& m = data->mesh;
      for (const render::Submesh& sm : m.submeshes) {
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}, ulo[2] = {1e9f, 1e9f}, uhi[2] = {-1e9f, -1e9f};
        for (uint32_t k = 0; k < sm.indexCount; ++k) {
          uint32_t v = m.indices[sm.firstIndex + k];
          for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], m.positions[v * 3 + a]), hi[a] = std::max(hi[a], m.positions[v * 3 + a]);
          if (m.uv0.size() > v * 2 + 1)
            for (int a = 0; a < 2; ++a) ulo[a] = std::min(ulo[a], m.uv0[v * 2 + a]), uhi[a] = std::max(uhi[a], m.uv0[v * 2 + a]);
        }
        if (std::getenv("REGS_DUMP_VERTICES"))
          for (uint32_t k = sm.firstVertex; k < sm.firstVertex + sm.vertexCount; ++k)
            LOG_INFO("dump %s:     v %.2f %.2f %.2f uv %.4f %.4f", name.c_str(), m.positions[k * 3], m.positions[k * 3 + 1],
                     m.positions[k * 3 + 2], m.uv0.size() > k * 2 + 1 ? m.uv0[k * 2] : 0.0f, m.uv0.size() > k * 2 + 1 ? m.uv0[k * 2 + 1] : 0.0f);
        LOG_INFO("dump %s:   uv %.4f..%.4f, %.4f..%.4f", name.c_str(), ulo[0], uhi[0], ulo[1], uhi[1]);
        std::string mat = size_t(sm.material) < data->materials.size() ? data->materials[size_t(sm.material)].name : "?";
        LOG_INFO("dump %s:   material %d %s: x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f (%u tris)", name.c_str(), sm.material,
                 mat.c_str(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], sm.indexCount / 3);
      }
    }
    PathSetObj* ps = pathSet(vm, o, true);
    if (ps) buildPaths(vm, o, ps);
    if (ps)
      for (const auto& [pn, p] : ps->paths)
        LOG_INFO("dump %s:   path %s flags %x  %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f  f0 %.2f,%.2f f1 %.2f,%.2f", name.c_str(), pn.c_str(),
                 p.flags, p.p0.x, p.p0.y, p.p0.z, p.p1.x, p.p1.y, p.p1.z, p.f0.x, p.f0.z, p.f1.x, p.f1.z);
  }
}

void buildLaneGraph(VM& vm, GridObj* grid, const GridData& g, LaneGraph& G, const std::vector<uint16_t>* waterFlags) {
  dumpTemplates(vm, grid);
  G = LaneGraph();
  G.width = g.width;
  G.length = g.length;
  G.cellSize = g.cellSize;
  G.bucketsX = (g.width + 7) >> 3;
  G.bucketsZ = (g.length + 7) >> 3;
  G.buckets.assign(size_t(G.bucketsX) * size_t(G.bucketsZ), -1);
  G.itemNodes.assign(g.items.size(), {});
  addWaterLattice(G, g, waterFlags ? *waterFlags : g.flags);
  std::vector<Tpl> types(g.types.size());
  for (size_t t = 0; t < g.types.size(); ++t) types[t] = templateOf(vm, grid, g, t);
  std::vector<int> scratch;
  for (size_t i = 0; i < g.items.size(); ++i)
    if (g.items[i].type < types.size()) addPaths(g, G, i, types[g.items[i].type], scratch);
  size_t links = 0;
  for (const auto& n : G.nodes) links += n.next.size();
  LOG_INFO("lane graph: %zu nodes (%d generated water lanes), %zu links", G.nodes.size(), G.latticeCount, links);
  LOG_INFO("lane graph: %zu crossing pairs", linkCrossings(G));
}

void addItemNodes(VM& vm, GridObj* grid, const GridData& g, LaneGraph& G, const std::vector<uint32_t>& items) {
  if (G.itemNodes.size() < g.items.size()) G.itemNodes.resize(g.items.size());
  size_t first = G.nodes.size();
  std::vector<int> scratch;
  for (uint32_t i : items)
    if (i < g.items.size() && G.itemNodes[i].empty()) addPaths(g, G, i, templateOf(vm, grid, g, g.items[i].type), scratch);
  linkCrossings(G, first);
}

void removeItemNodes(LaneGraph& G, uint32_t item) {
  if (item >= G.itemNodes.size()) return;
  removeNodeList(G, G.itemNodes[item]);
}

void removeNodeList(LaneGraph& G, std::vector<int>& list) {
  std::vector<int> found;
  float cs = G.cellSize;
  for (int ni : list) {
    LaneNode& n = G.nodes[size_t(ni)];
    if (n.flags & kPathDeleted) continue;
    // predecessors: the candidates insertNode linked around p0
    bool air = isAir(n);
    int r = air ? 12 : 0, o = air ? -6 : 0;
    G.query(int(n.p0.x / cs) + G.width / 2 + o, int(n.p0.z / cs) + G.length / 2 + o, r, r, 0, found);
    for (int ci : found) {
      auto& next = G.nodes[size_t(ci)].next;
      next.erase(std::remove(next.begin(), next.end(), ni), next.end());
    }
    for (int si : n.next) --G.nodes[size_t(si)].inDegree;
    n.next.clear();
    for (int ci : n.crossings) {
      auto& cr = G.nodes[size_t(ci)].crossings;
      cr.erase(std::remove(cr.begin(), cr.end(), ni), cr.end());
    }
    n.crossings.clear();
    int bx = std::clamp(n.cellX >> 3, 0, G.bucketsX - 1), bz = std::clamp(n.cellZ >> 3, 0, G.bucketsZ - 1);
    for (int* link = &G.buckets[size_t(bz * G.bucketsX + bx)]; *link >= 0; link = &G.nodes[size_t(*link)].bucketNext)
      if (*link == ni) {
        *link = n.bucketNext;
        break;
      }
    n.bucketNext = -1;
    n.inDegree = 0;
    n.flags |= kPathDeleted;
  }
  list.clear();
}

// port: the curved roads' lanes (engine/curve_roads.h), all rebuilt together after the items' (at load and after a
// curve edit): per curve a lane each way 1.5 units right of the centre line, at the asphalt (item height − 0.2) as
// the straight small road's paths lie, cut into pieces of at most 5 units (a node's curve is fixed by its ends and
// directions, so short pieces follow the Bézier). The ends meet the straight blocks' lanes exactly and link as theirs.
void rebuildCurveNodes(VM& vm, GridObj* grid, const GridData& g, LaneGraph& G) {
  removeNodeList(G, G.curveNodes);
  size_t first = G.nodes.size();
  std::vector<int> scratch;
  auto add = [&](LaneNode n) {
    finishLaneNode(n);
    int ni = int(G.nodes.size());
    G.nodes.push_back(n);
    insertNode(G, ni, scratch);
    G.curveNodes.push_back(ni);
  };
  // the other kinds than the small road: their straight piece's paths on every copy along the curve (local x along
  // it, z to its right), the street tram piece's with tracks, the dead-end piece's at a free end (its U-turn); the
  // building access paths (START / END) and the stop marks are left out
  auto templateObject = [&](const char* name) -> ObjectObj* {
    if (!grid || !name[0]) return nullptr;
    auto it = grid->templates.find(name);
    return it == grid->templates.end() ? nullptr : dynamic_cast<ObjectObj*>(it->second.get());
  };
  auto pathsOf = [&](ObjectObj* o) -> PathSetObj* {
    PathSetObj* ps = o ? pathSet(vm, o, true) : nullptr;
    if (ps) buildPaths(vm, o, ps);
    return ps;
  };
  auto keep = [](const PathRecord& p) { return !(p.flags & (kPathStart | kPathEnd)); };
  auto bentNodes = [&](const CurveRoad& c, float t0, float t1) {
    const CurveProfile& pf = curveProfile(c.kind);
    const bool track = curveIsTrack(c.kind);  // tracks keep their dead end's START / END paths (trains turn there)
    const float kept = curveLength(c) - t0 - t1;
    CurvePieces cp = curvePieces(c, kept);
    std::vector<CurveSample> shape = curveShape(c);
    auto lay = [&](PathSetObj* ps) {
      if (!ps) return;
      for (int i = 0; i < cp.count && kept > 0.05f; ++i) {
        float s0 = t0 + (float(i) + 0.5f) * pf.piece * cp.stretch;
        for (const auto& [name, p] : ps->paths) {
          if (!track && !keep(p)) continue;
          CurveSample a = curveAt(shape, s0 + p.p0.x * cp.stretch), b = curveAt(shape, s0 + p.p1.x * cp.stretch);
          LaneNode n;
          n.flags = p.flags & ~kPathStop;
          n.item = -1;
          n.p0 = a.p + a.right * p.p0.z + Vec3{0, p.p0.y, 0};
          n.p1 = b.p + b.right * p.p1.z + Vec3{0, p.p1.y, 0};
          n.f0 = render::normalize(a.dir * p.f0.x + a.right * p.f0.z);
          n.f1 = render::normalize(b.dir * p.f1.x + b.right * p.f1.z);
          add(n);
        }
      }
    };
    lay(pathsOf(templateObject(pf.straight)));
    if (c.ends & 4) lay(pathsOf(templateObject(pf.tram)));
    ObjectObj* dead = templateObject(pf.deadend);
    PathSetObj* dp = pathsOf(dead);
    if (!dp) return;
    bool openPlusX = ((uint32_t(fieldInt(dead, u"connections", 0x40 << pf.shift, vm)) >> pf.shift) & 0x40) != 0;
    CurveEnds free = curveFreeEnds(g, c);
    for (int end = 0; end < 2; ++end) {
      if (!(end ? free.free1 : free.free0)) continue;
      Vec3 o, ax, az, up{0, 1, 0};
      curveCapFrame(c, end, openPlusX, o, ax, az);
      for (const auto& [name, p] : dp->paths) {
        if (!track && !keep(p)) continue;
        LaneNode n;
        n.flags = p.flags & ~kPathStop;
        n.item = -1;
        n.p0 = o + ax * p.p0.x + up * p.p0.y + az * p.p0.z;
        n.p1 = o + ax * p.p1.x + up * p.p1.y + az * p.p1.z;
        n.f0 = render::normalize(ax * p.f0.x + az * p.f0.z);
        n.f1 = render::normalize(ax * p.f1.x + az * p.f1.z);
        add(n);
      }
    }
  };
  std::vector<CurveJunction> junctions = findCurveJunctions(g);
  for (size_t ci = 0; ci < g.curves.size(); ++ci) {
    const CurveRoad& c = g.curves[ci];
    float trim0, trim1;
    curveTrims(junctions, int(ci), trim0, trim1);
    if (c.kind != 0) {
      bentNodes(c, trim0, trim1);
      continue;
    }
    std::vector<CurveSample> s = sampleCurveRange(c, trim0, curveLength(c) - trim1, 5.0f);
    if (s.size() < 2) continue;
    // a free end's U-turn (also an end on a grass tram track, whose turning circle lies over the track): from the lane
    // arriving there (right of the way out) around the point 3 units further on
    // (radius 1.5) into the lane leaving
    CurveEnds free = curveFreeEnds(g, c);
    for (int end = 0; end < 2; ++end) {
      if (!(end ? free.free1 : free.free0)) continue;
      Vec3 e = end ? Vec3{c.x1, c.y1 - 0.2f, c.z1} : Vec3{c.x0, c.y0 - 0.2f, c.z0};
      Vec3 u = end ? Vec3{c.dx1, 0, c.dz1} : Vec3{-c.dx0, 0, -c.dz0};
      Vec3 r = render::normalize(Vec3{-u.z, 0, u.x});
      Vec3 q = e + u * kCurveCapReach;
      Vec3 pts[5] = {e + r * 1.5f, q + r * 1.5f, q + u * 1.5f, q - r * 1.5f, e - r * 1.5f};
      Vec3 dirs[5] = {u, u, r * -1.0f, u * -1.0f, u * -1.0f};
      for (int k = 0; k < 4; ++k) {
        LaneNode n;
        n.flags = 0x300;
        n.item = -1;
        n.p0 = pts[k], n.p1 = pts[k + 1], n.f0 = dirs[k], n.f1 = dirs[k + 1];
        add(n);
      }
    }
    // tram tracks (ends bit 2): tram lanes on the road's lane lines, at the height of the street tram lanes they meet
    // at each end (a tram item's own height; the item height when no tram lane is there)
    const float L = s.back().s;
    auto tramHeight = [&](const Vec3& at, float fallback) {
      std::vector<int> found;
      G.query(int(at.x / G.cellSize) + G.width / 2 - 1, int(at.z / G.cellSize) + G.length / 2 - 1, 2, 2, 0, found);
      for (int ni : found) {
        const LaneNode& m = G.nodes[size_t(ni)];
        if ((m.flags & 0x1000f900) != 0x800 || (m.flags & kPathDeleted)) continue;
        for (const Vec3& p : {m.p0, m.p1})
          if ((p.x - at.x) * (p.x - at.x) + (p.z - at.z) * (p.z - at.z) < 0.09f) return p.y;
      }
      return fallback;
    };
    float tramDy0 = 0, tramDy1 = 0;
    if ((c.ends & 4) && std::getenv("REGS_DEBUG_CURVES"))
      for (const CurveSample* e : {&s.front(), &s.back()})
        for (float sd : {1.5f, -1.5f}) {
          Vec3 at = e->p + e->right * sd;
          float best = 1e9f;
          Vec3 bp;
          for (const LaneNode& m : G.nodes) {
            if ((m.flags & 0x1000f900) != 0x800 || (m.flags & kPathDeleted)) continue;
            for (const Vec3& p : {m.p0, m.p1}) {
              float d = (p.x - at.x) * (p.x - at.x) + (p.z - at.z) * (p.z - at.z);
              if (d < best) best = d, bp = p;
            }
          }
          LOG_INFO("curved roads: tram end %.1f,%.1f,%.1f: nearest street tram lane end %.2f away at %.1f,%.1f,%.1f", at.x, at.y,
                   at.z, std::sqrt(best), bp.x, bp.y, bp.z);
        }
    if (c.ends & 4) {
      tramDy0 = tramHeight(s.front().p + s.front().right * 1.5f, s.front().p.y) - s.front().p.y;
      tramDy1 = tramHeight(s.back().p + s.back().right * 1.5f, s.back().p.y) - s.back().p.y;
    }
    // one-way (ends bit 5, roundabouts): both lanes from the start to the end, or (bit 6) a single lane on the centre line
    const bool oneWay = (c.ends & 0x20) != 0, single = oneWay && (c.ends & 0x40);
    // a one-way road's end on a two-way road (a road block, a two-way curve, a free end; not a junction or another
    // one-way curve): its lanes meet there in the right-hand lane (merging out, splitting in) on the piece next to it
    bool meet0, meet1;
    curveOneWayMeets(g, c, trim0, trim1, meet0, meet1);
    for (int medium = 0; medium < ((c.ends & 4) ? 2 : 1); ++medium) {  // 0 road, 1 tram
      // one-way trams: a single track with the traffic, in the outer lane (two lanes) or on the centre line (one lane)
      for (int side = 0; side < ((single && !medium) || (oneWay && medium) ? 1 : 2); ++side) {  // 0: start → end on the right, 1: end → start
        const bool back = side && !(oneWay && !medium);  // a one-way road's second lane runs forward on the left
        const float across = single ? 0.0f : side ? -1.5f : 1.5f;
        for (size_t k = 0; k + 1 < s.size(); ++k) {
          const CurveSample &a = back ? s[s.size() - 1 - k] : s[k], &b = back ? s[s.size() - 2 - k] : s[k + 1];
          float sign = back ? -1.0f : 1.0f;
          auto dy = [&](const CurveSample& q) {
            return medium ? tramDy0 + (tramDy1 - tramDy0) * (L > s.front().s ? (q.s - s.front().s) / (L - s.front().s) : 0) : -0.2f;
          };
          LaneNode n;
          n.flags = medium ? 0x800 : 0x300;  // street tram / small road lanes (classOfType of types 10 and 2)
          n.item = -1;
          const bool firstPiece = k == 0, lastPiece = k + 2 == s.size();
          const float across0 = !back && meet0 && firstPiece ? 1.5f : across, across1 = !back && meet1 && lastPiece ? 1.5f : across;
          n.p0 = a.p + a.right * across0 + Vec3{0, dy(a), 0};
          n.p1 = b.p + b.right * across1 + Vec3{0, dy(b), 0};
          n.f0 = a.dir * sign;
          n.f1 = b.dir * sign;
          // a stop of this medium on this lane's side within the piece: its platform lane (STOP)
          float lo = std::min(a.s, b.s), hi = std::max(a.s, b.s);
          for (const CurveRoad::Stop& st : c.stops)
            if (int(st.kind) == medium && (st.side > 0) == (side == 0) && st.s >= lo && st.s < hi) n.flags |= kPathStop;
          add(n);
        }
      }
    }
  }
  const size_t junctionFirst = G.nodes.size();
  // junctions: from every arm's incoming lanes into the same lanes of every other arm (Bézier turns; the crossing
  // links below keep crossing turns apart)
  for (const CurveJunction& j : junctions) {
    for (size_t a = 0; a < j.arms.size(); ++a)
      for (size_t b = 0; b < j.arms.size(); ++b) {
        if (a == b) continue;
        const CurveArm &A = j.arms[a], &B = j.arms[b];
        Vec3 ra = render::normalize(Vec3{-A.out.z, 0, A.out.x}), rb = render::normalize(Vec3{-B.out.z, 0, B.out.x});
        // an arm's lane ends at its mouth, the rightmost (in the direction of travel) first: two-way arms both ways,
        // a one-way arm (roundabout ring) only its own way, with both lanes (or the single one on the centre line)
        // (each arm with its own road kind's lanes: a large road meeting a roundabout pairs its two lanes with the ring's)
        auto laneEnds = [&](const CurveArm& X, const Vec3& rx, bool incoming) {
          const CurveLanes& ln = curveLanes(X.kind);
          std::vector<Vec3> pts;
          if (X.flow == 0) {
            for (int k = ln.count - 1; k >= 0; --k) pts.push_back(X.mouth + rx * (incoming ? -ln.offset[k] : ln.offset[k]));
          } else if ((X.flow == 1) == incoming) {
            if (X.single) pts.push_back(X.mouth);
            else if (incoming) pts = {X.mouth - rx * 1.5f, X.mouth + rx * 1.5f};
            else pts = {X.mouth + rx * 1.5f, X.mouth - rx * 1.5f};
          }
          return pts;
        };
        std::vector<Vec3> ins = laneEnds(A, ra, true), outs = laneEnds(B, rb, false);
        for (size_t k = 0; k < ins.size() && !outs.empty(); ++k) {  // lane to lane, extra ones into the leftmost
          LaneNode n;
          n.flags = curveLanes(A.kind).medium | kPathIntersection;
          n.item = -1;
          n.p0 = ins[k] + Vec3{0, -0.2f, 0};
          n.p1 = outs[std::min(k, outs.size() - 1)] + Vec3{0, -0.2f, 0};
          n.f0 = A.out * -1.0f;
          n.f1 = B.out;
          add(n);
        }
        Vec3 tin, tout;  // trams between two tracked arms (at the item height, as on the curves), one-way arms their way
        if (A.tram && B.tram && junctionTramEnd(j, A, true, tin) && junctionTramEnd(j, B, false, tout)) {
          LaneNode n;
          n.flags = 0x800u | kPathIntersection;
          n.item = -1;
          n.p0 = tin;
          n.p1 = tout;
          n.f0 = A.out * -1.0f;
          n.f1 = B.out;
          add(n);
        }
      }
  }
  linkCrossings(G, first);
  if (junctionFirst < G.nodes.size()) {  // every turn should continue both ways
    size_t joined = 0, total = G.nodes.size() - junctionFirst;
    for (size_t i = junctionFirst; i < G.nodes.size(); ++i)
      if (G.nodes[i].inDegree > 0 && !G.nodes[i].next.empty()) ++joined;
    LOG_INFO("curved roads: %zu junctions, %zu turn lanes, %zu joined both ways", junctions.size(), total, joined);
  }
  if (!g.curves.empty()) {  // how they joined the items' lanes (both ways at each road block they meet)
    std::vector<uint8_t> isCurve(G.nodes.size(), 0);
    for (int n : G.curveNodes) isCurve[size_t(n)] = 1;
    int in = 0, out = 0, trackIn = 0, trackOut = 0;
    for (size_t i = 0; i < G.nodes.size(); ++i)
      for (int s : G.nodes[i].next)
        if (isCurve[i] != isCurve[size_t(s)]) {
          ++(isCurve[i] ? out : in);
          if (G.nodes[i].flags & (kPathMetro | kPathTrain)) ++(isCurve[i] ? trackOut : trackIn);
        }
    LOG_INFO("curved roads: %zu curves, %zu lane nodes, %d links from the road network into them, %d out (tracks: %d in, "
             "%d out)", g.curves.size(), G.curveNodes.size(), in, out, trackIn, trackOut);
  }
}

void registerPathNatives(VM& vm) {
  // setPathFlags(name, flags) (L:0x08180460) and addPath(name, flags, p0, q0, p1, q1) (L:0x08180e50) edit the
  // template's path map; recorded and replayed when the map needs the paths
  implement(vm, "gsGridObject", u"setPathFlags", [](VM& v, Object* s, int, const Value* a) {
    PathSetObj* set = pathSet(v, s, true);
    if (!set) return Value();
    std::string name = utf16ToUtf8(v.toStr(a[0]));
    uint32_t flags = uint32_t(v.toInt(a[1]));
    if (set->built) applyFlags(set, name, flags);
    else set->ops.push_back({PathSetObj::Op::Flags, name, flags, {}});
    return Value();
  });
  implement(vm, "gsGridObject", u"addPath", [](VM& v, Object* s, int argc, const Value* a) {
    PathSetObj* set = pathSet(v, s, true);
    if (!set) return Value();
    PathSetObj::Op op;
    op.kind = PathSetObj::Op::Add;
    op.name = utf16ToUtf8(v.toStr(a[0]));
    op.path.flags = uint32_t(v.toInt(a[1]));
    auto vec = [&](int i) {
      Value x = v.resolve(a[i]);
      return i < argc && x.type() == VT::Vector ? Vec3{x.vec()[0], x.vec()[1], x.vec()[2]} : Vec3{};
    };
    auto quat = [&](int i) {
      Value x = v.resolve(a[i]);
      return i < argc && x.type() == VT::Quat ? Quat{x.vec()[0], x.vec()[1], x.vec()[2], x.vec()[3]} : Quat{};
    };
    op.path.p0 = vec(2);
    op.path.f0 = pathForward(quat(3));
    op.path.p1 = vec(4);
    op.path.f1 = pathForward(quat(5));
    if (set->built) set->paths[op.name] = op.path;
    else set->ops.push_back(op);
    return Value();
  });
}

// called by loadGridObject once the template's mesh is loaded, before onLoaded: locator paths join the map here
void markTemplateMeshLoaded(VM& vm, Object* tpl) {
  if (PathSetObj* set = pathSet(vm, tpl, true)) set->ops.push_back({PathSetObj::Op::Locators, {}, 0, {}});
}

}  // namespace regs::engine
