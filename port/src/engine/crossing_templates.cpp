// Port: level-crossing pieces the game does not ship (crossing_templates.h), made from the game's meshes, paths and
// skeleton when the road envset has registered their sources. Template space: x along the road, z along the track
// (the frame of track_double_cross_small and of the straight road pieces, orientation 0), origin at the footprint's
// centre, units of 3 per cell.
#include "engine/crossing_templates.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/text.h"
#include "engine/grid.h"
#include "engine/scene3d.h"
#include "engine/sim_graph.h"
#include "engine/skinning.h"
#include "render/mesh.h"
#include "render/skeleton.h"
#include "vfs/vfs.h"

namespace regs::engine {

namespace sim {
void setPortWalkMap(const script::Object* tpl, std::vector<uint32_t> words);  // engine/sim_citizens.cpp
}

using namespace regs::script;
using render::Mat4;
using render::MeshData;
using render::ObjectData;
using render::Vec3;

namespace {

constexpr uint32_t kCrossTrack = 0x1000;     // GRID_ITEM_CROSS_TRACK / the key's SPECIAL0
constexpr uint32_t kPathIntersection = 0x10;  // GRID_PATH_INTERSECTION
constexpr uint32_t kPathStop = 0x40;          // GRID_PATH_STOP

// one vertex with every attribute the meshes carry
struct V {
  Vec3 p, n{0, 1, 0};
  float uv0[2] = {0, 0}, uv1[2] = {0, 0}, tan[4] = {1, 0, 0, 1};
  uint32_t color = 0xFFFFFFFFu;
  uint16_t bone[4] = {0, 0, 0, 0};
  float w[4] = {1, 0, 0, 0};
  bool dropped = false;  // left out (its triangles go)
};

V readV(const MeshData& m, uint32_t i) {
  V v;
  v.p = {m.positions[3 * i], m.positions[3 * i + 1], m.positions[3 * i + 2]};
  if (m.normals.size() >= 3 * (size_t(i) + 1)) v.n = {m.normals[3 * i], m.normals[3 * i + 1], m.normals[3 * i + 2]};
  if (m.uv0.size() >= 2 * (size_t(i) + 1)) v.uv0[0] = m.uv0[2 * i], v.uv0[1] = m.uv0[2 * i + 1];
  if (m.uv1.size() >= 2 * (size_t(i) + 1)) v.uv1[0] = m.uv1[2 * i], v.uv1[1] = m.uv1[2 * i + 1];
  if (m.tangents.size() >= 4 * (size_t(i) + 1))
    for (int k = 0; k < 4; ++k) v.tan[k] = m.tangents[4 * i + k];
  if (m.colors.size() > i) v.color = m.colors[i];
  if (m.skinBones.size() >= 4 * (size_t(i) + 1) && m.skinWeights.size() >= 4 * (size_t(i) + 1))
    for (int k = 0; k < 4; ++k) v.bone[k] = m.skinBones[4 * i + k], v.w[k] = m.skinWeights[4 * i + k];
  return v;
}

void writeV(MeshData& m, const V& v, bool skinned) {
  m.positions.insert(m.positions.end(), {v.p.x, v.p.y, v.p.z});
  m.normals.insert(m.normals.end(), {v.n.x, v.n.y, v.n.z});
  m.uv0.insert(m.uv0.end(), {v.uv0[0], v.uv0[1]});
  m.uv1.insert(m.uv1.end(), {v.uv1[0], v.uv1[1]});
  m.tangents.insert(m.tangents.end(), {v.tan[0], v.tan[1], v.tan[2], v.tan[3]});
  m.colors.push_back(v.color);
  if (skinned) {
    m.skinBones.insert(m.skinBones.end(), {v.bone[0], v.bone[1], v.bone[2], v.bone[3]});
    m.skinWeights.insert(m.skinWeights.end(), {v.w[0], v.w[1], v.w[2], v.w[3]});
  }
}

V lerpV(const V& a, const V& b, float t) {
  V r = t < 0.5f ? a : b;  // the skin of the nearer end
  r.p = a.p + (b.p - a.p) * t;
  r.n = render::normalize(a.n + (b.n - a.n) * t);
  for (int k = 0; k < 2; ++k) r.uv0[k] = a.uv0[k] + (b.uv0[k] - a.uv0[k]) * t, r.uv1[k] = a.uv1[k] + (b.uv1[k] - a.uv1[k]) * t;
  for (int k = 0; k < 4; ++k) r.tan[k] = a.tan[k] + (b.tan[k] - a.tan[k]) * t;
  uint32_t c = 0;
  for (int s = 0; s < 32; s += 8) {
    float ca = float((a.color >> s) & 0xff), cb = float((b.color >> s) & 0xff);
    c |= uint32_t(std::lround(ca + (cb - ca) * t)) << s;
  }
  r.color = c;
  return r;
}

float coord(const Vec3& p, int axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }

struct Clip {
  int axis;
  float value;
  bool above;  // keep coord >= value (else coord <= value)
};

std::vector<V> clipPoly(const std::vector<V>& in, const Clip& c) {
  std::vector<V> out;
  auto dist = [&](const V& v) { return coord(v.p, c.axis) - c.value; };
  auto inside = [&](const V& v) { return c.above ? dist(v) >= -1e-5f : dist(v) <= 1e-5f; };
  for (size_t i = 0; i < in.size(); ++i) {
    const V& a = in[i];
    const V& b = in[(i + 1) % in.size()];
    bool ia = inside(a), ib = inside(b);
    if (ia) out.push_back(a);
    if (ia != ib) {
      float da = dist(a), db = dist(b);
      out.push_back(lerpV(a, b, da / (da - db)));
    }
  }
  return out;
}

// appends src's triangles to dst, every vertex through xf and the polygons clipped (in dst space); vertices of an
// unskinned source take staticBone when dst is skinned
void appendMesh(ObjectData& dst, const ObjectData& src, const std::function<void(V&)>& xf, const std::vector<Clip>& clips,
                bool skinned, uint16_t staticBone) {
  const MeshData& s = src.mesh;
  MeshData& d = dst.mesh;
  const int matBase = int(dst.materials.size());
  dst.materials.insert(dst.materials.end(), src.materials.begin(), src.materials.end());
  const bool srcSkinned = !s.skinBones.empty();
  for (const render::Submesh& sm : s.submeshes) {
    render::Submesh out;
    out.firstVertex = uint32_t(d.vertexCount());
    out.firstIndex = uint32_t(d.indices.size());
    out.material = matBase + sm.material;
    out.colorize = sm.colorize;
    for (uint32_t k = 0; k + 2 < sm.indexCount; k += 3) {
      std::vector<V> poly;
      for (uint32_t j = 0; j < 3; ++j) {
        V v = readV(s, s.indices[sm.firstIndex + k + j]);
        if (!srcSkinned) {
          v.bone[0] = staticBone, v.bone[1] = v.bone[2] = v.bone[3] = 0;
          v.w[0] = 1, v.w[1] = v.w[2] = v.w[3] = 0;
        }
        xf(v);
        poly.push_back(v);
      }
      if (poly[0].dropped || poly[1].dropped || poly[2].dropped) continue;
      for (const Clip& c : clips) {
        poly = clipPoly(poly, c);
        if (poly.size() < 3) break;
      }
      if (poly.size() < 3) continue;
      uint32_t base = uint32_t(d.vertexCount());
      for (const V& v : poly) writeV(d, v, skinned);
      for (uint32_t j = 1; j + 1 < poly.size(); ++j) d.indices.insert(d.indices.end(), {base, base + j, base + j + 1});
    }
    out.vertexCount = uint32_t(d.vertexCount()) - out.firstVertex;
    out.indexCount = uint32_t(d.indices.size()) - out.firstIndex;
    if (out.indexCount) d.submeshes.push_back(out);
  }
}

void finishMesh(MeshData& m) {
  float r = 0;
  m.boundsMin = {1e30f, 1e30f, 1e30f};
  m.boundsMax = {-1e30f, -1e30f, -1e30f};
  for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
    for (int k = 0; k < 3; ++k) {
      m.boundsMin[size_t(k)] = std::min(m.boundsMin[size_t(k)], m.positions[i + size_t(k)]);
      m.boundsMax[size_t(k)] = std::max(m.boundsMax[size_t(k)], m.positions[i + size_t(k)]);
    }
    r = std::max(r, std::sqrt(m.positions[i] * m.positions[i] + m.positions[i + 1] * m.positions[i + 1] +
                              m.positions[i + 2] * m.positions[i + 2]));
  }
  m.radius = r;
}

// the crossing's road over the rails: its height along x (the road paths' curves), level beyond them
struct Profile {
  std::vector<std::pair<float, float>> xy;  // ascending x
  float base = -0.2f, x0 = 0, x1 = 0;
  float at(float x) const {
    if (xy.size() < 2 || x <= x0 || x >= x1) return base;
    auto it = std::lower_bound(xy.begin(), xy.end(), std::make_pair(x, -1e30f));
    if (it == xy.begin()) return it->second;
    auto a = std::prev(it);
    float t = (x - a->first) / std::max(1e-6f, it->first - a->first);
    return a->second + (it->second - a->second) * t;
  }
  float slope(float x) const { return (at(x + 0.05f) - at(x - 0.05f)) / 0.1f; }
};

Vec3 bezier(const TemplatePathDef& p, float t) {
  Vec3 d = p.p1 - p.p0;
  float L = std::sqrt(dot(d, d));
  Vec3 c1 = p.p0 + p.f0 * (L / 3), c2 = p.p1 - p.f1 * (L / 3);
  float u = 1 - t;
  return p.p0 * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + p.p1 * (t * t * t);
}

Profile crossingProfile(const std::vector<TemplatePathDef>& paths) {
  Profile pr;
  bool any = false;
  for (const TemplatePathDef& p : paths) {
    if ((p.flags & 0x1000) || std::fabs(p.p0.z - p.p1.z) > 0.01f || p.p0.z > 0) continue;  // one lane of the road
    for (int k = 0; k <= 32; ++k) {
      Vec3 q = bezier(p, float(k) / 32.0f);
      pr.xy.push_back({q.x, q.y});
    }
    any = true;
  }
  std::sort(pr.xy.begin(), pr.xy.end());
  if (any) pr.x0 = pr.xy.front().first, pr.x1 = pr.xy.back().first, pr.base = pr.xy.front().second;
  return pr;
}

// a template's walk map (<stem>.bmp, w × l cells of 16 two-bit q-cell codes, as sim_citizens.cpp walkMap reads it)
std::vector<uint32_t> walkWords(VM& vm, const ObjectObj* tpl, int w, int l) {
  std::vector<uint32_t> out;
  const std::string& path = tpl->pendingPath;
  size_t dot = path.rfind('.');
  std::vector<uint8_t> file;
  int bw = 0, bh = 0;
  std::vector<uint32_t> px;
  if (dot == std::string::npos || !vm.vfs().read(path.substr(0, dot) + ".bmp", file) || !parseBmp(file, bw, bh, px) ||
      bw < w * 4 || bh < l * 4)
    return out;
  out.assign(size_t(w) * size_t(l), 0);
  for (int cz = 0; cz < l; ++cz)
    for (int cx = 0; cx < w; ++cx) {
      uint32_t word = 0;
      for (int qz = 0; qz < 4; ++qz)
        for (int qx = 0; qx < 4; ++qx) {
          uint32_t p = px[size_t(cz * 4 + qz) * size_t(bw) + size_t(cx * 4 + qx)];
          uint32_t code = ((p >> 16) & 0xff) == 0xff ? 2 : ((p >> 8) & 0xff) == 0xff ? 3 : (p & 0xff) == 0xff ? 1 : 0;
          word |= code << (2 * (qx + qz * 4));
        }
      out[size_t(cz) * size_t(w) + size_t(cx)] = word;
    }
  return out;
}

ObjectObj* templateObj(GridObj* g, const char* name) {
  auto it = g->templates.find(name);
  return it == g->templates.end() ? nullptr : dynamic_cast<ObjectObj*>(it->second.get());
}

// a new gsGridObject registered as loadGridObject does, its fields from the companion script of `scriptOf`'s object
ObjectObj* newTemplate(VM& vm, GridObj* g, const std::string& name, ObjectObj* scriptOf) {
  Ref<Object> t = vm.newObject(vm.findClass("gsGridObject"), nullptr);
  auto* o = dynamic_cast<ObjectObj*>(t.get());
  if (!o) return nullptr;
  t->fieldValues[u"idName"] = Value::string(utf8ToUtf16(name));
  if (auto pk = scriptOf->fieldValues.find(u"<package>"); pk != scriptOf->fieldValues.end()) t->fieldValues[u"<package>"] = pk->second;
  g->templates[name] = t;
  g->templateOrder.push_back(name);
  runCompanionScript(vm, o, utf8ToUtf16(scriptOf->pendingPath));
  o->pendingPath = scriptOf->pendingPath;
  return o;
}

// tram_small_cross: tram_small_basic raised over the crossing's road
void makeTramCross(VM& vm, GridObj* g, ObjectObj* tram, ObjectObj* cross) {
  const Profile pr = crossingProfile(templatePathDefs(vm, cross));
  if (pr.xy.size() < 2 || !tram->data) return;
  ObjectObj* o = newTemplate(vm, g, "tram_small_cross", tram);
  if (!o) return;
  auto data = std::make_shared<ObjectData>();
  data->name = "tram_small_cross";
  data->nodePosition = tram->data->nodePosition;
  data->nodeRotation = tram->data->nodeRotation;
  appendMesh(*data, *tram->data, [&](V& v) {
    float s = pr.slope(v.p.x);
    v.p.y += pr.at(v.p.x) - pr.base;
    v.n = render::normalize(Vec3{v.n.x - s * v.n.y, v.n.y + s * v.n.x, v.n.z});
  }, {}, false, 0);
  finishMesh(data->mesh);
  o->data = data;
  std::vector<TemplatePathDef> paths = templatePathDefs(vm, tram);
  for (TemplatePathDef& p : paths) {
    auto lift = [&](Vec3& q, Vec3& f) {
      float s = pr.slope(q.x);
      q.y += pr.at(q.x) - pr.base;
      f = render::normalize(Vec3{f.x, f.y + s * f.x, f.z});
    };
    lift(p.p0, p.f0);
    lift(p.p1, p.f1);
    p.flags = (p.flags & ~kPathStop) | kPathIntersection;  // no tram stop on a crossing
  }
  setTemplatePaths(vm, o, paths);
  o->fieldValues[u"flags"] = Value::integer(int32_t(uint32_t(vm.toInt(o->fieldValues[u"flags"])) | kCrossTrack));
  LOG_INFO("port crossings: tram_small_cross (%zu tris, %zu paths, hump %.2f)", data->mesh.indices.size() / 3,
           paths.size(), pr.at(0) - pr.base);
}

// the crossing's barrier rig: its bones' bind pivots, which are barriers, each barrier's arm axis and length, the
// static bone the rest of the mesh hangs on
struct Rig {
  size_t nb = 0;
  std::vector<bool> barrier;
  std::vector<Vec3> pivot;
  std::vector<int> armAxis;
  std::vector<float> armLength;
  int staticBone = 0;
  int dominant(const V& v) const {
    int k = 0;
    for (int j = 1; j < 4; ++j)
      if (v.w[j] > v.w[k]) k = j;
    return size_t(v.bone[k]) < nb ? int(v.bone[k]) : 0;
  }
  // the barrier bone on the side of z (−1, +1) of the crossing, −1 when none
  int barrierOn(float z) const {
    for (size_t b = 0; b < nb; ++b)
      if (barrier[b] && (pivot[b].z > 0) == (z > 0)) return int(b);
    return -1;
  }
};

Rig crossingRig(const render::SkeletonData& sd, const MeshData& cm) {
  Rig r;
  r.nb = sd.bones.size();
  r.barrier.assign(r.nb, false);
  r.pivot.resize(r.nb);
  for (size_t b = 0; b < r.nb; ++b) {
    Mat4 w = render::inverseAffine(sd.inverseBind[b]);
    r.pivot[b] = {w.at(0, 3), w.at(1, 3), w.at(2, 3)};
    r.barrier[b] = sd.bones[b].name.find("barrier") != std::string::npos;
  }
  std::vector<int> count(r.nb, 0);
  std::vector<Vec3> extent(r.nb, Vec3{0, 0, 0});
  for (uint32_t i = 0; i < cm.vertexCount(); ++i) {
    V v = readV(cm, i);
    size_t b = size_t(r.dominant(v));
    ++count[b];
    Vec3 d = v.p - r.pivot[b];
    extent[b] = {std::max(extent[b].x, std::fabs(d.x)), std::max(extent[b].y, std::fabs(d.y)), std::max(extent[b].z, std::fabs(d.z))};
  }
  for (size_t b = 0; b < r.nb; ++b)
    if (!r.barrier[b] && count[b] > count[size_t(r.staticBone)]) r.staticBone = int(b);
  r.armAxis.assign(r.nb, 1);
  r.armLength.assign(r.nb, 1);
  for (size_t b = 0; b < r.nb; ++b) {
    const Vec3& e = extent[b];
    r.armAxis[b] = e.x >= e.y && e.x >= e.z ? 0 : e.y >= e.z ? 1 : 2;
    r.armLength[b] = std::max(0.5f, r.armAxis[b] == 0 ? e.x : r.armAxis[b] == 1 ? e.y : e.z);
  }
  return r;
}

Vec3 scaleAxis(Vec3 v, int axis, float k) {
  (axis == 0 ? v.x : axis == 1 ? v.y : v.z) *= k;
  return v;
}

// a barrier vertex moved with its pivot (to pivot2) and its arm scaled by k about the pivot
void moveBarrierVertex(V& v, const Rig& r, int b, const Vec3& pivot2, float k) {
  int ax = r.armAxis[size_t(b)];
  v.p = pivot2 + scaleAxis(v.p - r.pivot[size_t(b)], ax, k);
  v.n = render::normalize(scaleAxis(v.n, ax, 1.0f / k));
  Vec3 t = render::normalize(scaleAxis(Vec3{v.tan[0], v.tan[1], v.tan[2]}, ax, k));
  v.tan[0] = t.x, v.tan[1] = t.y, v.tan[2] = t.z;
}

// the template's own skeleton (from the crossing's companion script) and its animation with the barriers' pivots at
// pivot2; their parents stay where they are
void moveSkeleton(SkeletonObj* sk, const Rig& r, const std::vector<Vec3>& pivot2) {
  const render::SkeletonData& sd = *sk->data;
  auto skel = std::make_shared<render::SkeletonData>(sd);
  std::vector<Vec3> delta(r.nb, Vec3{0, 0, 0});
  for (size_t b = 0; b < r.nb; ++b) {
    if (!r.barrier[b]) continue;
    const render::BoneDef& bd = sd.bones[b];
    Mat4 parentInv = bd.parent >= 0 ? sd.inverseBind[size_t(bd.parent)] : Mat4{};
    Vec3 dw = pivot2[b] - r.pivot[b];
    delta[b] = {parentInv.at(0, 0) * dw.x + parentInv.at(0, 1) * dw.y + parentInv.at(0, 2) * dw.z,
                parentInv.at(1, 0) * dw.x + parentInv.at(1, 1) * dw.y + parentInv.at(1, 2) * dw.z,
                parentInv.at(2, 0) * dw.x + parentInv.at(2, 1) * dw.y + parentInv.at(2, 2) * dw.z};
    skel->bones[b].t = bd.t + delta[b];
  }
  std::vector<Mat4> world(r.nb);
  for (size_t b = 0; b < r.nb; ++b) {
    const render::BoneDef& bd = skel->bones[b];
    Mat4 local = render::boneLocal(bd.t, bd.q, bd.s);
    world[b] = bd.parent >= 0 ? world[size_t(bd.parent)] * local : local;
    skel->inverseBind[b] = render::inverseAffine(world[b]);
  }
  sk->data = skel;
  for (auto& [name, slot] : sk->slots) {
    if (!slot.anim) continue;
    auto anim = std::make_shared<render::AnimationData>(*slot.anim);
    for (size_t b = 0; b < r.nb && b < slot.trackOfBone.size(); ++b) {
      int t = slot.trackOfBone[b];
      if (!r.barrier[b] || t < 0 || size_t(t) >= anim->tracks.size()) continue;
      for (render::TrackKey& k : anim->tracks[size_t(t)].keys) k.t = k.t + delta[b];
    }
    slot.anim = anim;
  }
}

// a crossing template made from track_double_cross_small's script (skeleton, barrier timing, bell, flags)
ObjectObj* newCrossing(VM& vm, GridObj* g, const std::string& name, ObjectObj* cross, SkeletonObj*& sk) {
  ObjectObj* o = newTemplate(vm, g, name, cross);
  sk = o ? modelSkeleton(vm, o) : nullptr;
  if (!sk || !sk->data) {
    LOG_WARN("port crossings: %s has no skeleton", name.c_str());
    return nullptr;
  }
  return o;
}

float crossingTrackY(const std::vector<TemplatePathDef>& crossPaths) {
  for (const TemplatePathDef& p : crossPaths)
    if (p.flags & 0x1000) return p.p0.y;
  return 0.4f;
}

// the two tracks of a crossing along z (x = ∓1.5, as the crossing's), split at the given z
void addRails(std::vector<TemplatePathDef>& paths, float y, const std::vector<float>& zs) {
  for (int side = 0; side < 2; ++side) {
    const float x = side == 0 ? -1.5f : 1.5f;
    const int dir = side == 0 ? 1 : -1;
    const size_t n = zs.size();
    for (size_t k = 0; k + 1 < n; ++k) {
      TemplatePathDef p;
      p.name = std::string(side == 0 ? "u-c" : "u-d") + std::to_string(k);
      p.flags = 0x1000 | kPathIntersection;
      float a = dir > 0 ? zs[k] : zs[n - 1 - k], b = dir > 0 ? zs[k + 1] : zs[n - 2 - k];
      p.p0 = {x, y, a};
      p.p1 = {x, y, b};
      p.f0 = p.f1 = {0, 0, float(dir)};
      paths.push_back(p);
    }
  }
}

// track_double_cross_large: the double track across a large road (crossing_templates.h)
void makeLargeCross(VM& vm, GridObj* g, ObjectObj* cross, ObjectObj* large) {
  if (!cross->data || !large->data || cross->data->mesh.skinBones.empty()) return;
  const std::vector<TemplatePathDef> crossPaths = templatePathDefs(vm, cross);
  const Profile pr = crossingProfile(crossPaths);
  SkeletonObj* sk = nullptr;
  ObjectObj* o = newCrossing(vm, g, "track_double_cross_large", cross, sk);
  if (!o) return;
  const float kz = 2.0f, kArm = 2.0f;  // the large road is twice as wide (asphalt 12, pavements 6); two lanes a side
  const Rig r = crossingRig(*sk->data, cross->data->mesh);
  // the crossing's end ramp over the track bed (its surface falls from the pavement to the ballast over the last units,
  // where its rails have ended): kept at its own width at the large crossing's ends; the asphalt (|z| ≤ 3) doubles and
  // the pavement takes the rest
  float flatEnd = 0;
  {
    const MeshData& cm = cross->data->mesh;
    for (uint32_t i = 0; i < cm.vertexCount(); ++i) {
      V v = readV(cm, i);
      if (r.barrier[size_t(r.dominant(v))] || std::fabs(v.p.x) > 3.0f || std::fabs(v.p.z) > 6.0f) continue;
      if (v.p.y >= -0.05f && v.p.y < 0.3f) flatEnd = std::max(flatEnd, std::fabs(v.p.z));
    }
  }
  const float ramp = flatEnd > 3.5f && flatEnd < 5.95f ? 6.0f - flatEnd : 0.0f;
  auto stretch = [ramp](float z, float& k) {
    float a = std::fabs(z), s = z < 0 ? -1.0f : 1.0f;
    if (a <= 3.0f) return k = 2.0f, z * 2.0f;
    if (a <= 6.0f - ramp) {
      k = (6.0f - ramp) / (3.0f - ramp);
      return s * (6.0f + (a - 3.0f) * k);
    }
    return k = 1.0f, s * (a + 6.0f);
  };
  std::vector<Vec3> pivot2(r.nb);
  for (size_t b = 0; b < r.nb; ++b) pivot2[b] = {r.pivot[b].x, r.pivot[b].y, r.pivot[b].z * kz};

  auto data = std::make_shared<ObjectData>();
  data->name = "track_double_cross_large";
  data->nodePosition = cross->data->nodePosition;
  data->nodeRotation = cross->data->nodeRotation;
  // the crossing, stretched across the road (the middle 12 units of road)
  appendMesh(*data, *cross->data, [&](V& v) {
    int b = r.dominant(v);
    if (r.barrier[size_t(b)]) {  // the arm twice as long, its pivot out on the large road's pavement
      moveBarrierVertex(v, r, b, pivot2[size_t(b)], kArm);
    } else if (v.p.y > 0.3f && std::fabs(v.p.z) > 3.2f) {  // posts and signals on the pavements: moved, not widened
      int s = r.barrierOn(v.p.z);
      if (s >= 0) v.p.z += pivot2[size_t(s)].z - r.pivot[size_t(s)].z;
    } else {
      float k = kz;
      v.p.z = stretch(v.p.z, k);
      v.n = render::normalize(Vec3{v.n.x, v.n.y, v.n.z / k});
      Vec3 t = render::normalize(Vec3{v.tan[0], v.tan[1], v.tan[2] * k});
      v.tan[0] = t.x, v.tan[1] = t.y, v.tan[2] = t.z;
    }
  }, {{0, -6.0f, true}, {0, 6.0f, false}, {2, -12.0f, true}, {2, 12.0f, false}}, true, uint16_t(r.staticBone));
  // the large road on both sides of it
  auto same = [](V&) {};
  appendMesh(*data, *large->data, same, {{0, -6.0f, false}}, true, uint16_t(r.staticBone));
  appendMesh(*data, *large->data, same, {{0, 6.0f, true}}, true, uint16_t(r.staticBone));
  finishMesh(data->mesh);
  o->data = data;
  moveSkeleton(sk, r, pivot2);

  // lanes: the large road's four (no stops, lane changes or kerb exits) over the crossing's hump, and the two tracks
  std::vector<TemplatePathDef> paths;
  auto lane = [&](const std::string& name, float z, int dir) {  // dir −1: towards −x
    const float xs[5] = {12, 6, 0, -6, -12};
    for (int k = 0; k < 4; ++k) {
      float a = xs[k] * float(-dir), b = xs[k + 1] * float(-dir);
      TemplatePathDef p;
      p.name = name + std::to_string(k);
      p.flags = 0x500 | kPathIntersection;
      p.p0 = {a, pr.at(a), z};
      p.p1 = {b, pr.at(b), z};
      float dx = float(dir);
      p.f0 = render::normalize(Vec3{dx, pr.slope(a) * dx, 0});
      p.f1 = render::normalize(Vec3{dx, pr.slope(b) * dx, 0});
      paths.push_back(p);
    }
  };
  lane("t-a", -4.5f, -1);
  lane("t-b", -1.5f, -1);
  lane("t-c", 4.5f, 1);
  lane("t-d", 1.5f, 1);
  addRails(paths, crossingTrackY(crossPaths), {-12, 0, 12});
  setTemplatePaths(vm, o, paths);
  // the walk map: the large road's (pavements walkable across the track)
  std::vector<uint32_t> walk = walkWords(vm, large, 8, 8);
  if (walk.size() == 64) sim::setPortWalkMap(o, std::move(walk));

  o->fieldValues[u"width"] = Value::integer(8);
  o->fieldValues[u"length"] = Value::integer(8);
  o->fieldValues[u"type"] = Value::integer(5);  // GRID_ITEM_LARGE_ROAD
  o->fieldValues[u"connections"] = Value::integer(int32_t(0x11004400u));
  o->fieldValues[u"offsetx"] = Value::integer(-2);
  o->fieldValues[u"offsetz"] = Value::integer(-2);
  LOG_INFO("port crossings: track_double_cross_large (%zu tris, %zu paths, static bone %d, end ramp %.2f)",
           data->mesh.indices.size() / 3, paths.size(), r.staticBone, ramp);
}

// track_double_half: half a straight double track (2 cells), for the track blocks a large crossing covers half of;
// SPECIAL0 in its key
void makeHalfTrack(VM& vm, GridObj* g, ObjectObj* track) {
  if (!track->data) return;
  ObjectObj* o = newTemplate(vm, g, "track_double_half", track);
  if (!o) return;
  auto data = std::make_shared<ObjectData>();
  data->name = "track_double_half";
  data->nodePosition = track->data->nodePosition;
  data->nodeRotation = track->data->nodeRotation;
  appendMesh(*data, *track->data, [](V& v) { v.p.x += 3.0f; }, {{0, -3.0f, true}, {0, 3.0f, false}}, false, 0);
  finishMesh(data->mesh);
  o->data = data;
  std::vector<TemplatePathDef> paths;
  for (TemplatePathDef p : templatePathDefs(vm, track)) {  // the halves on x ≤ 0 of each track, moved by 3
    if (std::max(p.p0.x, p.p1.x) > 0.01f) continue;
    p.p0.x += 3.0f, p.p1.x += 3.0f;
    paths.push_back(p);
  }
  setTemplatePaths(vm, o, paths);
  o->fieldValues[u"width"] = Value::integer(2);
  o->fieldValues[u"flags"] = Value::integer(int32_t(uint32_t(vm.toInt(o->fieldValues[u"flags"])) | kCrossTrack));
  LOG_INFO("port crossings: track_double_half (%zu tris, %zu paths)", data->mesh.indices.size() / 3, paths.size());
}

// track_double_cross_alley<r>: the double track's block (2 cells of bed across, 4 along) with a footpath over the bed
// at row r (0..3 along the track) and two barriers as wide as the path on the alley either side of the bed
void makeAlleyCross(VM& vm, GridObj* g, ObjectObj* cross, ObjectObj* track, ObjectObj* footpath, int row) {
  if (!cross->data || !track->data || !footpath->data || cross->data->mesh.skinBones.empty()) return;
  const std::vector<TemplatePathDef> crossPaths = templatePathDefs(vm, cross);
  const std::string name = "track_double_cross_alley" + std::to_string(row);
  SkeletonObj* sk = nullptr;
  ObjectObj* o = newCrossing(vm, g, name, cross, sk);
  if (!o) return;
  const Rig r = crossingRig(*sk->data, cross->data->mesh);
  const float zr = -4.5f + 3.0f * float(row);  // the path's centre along the track (z)
  const float trackY = crossingTrackY(crossPaths);
  // each barrier on the alley beyond the bed on its own side (x), beside the path on its side (z), its arm across the
  // path (3.6 units)
  std::vector<Vec3> pivot2(r.nb);
  std::vector<float> arm(r.nb, 1.0f);
  for (size_t b = 0; b < r.nb; ++b) {
    pivot2[b] = r.pivot[b];
    if (!r.barrier[b]) continue;
    pivot2[b] = {r.pivot[b].x > 0 ? 4.6f : -4.6f, r.pivot[b].y, zr + (r.pivot[b].z > 0 ? 1.9f : -1.9f)};
    arm[b] = 3.6f / r.armLength[b];
  }
  auto data = std::make_shared<ObjectData>();
  data->name = name;
  data->nodePosition = cross->data->nodePosition;
  data->nodeRotation = cross->data->nodeRotation;
  // the track, turned along z
  auto turn = [](const Vec3& a) { return Vec3{a.z, a.y, -a.x}; };
  appendMesh(*data, *track->data, [&](V& v) {
    v.p = turn(v.p);
    v.n = turn(v.n);
    Vec3 t = turn(Vec3{v.tan[0], v.tan[1], v.tan[2]});
    v.tan[0] = t.x, v.tan[1] = t.y, v.tan[2] = t.z;
  }, {}, true, uint16_t(r.staticBone));
  // the footpath over the bed: two path cells at row r, over the sleepers, under the rails' tops
  for (float x : {-1.5f, 1.5f})
    appendMesh(*data, *footpath->data, [&](V& v) { v.p.x += x, v.p.z += zr, v.p.y += trackY + 0.03f; }, {}, true,
               uint16_t(r.staticBone));
  // the barriers and the posts and signals with them; the crossing's road and rails are left out
  appendMesh(*data, *cross->data, [&](V& v) {
    int b = r.dominant(v);
    if (r.barrier[size_t(b)]) {
      moveBarrierVertex(v, r, b, pivot2[size_t(b)], arm[size_t(b)]);
    } else if (v.p.y > 0.3f && std::fabs(v.p.z) > 3.2f) {
      int s = r.barrierOn(v.p.z);
      if (s >= 0) v.p = v.p + (pivot2[size_t(s)] - r.pivot[size_t(s)]);
    } else {
      v.dropped = true;
    }
  }, {}, true, uint16_t(r.staticBone));
  finishMesh(data->mesh);
  o->data = data;
  moveSkeleton(sk, r, pivot2);

  std::vector<TemplatePathDef> paths;
  addRails(paths, trackY, {-6, 0, 6});
  setTemplatePaths(vm, o, paths);
  // the walk map (stored half a turn from the mesh, sim_citizens.cpp stampWalkItem): the path's row walkable
  std::vector<uint32_t> walk(size_t(2) * 4, 0);
  for (int cx = 0; cx < 2; ++cx) walk[size_t(3 - row) * 2 + size_t(cx)] = 0xAAAAAAAAu;  // code 2 in every q-cell
  sim::setPortWalkMap(o, std::move(walk));

  o->fieldValues[u"width"] = Value::integer(2);
  o->fieldValues[u"length"] = Value::integer(4);
  o->fieldValues[u"type"] = Value::integer(9);  // GRID_ITEM_DOUBLE_TRACK
  o->fieldValues[u"connections"] = Value::integer(int32_t(0x11000044u));
  o->fieldValues[u"variation"] = Value::integer(row);
  o->fieldValues[u"offsetx"] = Value::integer(1);
  o->fieldValues[u"offsetz"] = Value::integer(0);
  if (row == 0)
    LOG_INFO("port crossings: track_double_cross_alley0..3 (%zu tris, %zu paths)", data->mesh.indices.size() / 3, paths.size());
}

}  // namespace

