// Snowplows for the mods (engine/snowplows.h).
#include "engine/snowplows.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "core/log.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/sim_graph.h"
#include "engine/sim_state.h"
#include "engine/traffic_maps.h"

namespace regs::engine {

using namespace regs::engine::sim;
using render::Vec3;

namespace {

constexpr uint32_t kOrange = 0xFFE8780Cu;  // municipal orange (colorize of the car's paint)
constexpr uint32_t kPurple = 0xFF7A2FC0u;  // the easter egg: the fifth plow on the streets is purple
constexpr float kMinTrip = 400.0f;         // m, start to end

uint32_t plowRandom() {  // its own stream: the simulation's (rnd) stays as it is without the mod
  static uint32_t seed = 0x9e3779b9u;
  seed = seed * 1664525u + 1013904223u;
  return seed >> 8;
}

int plowModel(script::VM& vm, SimState& s) {
  int m = modelFor(vm, s.grid, s, "era4-suv");
  return m >= 0 ? m : modelFor(vm, s.grid, s, "era3-suv-01");
}

// a long road trip from a free road START node: the farthest of a few random END nodes at least kMinTrip away
void spawnPlow(script::VM& vm, SimState& s) {
  if (s.roadStarts.empty() || s.roadEnds.empty()) return;
  int model = plowModel(vm, s);
  if (model < 0) return;
  int from = s.roadStarts[plowRandom() % s.roadStarts.size()];
  if (!canEnter(s, from, -2, false) || s.rt[size_t(from)].occupant >= 0) return;
  const Vec3 a = s.graph.nodes[size_t(from)].p0;
  int to = -1;
  float best = kMinTrip * kMinTrip;
  for (int t = 0; t < 24; ++t) {
    int e = s.roadEnds[plowRandom() % s.roadEnds.size()];
    Vec3 d = s.graph.nodes[size_t(e)].p0 - a;
    float d2 = d.x * d.x + d.z * d.z;
    if (d2 >= best) best = d2, to = e;
  }
  std::vector<int> path;
  float cost = 0;
  if (to < 0 || !s.graph.search(from, to, false, path, cost) || path.empty()) return;
  Unit u;
  u.model = model;
  int id = spawnTraffic(vm, s, {u}, from, path, false);
  Mover& m = s.movers[size_t(id)];
  m.plow = true;
  --s.roadLive, ++s.plowsLive;
  bool purple = false;  // one is purple already
  int others = 0;
  for (const Mover& o : s.movers)
    if (&o != &m && o.live && o.plow && !o.units.empty()) ++others, purple |= o.units[0].tint == kPurple;
  for (Unit& unit : m.units) unit.tint = !purple && others >= 4 ? kPurple : kOrange;  // the fifth on the streets
  static const bool debug = std::getenv("REGS_DEBUG_TRAFFIC_MAPS") != nullptr;
  if (debug)
    LOG_INFO("snowplows: plow %d out at (%.0f, %.0f), %zu nodes, %.0f m away; %d on the streets", id, a.x, a.z,
             path.size(), std::sqrt(best), s.plowsLive);
}

// ---- the plough's parts (model space: +Y up, the front towards −Z, +X to the right) ----

struct Builder {
  render::MeshData m;
  uint32_t firstIndex = 0;
  uint32_t vertex(const Vec3& p, const Vec3& n) {
    m.positions.insert(m.positions.end(), {p.x, p.y, p.z});
    m.normals.insert(m.normals.end(), {n.x, n.y, n.z});
    m.uv0.insert(m.uv0.end(), {0.0f, 0.0f});
    m.colors.push_back(0xFFFFFFFFu);
    return uint32_t(m.vertexCount() - 1);
  }
  // the face p + s·u + t·v (s, t in 0..1), facing cross(u, v)
  void quad(const Vec3& p, const Vec3& u, const Vec3& v) {
    Vec3 n = render::normalize(render::cross(u, v));
    uint32_t a = vertex(p, n), b = vertex(p + u, n), c = vertex(p + u + v, n), d = vertex(p + v, n);
    m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
  }
  void box(const Vec3& lo, const Vec3& hi) {
    Vec3 e = hi - lo, X{e.x, 0, 0}, Y{0, e.y, 0}, Z{0, 0, e.z};
    quad({hi.x, lo.y, lo.z}, Y, Z);
    quad(lo, Z, Y);
    quad({lo.x, hi.y, lo.z}, Z, X);
    quad(lo, X, Z);
    quad({lo.x, lo.y, hi.z}, X, Y);
    quad(lo, Y, X);
  }
  void part(int material) {
    render::Submesh sm;
    sm.firstVertex = 0;
    sm.vertexCount = uint32_t(m.vertexCount());
    sm.firstIndex = firstIndex;
    sm.indexCount = uint32_t(m.indices.size()) - firstIndex;
    sm.material = material;
    if (sm.indexCount) m.submeshes.push_back(sm);
    firstIndex = uint32_t(m.indices.size());
  }
};

struct PlowParts {
  Display* display = nullptr;
  const void* model = nullptr;
  render::GpuMesh mesh;
  std::vector<render::GpuMaterial> lit, dark;  // the beacon on and off
};

render::GpuMaterial plain(float r, float g, float b, uint32_t flags = 0) {
  render::GpuMaterial m;
  m.flags = flags;
  m.ambient[0] = r, m.ambient[1] = g, m.ambient[2] = b;
  m.diffuse[0] = r, m.diffuse[1] = g, m.diffuse[2] = b;
  m.surfaceClass = render::kSurfaceVehicle;
  return m;
}

// built once per car model from its bounds: an angled, curved blade (its left end ahead, so the snow goes to the
// right, to the kerb) with a steel cutting edge, two push arms into the bumper and an amber beacon on the roof
std::unique_ptr<PlowParts> buildParts(Display& d, const UnitModel& um) {
  auto parts = std::make_unique<PlowParts>();
  const Vec3 lo = um.boundsMin, hi = um.boundsMax;
  const float width = (hi.x - lo.x) + 0.45f, front = lo.z;
  const float zBlade = front - 0.55f, tanA = std::tan(0.42f);  // about 24°
  const float y0 = 0.06f, y1 = 0.82f;
  Builder b;
  const int nx = 10, nh = 8;
  auto at = [&](int i, int j) {
    float x = -width * 0.5f + width * float(i) / float(nx), h = float(j) / float(nh);
    float z = zBlade + tanA * x + 0.17f * std::sin(3.14159265f * h) - 0.14f * h * h;  // a moldboard, curling forwards
    return Vec3{x, y0 + (y1 - y0) * h, z};
  };
  std::vector<uint32_t> grid;
  for (int j = 0; j <= nh; ++j)
    for (int i = 0; i <= nx; ++i) {
      Vec3 dx = at(std::min(i + 1, nx), j) - at(std::max(i - 1, 0), j);
      Vec3 dh = at(i, std::min(j + 1, nh)) - at(i, std::max(j - 1, 0));
      grid.push_back(b.vertex(at(i, j), render::normalize(render::cross(dh, dx))));  // facing forwards
    }
  auto cells = [&](int j0, int j1) {
    for (int j = j0; j < j1; ++j)
      for (int i = 0; i < nx; ++i) {
        uint32_t a = grid[size_t(j * (nx + 1) + i)], c = grid[size_t(j * (nx + 1) + i + 1)];
        uint32_t e = grid[size_t((j + 1) * (nx + 1) + i + 1)], f = grid[size_t((j + 1) * (nx + 1) + i)];
        b.m.indices.insert(b.m.indices.end(), {a, c, e, a, e, f});
      }
  };
  cells(1, nh);
  b.part(0);  // the blade: orange, two-sided
  cells(0, 1);
  // the push arms: from behind the blade into the bumper, and a cross bar
  for (float x : {-0.5f, 0.5f}) {
    float z0 = zBlade + tanA * x + 0.12f;
    b.box({x - 0.05f, 0.3f, z0}, {x + 0.05f, 0.42f, front + 0.25f});
  }
  b.box({-0.55f, 0.32f, front - 0.02f}, {0.55f, 0.44f, front + 0.2f});
  b.part(1);  // steel
  // the beacon: a dark base and an amber bar across the roof, a little ahead of the middle
  float zr = lo.z + (hi.z - lo.z) * 0.42f;
  b.box({-0.32f, hi.y - 0.03f, zr - 0.09f}, {0.32f, hi.y + 0.03f, zr + 0.09f});
  b.part(1);
  b.box({-0.28f, hi.y + 0.03f, zr - 0.07f}, {0.28f, hi.y + 0.13f, zr + 0.07f});
  b.part(2);  // the lens
  float r = 0;
  for (float p : b.m.positions) r = std::max(r, std::fabs(p));
  b.m.radius = r;
  parts->mesh = d.r3d().upload(b.m);
  render::GpuMaterial blade = plain(0.92f, 0.46f, 0.05f, 0x1000 /* two-sided */);
  render::GpuMaterial steel = plain(0.16f, 0.16f, 0.17f);
  render::GpuMaterial lensOn = plain(1.0f, 0.62f, 0.12f), lensOff = plain(0.45f, 0.25f, 0.05f);
  lensOn.emission[0] = 1.0f, lensOn.emission[1] = 0.55f, lensOn.emission[2] = 0.08f;
  parts->lit = {blade, steel, lensOn};
  parts->dark = {blade, steel, lensOff};
  parts->display = &d;
  return parts;
}

}  // namespace

void stepSnowplows(script::VM& vm, GridObj* grid, SimState& s, float dt) {
  int want = 0;
  if (grid)
    if (auto f = grid->fieldValues.find(u"regsSnowplows"); f != grid->fieldValues.end()) want = int(vm.toFloat(vm.resolve(f->second)));
  // the road speed factors (gsGrid.regsRoadSpeed, regsRoadSpeedCleared; 1 when unset)
  auto factor = [&](const char16_t* name) {
    if (!grid) return 1.0f;
    auto f = grid->fieldValues.find(name);
    return f == grid->fieldValues.end() ? 1.0f : std::clamp(vm.toFloat(vm.resolve(f->second)), 0.3f, 1.0f);
  };
  s.roadSpeed = factor(u"regsRoadSpeed");
  s.roadSpeedCleared = factor(u"regsRoadSpeedCleared");
  if (const char* d = std::getenv("REGS_DEBUG_ROAD_SPEED")) {  // the factors and the mean speed of road traffic
    static float logClock = 0;
    if ((logClock += dt) > 10.0f && *d) {
      logClock = 0;
      float sum = 0, sumPlowed = 0;
      int n = 0, nPlowed = 0;
      for (const Mover& m : s.movers) {
        if (!m.live || m.parked || m.train || m.boat || m.aircraft || m.track.nodes.empty()) continue;
        int node = m.track.nodes[m.track.at(m.dist)];
        if (!(s.graph.nodes[size_t(node)].flags & (kPathRoad | kPathTram))) continue;
        bool cleared = s.trafficMaps && size_t(node) < s.trafficMaps->lanePlow.size() && s.trafficMaps->lanePlow[size_t(node)] > 0.5f;
        (cleared ? sumPlowed : sum) += m.speed;
        ++(cleared ? nPlowed : n);
      }
      LOG_INFO("road speed: factor %.2f, cleared %.2f; mean speed %.2f of %d vehicles, %.2f of %d on plowed lanes",
               s.roadSpeed, s.roadSpeedCleared, n ? sum / float(n) : 0.0f, n, nPlowed ? sumPlowed / float(nPlowed) : 0.0f, nPlowed);
    }
  }
  // test switch REGS_TEST_PLOWS=<n>: that many plows whenever the mod asks for any
  if (const char* t = std::getenv("REGS_TEST_PLOWS"); t && want > 0) want = std::atoi(t);
  static float clock = 0;
  clock += dt;
  if (want > s.plowsLive && clock >= 2.0f) {  // at most one new plow every 2 s
    clock = 0;
    spawnPlow(vm, s);
  }
  // debugging: REGS_DEBUG_FOLLOW_PLOW=<n> keeps the camera (ui.setCameraPos) on the n-th plow, =purple on the purple one
  if (const char* f = std::getenv("REGS_DEBUG_FOLLOW_PLOW")) {
    const bool purple = std::strcmp(f, "purple") == 0;
    int nth = purple ? 0 : std::atoi(f);
    for (const Mover& m : s.movers) {
      if (!m.live || !m.plow || m.units.empty() || (purple && m.units[0].tint != kPurple) || nth-- > 0) continue;
      Vec3 p = unitPose(s, m, m.units[0], m.dist).position;
      try {
        if (script::Value* main = vm.findGlobal(u"MAIN"))
          vm.call(vm.getMember(vm.getMember(*main, u"$gameUI"), u"setCameraPos"),
                  {script::Value::real(p.x), script::Value::real(p.z), script::Value::boolean(true)});
      } catch (...) {
      }
      break;
    }
  }
}

void collectPlowParts(Display& d, const UnitModel& model, const PlacedPose& p, double time,
                      std::vector<render::DrawItem>& items) {
  static std::vector<std::unique_ptr<PlowParts>> built;  // per car model
  PlowParts* parts = nullptr;
  for (auto& b : built)
    if (b->display == &d && b->model == model.object) parts = b.get();
  if (!parts) {
    built.push_back(buildParts(d, model));
    parts = built.back().get();
    parts->model = model.object;
  }
  // a double flash, 1.4 times a second
  double phase = std::fmod(time * 1.4, 1.0);
  bool on = phase < 0.1 || (phase > 0.2 && phase < 0.3);
  render::DrawItem it;
  it.mesh = &parts->mesh;
  it.materials = on ? &parts->lit : &parts->dark;
  it.world = poseMatrix(p);
  it.surfaceClass = render::kSurfaceVehicle;
  items.push_back(it);
}

void registerSnowplowNatives(script::VM& vm) {
  using namespace regs::script;
  if (ClassInfo* g = vm.findClass("gsGrid")) {
    FieldInfo& f = g->fields[u"regsSnowplows"];
    f.name = u"regsSnowplows";
    f.get = [](script::VM&, script::Object* o) {
      auto it = o->fieldValues.find(u"regsSnowplows");
      return it == o->fieldValues.end() ? script::Value::integer(0) : it->second;
    };
    f.set = [](script::VM& v, script::Object* o, const script::Value& x) { o->fieldValues[u"regsSnowplows"] = v.resolve(x); };
    for (const char16_t* name : {u"regsRoadSpeed", u"regsRoadSpeedCleared"}) {
      FieldInfo& r = g->fields[name];
      r.name = name;
      r.get = [key = std::u16string(name)](script::VM&, script::Object* o) {
        auto it = o->fieldValues.find(key);
        return it == o->fieldValues.end() ? script::Value::real(1.0f) : it->second;
      };
      r.set = [key = std::u16string(name)](script::VM& v, script::Object* o, const script::Value& x) {
        o->fieldValues[key] = script::Value::real(v.toFloat(v.resolve(x)));
      };
    }
  }
}

}  // namespace regs::engine
