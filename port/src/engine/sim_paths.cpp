// The route strips of the transport lines (gsTransportSegment::updateMesh, L:0x081ba9f0; docs/ui/lines.md §3): every
// segment of a visible line is drawn as a decal ribbon over its path with the line's pathMaterial (path.shader: the
// arrow texture scrolling with the timer, the selected line blinking through custom0.x), or tempPathMaterial when one
// of its two stops is a temporary one (the add-stop tool's hover).
//
// Geometry (general case, :767-860): per path node the Bézier p0, p0 + f0·L/3, p1 − f1·L/3, p1 sampled at t = 0, ¼, ½, ¾,
// 1, two vertices a unit to either side (u = 0 / 1), v growing by L/16 per step (L/4 per node), normal +Y.
// Helicopter legs (class 0x4000) are one arc between the pads with the inner control points 100 units up (port: the
// original also pushes them along the pad directions and samples round(0.6·cost) + 20 steps; here 24 steps).
// Water bus legs (class 0x8000) are smoothed: a C1 Bézier chain through every (nodes / 8)-th node (see buildStrip).
// Not ported: the coverage counters the strips add to grid+0x1e8 (minimap data, transport.md §3.5).
#include <algorithm>
#include <cmath>

#include "engine/display.h"
#include "engine/grid.h"
#include "engine/scene3d.h"
#include "engine/sim_state.h"

