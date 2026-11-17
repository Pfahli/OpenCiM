// 3D scene objects and the viewport's 3D pass for what the main menu needs: .object meshes with their materials,
// colorize, a fixed camera, the sun's light, render-to-texture. Conventions: docs/renderer/scene-basics.md.
#include "engine/scene3d.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/effects.h"
#include "engine/grid.h"
#include "engine/gui.h"
#include "engine/night_lights.h"
#include "engine/traffic_lights.h"
#include "engine/particles.h"
#include "engine/settings.h"
#include "engine/shaders.h"
#include "engine/sim.h"
#include "engine/skinning.h"
#include "engine/sky.h"
#include "engine/terrain.h"
#include "engine/traffic_maps.h"
#include "render/glsl.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

Value fieldOf(VM& vm, Object* o, const char16_t* name) {
  try {
    return vm.resolve(vm.getMember(Value::heap(VT::Object, o), name));
  } catch (const ScriptError&) {
    return Value();
  }
}

Vec3 vec3Of(const Value& v, Vec3 def = {}) {
  if (v.type() != VT::Vector) return def;
  return {v.vec()[0], v.vec()[1], v.vec()[2]};
}

Quat quatOf(const Value& v) {
  if (v.type() != VT::Quat) return {};
  return {v.vec()[0], v.vec()[1], v.vec()[2], v.vec()[3]};
}

void argbToFloats(uint32_t c, float out[4]) {
  out[0] = float(c >> 16 & 0xFF) / 255;
  out[1] = float(c >> 8 & 0xFF) / 255;
  out[2] = float(c & 0xFF) / 255;
  out[3] = float(c >> 24) / 255;
}

std::map<std::string, std::shared_ptr<const ObjectData>>& objectCache() {
  static std::map<std::string, std::shared_ptr<const ObjectData>> cache;
  return cache;
}

// The engine rotates with rot(q, v) = q* ⊗ v ⊗ q, i.e. v·M(q) with row vectors: the conjugate of the textbook
// rotation (scene-basics.md §1).
Vec3 rotEngine(const Quat& q, const Vec3& v) { return rotate(conjugate(q), v); }

