// The sky dome and the sun disc (gsSky::generateBackground, gsSun::generateMesh), the port's day/night cycle and
// gsTracerCtrl.enableFade. Spec: docs/renderer/sky-and-night.md.
#include "engine/sky.h"
#include "core/env.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/gui.h"
#include "engine/scene3d.h"
#include "engine/settings.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

constexpr float kPi = 3.14159265f;

Value fieldOf(VM& vm, Object* o, std::u16string_view name) {
  if (!o) return Value();
  try {
    return vm.resolve(vm.getMember(Value::heap(VT::Object, o), name));
  } catch (const ScriptError&) {
    return Value();
  }
}

void setField(VM& vm, Object* o, const char16_t* name, const Value& v) {
  if (!o) return;
  try {
    vm.setMember(Value::heap(VT::Object, o), name, v);
  } catch (const ScriptError&) {
  }
}

Object* mainVar(VM& vm, const char16_t* name) {
  Value* main = vm.findGlobal(u"MAIN");
  Object* m = main ? main->object() : nullptr;
  Value* v = m ? m->findVar(name) : nullptr;
  return v ? vm.resolve(*v).object() : nullptr;
}

uint32_t intOr(VM& vm, const Value& v, uint32_t def) { return v.type() == VT::Int ? uint32_t(vm.toInt(v)) : def; }
float floatOr(VM& vm, const Value& v, float def) {
  return v.type() == VT::Float || v.type() == VT::Int ? vm.toFloat(v) : def;
}

Quat axisAngle(float angle, const Vec3& axis) {
  Vec3 a = normalize(axis);
  float s = std::sin(angle * 0.5f);
  return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
}