namespace regs::engine {

using namespace regs::script;
using render::Vec3;
using namespace sim;

namespace {

Vec3 bezier(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, float t) {
  float u = 1 - t;
  return a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + d * (t * t * t);
}
Vec3 bezierTangent(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, float t) {
  float u = 1 - t;
  return (b - a) * (3 * u * u) + (c - b) * (6 * u * t) + (d - c) * (3 * t * t);
}

struct Strip {
  render::MeshData m;
  float v = 0;
  void pair(const Vec3& p, const Vec3& tangent, bool join) {
    Vec3 side = render::cross(tangent, Vec3{0, 1, 0});
    float l = std::sqrt(render::dot(side, side));
    side = l > 1e-6f ? side * (1.0f / l) : Vec3{1, 0, 0};
    uint32_t base = uint32_t(m.vertexCount());
    for (int k = 0; k < 2; ++k) {
      Vec3 q = k == 0 ? p + side : p - side;
      m.positions.insert(m.positions.end(), {q.x, q.y, q.z});
      m.normals.insert(m.normals.end(), {0.0f, 1.0f, 0.0f});
      m.uv0.insert(m.uv0.end(), {float(k), v});
      m.colors.push_back(0xFFFFFFFFu);
    }
    if (join && base >= 2) m.indices.insert(m.indices.end(), {base, base - 1, base - 2, base + 1, base - 1, base});
  }
};

uint64_t mix(uint64_t h, uint64_t x) { return (h ^ x) * 0x100000001b3ull; }

Object* member(VM& vm, Object* o, const char16_t* name) {
  try {
    return vm.resolve(vm.getMember(Value::heap(VT::Object, o), name)).object();
  } catch (...) {
    return nullptr;
  }
}

std::shared_ptr<render::GpuMesh> buildStrip(Display& d, const SimState& s, const LineState& line, const LineStopState& st) {
  const LaneGraph& G = s.graph;
  Strip strip;
  if (line.type == 4) {  // a helicopter: one arc pad to pad
    if (st.legStart < 0 || st.legEnd < 0) return nullptr;
    Vec3 a = G.nodes[size_t(st.legStart)].p1, dd = G.nodes[size_t(st.legEnd)].p0;
    Vec3 b = a + Vec3{0, 100, 0}, c = dd + Vec3{0, 100, 0};
    const int n = 24;
    for (int i = 0; i <= n; ++i) {
      float t = float(i) / float(n);
      Vec3 p = bezier(a, b, c, dd, t);
      if (i > 0) {
        Vec3 q = bezier(a, b, c, dd, float(i - 1) / float(n));
        Vec3 e = p - q;
        strip.v += std::sqrt(render::dot(e, e)) * 0.25f;
      }
      strip.pair(p, bezierTangent(a, b, c, dd, t), i > 0);
    }
  } else if (line.type == 3 && st.path.size() >= 16) {
    // a water bus (class 0x8000, L:0x081bab6f-0x081bb70d = W:0x101119d0.c:320-509): N = nodes / 8 Bézier pieces
    // through every (nodes / N)-th node, 16 steps each, v += 2 per step. The first piece starts at the first node's
    // start with its control 8 units up; inner anchors are their node's start 6 units up, with the incoming control
    // D − (next anchor node's start − A) · 0.167; the last piece ends at the last node's end, control 8 units up; each
    // piece starts with B = 2D − C of the one before (C1 joins). Tangents at t = 1/16 for the first and 15/16 for the
    // last step. Port: legs under 16 nodes keep the general strip (the original walks past the path's end there and
    // draws none, inf).
    const int count = int(st.path.size()), pieces = count >> 3;
    auto node = [&](int i) -> const LaneNode& { return G.nodes[size_t(st.path[size_t(std::clamp(i, 0, count - 1))])]; };
    Vec3 a = node(0).p0, b = a + Vec3{0, 8, 0};
    int current = count / pieces;
    for (int k = 1; k <= pieces; ++k) {
      Vec3 c, dd;
      if (k == pieces) {
        dd = node(current).p1;
        c = dd + Vec3{0, 8, 0};
      } else {
        int next = std::min((k + 1) * count / pieces, count - 1);
        dd = node(current).p0 + Vec3{0, 6, 0};
        c = dd - (node(next).p0 - a) * 0.167f;
        current = next;
      }
      for (int i = 0; i <= 16; ++i) {
        float t = float(i) * 0.0625f, tt = i == 0 ? 0.0625f : i == 16 ? 0.9375f : t;
        if (i > 0) strip.v += 2.0f;
        strip.pair(bezier(a, b, c, dd, t), bezierTangent(a, b, c, dd, tt), i > 0);
      }
      a = dd;
      b = dd * 2.0f - c;
    }
  } else {
    for (int nd : st.path) {
      const LaneNode& n = G.nodes[size_t(nd)];
      float L = n.length;
      Vec3 a = n.p0, b = n.p0 + n.f0 * (L / 3), c = n.p1 - n.f1 * (L / 3), e = n.p1;
      for (int k = 0; k <= 4; ++k) {
        float t = float(k) * 0.25f;
        if (k > 0) strip.v += L * 0.0625f;
        strip.pair(bezier(a, b, c, e, t), bezierTangent(a, b, c, e, t), k > 0);
      }
    }
  }
  if (strip.m.indices.empty()) return nullptr;
  render::Submesh sm;
  sm.vertexCount = uint32_t(strip.m.vertexCount());
  sm.indexCount = uint32_t(strip.m.indices.size());
  strip.m.submeshes.push_back(sm);
  float r = 0;
  for (size_t i = 0; i < strip.m.positions.size(); ++i) r = std::max(r, std::fabs(strip.m.positions[i]));
  strip.m.radius = r;
  auto mesh = std::shared_ptr<render::GpuMesh>(new render::GpuMesh(d.r3d().upload(strip.m)), [](render::GpuMesh* g) {
    if (Display* dd = display()) dd->r3d().destroy(*g);
    delete g;
  });
  return mesh;
}

}  // namespace

void collectLinePaths(Display& d, VM& vm, SimState& s, const Vec3& eye, float farZ, std::vector<render::DrawItem>& items) {
  for (LineState& line : s.lines) {
    if (line.deleted || !line.object || !(line.object->flags & 2)) continue;  // gsModule visible (bit 2)
    size_t n = line.stops.size();
    if (n < 2) continue;
    line.pathMaterials = {scriptMaterial(d, vm, member(vm, line.object.get(), u"pathMaterial"))};
    line.tempPathMaterials = {scriptMaterial(d, vm, member(vm, line.object.get(), u"tempPathMaterial"))};
    for (size_t k = 0; k < n; ++k) {
      LineStopState& st = line.stops[k];
      if (k + 1 == n && !line.closed) break;
      if (!st.reachable) {
        st.pathMesh.reset();
        continue;
      }
      const LineStopState& next = line.stops[(k + 1) % n];
      bool temp = ((st.flags | next.flags) & 1) != 0;
      uint64_t key = 0xcbf29ce484222325ull;
      key = mix(key, uint64_t(uint32_t(st.legStart))), key = mix(key, uint64_t(uint32_t(st.legEnd))), key = mix(key, uint64_t(line.type));
      for (int nd : st.path) key = mix(key, uint64_t(uint32_t(nd)));
      if (!st.pathMesh || st.pathKey != key) {
        st.pathMesh = buildStrip(d, s, line, st);
        st.pathKey = key;
      }
      if (!st.pathMesh) continue;
      // near enough: the leg's platform within the view distance plus a margin for long legs
      if (st.legStart >= 0) {
        Vec3 e = s.graph.nodes[size_t(st.legStart)].p0 - eye;
        float reach = farZ + 600.0f;
        if (render::dot(e, e) > reach * reach) continue;
      }
      render::DrawItem it;
      it.mesh = st.pathMesh.get();
      it.materials = temp ? &line.tempPathMaterials : &line.pathMaterials;
      it.world = render::Mat4();
      it.surfaceClass = render::kSurfaceOverlay;  // route strips keep their colour under the mods' shader hooks
      items.push_back(it);
    }
  }
}

}  // namespace regs::engine