void portCrossingTemplates(VM& vm, GridObj* g, const std::string& justLoaded) {
  static const char* kSources[] = {"track_double_cross_small", "tram_small_basic", "road_large_basic", "track_double_basic",
                                   "alley_footpath_basic"};
  if (!g || std::none_of(std::begin(kSources), std::end(kSources), [&](const char* s) { return justLoaded == s; })) return;
  ObjectObj* cross = templateObj(g, "track_double_cross_small");
  ObjectObj* track = templateObj(g, "track_double_basic");
  try {
    if (track && !templateObj(g, "track_double_half")) makeHalfTrack(vm, g, track);
    if (!cross) return;
    if (!templateObj(g, "tram_small_cross"))
      if (ObjectObj* tram = templateObj(g, "tram_small_basic")) makeTramCross(vm, g, tram, cross);
    if (!templateObj(g, "track_double_cross_large"))
      if (ObjectObj* large = templateObj(g, "road_large_basic")) makeLargeCross(vm, g, cross, large);
    if (!templateObj(g, "track_double_cross_alley0"))
      if (ObjectObj* footpath = templateObj(g, "alley_footpath_basic"); footpath && track)
        for (int row = 0; row < 4; ++row) makeAlleyCross(vm, g, cross, track, footpath, row);
  } catch (const std::exception& e) {
    LOG_ERROR("port crossings: %s", e.what());
  }
}

}  // namespace regs::engine