Quat qmul(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// gsSun::move (W:0x100964c0, scene-basics.md §4.1): the orientation for a fraction d of the day; the sun direction
// is rot(Q, +Z), the engine's rot being the conjugate rotation
Quat sunOrientation(float d, float latitude) {
  Quat q1 = axisAngle(2 * kPi * d + kPi, {0, 1, 0});
  float a = (latitude - kPi / 2) + 2 * (kPi / 2 - latitude) * (1 - 2 * std::fabs(0.5f - d)) + 0.40142575f;
  return qmul(axisAngle(a, {1, 0, 0}), q1);
}

float sunElevation(float d, float latitude) {
  Vec3 dir = rotate(conjugate(sunOrientation(d, latitude)), {0, 0, 1});
  return std::asin(std::clamp(dir.y, -1.0f, 1.0f));
}

float dayFraction(VM& vm, Object* sun) {
  Object* clock = fieldOf(vm, sun, u"clock").object();
  if (!clock) return 0.6f;  // no clock: d = 0.6 (W:0x100964c0.c:27-29)
  return float(std::fmod(double(vm.toInt(fieldOf(vm, clock, u"time"))) / 1000.0, 86400.0) / 86400.0);
}

// ---- meshes ----

// gsSky::generateBackground(n) (L:0x08125580): a hemisphere of n rings × 4n segments plus a pole, unit radius, the
// lowest ring pulled down by 0.5; normals −Y, uv (segment / 4n, 0.95 − 0.9·ring / n), colour (128 − 128·f, same,
// 255, 255). The sky shader only uses the positions (the cube-map direction).
MeshData domeMesh(int n) {
  MeshData m;
  int cols = 4 * n, stride = cols + 1;
  for (int r = 0; r < n; ++r) {
    float f = float(r) / float(n), phi = f * kPi / 2;
    float cp = std::cos(phi), sp = std::sin(phi);
    uint8_t c = uint8_t(128 - int(std::lround(f * 128.0f)));
    for (int s = 0; s <= cols; ++s) {
      float t = float(s) / float(cols) * 2 * kPi;
      m.positions.insert(m.positions.end(), {std::cos(t) * cp, sp - (r == 0 ? 0.5f : 0.0f), std::sin(t) * cp});
      m.normals.insert(m.normals.end(), {0, -1, 0});
      m.uv0.insert(m.uv0.end(), {float(s) / float(cols), 0.95f - f * 0.9f});
      m.colors.push_back(uint32_t(c) | uint32_t(c) << 8 | 0xFFu << 16 | 0xFFu << 24);
    }
  }
  uint32_t pole = uint32_t(n * stride);
  m.positions.insert(m.positions.end(), {0, 1, 0});
  m.normals.insert(m.normals.end(), {0, -1, 0});
  m.uv0.insert(m.uv0.end(), {0, 0.05f});
  m.colors.push_back(0xFF000000u | 0xFFu << 16);
  for (int r = 0; r + 1 < n; ++r)
    for (int s = 0; s < cols; ++s) {
      uint32_t a = uint32_t(r * stride + s), b = a + uint32_t(stride);
      m.indices.insert(m.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
    }
  for (int s = 0; s < cols; ++s) {
    uint32_t a = uint32_t((n - 1) * stride + s);
    m.indices.insert(m.indices.end(), {a, a + 1, pole});
  }
  m.submeshes.push_back({0, uint32_t(m.vertexCount()), 0, uint32_t(m.indices.size()), 0});
  return m;
}

// gsSun::generateMesh(n, size) (L:0x08125fc0): a disc of n segments at z = 1, rim (cos, −sin)·size, uv (cos, −sin)
// on the rim and (0.5, 0.5) at the centre, white
MeshData discMesh(int n, float size) {
  MeshData m;
  for (int i = 0; i <= n; ++i) {
    float t = float(i) / float(n) * 2 * kPi, c = std::cos(t), s = -std::sin(t);
    m.positions.insert(m.positions.end(), {c * size, s * size, 1});
    m.normals.insert(m.normals.end(), {0, -1, 0});
    m.uv0.insert(m.uv0.end(), {c, s});
    m.colors.push_back(0xFFFFFFFFu);
  }
  m.positions.insert(m.positions.end(), {0, 0, 1});
  m.normals.insert(m.normals.end(), {0, -1, 0});
  m.uv0.insert(m.uv0.end(), {0.5f, 0.5f});
  m.colors.push_back(0xFFFFFFFFu);
  uint32_t centre = uint32_t(n + 1);
  for (uint32_t i = 0; i < uint32_t(n); ++i) m.indices.insert(m.indices.end(), {i, i + 1, centre});
  m.submeshes.push_back({0, uint32_t(m.vertexCount()), 0, uint32_t(m.indices.size()), 0});
  return m;
}

// ---- gsSky ----

// The day/night cycle (port option $regsDayNight with $regsDayLength real minutes per day at normal speed, default
// 12; REGS_DEBUG_TIME=<hour> sets the time of day and holds it unless the cycle is on):
// the scene clock follows its own time of day and the light, fog and sky colours blend between a daylight and a night
// set. The location's own values are one end: daylight for the European locations, night for New York and Tokyo
// (whose fog is near black); the other end is a shipped preset, the New York night (addons/usa/locations/newyork/
// location.script, the same as Tokyo's) or the engine.script daylight defaults. Not an engine feature.
struct LightSet {
  uint32_t diffuse = 0, ambient = 0, specular = 0, fog = 0, sky = 0;
  float magnitude = 1;
};
constexpr LightSet kNightPreset{0xFF8080FF, 0xFF706080, 0xFFDDDDFF, 0x080808, 0x101010, 1.0f};
constexpr LightSet kDayPreset{0xFFFFF0D0, 0xFF202030, 0xFFFFFFDD, 0xCCC8BB, 0xFFFFFFFF, 2.5f};

struct DayNight {
  bool on = false;
  bool nightLocation = false;  // the location itself is a night scene (New York, Tokyo)
  double hours = 0;            // the time of day the cycle shows
  int32_t locationTime = 0;    // the scene clock's time as the location set it
  LightSet location, day, night;
  float night01 = 0;  // 0 by day, 1 at night (the emission factor follows it)
  int logged = -1;    // REGS_DEBUG_SKY: the last logged quarter hour
};

// self-illumination factor: at night ×2.5 on day locations; by day ×0.4 on night locations (their lit windows)
constexpr float kNightEmission = 2.5f, kDayEmissionOfNightLocation = 0.4f;

class SkyObj : public Object {
 public:
  using Object::Object;
  int rings = 0;  // generateMesh(n); 0 = no dome
  std::shared_ptr<GpuMesh> mesh;
  std::vector<GpuMaterial> material;  // the dome's material as last bound (programs kept between frames)
  DayNight dn;

  void tick(VM& vm, float dt) override {
    Object::tick(vm, dt);
    dayNight(vm, dt);
  }

  void dayNight(VM& vm, float dt);
};

struct Rgb {
  float r = 0, g = 0, b = 0;
};
Rgb rgbOf(uint32_t c, float m = 1) { return {float(c >> 16 & 255) / 255 * m, float(c >> 8 & 255) / 255 * m, float(c & 255) / 255 * m}; }
Rgb mix(const Rgb& a, const Rgb& b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Rgb mul(const Rgb& a, const Rgb& b) { return {a.r * b.r, a.g * b.g, a.b * b.b}; }
Rgb scale(const Rgb& a, float s) { return {a.r * s, a.g * s, a.b * s}; }
float maxOf(const Rgb& a) { return std::max({a.r, a.g, a.b}); }
uint32_t argbOf(const Rgb& c, float unit, uint32_t alpha) {
  auto ch = [unit](float v) { return uint32_t(std::clamp(int(std::lround(v / unit * 255.0f)), 0, 255)); };
  return alpha | ch(c.r) << 16 | ch(c.g) << 8 | ch(c.b);
}
float smooth(float a, float b, float x) {
  float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

void applyLights(VM& vm, Object* light, Object* sky, Object* mat, const LightSet& s) {
  setField(vm, light, u"diffuse", Value::integer(int32_t(s.diffuse)));
  setField(vm, light, u"ambient", Value::integer(int32_t(s.ambient)));
  setField(vm, light, u"specular", Value::integer(int32_t(s.specular)));
  setField(vm, light, u"magnitude", Value::real(s.magnitude));
  setField(vm, sky, u"fogColor", Value::integer(int32_t(s.fog)));
  if (mat) setField(vm, mat, u"diffuse", Value::integer(int32_t(s.sky)));
}

void SkyObj::dayNight(VM& vm, float dt) {
  if (mainVar(vm, u"sky") != this) return;  // the game's sky only (not the menu or object-list scenes)
  static const char* debugTime = regs::envVar("REGS_DEBUG_TIME");
  float minutes = settingNumber(vm, u"regsDayNight", 1) != 0 ? std::max(1.0f, settingNumber(vm, u"regsDayLength", 12)) : 0.0f;
  Object* sun = mainVar(vm, u"sun");
  Object* clock = mainVar(vm, u"sceneClock");
  Object* light = sun ? fieldOf(vm, sun, u"light").object() : nullptr;
  Object* mat = fieldOf(vm, this, u"material").object();
  if (!sun || !clock || !light) return;
  if (minutes <= 0 && !debugTime) {
    if (dn.on) {  // switched off: back to the location's values and time
      applyLights(vm, light, this, mat, dn.location);
      setField(vm, clock, u"time", Value::integer(int32_t(dn.locationTime)));
      dn.on = false;
      dn.night01 = 0;
    }
    return;
  }
  if (!dn.on) {  // the location's values, as its location script left them
    dn.on = true;
    LightSet& l = dn.location;
    l.diffuse = intOr(vm, fieldOf(vm, light, u"diffuse"), 0xFF999990);
    l.ambient = intOr(vm, fieldOf(vm, light, u"ambient"), 0xFF333333);
    l.specular = intOr(vm, fieldOf(vm, light, u"specular"), 0xFFFFFFE6);
    l.magnitude = floatOr(vm, fieldOf(vm, light, u"magnitude"), 2.0f);
    l.fog = intOr(vm, fieldOf(vm, this, u"fogColor"), 0xB3CCFF);
    l.sky = mat ? intOr(vm, fieldOf(vm, mat, u"diffuse"), 0xFFFFFFFF) : 0xFFFFFFFF;
    Rgb fog = rgbOf(l.fog);
    dn.nightLocation = 0.3f * fog.r + 0.59f * fog.g + 0.11f * fog.b < 0.15f;
    dn.day = dn.nightLocation ? kDayPreset : l;
    dn.night = dn.nightLocation ? l : kNightPreset;
    dn.locationTime = vm.toInt(fieldOf(vm, clock, u"time"));
    // a night location's sun stands for the moon: its scene matches midnight (the moon is half a day off the sun)
    dn.hours = debugTime ? std::atof(debugTime) : std::fmod(dn.locationTime / 3600000.0 + (dn.nightLocation ? 12 : 0), 24.0);
  }
  // the cycle runs with the game: the main clock's speed over its normal 10000 (0 while paused)
  if (minutes > 0) {
    Object* mainClock = mainVar(vm, u"mainClock");
    float speed = mainClock && !vm.truthy(fieldOf(vm, mainClock, u"paused"))
                      ? floatOr(vm, fieldOf(vm, mainClock, u"speed"), 0) / 10000.0f : 0.0f;
    dn.hours = std::fmod(dn.hours + double(dt) * speed * 24.0 / (double(minutes) * 60.0), 24.0);
    if (dn.hours < 0) dn.hours += 24;
  }
  float lat = floatOr(vm, fieldOf(vm, sun, u"latitude"), kPi / 4);
  float d = float(dn.hours / 24.0), e = sunElevation(d, lat);
  // weights: day 1 above 8° of sun, 0 below −8°; the warm tint peaks with the sun at the horizon
  float day = smooth(-0.14f, 0.14f, e);
  float dusk = smooth(-0.14f, 0.0f, e) * (1 - smooth(0.05f, 0.45f, e));
  dn.night01 = 1 - day;
  static const bool logSky = regs::envVar("REGS_DEBUG_SKY") != nullptr;
  if (logSky && int(dn.hours * 4) != dn.logged) {  // every quarter hour of the cycle
    dn.logged = int(dn.hours * 4);
    LOG_INFO("day/night: %02d:%02d, sun %.1f deg, day %.2f, dusk %.2f", int(dn.hours), int(dn.hours * 60) % 60,
             e * 180 / kPi, day, dusk);
  }
  // the light comes from the sun while it is up, else from a moon opposite it (the scene clock is turned by half a
  // day); its direct part fades out on both sides of the switch, so the jump is not seen
  bool sunUp = e > -0.04f;
  float direct = sunUp ? smooth(-0.04f, 0.10f, e) : smooth(0.04f, 0.18f, -e);
  double shown = sunUp ? dn.hours : std::fmod(dn.hours + 12.0, 24.0);
  setField(vm, clock, u"time", Value::integer(int32_t(shown * 3600000.0)));

  const LightSet &D = dn.day, &N = dn.night;
  const Rgb white{1, 1, 1}, warm{1.0f, 0.68f, 0.42f};
  Rgb dif = mix(rgbOf(N.diffuse, N.magnitude), rgbOf(D.diffuse, D.magnitude), day);
  dif = scale(mul(dif, mix(white, warm, dusk)), direct);
  Rgb spe = scale(mix(rgbOf(N.specular, N.magnitude), rgbOf(D.specular, D.magnitude), day), direct);
  Rgb amb = mix(rgbOf(N.ambient, N.magnitude), rgbOf(D.ambient, D.magnitude), day);
  amb = mul(amb, mix(white, Rgb{1.0f, 0.85f, 0.75f}, dusk));
  // sky light while the sun is low: the direct light on the ground falls with sin(elevation), so a part of the
  // daylight that the sun at 45° (the shipped locations' times have it at 40-50°) would give becomes ambient
  float lift = 0.45f * std::clamp(0.7071f - std::sin(e), 0.0f, 0.7071f) * day;
  Rgb sunCol = mul(rgbOf(D.diffuse, D.magnitude), mix(white, warm, 0.5f * dusk));
  amb = {amb.r + sunCol.r * lift, amb.g + sunCol.g * lift, amb.b + sunCol.b * lift};
  float unit = std::max({maxOf(dif), maxOf(spe), maxOf(amb), 1.0f / 255});
  LightSet out;
  out.diffuse = argbOf(dif, unit, 0xFF000000u);
  out.ambient = argbOf(amb, unit, 0xFF000000u);
  out.specular = argbOf(spe, unit, 0xFF000000u);
  out.magnitude = unit;
  Rgb fog = mix(rgbOf(N.fog), rgbOf(D.fog), day);
  out.fog = argbOf(mix(fog, mul(rgbOf(D.fog), Rgb{0.95f, 0.62f, 0.45f}), dusk * 0.7f), 1, 0);
  out.sky = argbOf(mul(mix(rgbOf(N.sky), rgbOf(D.sky), day), mix(white, warm, dusk)), 1, dn.location.sky & 0xFF000000u);
  applyLights(vm, light, this, mat, out);
}

// ---- gsSun disc ----

struct SunDisc {
  std::shared_ptr<GpuMesh> mesh;
  std::vector<GpuMaterial> material;
};

}  // namespace

void addSkyItems(Display& d, VM& vm, Object* skyObj, const std::vector<Object*>& suns, const Vec3& eye, float farDistance,
                 std::vector<DrawItem>& items) {
  // gsSky: dome at the eye, scale 0.99 (gsSky::move, L:0x081251f0) × far distance (mesh flag 0x400)
  if (auto* sky = dynamic_cast<SkyObj*>(skyObj); sky && sky->visible() && sky->rings > 0) {
    if (!sky->mesh) sky->mesh = std::make_shared<GpuMesh>(d.r3d().upload(domeMesh(sky->rings)));
    GpuMaterial g = scriptMaterial(d, vm, fieldOf(vm, sky, u"material").object());
    g.flags |= kMatNoShadow;
    if (!sky->material.empty() && sky->material[0].shader == g.shader && sky->material[0].flags == g.flags)
      std::copy(std::begin(sky->material[0].variants), std::end(sky->material[0].variants), std::begin(g.variants));
    sky->material.assign(1, g);
    DrawItem it;
    it.mesh = sky->mesh.get();
    it.materials = &sky->material;
    it.world = translation(eye) * scaling(0.99f * farDistance);
    it.surfaceClass = kSurfaceOverlay;  // not a surface for the mods' shader hooks
    items.push_back(it);
  }
  // gsSun: disc oriented as the sun, scale 0.94 (gsSun::move, W:0x100964c0.c:56) × far distance
  static std::map<std::pair<int, float>, SunDisc> discs;
  for (Object* sun : suns) {
    auto seg = sun->fieldValues.find(u"<meshSegments>");
    if (seg == sun->fieldValues.end()) continue;
    int n = std::max(3, vm.toInt(seg->second));
    float size = vm.toFloat(sun->fieldValues[u"<meshSize>"]);
    SunDisc& disc = discs[{n, size}];
    if (!disc.mesh) {
      disc.mesh = std::make_shared<GpuMesh>(d.r3d().upload(discMesh(n, size)));
      // the material: sun.dds, sun.shader, MATERIAL_GLOW, black colours, illumination (255, 255, 224) × 30
      GpuMaterial g;
      g.flags = 0x20 | kMatNoShadow;
      for (float* c : {g.ambient, g.diffuse, g.specular}) c[0] = c[1] = c[2] = 0, c[3] = 1;
      g.emission[0] = g.emission[1] = 30.0f;
      g.emission[2] = 224.0f / 255.0f * 30.0f;
      g.shader = "effects/sky/sun.shader";
      auto* t = dynamic_cast<TextureObj*>(getTexture(vm, u"effects/sky/sun.dds").get());
      g.textures.push_back(t ? d.texture(vm, t) : 0);
      g.cube.push_back(t && t->cube ? 1 : 0);
      disc.material.assign(1, g);
    }
    float lat = floatOr(vm, fieldOf(vm, sun, u"latitude"), kPi / 4);
    Quat q = sunOrientation(dayFraction(vm, sun), lat);
    DrawItem it;
    it.mesh = disc.mesh.get();
    it.materials = &disc.material;
    it.world = translation(eye) * rotation(conjugate(q)) * scaling(0.94f * farDistance);
    it.surfaceClass = kSurfaceOverlay;
    items.push_back(it);
  }
}

float skyEmissionScale(Object* skyObj) {
  auto* sky = dynamic_cast<SkyObj*>(skyObj);
  if (!sky || !sky->dn.on) return 1.0f;
  const DayNight& dn = sky->dn;
  return dn.nightLocation ? kDayEmissionOfNightLocation + (1 - kDayEmissionOfNightLocation) * dn.night01
                          : 1 + (kNightEmission - 1) * dn.night01;
}

NightState skyNightState(Object* skyObj) {
  NightState s;
  auto* sky = dynamic_cast<SkyObj*>(skyObj);
  if (!sky || !sky->dn.on) return s;
  s.on = true;
  s.night01 = sky->dn.night01;
  s.hours = sky->dn.hours;
  return s;
}

void tracerFade(VM& vm, ControlObj* tracer, const Vec3& eye, const Vec3& target) {
  auto it = tracer->fieldValues.find(u"<fade>");
  if (it == tracer->fieldValues.end() || it->second.type() != VT::Quat) return;
  const float* f = it->second.vec();
  Vec3 dv = target - eye;
  float dist = std::sqrt(dot(dv, dv)), v;
  if (dist <= f[0] || dist >= f[3]) v = 0;
  else if (dist < f[1]) v = 1 - (f[1] - dist) / (f[1] - f[0]);
  else if (dist <= f[2]) v = 1;
  else v = 1 - (dist - f[2]) / (f[3] - f[2]);
  tracer->visibility = v;  // gsControl::setVisibility (vtable 0x174, L:0x081c7830)
  tracer->shadowPushed = true;
}

void registerSkyNatives(VM& vm) {
  if (ClassInfo* c = vm.findClass("gsSky")) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new SkyObj(ci)); };
  // generateMesh(n) (W:0x10097720 → gsSky::generateBackground): the dome mesh and its material: envmap.dds as
  // texture 0, effects/sky/sky.shader, ambient black, diffuse white, specular black, illumination 0x202020 × 0
  implement(vm, "gsSky", u"generateMesh", [](VM& v, Object* s, int, const Value* a) {
    auto* sky = dynamic_cast<SkyObj*>(s);
    if (!sky) return Value();
    sky->rings = std::max(1, v.toInt(a[0]));
    sky->mesh.reset();
    if (Object* m = fieldOf(v, sky, u"material").object()) {
      m->fieldValues[u"<texture0>"] = Value::string(u"textures/envmap.dds");
      m->fieldValues[u"<shader>"] = Value::string(u"effects/sky/sky.shader");
      m->fieldValues[u"ambient"] = Value::integer(int32_t(0xFF000000u));
      m->fieldValues[u"diffuse"] = Value::integer(int32_t(0xFFFFFFFFu));
      m->fieldValues[u"specular"] = Value::integer(int32_t(0xFF000000u));
      m->fieldValues[u"illumination"] = Value::integer(int32_t(0xFF202020u));
    }
    return Value();
  });
  // gsSun.generateMesh(n, size) (L:0x08125fc0): sets the direction vector (+Z; scene3d's sunLight checks <mesh>) and
  // builds the disc
  implement(vm, "gsSun", u"generateMesh", [](VM& v, Object* s, int argc, const Value* a) {
    s->fieldValues[u"<mesh>"] = Value::boolean(true);
    s->fieldValues[u"<meshSegments>"] = Value::integer(v.toInt(a[0]));
    s->fieldValues[u"<meshSize>"] = Value::real(argc > 1 ? v.toFloat(a[1]) : 0.0f);
    return Value();
  });
  // enableFade(a, b, c, d) (L:0x081ce870): +0x1c9 = 1, the four distances at +0x1cc..+0x1d8 (defaults 0, 100, 10000,
  // 11000 while disabled)
  implement(vm, "gsTracerCtrl", u"enableFade", [](VM& v, Object* s, int, const Value* a) {
    s->fieldValues[u"<fade>"] = Value::quat(v.toFloat(a[0]), v.toFloat(a[1]), v.toFloat(a[2]), v.toFloat(a[3]));
    return Value();
  });
}

}  // namespace regs::engine