Quat qmul(const Quat& a, const Quat& b) {  // Hamilton a ⊗ b
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat axisAngle(float angle, const Vec3& axis) {  // script quat(angle, axis)
  Vec3 a = normalize(axis);
  float s = std::sin(angle * 0.5f);
  return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
}

float floatOr(VM& vm, const Value& v, float def) {
  return v.type() == VT::Float || v.type() == VT::Int ? vm.toFloat(v) : def;
}

// world = S(scale)·M(q)·T(p) in row vectors (scene-basics.md §3); scene children have an identity parent
Mat4 worldOf(VM& vm, Object* o) {
  Vec3 p = vec3Of(fieldOf(vm, o, u"position"));
  Quat q = quatOf(fieldOf(vm, o, u"orientation"));
  float sc = floatOr(vm, fieldOf(vm, o, u"scale"), 1.0f);
  if (sc == 0) sc = 1;
  return translation(p) * rotation(conjugate(q)) * scaling(sc);
}

bool isFloatingCamera(VM& vm, Object* cam) { return cam->cls->isA(vm.findClass("gsFloatingCamera")); }

Quat slerp(Quat a, Quat b, float t) {  // shortest arc, a at t = 0
  float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (d < 0) {
    b = {-b.x, -b.y, -b.z, -b.w};
    d = -d;
  }
  float wa = 1 - t, wb = t;
  if (d < 0.9995f) {
    float th = std::acos(d), s = std::sin(th);
    wa = std::sin((1 - t) * th) / s;
    wb = std::sin(t * th) / s;
  }
  Quat q{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
  float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return n > 0 ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : a;
}

// gsFloatingCamera::updateFinalOrient (game-load.md §6.4): the view eases towards orientation, offset, min/max
// distance and (smoothPosition) position, keeping pow(smoothness, dt) of the old value; snap = reset(). The eased
// pose is kept in hidden fields, the eased position in the script field currentPosition.
void updateFloatingCamera(VM& vm, Object* cam, float dt, bool snap) {
  Quat q = quatOf(fieldOf(vm, cam, u"orientation"));
  Vec3 p = vec3Of(fieldOf(vm, cam, u"position")), off = vec3Of(fieldOf(vm, cam, u"offset"));
  float mn = floatOr(vm, fieldOf(vm, cam, u"minDistance"), -1), mx = floatOr(vm, fieldOf(vm, cam, u"maxDistance"), -1);
  auto& fv = cam->fieldValues;
  bool smoothPos = vm.truthy(fieldOf(vm, cam, u"smoothPosition"));
  if (snap || !fv.count(u"<orient>")) {
    fv[u"<orient>"] = Value::quat(q.x, q.y, q.z, q.w);
    fv[u"<offset>"] = Value::vector(off.x, off.y, off.z);
    fv[u"<minDistance>"] = Value::real(mn);
    fv[u"<maxDistance>"] = Value::real(mx);
    fv[u"currentPosition"] = Value::vector(p.x, p.y, p.z);
    return;
  }
  auto it = fv.find(u"smoothness");
  float f = std::pow(it == fv.end() ? 0.1f : vm.toFloat(it->second), std::max(dt, 0.0f));
  auto lerp = [f](float target, float old) { return (1 - f) * target + f * old; };
  if (!vm.truthy(fieldOf(vm, cam, u"lockOrient"))) {
    Quat cur = quatOf(fv[u"<orient>"]);
    Quat s = slerp(q, cur, f);
    fv[u"<orient>"] = Value::quat(s.x, s.y, s.z, s.w);
  }
  if (smoothPos) {
    Vec3 c = vec3Of(fv[u"currentPosition"]);
    fv[u"currentPosition"] = Value::vector(lerp(p.x, c.x), lerp(p.y, c.y), lerp(p.z, c.z));
  }
  Vec3 o = vec3Of(fv[u"<offset>"]);
  fv[u"<offset>"] = Value::vector(lerp(off.x, o.x), lerp(off.y, o.y), lerp(off.z, o.z));
  fv[u"<minDistance>"] = Value::real(lerp(mn, vm.toFloat(fv[u"<minDistance>"])));
  fv[u"<maxDistance>"] = Value::real(lerp(mx, vm.toFloat(fv[u"<maxDistance>"])));
}

// gs3DCamera::updateFinalOrient + gsCamera::updateCamera (scene-basics.md §2.1) for scene-owned cameras; floating
// cameras use their eased pose
void cameraView(VM& vm, Object* cam, Vec3& eye, Vec3& look, Vec3& up) {
  bool floating = isFloatingCamera(vm, cam) && cam->fieldValues.count(u"<orient>");
  Quat q = quatOf(floating ? cam->fieldValues[u"<orient>"] : fieldOf(vm, cam, u"orientation"));
  if (vm.truthy(fieldOf(vm, cam, u"lookback"))) q = qmul(axisAngle(3.14159265f, {0, 1, 0}), q);
  bool eased = floating && vm.truthy(fieldOf(vm, cam, u"smoothPosition"));
  Vec3 p = vec3Of(fieldOf(vm, cam, eased ? u"currentPosition" : u"position"));
  if (Object* t = fieldOf(vm, cam, u"target").object()) p = vec3Of(fieldOf(vm, t, u"position"));
  Vec3 off = vec3Of(floating ? cam->fieldValues[u"<offset>"] : fieldOf(vm, cam, u"offset"));
  eye = p + rotEngine(q, off);
  look = p + rotEngine(q, Vec3{off.x, off.y, std::min(off.z - 1.0f, 0.0f)});
  up = rotEngine(q, Vec3{0, 1, 0});
  // screenshots: REGS_DEBUG_TOPDOWN=x,z,height puts the floating camera straight above world (x, z), north up; a
  // fourth number d looks at it from d units south instead (at the ground height y, a fifth number; a sixth turns the
  // eye that many degrees from the south toward the east)
  if (floating)
    if (const char* td = std::getenv("REGS_DEBUG_TOPDOWN"); td && *td) {
      float x = 0, z = 0, h = 200, d = 0, y = 0, az = 0;
      int n = std::sscanf(td, "%f,%f,%f,%f,%f,%f", &x, &z, &h, &d, &y, &az);
      const float ar = az * 0.017453293f;
      if (n >= 4 && d != 0) eye = {x + d * std::sin(ar), y + h, z + d * std::cos(ar)}, look = {x, y, z}, up = {0, 1, 0};
      else if (n >= 2) eye = {x, h, z}, look = {x, 0, z}, up = {0, 0, -1};
    }
}

// gsSun::move (0x100964c0, scene-basics.md §4): direction from the clock's time of day, light 20000 units away;
// colours are ARGB · magnitude / 255, unclamped
bool sunLight(VM& vm, Object* sun, Light& l) {
  Object* light = fieldOf(vm, sun, u"light").object();
  if (!light) return false;
  float mag = floatOr(vm, fieldOf(vm, light, u"magnitude"), 2.0f);
  auto col = [&](const char16_t* n, float out[4], uint32_t def) {
    Value c = fieldOf(vm, light, n);
    uint32_t v = c.type() == VT::Int ? uint32_t(vm.toInt(c)) : def;
    out[0] = float(v >> 16 & 0xFF) * mag / 255;
    out[1] = float(v >> 8 & 0xFF) * mag / 255;
    out[2] = float(v & 0xFF) * mag / 255;
    out[3] = float(v >> 24) / 255;
  };
  col(u"diffuse", l.diffuse, 0xFF999990);
  col(u"ambient", l.ambient, 0xFF333333);
  col(u"specular", l.specular, 0xFFFFFFE6);
  const float pi = 3.14159265f;
  double d = 0.6;
  if (Object* clock = fieldOf(vm, sun, u"clock").object())
    d = std::fmod(double(vm.toInt(fieldOf(vm, clock, u"time"))) / 1000.0, 86400.0) / 86400.0;
  float lat = floatOr(vm, fieldOf(vm, sun, u"latitude"), pi / 4);
  Quat q1 = axisAngle(float(2 * pi * d + pi), {0, 1, 0});
  float a = (lat - pi / 2) + 2 * (pi / 2 - lat) * float(1 - 2 * std::fabs(0.5 - d)) + 0.40142575f;
  Quat q = qmul(axisAngle(a, {1, 0, 0}), q1);
  bool mesh = sun->fieldValues.count(u"<mesh>") != 0;  // the direction vector is set by generateMesh
  l.position = rotEngine(q, mesh ? Vec3{0, 0, 1} : Vec3{0, 0, 0}) * 20000.0f;
  return true;
}

GpuMaterial gpuMaterial(Display& d, VM& vm, const MaterialDef& m) {
  // gsMaterial_OGL from the .material header (scene-basics.md §4.3)
  GpuMaterial g;
  g.flags = m.flags;
  for (int k = 0; k < 4; ++k) {
    g.ambient[k] = m.ambient[k];
    g.diffuse[k] = m.diffuse[k];
    g.specular[k] = m.specular[k];
    g.emission[k] = m.emissive[k];
  }
  if (m.opacity != 255) g.diffuse[3] *= m.opacity / 256.0f;
  for (int k = 0; k < 3; ++k) {
    // with a specular map the colour is white × level >> 8, else the header colour × level / 256
    g.specular[k] = (m.flags & kMatSpecularMap) ? float((255 * m.specularLevel) >> 8) / 255.0f : g.specular[k] * m.specularLevel / 256.0f;
    g.emission[k] *= m.selfIllumination * 0.1f;
  }
  g.shininess = std::max(1.0f, float(m.glossiness));
  g.reflection = m.reflection / 255.0f;
  for (const auto& path : m.textures) {  // unit order, the environment cube last
    auto* t = dynamic_cast<TextureObj*>(getTexture(vm, utf8ToUtf16(path)).get());
    uint32_t gl = t ? d.texture(vm, t) : 0;
    g.textures.push_back(gl);
    g.cube.push_back(t && t->cube ? 1 : 0);
    if ((m.flags & kMatReflection) && path == "textures/envmap.dds" && t && t->cube) g.envCube = gl;
  }
  return g;
}

// the variant of a combiner (empty path: the default shader) for (material flags, mesh flags, environment flags),
// compiled on first use
uint32_t programFor(Display& d, VM& vm, const std::string& shader, uint32_t flags, uint32_t env, uint32_t meshFlags = 0) {
  static const bool off = std::getenv("REGS_NO_COMBINER") != nullptr;  // debugging: the built-in shader only
  if (off) return 0;
  ShaderCombiner* c = getCombiner(vm, utf8ToUtf16(shader));
  if (!c) return 0;
  ShaderVariant* v = combinerVariant(vm, c, flags, meshFlags, env);
  if (!v || v->failed) return 0;
  if (!v->program) {
    std::string err;
    v->program = d.r3d().buildProgram(v->vertex, v->fragment, &err, true);  // with the mods' shader hooks
    if (!v->program) {
      v->failed = true;
      LOG_WARN("shader variant %s 0x%x/0x%x: %s", shader.empty() ? "default" : shader.c_str(), flags, env, err.c_str());
    }
  }
  return v->program;
}

// makes sure every material of the draw list has its combiner variant for a pass environment
void assignPrograms(Display& d, VM& vm, std::vector<DrawItem>& items, uint32_t env) {
  std::vector<const std::vector<GpuMaterial>*> seen;  // grid divisions share one material table
  for (DrawItem& it : items)
    if (auto* mats = const_cast<std::vector<GpuMaterial>*>(it.materials);
        mats && std::find(seen.begin(), seen.end(), mats) == seen.end() && (seen.push_back(mats), true))
      for (GpuMaterial& m : *mats)
        if (!m.hasProgramFor(env)) m.setProgram(env, programFor(d, vm, m.shader, m.flags, env, m.meshFlags));
}

// collects the drawable objects, suns, grids and the sky (fog) of a scene subtree
void collect(Display& d, VM& vm, Object* node, std::vector<DrawItem>& items, std::vector<Object*>& suns,
             std::vector<GridObj*>& grids, Object*& sky) {
  for (auto& k : node->children) {
    Object* c = k.get();
    if (c->destroyed) continue;
    if (c->cls->isA(vm.findClass("gsSky")) && c->enabled()) sky = c;  // fog applies even when the dome is hidden
    if (!c->visible()) continue;
    if (auto* g = dynamic_cast<GridObj*>(c)) grids.push_back(g);
    if (auto* o = dynamic_cast<ObjectObj*>(c); o && o->data && isSkinnedModel(vm, o)) {
      // a skinned model (vehicle templates in the vehicle shop's preview scene): the skinning variant, posed as a line
      // vehicle with closed doors (its first animation at 0, as sim.cpp draws them); the bind pose has some trams' doors
      // elsewhere (owner's report 2026-11-07: odd tram models in the shop)
      size_t first = items.size();
      collectSkinned(d, vm, o, worldOf(vm, o), poseFirstAnimation(vm, o, 0.0f), items);
      for (size_t i = first; i < items.size(); ++i) {
        argbToFloats(o->colorizeArgb, items[i].colorize);
        items[i].surfaceClass = kSurfaceOverlay;  // scene objects (previews, markers, cursors): not a surface
      }
    } else if (o && o->data) {
      if (!o->gpu) {
        o->gpu = std::make_shared<GpuMesh>(d.r3d().upload(o->data->mesh));
        o->materials = std::make_shared<std::vector<GpuMaterial>>();
        for (const auto& m : o->data->materials) o->materials->push_back(objectMaterial(d, vm, o, m));
      }
      DrawItem it;
      it.mesh = o->gpu.get();
      it.materials = o->materials.get();
      it.world = worldOf(vm, o);
      argbToFloats(o->colorizeArgb, it.colorize);
      // scene objects in a map view are tool previews, selection markers and cursors: the mods' shader hooks leave
      // them alone (the map's surfaces are the grid's and the simulation's)
      it.surfaceClass = kSurfaceOverlay;
      items.push_back(it);
    }
    if (c->cls->isA(vm.findClass("gsSun"))) suns.push_back(c);
    collect(d, vm, c, items, suns, grids, sky);
  }
}

// Port atmosphere fields of gsScene (port/docs/mod-manager.md §4.2, for mods such as weather): unset = no change.
//   regsLightScale     × the lights' diffuse and specular (direct light)
//   regsAmbientScale   × the lights' ambient and the light-model ambient
//   regsLightDesaturate  0..1: the light colours mixed towards their grey
//   regsFogTint        ARGB: the fog colour mixed towards its RGB by alpha / 255
//   regsFogScale       × the fog start and end distances
//   regsSurfaceDiffuse × every material's ambient and diffuse colour (darker surfaces)
//   regsSurfaceSpecular × every material's specular colour (only materials with MATERIAL_SPECULAR show it)
//   regsLightTint      ARGB: the direct light (diffuse, specular) × its RGB / 255, by alpha / 255 (warm or cool light)
//   regsPostHooks      > 0: the mods' post hooks run on this scene's map views (a full-screen pass; render/glsl.h)
struct Atmosphere {
  float light = 1, ambient = 1, desaturate = 0, fogScale = 1, surfaceDiffuse = 1, surfaceSpecular = 1;
  uint32_t fogTint = 0, lightTint = 0;
};
const char16_t* const kAtmosphereFloats[] = {u"regsLightScale", u"regsAmbientScale", u"regsLightDesaturate",
                                             u"regsFogScale", u"regsSurfaceDiffuse", u"regsSurfaceSpecular"};

Atmosphere atmosphereOf(VM& vm, Object* scene) {
  Atmosphere a;
  if (!scene) return a;
  auto num = [&](const char16_t* n, float def) {
    auto it = scene->fieldValues.find(n);
    return it == scene->fieldValues.end() ? def : floatOr(vm, it->second, def);
  };
  a.light = std::max(0.0f, num(u"regsLightScale", 1));
  a.ambient = std::max(0.0f, num(u"regsAmbientScale", 1));
  a.desaturate = std::clamp(num(u"regsLightDesaturate", 0), 0.0f, 1.0f);
  a.fogScale = std::max(0.01f, num(u"regsFogScale", 1));
  a.surfaceDiffuse = std::max(0.0f, num(u"regsSurfaceDiffuse", 1));
  a.surfaceSpecular = std::max(0.0f, num(u"regsSurfaceSpecular", 1));
  if (auto it = scene->fieldValues.find(u"regsFogTint"); it != scene->fieldValues.end() && it->second.type() == VT::Int)
    a.fogTint = uint32_t(vm.toInt(it->second));
  if (auto it = scene->fieldValues.find(u"regsLightTint"); it != scene->fieldValues.end() && it->second.type() == VT::Int)
    a.lightTint = uint32_t(vm.toInt(it->second));
  return a;
}

void applyAtmosphere(const Atmosphere& a, SceneView& v) {
  auto grade = [&](float* c, float scale) {
    float grey = 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
    for (int k = 0; k < 3; ++k) c[k] = (c[k] + (grey - c[k]) * a.desaturate) * scale;
  };
  for (Light& l : v.lights) {
    grade(l.diffuse, a.light);
    grade(l.specular, a.light);
    grade(l.ambient, a.ambient);
    if (a.lightTint >> 24) {
      float t = float(a.lightTint >> 24) / 255.0f, tint[4];
      argbToFloats(a.lightTint, tint);
      for (int k = 0; k < 3; ++k) {
        l.diffuse[k] *= 1.0f + (tint[k] - 1.0f) * t;
        l.specular[k] *= 1.0f + (tint[k] - 1.0f) * t;
      }
    }
  }
  grade(v.lightModelAmbient, a.ambient);
  if (a.fogTint >> 24) {
    float t = float(a.fogTint >> 24) / 255.0f, tint[4];
    argbToFloats(a.fogTint, tint);
    // the tint is a daylight colour: at night (a dark fog) it is as much darker, so an overcast night stays dark
    const float fogLum = 0.3f * v.fogColor[0] + 0.59f * v.fogColor[1] + 0.11f * v.fogColor[2];
    const float dark = std::clamp(fogLum / 0.6f, 0.0f, 1.0f);
    for (int k = 0; k < 3; ++k) v.fogColor[k] += (tint[k] * dark - v.fogColor[k]) * t;
  }
  v.fogStart *= a.fogScale;
  v.fogEnd *= a.fogScale;
  v.surfaceDiffuse = a.surfaceDiffuse;
  v.surfaceSpecular = a.surfaceSpecular;
}

}  // namespace

std::shared_ptr<const ObjectData> loadObjectCached(VM& vm, const std::string& path) {
  auto& cache = objectCache();
  auto it = cache.find(path);
  if (it != cache.end()) return it->second;
  auto data = std::make_shared<ObjectData>();
  std::string err;
  if (!loadObject(path, [&](const std::string& p, std::vector<uint8_t>& out) { return vm.vfs().read(p, out); }, *data, &err)) {
    LOG_WARN("object %s: %s", path.c_str(), err.c_str());
    cache.emplace(path, nullptr);  // remembered as missing
    return nullptr;
  }
  return cache.emplace(path, data).first->second;
}

GpuMaterial objectMaterial(Display& d, VM& vm, Object* object, const MaterialDef& m) {
  GpuMaterial g = gpuMaterial(d, vm, m);
  if (!object) return g;
  auto it = object->fieldValues.find(u"<material:" + utf8ToUtf16(m.name) + u">");
  Object* v = it == object->fieldValues.end() ? nullptr : it->second.object();
  if (!v) return g;
  if (auto f = v->fieldValues.find(u"flags"); f != v->fieldValues.end()) g.flags = uint32_t(vm.toInt(f->second));
  if (auto sh = v->fieldValues.find(u"<shader>"); sh != v->fieldValues.end()) g.shader = utf16ToUtf8(sh->second.toString());
  for (int i = 0; i < 8; ++i)
    if (auto p = v->fieldValues.find(u"<param" + utf8ToUtf16(std::to_string(i)) + u">"); p != v->fieldValues.end())
      for (int k = 0; k < 4; ++k) g.custom[i][k] = p->second.vec()[k];
  return g;
}

GpuMaterial scriptMaterial(Display& d, VM& vm, Object* variant) {
  // a gsMaterialVariant: the script-visible colours and flags over the gsMaterial_OGL defaults
  // (L:0x08214280: ambient/diffuse 0xFF808080, specular/illumination 0xFF000000)
  GpuMaterial g;
  if (!variant) {
    argbToFloats(0xFF808080, g.ambient);
    argbToFloats(0xFF808080, g.diffuse);
    return g;
  }
  auto num = [&](const char16_t* n, uint32_t def) {
    auto it = variant->fieldValues.find(n);
    return it == variant->fieldValues.end() ? def : uint32_t(vm.toInt(it->second));
  };
  auto real = [&](const char16_t* n) {
    auto it = variant->fieldValues.find(n);
    return it == variant->fieldValues.end() ? 0.0f : vm.toFloat(it->second);
  };
  g.flags = num(u"flags", 0);
  argbToFloats(num(u"ambient", 0xFF808080), g.ambient);
  argbToFloats(num(u"diffuse", 0xFF808080), g.diffuse);
  argbToFloats(num(u"specular", 0xFF000000), g.specular);
  argbToFloats(num(u"illumination", 0xFF000000), g.emission);
  g.shininess = real(u"glossiness");
  g.reflection = real(u"reflection");
  g.depthOffset = real(u"depthOffset");
  if (auto it = variant->fieldValues.find(u"<shader>"); it != variant->fieldValues.end()) g.shader = utf16ToUtf8(it->second.toString());
  for (int i = 0; i < 7; ++i) {
    auto it = variant->fieldValues.find(u"<texture" + utf8ToUtf16(std::to_string(i)) + u">");
    if (it == variant->fieldValues.end()) continue;
    auto* t = dynamic_cast<TextureObj*>(getTexture(vm, it->second.toString()).get());
    g.textures.resize(size_t(i) + 1, 0);
    g.cube.resize(size_t(i) + 1, 0);
    g.textures[size_t(i)] = t ? d.texture(vm, t) : 0;
    g.cube[size_t(i)] = t && t->cube ? 1 : 0;
    if (t && t->cube) g.envCube = g.textures[size_t(i)];
  }
  for (int i = 0; i < 8; ++i)
    if (auto it = variant->fieldValues.find(u"<param" + utf8ToUtf16(std::to_string(i)) + u">"); it != variant->fieldValues.end())
      for (int k = 0; k < 4; ++k) g.custom[i][k] = it->second.vec()[k];
  return g;
}

// seconds per frame phase (load-time measurements, Display::renderFrame logs them for slow frames): 1 scene nodes,
// 2 grid, 3 sim vehicles, 4 particles, 5 night lights, 6 programs
double gPhaseSeconds[8] = {};

void renderViewportScene(Display& d, VM& vm, ControlObj* vp) {
  Object* scene = fieldOf(vm, vp, u"scene").object();
  Object* cam = fieldOf(vm, vp, u"camera").object();
  if (cam && !cam->destroyed && isFloatingCamera(vm, cam)) updateFloatingCamera(vm, cam, vm.frameDt(), false);
  if (std::getenv("REGS_DEBUG_VIEWPORT")) {
    static std::map<std::pair<Object*, Object*>, int> seen;
    if (seen[{vp, scene}]++ == 0)
      LOG_INFO("viewport %p: scene %s camera %s visible %d size %.0fx%.0f", (void*)vp, scene ? scene->cls->name.c_str() : "-",
               cam ? cam->cls->name.c_str() : "-", vp->visible() ? 1 : 0, vp->width, vp->height);
  }
  if (!scene || !cam || scene->destroyed || cam->destroyed) return;
  // the render target is the texture of a gsTextureTarget_OGL effect; bloom and shadows are post effects too
  TextureObj* target = nullptr;
  Object* bloom = nullptr;
  Object* shadow = nullptr;
  if (auto it = vp->fieldValues.find(u"<effects>"); it != vp->fieldValues.end())
    if (ArrayObj* list = it->second.array())
      for (const Value& e : list->items) {
        Object* eo = e.object();
        if (!eo || eo->destroyed) continue;
        if (eo->cls->name == "gsTextureTarget_OGL") {
          if (auto t = eo->fieldValues.find(u"<texture>"); t != eo->fieldValues.end())
            target = dynamic_cast<TextureObj*>(t->second.object());
        } else if (eo->cls->name == "gsBloomEffect_OGL") {
          bloom = eo;
        } else if (eo->cls->name == "gsShadowEffect_OGL") {
          shadow = eo;
        }
      }
  // with a target: the viewport rect as is (pixel mode) or × target height; on screen: its window rect, units being
  // the window height when not in pixel mode (scene-basics.md §7.1, interfaces-2d-output.md §1.5.3)
  // Port UI scale (docs/ui/scaling.md): script pixels and target texels are d.uiScale window pixels / FBO texels, so
  // views in the GUI render at full resolution; height units on screen are the real window height
  bool pixels = vm.truthy(fieldOf(vm, vp, u"pixelCoordinates"));
  int bw = d.window().width(), bh = d.window().height();
  float scale = pixels ? d.uiScale : target ? float(target->height) * d.uiScale : float(bh);
  int x = 0, y = 0, w = std::max(1, int(vp->width * scale)), h = std::max(1, int(vp->height * scale));
  RenderTarget* rt = nullptr;
  if (target) {
    rt = d.renderTarget(target, w, h);
    if (!rt) return;
  } else {
    x = int(vp->desktopLeft() * scale);
    y = int(vp->desktopTop() * scale);
  }

  std::vector<DrawItem> items;
  std::vector<Object*> suns;
  std::vector<GridObj*> grids;
  Object* sky = nullptr;
  { auto ts_ = std::chrono::steady_clock::now(); collect(d, vm, scene, items, suns, grids, sky); gPhaseSeconds[1] += std::chrono::duration<double>(std::chrono::steady_clock::now() - ts_).count(); }

  SceneView v;
  v.timer = d.effectTimer;
  Vec3 eye, look, up;
  cameraView(vm, cam, eye, look, up);
  v.eye = eye;
  v.view = lookAt(eye, look, up);
  float nearZ, farZ;
  viewDepthRange(vm, vp, cam, nearZ, farZ);
  // port graphics options (settings regsViewDistance / regsDetailDistance, 1 = the original): the map view reaches
  // farther and keeps near detail longer; the shadow fit below keeps the original distance
  float viewScale = 1, detailScale = 1;
  if (!grids.empty()) {
    viewScale = std::clamp(settingNumber(vm, u"regsViewDistance", 1.0f), 0.5f, 4.0f);
    detailScale = std::clamp(settingNumber(vm, u"regsDetailDistance", 1.0f), 0.5f, 4.0f);
  }
  v.proj = perspective(floatOr(vm, fieldOf(vm, cam, u"fov"), 60.0f), float(w) / float(h), nearZ, farZ * viewScale);
  { auto ts_ = std::chrono::steady_clock::now(); for (GridObj* g : grids) collectGrid(d, vm, g, eye, v.proj * v.view, farZ * viewScale, viewScale, detailScale, items); gPhaseSeconds[2] += std::chrono::duration<double>(std::chrono::steady_clock::now() - ts_).count(); }
  { auto ts_ = std::chrono::steady_clock::now(); for (GridObj* g : grids) collectSimVehicles(d, vm, g, eye, v.proj * v.view, farZ * viewScale, items); gPhaseSeconds[3] += std::chrono::duration<double>(std::chrono::steady_clock::now() - ts_).count(); }
  addSkyItems(d, vm, sky, suns, eye, farZ * viewScale, items);  // sky dome and sun disc (engine/sky.cpp)
  for (Object* s : suns) {
    Light l;
    if (sunLight(vm, s, l)) v.lights.push_back(l);
  }
  if (sky) {  // gsSky::move: scene fog from fogColor/fogOffset/fogDistance (scene-basics.md §5)
    float dist = floatOr(vm, fieldOf(vm, sky, u"fogDistance"), 400.0f);
    if (dist > 0) {
      Value fc = fieldOf(vm, sky, u"fogColor");
      argbToFloats(fc.type() == VT::Int ? uint32_t(vm.toInt(fc)) : 0xB3CCFFu, v.fogColor);
      v.fog = true;
      v.fogStart = floatOr(vm, fieldOf(vm, sky, u"fogOffset"), 0.0f);
      v.fogEnd = dist;
    }
    v.emissionScale = skyEmissionScale(sky);  // port day/night cycle (engine/sky.cpp)
  }
  applyAtmosphere(atmosphereOf(vm, scene), v);  // the port's atmosphere fields of the scene (mods: weather)
  // the debug camera (REGS_DEBUG_TOPDOWN) high up: the fog starts where the game camera's would, behind the eye's
  // distance to its point (installer shots, 2026-11-17: 1000 units up the town was all fog colour)
  if (const char* td = std::getenv("REGS_DEBUG_TOPDOWN"); td && *td && v.fog) {
    const float dx = eye.x - look.x, dy = eye.y - look.y, dz = eye.z - look.z;
    const float away = std::sqrt(dx * dx + dy * dy + dz * dz);
    v.fogStart += away;
    v.fogEnd += away * 1.5f;
  }
  if (!grids.empty() && !shaderHooks().empty()) {  // the mods' shader hooks run in map views (render/glsl.h)
    v.hooks = true;
    auto* env = dynamic_cast<TextureObj*>(getTexture(vm, u"textures/envmap.dds").get());
    if (env && env->cube) v.hookEnvCube = d.texture(vm, env);
    gridSurfaceMap(grids[0], v.hookSurfaceMap, v.hookSurfaceRect, &v.hookTopMap);
    gridTrafficMaps(grids[0], v.hookLaneTraffic, v.hookTrafficRect, v.hookLaneCells);
    gridFootprints(grids[0], v.hookFootAtlas, v.hookFootRect);
    for (auto& t : shaderHookTextures())  // the mods' own textures (regsShaderTexture), loaded once
      if (!t.tried) {
        t.tried = true;
        auto* tex = dynamic_cast<TextureObj*>(getTexture(vm, utf8ToUtf16(t.path)).get());
        t.texture = tex ? d.texture(vm, tex) : 0;
        if (!t.texture) LOG_WARN("mods: texture %s for %s not found", t.path.c_str(), t.sampler.c_str());
      }
  }
  // the scene's particles as billboards for this camera (engine/particles.cpp, docs/renderer/particles.md)
  ParticleDrawList particles;
  {
    float t = std::tan(floatOr(vm, fieldOf(vm, cam, u"fov"), 60.0f) * 3.14159265f / 360.0f), ta = t * float(w) / float(h);
    { auto ts_ = std::chrono::steady_clock::now(); collectParticles(d, vm, scene, eye, look - eye, v.proj * v.view, std::sqrt(t * t + ta * ta), particles); gPhaseSeconds[4] += std::chrono::duration<double>(std::chrono::steady_clock::now() - ts_).count(); }
    // (working traffic lights, by day too: engine/traffic_lights.h; before the night lights, which take their pools)
    addTrafficLights(d, vm, grids, v, particles);
    { auto ts_ = std::chrono::steady_clock::now(); addNightLights(d, vm, grids, sky, v, particles, items); gPhaseSeconds[5] += std::chrono::duration<double>(std::chrono::steady_clock::now() - ts_).count(); }
    if (!particles.sprites.empty()) v.particles = &particles;
  }
  // environment flags: light count << 12 outside shadow passes (FUN_10051120)
  v.env = uint32_t(std::min<size_t>(v.lights.size(), 15)) << 12;
  d.r2d().flushAll();
  // shadows (docs/renderer/shadows.md): fit the light camera to this view, render the depth map from the light,
  // then the main pass samples it on unit 7 with ENVIROMENT_SHADOWMAP
  if (shadow && !rt && fieldOf(vm, shadow, u"viewport").object() == vp) {
    Object* lightObj = fieldOf(vm, shadow, u"light").object();
    Light sun;
    bool haveLight = false;
    for (Object* s : suns)
      if (lightObj && fieldOf(vm, s, u"light").object() == lightObj) haveLight = sunLight(vm, s, sun);
    const RenderTarget* map = haveLight ? shadowTarget(d, shadow) : nullptr;
    if (map) {
      ShadowFitIn in;
      in.eye = eye;
      in.target = look;
      in.up = up;
      in.fov = floatOr(vm, fieldOf(vm, cam, u"fov"), 60.0f);
      in.aspect = float(w) / float(h);
      in.near = nearZ;
      in.maxDistance = floatOr(vm, fieldOf(vm, shadow, u"maxDistance"), -1.0f);
      in.far = in.maxDistance != -1.0f ? std::min(farZ, in.maxDistance) : farZ;
      in.clipYPlane = vm.truthy(fieldOf(vm, shadow, u"clipYPlane"));
      in.yPlane = floatOr(vm, fieldOf(vm, shadow, u"yPlane"), 0.0f);
      in.safeMargin = floatOr(vm, fieldOf(vm, shadow, u"safeMargin"), 0.0f);
      in.light = sun.position;
      in.mapWidth = map->width;
      in.mapHeight = map->height;
      ShadowFitOut fit;
      if (fitShadow(in, fit)) {
        bool backSide = vm.truthy(fieldOf(vm, shadow, u"useBackSide"));
        SceneView lv = v;  // culling, LOD and sorting stay with the main camera (§4.3)
        lv.view = fit.lightView;
        lv.proj = fit.lightProj;
        lv.env = kEnvShadowMapRender | (backSide ? 0x20u : 0u);
        lv.flipCull = backSide;
        lv.shadowWarp = fit.warp;
        lv.lights.clear();
        lv.fog = false;
        assignPrograms(d, vm, items, lv.env);
        if (v.particles) prepareParticlePrograms(d, vm, particles, lv.env);
        d.r3d().begin(map, 0, 0, map->width, map->height, 0, false, bw, bh);
        d.r3d().draw(lv, items);
        d.r3d().end(bw, bh);
        v.env |= kEnvShadowMap;
        v.shadowMap = map->depth;
        v.shadowTransform = fit.lightProj * fit.lightView * inverseRigid(v.view);  // main eye space → light clip
        v.shadowWarp = fit.warp;
        for (int k = 0; k < 4; ++k) v.shadowParams[k] = fit.params[k];
      }
    }
  }
  { auto ts_ = std::chrono::steady_clock::now(); assignPrograms(d, vm, items, v.env); gPhaseSeconds[6] += std::chrono::duration<double>(std::chrono::steady_clock::now() - ts_).count(); }
  if (v.particles) prepareParticlePrograms(d, vm, particles, v.env);
  uint32_t clear = uint32_t(vm.toInt(fieldOf(vm, vp, u"clearColor")));
  // bloom (on-screen viewports): the scene goes into its HDR target, bottom-left aligned, then the chain composites
  // it into this rect (docs/renderer/bloom.md §3.3)
  const RenderTarget* hdr = nullptr;
  if (bloom && !rt) {
    bloomTick(vm, bloom, vm.frameDt());
    hdr = bloomSceneTarget(d, vm, bloom, w, h);
  }
  // port style effects (game view): the scene, or the bloom composite, goes into the style input first
  const RenderTarget* styled = nullptr;
  const bool postHooks = !rt && !grids.empty() && postHooksWanted(vm, scene);  // the mods' post hooks (render/glsl.h)
  if (!rt && !grids.empty() && (styleActive(vm) || postHooks)) styled = styleTarget(d, vm, w, h, hdr ? 0 : d.antialiasing);
  if (hdr) {
    d.r3d().begin(hdr, 0, hdr->height - h, w, h, clear, (clear >> 24) != 0, bw, bh);
    d.r3d().draw(v, items);
    d.r3d().end(bw, bh);
    bloomFinish(d, vm, bloom, x, y, w, h, bw, bh, styled);
    if (styled) styleTakeDepth(d, *hdr, w, h);
    d.r3d().end(bw, bh);
  } else if (styled) {
    d.r3d().begin(styled, 0, 0, w, h, clear, (clear >> 24) != 0, bw, bh);
    d.r3d().draw(v, items);
    d.r3d().end(bw, bh);
  } else if (const RenderTarget* ms = !rt && d.antialiasing > 0 && (!vp->parentControl() || (clear >> 24))
                                          ? msaaTarget(d, w, h, d.antialiasing)
                                          : nullptr) {
    // window MSAA (port): an offscreen multisampled scene resolved into the view's rect, so the sample count can
    // change live; the window itself has no samples
    // always cleared: the blit replaces the rect, and the back buffer under a view without a clear colour is the
    // frame's black
    d.r3d().begin(ms, 0, 0, w, h, (clear >> 24) ? clear : 0u, true, bw, bh);
    d.r3d().draw(v, items);
    d.r3d().end(bw, bh);
    msaaFinish(d, x, y, w, h, bh);
    d.r3d().end(bw, bh);
  } else {
    d.r3d().begin(rt, x, y, w, h, clear, (clear >> 24) != 0, bw, bh);
    d.r3d().draw(v, items);
    if (const char* dump = std::getenv("REGS_DUMP_TARGET"); dump && rt) d.dumpTarget(*rt, dump);
    d.r3d().end(bw, bh);
  }
  if (styled) {
    Vec3 toTarget = look - eye;
    styleFinish(d, vm, x, y, w, h, bw, bh, nearZ, farZ * viewScale, std::sqrt(dot(toTarget, toTarget)), postHooks ? &v : nullptr);
    d.r3d().end(bw, bh);
  }
  d.r2d().resume();
}

void cameraFrame(VM& vm, Object* cam, Vec3& eye, Vec3& look, Vec3& up) { cameraView(vm, cam, eye, look, up); }

void viewDepthRange(VM& vm, Object* vp, Object* cam, float& nearZ, float& farZ) {
  // near = max(viewport, camera), far = min (camera −1 = use the viewport's)
  nearZ = floatOr(vm, fieldOf(vm, vp, u"minDistance"), 0.1f);
  farZ = floatOr(vm, fieldOf(vm, vp, u"maxDistance"), 400.0f);
  float cn = floatOr(vm, fieldOf(vm, cam, u"minDistance"), -1), cf = floatOr(vm, fieldOf(vm, cam, u"maxDistance"), -1);
  if (isFloatingCamera(vm, cam) && cam->fieldValues.count(u"<minDistance>")) {  // eased finalMin / finalMax
    cn = vm.toFloat(cam->fieldValues[u"<minDistance>"]);
    cf = vm.toFloat(cam->fieldValues[u"<maxDistance>"]);
  }
  if (cn != -1 && cn >= nearZ) nearZ = cn;
  if (cf != -1) farZ = std::min(cf, farZ);
  // port (owner 2026-11-16: "the 3D models clip into the camera very early"): the main camera's script sets its near
  // plane to 30 (engine.script), which cuts away whatever comes within 30 units: towers and trees when zoomed in or
  // tilted toward them. The near plane follows the camera's distance to its point instead, a tenth of it, 1 to 30
  // (depth precision stays: 0.01 at 300 units with a near plane of 1); the debug camera's own eye takes 1
  if (isFloatingCamera(vm, cam) && cam->fieldValues.count(u"<offset>")) {
    const Vec3 off = vec3Of(cam->fieldValues[u"<offset>"]);
    float want = std::clamp(std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z) * 0.1f, 1.0f, 30.0f);
    // the debug camera also sees far (installer shots from high up, 2026-11-17: the viewport's far plane left a
    // camera 1000 units up looking at nothing)
    if (const char* td = std::getenv("REGS_DEBUG_TOPDOWN"); td && *td) want = 2.0f, farZ = std::max(farZ, 8000.0f);
    nearZ = std::min(nearZ, want);
  }
}

uint32_t combinerProgram(Display& d, VM& vm, const std::string& shader, uint32_t matFlags, uint32_t env) {
  return programFor(d, vm, shader, matFlags, env);
}

void runCompanionScript(VM& vm, Object* o, const std::u16string& objectPath) {
  std::u16string script = objectPath.substr(0, objectPath.rfind(u'.')) + u".script";
  if (!vm.vfs().exists(utf16ToUtf8(script))) return;
  LoadOptions opt;
  opt.into = Ref<Object>(o);
  vm.load(u"/" + script, opt);
}

void registerSceneNatives(VM& vm) {
  if (ClassInfo* c = vm.findClass("gsObject")) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new ObjectObj(ci)); };
  // port fields for mods (port/docs/mod-manager.md §4.2): the scene's atmosphere (atmosphereOf above) and the
  // camera's eye and look-at point as the 3D pass draws them
  if (ClassInfo* c = vm.findClass("gsScene")) {
    auto stored = [c](const char16_t* name, int type, Value def) {
      FieldInfo& f = c->fields[name];
      f.name = name;
      f.type = type;
      f.get = [name, def](VM&, Object* o) {
        auto it = o->fieldValues.find(name);
        return it == o->fieldValues.end() ? def : it->second;
      };
      f.set = [name, type](VM& v, Object* o, const Value& x) {
        Value r = v.resolve(x);
        o->fieldValues[name] = type == 3 ? Value::integer(v.toInt(r)) : Value::real(v.toFloat(r));
      };
    };
    for (const char16_t* n : kAtmosphereFloats)
      stored(n, 4, Value::real(std::u16string_view(n) == u"regsLightDesaturate" ? 0.0f : 1.0f));
    stored(u"regsFogTint", 3, Value::integer(0));
    stored(u"regsLightTint", 3, Value::integer(0));
    stored(u"regsPostHooks", 4, Value::real(0.0f));
  }
  if (ClassInfo* c = vm.findClass("gs3DCamera")) {
    auto eyeField = [c](const char16_t* name, bool eyeNotLook) {
      FieldInfo& f = c->fields[name];
      f.name = name;
      f.type = 7;
      f.readOnly = true;
      f.get = [eyeNotLook](VM& v, Object* o) {
        Vec3 eye, look, up;
        cameraView(v, o, eye, look, up);
        Vec3 p = eyeNotLook ? eye : look;
        return Value::vector(p.x, p.y, p.z);
      };
      f.set = nullptr;
    };
    eyeField(u"regsEyePosition", true);
    eyeField(u"regsLookPosition", false);
  }
  // loadFromFile(path, [applyTransform]) (gsObject::load 0x1008bd90)
  implement(vm, "gsObject", u"loadFromFile", [](VM& v, Object* s, int argc, const Value* a) {
    auto* o = dynamic_cast<ObjectObj*>(s);
    if (!o) return Value::boolean(false);
    std::u16string path = v.resolvePath(v.toStr(a[0]), s);
    runCompanionScript(v, o, path);
    auto data = loadObjectCached(v, utf16ToUtf8(path));
    if (!data) return Value::boolean(false);
    o->data = data;
    o->gpu.reset();
    o->materials.reset();
    v.fireEvent(o, u"onLoaded", {});
    return Value::boolean(true);
  });
  // loadSkeleton and the gsSkeleton methods: engine/skinning.cpp
  // gsMaterialVariant (batch-B.md): textures and shader are kept on the variant and turned into a GpuMaterial by
  // scriptMaterial(); setTexture(0) also sets MATERIAL_DIFFUSEMAP
  implement(vm, "gsMaterialVariant", u"setTexture", [](VM& v, Object* s, int, const Value* a) {
    int slot = v.toInt(a[0]);
    if (slot < 0 || slot > 6) return Value();
    s->fieldValues[u"<texture" + utf8ToUtf16(std::to_string(slot)) + u">"] = Value::string(v.toStr(a[1]));
    if (slot == 0) {
      uint32_t f = 0;
      if (auto it = s->fieldValues.find(u"flags"); it != s->fieldValues.end()) f = uint32_t(v.toInt(it->second));
      s->fieldValues[u"flags"] = Value::integer(int32_t(f | kMatDiffuseMap));
    }
    return Value();
  });
  implement(vm, "gsMaterialVariant", u"setShader", [](VM& v, Object* s, int, const Value* a) {
    std::u16string p = v.toStr(a[0]);
    if (!p.empty() && p[0] == u'/') p = p.substr(1);
    s->fieldValues[u"<shader>"] = Value::string(p);
    return Value();
  });
  implement(vm, "gsMaterialVariant", u"setShaderParams", [](VM& v, Object* s, int, const Value* a) {
    int i = v.toInt(a[0]);
    Value q = v.resolve(a[1]);
    if (i >= 0 && i < 8 && q.type() == VT::Quat) s->fieldValues[u"<param" + utf8ToUtf16(std::to_string(i)) + u">"] = q;
    return Value();
  });
  // shaderParams (field, set W:0x10087490): stored at +0x44 and passed on as shader parameter 0 (custom0); the wave,
  // sliding-uv and flag shaders divide by it (mod(timer, custom0.z)), so a missing value turns the parts black
  if (ClassInfo* c = vm.findClass("gsMaterialVariant")) {
    FieldInfo& f = c->fields[u"shaderParams"];
    f.name = u"shaderParams";
    f.get = [](VM&, Object* o) {
      auto it = o->fieldValues.find(u"<param0>");
      return it != o->fieldValues.end() ? it->second : Value::quat(0, 0, 0, 0);
    };
    f.set = [](VM& v, Object* o, const Value& val) {
      Value q = v.resolve(val);
      if (q.type() == VT::Quat) o->fieldValues[u"<param0>"] = q;
    };
  }
  // getMaterial(name): a variant bound to the loaded mesh's material of that name (null when there is none); its
  // shader, flags and parameters override the .material values (objectMaterial)
  implement(vm, "gsObject", u"getMaterial", [](VM& v, Object* s, int, const Value* a) {
    auto* o = dynamic_cast<ObjectObj*>(s);
    if (!o || !o->data) return Value::null();
    std::string name = utf16ToUtf8(v.toStr(a[0]));
    const auto& mats = o->data->materials;
    auto it = std::find_if(mats.begin(), mats.end(), [&](const MaterialDef& m) { return m.name == name; });
    if (it == mats.end()) {
      if (std::getenv("REGS_DEBUG_MATERIALS")) {
        std::string have;
        for (const auto& m : mats) have += " \"" + m.name + "\"";
        LOG_INFO("getMaterial(\"%s\") on %s: not among%s", name.c_str(), o->pendingPath.c_str(), have.c_str());
      }
      return Value::null();
    }
    std::u16string key = u"<material:" + utf8ToUtf16(name) + u">";
    Value& slot = o->fieldValues[key];
    if (!slot.object()) {
      Ref<Object> m = v.newObject(v.findClass("gsMaterialVariant"), nullptr);
      m->fieldValues[u"flags"] = Value::integer(int32_t(it->flags));
      slot = Value::heap(VT::Object, m.get());
    }
    return slot;
  });
  // reset(): the camera jumps to its targets (W:0x10071940)
  implement(vm, "gs3DCamera", u"reset", [](VM& v, Object* s, int, const Value*) {
    if (isFloatingCamera(v, s)) updateFloatingCamera(v, s, 0, true);
    return Value();
  });
  implement(vm, "gsSun", u"generateMesh", [](VM&, Object* s, int, const Value*) {
    s->fieldValues[u"<mesh>"] = Value::boolean(true);  // the sun disc; it also sets the light direction vector
    return Value();
  });
  implement(vm, "gsObject", u"colorize", [](VM& v, Object* s, int, const Value* a) {
    if (auto* o = dynamic_cast<ObjectObj*>(s)) o->colorizeArgb = uint32_t(v.toInt(a[0]));
    return Value();
  });
}

}  // namespace regs::engine
