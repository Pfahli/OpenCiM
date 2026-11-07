// Particles (docs/renderer/particles.md): the script objects gsParticleEmitter / gsEmittingParticleEmitter /
// gsParticleEngine, emission (cone, ring, on/off cycles, density), the engine's physics, the grid's emitter instances
// on vehicles and items, and the billboards for the 3D pass. Addresses: L: = Linux build, W: = GSystem.dll.
//
// Port choices: particles draw from one capped pool per engine (kMaxParticles; emission stops at the cap), emission uses
// the port's own random stream (the original's rand() calls would shift the simulation's sequence), and the camera
// distance of a vehicle or item stands in for its bucket's eyeDist.
#include "engine/particles.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/gui.h"
#include "engine/scene3d.h"
#include "engine/sim.h"
#include "engine/sim_graph.h"
#include "engine/sim_state.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::engine::sim;
using render::Mat4;
using render::Vec3;

namespace {

constexpr size_t kMaxParticles = 24000;  // per engine; the original has no cap
constexpr float kTwoPi = 6.2831855f;
const char* const kParticleShader = "shaders/particles/particles.shader";

std::minstd_rand& rng() {
  static std::minstd_rand r{20111};
  return r;
}
float rnd01() { return float(rng()() - rng().min()) / float(rng().max() - rng().min()); }
// gsRandom(a, b) (L:0x08108f80): a + (b − a)·rand()/2³¹
float rnd(float a, float b) { return a + (b - a) * rnd01(); }

bool debugParticles() {
  static const bool on = std::getenv("REGS_DEBUG_PARTICLES") != nullptr;
  return on;
}
bool particlesOff() {
  static const bool off = std::getenv("REGS_NO_PARTICLES") != nullptr;
  return off;
}

Vec3 vecOf(const Value& v, Vec3 def = {}) { return v.type() == VT::Vector ? Vec3{v.vec()[0], v.vec()[1], v.vec()[2]} : def; }
Value vecValue(const Vec3& v) { return Value::vector(v.x, v.y, v.z); }

// v rotated by angle about the unit axis a (Rodrigues)
Vec3 rotateAxis(const Vec3& v, const Vec3& a, float angle) {
  float c = std::cos(angle), s = std::sin(angle);
  return v * c + render::cross(a, v) * s + a * (dot(a, v) * (1 - c));
}
// gsAnyCrossingVector / gsPerpendicular: some unit vector perpendicular to d
Vec3 perpendicular(const Vec3& d) {
  Vec3 p = std::fabs(d.y) < 0.9f ? render::cross(d, Vec3{0, 1, 0}) : render::cross(d, Vec3{1, 0, 0});
  return render::normalize(p);
}
Vec3 rotateBy(const Mat4& m, const Vec3& v) {  // the rotation part of m
  return {m.at(0, 0) * v.x + m.at(0, 1) * v.y + m.at(0, 2) * v.z, m.at(1, 0) * v.x + m.at(1, 1) * v.y + m.at(1, 2) * v.z,
          m.at(2, 0) * v.x + m.at(2, 1) * v.y + m.at(2, 2) * v.z};
}

// gsColorRange::interpolate (L:0x080ee9c0): per byte start·(1 − t) + end·t, rounded
uint32_t lerpArgb(uint32_t a, uint32_t b, float t) {
  uint32_t out = 0;
  for (int k = 0; k < 32; k += 8) {
    float x = float(a >> k & 0xFF) * (1 - t) + float(b >> k & 0xFF) * t;
    out |= uint32_t(std::clamp(int(std::lround(x)), 0, 255)) << k;
  }
  return out;
}
// gsColor::multAlpha (L:0x080ee2f0)
uint32_t multAlpha(uint32_t c, float f) {
  int a = int(std::lround(float(c >> 24) * f));
  return (c & 0xFFFFFF) | uint32_t(std::clamp(a, 0, 255)) << 24;
}

class ParticleEngineObj;

// ---------------------------------------------------------------------------------------------------------------
// gsParticleEmitter (ctor L:0x0822daf0, fields L:0x0822b140; Windows offsets in docs/native-api.md)

class ParticleEmitterObj : public Object {
 public:
  explicit ParticleEmitterObj(const ClassInfo* c) : Object(c) {}
  // fields with the constructor's defaults
  bool active = true, autoemit = false, sortParticles = false;
  float planeSize = 0, particleSize = 1, airResist = 0.1f, density = 20, ownerVelocity = 1, maxDistance = 0;
  int blend = 1;  // BLEND_ALPHA 1, BLEND_ADDITIVE 2, BLEND_ADDITIVE_ALPHA 4
  uint32_t conditions = 0;
  Vec3 gravitation{0, -9.81f, 0}, direction{0, 1, 0};
  float minSpeed = 3, maxSpeed = 10, minAngle = 0, maxAngle = 0.5f, minOrient = 0, maxOrient = kTwoPi, minLife = 5,
        maxLife = 7, minOn = -1, maxOn = -1, minOff = -1, maxOff = -1;
  float growth = 0, growStart = 0, growEnd = 1000;  // setParticleScale
  std::vector<std::pair<uint32_t, uint32_t>> colors;  // addColorRange (start, end ARGB)
  struct EmitPos {
    Vec3 v;
    bool relative = true;
  };
  std::vector<EmitPos> emitPositions;
  struct Term {
    float weight = 0;
    Value var;
  };
  std::vector<Term> densityTerms, speedTerms, alphaTerms;
  Ref<Object> texture;  // setTexture (TextureObj)
  Vec3 localPos;        // position (relative to the parent)
  float timer = 0;      // the emission timer of move() (+0x190)
  // port fields for mods (particles.md §6, mod-manager.md §4.2): a box of these half extents around the emit point
  // that each particle starts at a random point of, and streaks: drawn along the velocity, half length =
  // speed × regsStretch seconds (0 = the original's camera-facing billboard)
  Vec3 emitBox;
  float stretch = 0;
  bool vehicleOnly = false;  // listed in gsGrid.regsVehicleEmitters: emits only behind vehicles, not at its own place
  // gsGrid.regsTreeEmitters (tickTreeEmitters): the tree objects it emits from (their .object's mesh name, e.g.
  // "tree01"; empty: every tree), how long its particles lie on the ground once they reach the tree's base height
  // (regsLand, s; 0: they do not land), and a sideways swing while they fall (regsFlutter, m/s)
  std::vector<std::string> trees;
  float land = 0, flutter = 0;
  bool emitting = false;   // gsEmittingParticleEmitter: its particles carry its child emitters
  bool contrail = false;   // gsBoatContrain: a wake ribbon, not particles (not ported)

  float termSum(VM& vm, const std::vector<Term>& t) const {
    float s = 0;
    for (const Term& x : t) s += x.weight * vm.toFloat(vm.resolve(x.var));
    return s;
  }
  float rate(VM& vm) const { return density + termSum(vm, densityTerms); }

  Vec3 parentGlobal(VM& vm) const;
  Vec3 globalPos(VM& vm) const { return parentGlobal(vm) + localPos; }
  void tick(VM& vm, float dt) override;
};

struct Particle {
  Vec3 pos, vel, grav;
  float air = 0, life = 1, age = 0, orient = 0, size = 1, growth = 0, growStart = 0, growEnd = 1000;
  uint32_t c0 = 0xFFFFFFFF, c1 = 0xFFFFFFFF;
  float childTimer = 0;  // +0x58: the child emitters' accumulator
  uint32_t system = 0;
  bool children = false;  // emitted by a gsEmittingParticleEmitter: its child emitters follow the particle
  // port (regsLand, regsFlutter): lands at floorY and lies there for `lie` s (fading over the last third), swings
  // sideways with `flutter` m/s while falling
  bool landed = false;
  float floorY = -1e30f, lie = 0, flutter = 0, phase = 0;
};

// gsParticleEngine (ctor L:0x0822cd10, move L:0x0822d180): the scene's particles, moved in steps of at most 0.2 s
class ParticleEngineObj : public Object {
 public:
  explicit ParticleEngineObj(const ClassInfo* c) : Object(c) {}
  float timeSpeed = 1;
  Vec3 wind;
  std::vector<Particle> particles;
  std::vector<Ref<Object>> systems;  // emitters by system index
  std::unordered_map<const Object*, uint32_t> systemIndex;
  size_t dropped = 0;  // emissions refused at the cap

  uint32_t systemOf(ParticleEmitterObj* e) {
    auto it = systemIndex.find(e);
    if (it != systemIndex.end()) return it->second;
    uint32_t i = uint32_t(systems.size());
    systems.push_back(Ref<Object>(e));
    systemIndex.emplace(e, i);
    return i;
  }
  void tick(VM& vm, float dt) override;
  bool moveParticle(VM& vm, Particle& p, float dt);
};

Object* sceneOf(VM& vm, Object* o) {
  static const ClassInfo* sceneClass = vm.findClass("gsScene");
  for (; o; o = o->parent)
    if (o->cls->isA(sceneClass)) return o;
  return nullptr;
}

ParticleEngineObj* engineOfScene(Object* scene) {
  if (!scene || scene->destroyed) return nullptr;
  auto it = scene->fieldValues.find(u"particleEngine");
  if (it == scene->fieldValues.end()) return nullptr;
  auto* e = dynamic_cast<ParticleEngineObj*>(it->second.object());
  return e && !e->destroyed ? e : nullptr;
}

Vec3 ParticleEmitterObj::parentGlobal(VM& vm) const {
  // gs3DElement::getGlobalPos without the parent's rotation: emitters sit under the scene, a template or another
  // emitter (scripted ones never rotate their parents)
  Object* p = parent;
  if (!p || p->cls->isA(vm.findClass("gsScene"))) return {};
  if (auto* e = dynamic_cast<ParticleEmitterObj*>(p)) return e->globalPos(vm);
  auto g = p->fieldValues.find(u"globalPos");
  if (g != p->fieldValues.end() && g->second.type() == VT::Vector) return vecOf(g->second);
  auto l = p->fieldValues.find(u"position");
  return l != p->fieldValues.end() ? vecOf(l->second) : Vec3{};
}

// gsParticleEmitter::emitParticle (L:0x0822d2c0, W:0x1005c3f0): one particle at pos. The direction is tilted by
// rnd(minAngle, maxAngle) about an axis perpendicular to it, spun by rnd(0, 2π) about it and rotated into the world
// by basis; planeSize puts the particle on a ring of that radius around its direction; the velocity adds the owner's
// velocity × ownerVelocity. Returns false at the cap or without an engine.
bool emitParticle(VM& vm, ParticleEngineObj* eng, ParticleEmitterObj* e, const Vec3& pos, const Mat4& basis,
                  const Vec3& ownerVel) {
  if (!eng || e->contrail) return false;
  if (eng->particles.size() >= kMaxParticles) {
    ++eng->dropped;
    return false;
  }
  Particle p;
  p.pos = pos;
  float speed = rnd(e->minSpeed, e->maxSpeed) + e->termSum(vm, e->speedTerms);
  float tilt = rnd(e->minAngle, e->maxAngle), spin = rnd01() * kTwoPi;
  Vec3 dir = e->direction;
  Vec3 d = rotateAxis(rotateAxis(dir, perpendicular(dir), tilt), dir, spin);
  d = rotateBy(basis, d);
  if (e->planeSize != 0) {
    float a = rnd01() * kTwoPi;
    Vec3 u = perpendicular(d), w = render::cross(u, d);
    p.pos = p.pos + u * (std::cos(a) * e->planeSize) + w * (std::sin(a) * e->planeSize);
  }
  if (e->emitBox.x != 0 || e->emitBox.y != 0 || e->emitBox.z != 0)  // port: regsEmitBox
    p.pos = p.pos + Vec3{rnd(-1, 1) * e->emitBox.x, rnd(-1, 1) * e->emitBox.y, rnd(-1, 1) * e->emitBox.z};
  p.vel = d * speed + ownerVel * e->ownerVelocity;
  p.life = rnd(e->minLife, e->maxLife);
  p.orient = rnd(e->minOrient, e->maxOrient);
  if (!e->colors.empty()) {
    auto [a, b] = e->colors[size_t(rng()()) % e->colors.size()];
    p.c0 = a, p.c1 = b;
  }
  if (!e->alphaTerms.empty()) {
    float f = e->termSum(vm, e->alphaTerms);
    p.c0 = multAlpha(p.c0, f), p.c1 = multAlpha(p.c1, f);
  }
  p.air = e->airResist;
  p.grav = e->gravitation;
  p.growth = e->growth, p.growStart = e->growStart, p.growEnd = e->growEnd;
  p.size = e->particleSize;
  p.lie = e->land;
  p.flutter = e->flutter;
  p.phase = rnd01() * kTwoPi;
  p.system = eng->systemOf(e);
  p.children = e->emitting;
  eng->particles.push_back(p);
  return true;
}

// instantEmit(count) of a script emitter (L:0x0822c4e0, W:0x1005c8c0): count particles at each emit position (at the
// emitter when it has none); a non-relative position is taken as a world position, a relative one is added to the
// emitter's own position
void instantEmit(VM& vm, ParticleEmitterObj* e, int count) {
  ParticleEngineObj* eng = engineOfScene(sceneOf(vm, e));
  if (!eng || count <= 0) return;
  Mat4 basis;  // the emitter's global orientation: identity for scripted emitters
  Vec3 g = e->globalPos(vm);
  size_t positions = std::max<size_t>(1, e->emitPositions.size());
  for (size_t k = 0; k < positions; ++k) {
    Vec3 at = g;
    if (k < e->emitPositions.size()) at = e->emitPositions[k].relative ? g + e->emitPositions[k].v : e->emitPositions[k].v;
    for (int i = 0; i < count; ++i) emitParticle(vm, eng, e, at, basis, {});
  }
}

// the instance version (W:0x1005ca80): positions through the instance's placement
void instanceEmit(VM& vm, ParticleEngineObj* eng, ParticleEmitterObj* e, int count, const Mat4& place, const Vec3& vel) {
  Vec3 g = e->localPos;  // the template sits at the origin
  size_t positions = std::max<size_t>(1, e->emitPositions.size());
  for (size_t k = 0; k < positions; ++k) {
    Vec3 at = render::transformPoint(place, g);
    if (k < e->emitPositions.size())
      at = e->emitPositions[k].relative ? render::transformPoint(place, g + e->emitPositions[k].v) : e->emitPositions[k].v;
    for (int i = 0; i < count; ++i) emitParticle(vm, eng, e, at, place, vel);
  }
}

// The emission timer of gsParticleEmitter::move (L:0x0822c770) and instanceMove (L:0x0822e040, W:0x1005d0a0): with
// minOnTime = −1 a steady stream of rate = density + terms particles per second while autoemit; otherwise on/off
// cycles: the timer starts at rnd(−maxOff, maxOn), counts down through an on phase of rnd(minOn, maxOn) seconds
// (emitting trunc(t·rate) − trunc((t − dt)·rate) particles per update) and up through an off phase of
// −rnd(minOff, maxOff). Returns the particles to emit now.
int emissionCount(VM& vm, ParticleEmitterObj* e, float& timer, float dt) {
  if (e->minOn == -1.0f) {
    if (!e->autoemit) return 0;
    float r = e->rate(vm);
    if (r <= 0) return 0;
    float step = 1.0f / r;
    timer += dt;
    int n = 0;
    while (step <= timer && n < 100000) timer -= step, ++n;
    return n;
  }
  if (timer == 0) {
    timer = rnd(-e->maxOff, e->maxOn);
    return 0;
  }
  if (timer < 0) {
    timer += dt;
    if (timer >= 0) timer = rnd(e->minOn, e->maxOn);
    return 0;
  }
  if (!e->autoemit) return 0;
  float r = e->rate(vm);
  if (r <= 0) return 0;
  int before = int(timer * r);
  timer -= dt;
  int after = int(timer * r);
  if (timer <= 0) timer = -rnd(e->minOff, e->maxOff);
  return std::max(0, before - after);
}

void ParticleEmitterObj::tick(VM& vm, float dt) {
  if (!particlesOff() && !contrail && !vehicleOnly && dt > 0)
    if (int n = emissionCount(vm, this, timer, dt)) instantEmit(vm, this, n);
  Object::tick(vm, dt);
}

// gsParticleEngine::moveParticle (L:0x0822cd90, W:0x1005c020): age; alive: pos += v·dt,
// v += (gravity − airResist·(v − wind))·dt, the child emitters stream from it; dead (age ≥ life): the child emitters
// without autoemit burst density particles where it died. Returns true when the particle is gone.
bool ParticleEngineObj::moveParticle(VM& vm, Particle& p, float dt) {
  p.age += dt;
  ParticleEmitterObj* src = p.children && p.system < systems.size() ? static_cast<ParticleEmitterObj*>(systems[p.system].get()) : nullptr;
  if (p.age < p.life) {
    if (p.landed) return false;  // lying on the ground (regsLand)
    p.pos = p.pos + p.vel * dt;
    p.vel = p.vel + (p.grav - (p.vel - wind) * p.air) * dt;
    if (p.flutter > 0) {  // port: a falling leaf's swing and turn
      float t = p.age + p.phase;
      p.pos.x += std::cos(t * 2.3f + p.phase) * p.flutter * dt;
      p.pos.z += std::sin(t * 1.7f) * p.flutter * dt;
      p.orient += std::sin(t * 1.3f) * 2.0f * dt;
    }
    if (p.lie > 0 && p.pos.y <= p.floorY) {  // port: landed; it lies still from now on
      p.c0 = p.c1 = lerpArgb(p.c0, p.c1, std::clamp(p.age / p.life, 0.0f, 1.0f));
      p.pos.y = p.floorY;
      p.landed = true;
      p.age = 0;
      p.life = p.lie;
      return false;
    }
    if (src)
      for (auto& c : src->children) {
        auto* ce = dynamic_cast<ParticleEmitterObj*>(c.get());
        if (!ce || ce->destroyed || !ce->autoemit) continue;
        float r = ce->rate(vm);
        if (r <= 0) continue;
        float step = 1.0f / r;
        p.childTimer += dt;
        while (step < p.childTimer) {
          p.childTimer -= step;
          emitParticle(vm, this, ce, p.pos, Mat4{}, p.vel);
        }
      }
    return false;
  }
  if (src)
    for (auto& c : src->children) {
      auto* ce = dynamic_cast<ParticleEmitterObj*>(c.get());
      if (!ce || ce->destroyed || ce->autoemit) continue;
      int n = int(ce->rate(vm));
      for (int i = 0; i < n; ++i) emitParticle(vm, this, ce, p.pos, Mat4{}, p.vel);
    }
  return true;
}

void ParticleEngineObj::tick(VM& vm, float dt) {
  // systems whose emitter went away take their particles with them
  bool gone = false;
  for (auto& s : systems) gone = gone || s->destroyed;
  if (gone) {
    std::erase_if(particles, [&](const Particle& p) { return systems[p.system]->destroyed; });
    std::vector<Ref<Object>> kept;
    std::vector<uint32_t> remap(systems.size(), 0);
    systemIndex.clear();
    for (size_t i = 0; i < systems.size(); ++i)
      if (!systems[i]->destroyed) {
        remap[i] = uint32_t(kept.size());
        systemIndex.emplace(systems[i].get(), uint32_t(kept.size()));
        kept.push_back(systems[i]);
      }
    systems = std::move(kept);
    for (Particle& p : particles) p.system = remap[p.system];
  }
  float t = dt * timeSpeed;
  if (!particles.empty() && t > 0) {
    int n = std::max(1, int(t / 0.2f + 0.99f));
    float step = t / float(n);
    for (int k = 0; k < n; ++k)
      for (size_t i = 0; i < particles.size();) {
        Particle cur = particles[i];  // a copy: child emitters may append to the pool
        if (moveParticle(vm, cur, step)) {
          particles[i] = particles.back();
          particles.pop_back();
        } else {
          particles[i] = cur;
          ++i;
        }
      }
  }
  Object::tick(vm, dt);
}

// ---------------------------------------------------------------------------------------------------------------
// batch materials: one per (texture, blend), like the per-emitter gsMaterial_OGL of updateMaterial (L:0x0822dd50):
// shaders/particles/particles.shader, flags DIFFUSEMAP | POINT (0x4200) + ADDITIVE 0x80 / ADDITIVE_A 0x100; colours
// as startRendering sets them (L:0x08218b60): ambient = diffuse = 0.5 grey for blended, emission 1 for additive

struct BatchKey {
  const Object* texture;
  uint32_t flags;
  bool operator<(const BatchKey& o) const { return texture != o.texture ? texture < o.texture : flags < o.flags; }
};
std::map<BatchKey, std::unique_ptr<render::GpuMaterial>>& batchMaterials() {
  static std::map<BatchKey, std::unique_ptr<render::GpuMaterial>> m;
  return m;
}

uint32_t materialFlags(int blend) { return blend == 2 ? 0x4280u : blend == 4 ? 0x4300u : 0x4200u; }

render::GpuMaterial* batchMaterial(Display& d, VM& vm, ParticleEmitterObj* e) {
  uint32_t flags = materialFlags(e->blend);
  auto& slot = batchMaterials()[{e->texture.get(), flags}];
  if (!slot) {
    slot = std::make_unique<render::GpuMaterial>();
    render::GpuMaterial& m = *slot;
    m.flags = flags | render::kMatTwoSided;  // the system's flag 0x40: culling off (the combiner gets `flags`)
    m.shader = kParticleShader;
    bool additive = flags & 0x180;
    for (int k = 0; k < 4; ++k) {
      m.ambient[k] = m.diffuse[k] = additive ? 0.0f : 0.5f;
      m.emission[k] = additive ? 1.0f : 0.0f;
      m.specular[k] = 0;
    }
    m.ambient[3] = m.diffuse[3] = m.emission[3] = m.specular[3] = 1;
  }
  auto* t = dynamic_cast<TextureObj*>(e->texture.get());
  slot->textures.assign(1, t ? d.texture(vm, t) : 0);
  slot->cube.assign(1, 0);
  return slot.get();
}

// ---------------------------------------------------------------------------------------------------------------
// the grid's emitter instances (gsGridObjectEffect: one timer per emitter of the template)

struct ModelEffects {
  bool resolved = false;
  std::vector<ParticleEmitterObj*> emitters;  // the template's direct child emitters
  float range = 0;                            // the largest maxDistance
};

void resolveEffects(ModelEffects& m, Object* model) {
  m.resolved = true;
  m.emitters.clear();
  m.range = 0;
  if (!model) return;
  for (auto& c : model->children)
    if (auto* e = dynamic_cast<ParticleEmitterObj*>(c.get()); e && !e->destroyed && !e->contrail) {
      m.emitters.push_back(e);
      m.range = std::max(m.range, e->maxDistance);
    }
}

struct ItemEffects {
  uint32_t item = 0;
  Object* model = nullptr;
  std::vector<float> timers;
};

}  // namespace

struct SimParticles {
  std::vector<ModelEffects> models;              // by SimState::models index
  std::map<const Object*, ModelEffects> itemModels;  // item templates
  struct MoverFx {
    const Object* model = nullptr;
    std::vector<float> timers;  // per unit and emitter
  };
  std::vector<MoverFx> movers;
  std::vector<ItemEffects> items;
  uint64_t itemsGeneration = ~0ull, itemsVersion = ~0ull;
  std::unordered_map<uint64_t, float> sprayTimers;  // port vehicle emitters: (mover << 8 | emitter) → accumulator
  // port tree emitters (gsGrid.regsTreeEmitters): the map's trees, rebuilt when the items change
  struct Tree {
    Vec3 base;
    float height = 0, radius = 0;
    uint32_t kind = 0;  // index into treeNames
  };
  std::vector<Tree> trees;
  std::vector<std::string> treeNames;  // the trees' mesh names
  uint64_t treesGeneration = ~0ull, treesVersion = ~0ull;
  int fireTicks = 0;
  bool fireDone = false;

  ModelEffects& model(const SimState& s, int index) {
    if (models.size() < s.models.size()) models.resize(s.models.size());
    ModelEffects& m = models[size_t(index)];
    if (!m.resolved) resolveEffects(m, s.models[size_t(index)].object);
    return m;
  }
};

namespace {

struct Stats {
  double simMs = 0, collectMs = 0;
  int frames = 0, drawn = 0, emittersNear = 0;
} stats;

ParticleEngineObj* gridEngine(VM& vm, GridObj* grid) { return engineOfScene(sceneOf(vm, grid)); }

// vehicle conditions (L:0x081adca0.c:606-652): agent flags & 0xffff0000 (0x10000 broken down and halted, 0x40000
// crashing) | 1 (GRIDVEHICLE_HELICOPTER_DUST) for a rotor vehicle whose span touches a stop node
uint32_t vehicleConditions(const SimState& s, const Mover& m, const UnitModel& um, bool rotor) {
  uint32_t c = 0;
  const VehicleState* v = m.transit && m.vehicle >= 0 && size_t(m.vehicle) < s.vehicles.size() ? &s.vehicles[size_t(m.vehicle)] : nullptr;
  if (m.crashing) c |= 0x40000;
  if (v && v->brokenHalted) c |= 0x10000;
  // the dust test runs only while the rotor turns: spin r > 5 when halted, broken or crashing (agent flags 0x50400),
  // r > 1 otherwise (:630-660); −1 (not yet set) reads as full speed when moving, still when halted
  bool halted = m.crashing || (v && (v->halted || v->brokenHalted));
  float r = m.rotorSpin < 0 ? (halted ? 0.0f : 12.0f) : m.rotorSpin;
  if (rotor && !m.track.nodes.empty() && r > (halted ? 5.0f : 1.0f)) {
    size_t a = m.track.at(std::max(0.0f, m.dist - um.rearOffset)), b = m.track.at(m.dist + um.frontOffset + um.rearOffset);
    for (size_t k = a; k <= b && k < m.track.nodes.size(); ++k)
      if (s.graph.nodes[size_t(m.track.nodes[k])].flags & kPathStop) {
        c |= 1;
        break;
      }
  }
  return c;
}

// the instanceMove of every emitter of one instance
void runInstance(VM& vm, ParticleEngineObj* eng, const ModelEffects& fx, float* timers, float dt, float eyeDist,
                 const Mat4& place, const Vec3& vel, uint32_t cond) {
  for (size_t k = 0; k < fx.emitters.size(); ++k) {
    ParticleEmitterObj* e = fx.emitters[k];
    if (e->destroyed || !(eyeDist < e->maxDistance) || (e->conditions & cond) != e->conditions) continue;
    ++stats.emittersNear;
    if (int n = emissionCount(vm, e, timers[k], dt)) instanceEmit(vm, eng, e, n, place, vel);
  }
}

void tickVehicles(VM& vm, ParticleEngineObj* eng, SimState& s, SimParticles& sp, const Vec3& eye, float dt) {
  if (sp.movers.size() < s.movers.size()) sp.movers.resize(s.movers.size());
  const bool underground = fieldInt(vm, s.grid, u"viewMode", 0xfe) == 0x180;
  for (size_t id = 0; id < s.movers.size(); ++id) {
    Mover& m = s.movers[id];
    SimParticles::MoverFx& mf = sp.movers[id];
    if (!m.live || m.parked || m.track.nodes.empty() || m.units.empty() || m.units[0].model < 0) {
      mf.model = nullptr;
      continue;
    }
    const Object* head = s.models[size_t(m.units[0].model)].object;
    if (mf.model != head) {  // a new vehicle in this slot: fresh timers
      mf.model = head;
      mf.timers.clear();
    }
    // the cheap range check first: the node under the head against the largest range of the consist's models
    float range = 0;
    for (const Unit& u : m.units)
      if (u.model >= 0) range = std::max(range, sp.model(s, u.model).range);
    if (range <= 0) continue;
    const LaneNode& node = s.graph.nodes[size_t(m.track.nodes[m.track.at(m.dist)])];
    if (underground && !(node.flags & 0x800000)) continue;  // hidden by the underground view
    Vec3 q = node.p0 - eye;
    float reach = range + node.length + 20.0f;
    if (dot(q, q) >= reach * reach) continue;
    bool rotor = m.aircraft || (m.vehicle >= 0 && size_t(m.vehicle) < s.vehicles.size() && s.vehicles[size_t(m.vehicle)].line >= 0 &&
                                size_t(s.vehicles[size_t(m.vehicle)].line) < s.lines.size() &&
                                s.lines[size_t(s.vehicles[size_t(m.vehicle)].line)].type == 4);
    size_t slot = 0, need = 0;
    for (const Unit& u : m.units)
      if (u.model >= 0) need += sp.model(s, u.model).emitters.size();
    if (mf.timers.size() != need) mf.timers.assign(need, 0.0f);
    float r = m.dist;
    for (size_t k = 0; k < m.units.size(); ++k) {
      const Unit& u = m.units[k];
      if (k > 0) r -= unitLength(s, u);
      if (u.model < 0) continue;
      const ModelEffects& fx = sp.model(s, u.model);
      if (fx.emitters.empty()) continue;
      PlacedPose p = unitPose(s, m, u, r);
      Vec3 dv = p.position - eye;
      float dist = std::sqrt(dot(dv, dv));
      uint32_t cond = vehicleConditions(s, m, s.models[size_t(u.model)], rotor);
      // the velocity orient·(0, 0, −3·speed) (L:0x081adca0.c:790-801)
      runInstance(vm, eng, fx, &mf.timers[slot], dt, dist, poseMatrix(p), p.forward * (3.0f * m.speed), cond);
      slot += fx.emitters.size();
    }
  }
}

// Port: mod emitters on moving vehicles (gsGrid.regsVehicleEmitters, an array of emitters; mod-manager.md §4.2):
// each emits behind the rear unit of the moving road and rail vehicles near the camera (not boats, aircraft or
// vehicles in tunnels), its density in particles per second at 10 m/s, scaled with the square of the speed (up to 4×
// at 20 m/s: next to nothing behind slow vehicles); its position is an offset in the vehicle's frame (x right, y up,
// z backwards from the rear end); its direction turns with the vehicle; the owner velocity is the vehicle's. At most
// kMaxSprayVehicles nearest vehicles within the emitter's maxDistance (150 m when 0).
constexpr size_t kMaxSprayVehicles = 24;

void tickVehicleEmitters(VM& vm, ParticleEngineObj* eng, GridObj* grid, SimState& s, SimParticles& sp, const Vec3& eye,
                         float dt) {
  auto f = grid->fieldValues.find(u"regsVehicleEmitters");
  if (f == grid->fieldValues.end()) return;
  std::vector<ParticleEmitterObj*> emitters;
  if (ArrayObj* arr = vm.resolve(f->second).array())
    for (const Value& e : arr->items)
      if (auto* pe = dynamic_cast<ParticleEmitterObj*>(vm.resolve(e).object()); pe && !pe->destroyed && pe->autoemit)
        emitters.push_back(pe);
  if (emitters.empty()) return;
  float range = 0;
  for (auto* e : emitters) range = std::max(range, e->maxDistance > 0 ? e->maxDistance : 150.0f);
  struct Near {
    float d2;
    size_t mover;
  };
  std::vector<Near> near;
  for (size_t id = 0; id < s.movers.size(); ++id) {
    const Mover& m = s.movers[id];
    if (!m.live || m.parked || m.boat || m.aircraft || m.track.nodes.empty() || m.units.empty() || m.speed < 0.5f) continue;
    const LaneNode& node = s.graph.nodes[size_t(m.track.nodes[m.track.at(m.dist)])];
    if (node.flags & 0x800000) continue;  // tunnel
    Vec3 q = node.p0 - eye;
    float d2 = dot(q, q);
    if (d2 < (range + 40.0f) * (range + 40.0f)) near.push_back({d2, id});
  }
  if (near.size() > kMaxSprayVehicles) {
    std::partial_sort(near.begin(), near.begin() + kMaxSprayVehicles, near.end(), [](const Near& a, const Near& b) { return a.d2 < b.d2; });
    near.resize(kMaxSprayVehicles);
  }
  for (const Near& n : near) {
    const Mover& m = s.movers[n.mover];
    // the rear unit's pose
    float r = m.dist;
    size_t last = m.units.size() - 1;
    for (size_t k = 1; k <= last; ++k) r -= unitLength(s, m.units[k]);
    const Unit& u = m.units[last];
    PlacedPose p = unitPose(s, m, u, r);
    Vec3 fwd = render::normalize(p.forward), up{0, 1, 0};
    Vec3 right = render::normalize(render::cross(fwd, up));
    Vec3 rear = p.position - fwd * (0.5f * unitLength(s, u));
    Vec3 dv = rear - eye;
    float dist = std::sqrt(dot(dv, dv));
    Mat4 basis = poseMatrix(p);
    for (size_t k = 0; k < emitters.size(); ++k) {
      ParticleEmitterObj* e = emitters[k];
      if (dist >= (e->maxDistance > 0 ? e->maxDistance : 150.0f)) continue;
      float& timer = sp.sprayTimers[(uint64_t(n.mover) << 8) | k];
      float sp = std::min(2.0f, m.speed / 10.0f);
      timer += dt * e->rate(vm) * sp * sp;
      int count = int(timer);
      if (count <= 0) continue;
      timer -= float(count);
      Vec3 at = rear + right * e->localPos.x + up * e->localPos.y - fwd * e->localPos.z;
      for (int i = 0; i < count && i < 200; ++i) emitParticle(vm, eng, e, at, basis, fwd * m.speed);
    }
  }
}

// gsGridItem::move (L:0x08182b10.c:84-91): the emitters of items at the item centre, turned (orient+2)·90° (the
// mesh's placement without the "b" variants' 45°), with grid.effectConditions
void tickItems(VM& vm, ParticleEngineObj* eng, GridObj* grid, SimParticles& sp, const Vec3& eye, float dt) {
  if (!grid->data) return;
  const GridData& g = *grid->data;
  if (sp.itemsGeneration != grid->generation || sp.itemsVersion != grid->itemsVersion) {
    std::map<uint32_t, std::vector<float>> keep;  // timers of items that stay
    for (ItemEffects& it : sp.items) keep[it.item] = std::move(it.timers);
    sp.items.clear();
    sp.itemsGeneration = grid->generation;
    sp.itemsVersion = grid->itemsVersion;
    std::vector<int> typeFx(g.types.size(), 0);  // 0 unknown, 1 none, 2 has
    std::vector<Object*> typeModel(g.types.size(), nullptr);
    for (size_t i = 0; i < g.items.size(); ++i) {
      const GridData::Item& it = g.items[i];
      if (it.removed || it.type >= g.types.size()) continue;
      int& known = typeFx[it.type];
      if (!known) {
        auto t = grid->templates.find(g.types[it.type]);
        Object* model = t == grid->templates.end() ? nullptr : t->second.get();
        ModelEffects& fx = sp.itemModels[model];
        if (!fx.resolved) resolveEffects(fx, model);
        known = fx.emitters.empty() ? 1 : 2;
        typeModel[it.type] = model;
      }
      if (known != 2) continue;
      ItemEffects e{uint32_t(i), typeModel[it.type], {}};
      auto k = keep.find(uint32_t(i));
      if (k != keep.end()) e.timers = std::move(k->second);
      sp.items.push_back(std::move(e));
    }
    if (debugParticles()) {
      LOG_INFO("particles: %zu items with emitters", sp.items.size());
      for (size_t k = 0; k < sp.items.size() && k < 40; ++k)
        LOG_INFO("particles:   item %u %s", sp.items[k].item, g.types[g.items[sp.items[k].item].type].c_str());
    }
  }
  if (sp.items.empty()) return;
  if (fieldInt(vm, grid, u"viewMode", 0xfe) == 0x180) return;  // the underground view hides the surface items
  uint32_t cond = uint32_t(fieldInt(vm, grid, u"effectConditions", 0));
  for (ItemEffects& it : sp.items) {
    const ModelEffects& fx = sp.itemModels[it.model];
    if (it.item >= g.items.size() || g.items[it.item].removed) continue;
    Vec3 c = itemCentre(vm, grid, int(it.item));
    Vec3 q = c - eye;
    float dist = std::sqrt(dot(q, q));
    if (dist >= fx.range) continue;
    if (it.timers.size() != fx.emitters.size()) it.timers.assign(fx.emitters.size(), 0.0f);
    static const float kCos[4] = {-1, 0, 1, 0}, kSin[4] = {0, -1, 0, 1};
    int o = g.items[it.item].orient & 3;
    Mat4 place = render::translation(c);
    place.at(0, 0) = kCos[o], place.at(0, 2) = kSin[o], place.at(2, 0) = -kSin[o], place.at(2, 2) = kCos[o];
    runInstance(vm, eng, fx, it.timers.data(), dt, dist, place, {}, cond);
  }
}

// Port: mod emitters on trees (gsGrid.regsTreeEmitters, an array of emitters; mod-manager.md §4.2): each emits from
// the crowns of the trees near the camera whose mesh name is in its regsTrees (all trees when empty): density = particles
// per second and tree, from a random point in the crown (45-90 % of the tree's height, within 0.7 of its radius); with
// regsLand its particles land at the tree's base height and lie there. At most kMaxEmittingTrees nearest trees within
// the emitter's maxDistance (200 m when 0); none while the trees are hidden or in the underground view.
constexpr size_t kMaxEmittingTrees = 500;

void collectTrees(VM& vm, GridObj* grid, SimParticles& sp) {
  const GridData& g = *grid->data;
  sp.trees.clear();
  sp.treeNames.clear();
  sp.treesGeneration = grid->generation;
  sp.treesVersion = grid->itemsVersion;
  struct TypeInfo {
    int known = 0;  // 0 unknown, 1 not a tree, 2 tree
    uint32_t kind = 0;
    float height = 0, radius = 0;
  };
  std::vector<TypeInfo> types(g.types.size());
  for (size_t i = 0; i < g.items.size(); ++i) {
    const GridData::Item& it = g.items[i];
    if (it.removed || it.type >= g.types.size()) continue;
    TypeInfo& ti = types[it.type];
    if (!ti.known) {
      ti.known = 1;
      auto t = grid->templates.find(g.types[it.type]);
      auto* o = t == grid->templates.end() ? nullptr : dynamic_cast<ObjectObj*>(t->second.get());
      if (o && o->data && fieldInt(vm, o, u"type", 0) == 0x13) {  // GRID_ITEM_TREE
        const render::MeshData& m = o->data->mesh;
        ti.known = 2;
        ti.height = m.boundsMax[1];
        ti.radius = std::max({std::fabs(m.boundsMin[0]), std::fabs(m.boundsMax[0]), std::fabs(m.boundsMin[2]), std::fabs(m.boundsMax[2])});
        auto n = std::find(sp.treeNames.begin(), sp.treeNames.end(), o->data->name);
        ti.kind = uint32_t(n - sp.treeNames.begin());
        if (n == sp.treeNames.end()) sp.treeNames.push_back(o->data->name);
      }
    }
    if (ti.known != 2 || ti.height <= 0.5f) continue;
    SimParticles::Tree tr;
    tr.base = itemCentre(vm, grid, int(i));
    tr.height = ti.height;
    tr.radius = ti.radius;
    tr.kind = ti.kind;
    sp.trees.push_back(tr);
  }
  if (debugParticles()) {
    LOG_INFO("particles: %zu trees of %zu kinds for the tree emitters", sp.trees.size(), sp.treeNames.size());
    for (const std::string& n : sp.treeNames) LOG_INFO("particles:   tree kind %s", n.c_str());
    std::map<std::pair<int, int>, int> bins;  // the densest 60 m squares (world x/z for the cam:x,z test action)
    for (const SimParticles::Tree& t : sp.trees) ++bins[{int(std::floor(t.base.x / 60.0f)), int(std::floor(t.base.z / 60.0f))}];
    std::vector<std::pair<int, std::pair<int, int>>> top;
    for (auto& [k, n] : bins) top.push_back({n, k});
    std::sort(top.rbegin(), top.rend());
    for (size_t k = 0; k < top.size() && k < 8; ++k)
      LOG_INFO("particles:   %d trees around %d,%d", top[k].first, top[k].second.first * 60 + 30, top[k].second.second * 60 + 30);
  }
}

void tickTreeEmitters(VM& vm, ParticleEngineObj* eng, GridObj* grid, SimParticles& sp, const Vec3& eye, float dt) {
  auto f = grid->fieldValues.find(u"regsTreeEmitters");
  if (f == grid->fieldValues.end() || !grid->data) return;
  std::vector<ParticleEmitterObj*> emitters;
  if (ArrayObj* arr = vm.resolve(f->second).array())
    for (const Value& e : arr->items)
      if (auto* pe = dynamic_cast<ParticleEmitterObj*>(vm.resolve(e).object()); pe && !pe->destroyed && pe->autoemit && pe->rate(vm) > 0)
        emitters.push_back(pe);
  if (emitters.empty()) return;
  int viewMode = fieldInt(vm, grid, u"viewMode", 0xfe);
  if (viewMode == 0x180 || !(viewMode & 0x08)) return;  // underground view, or trees hidden
  if (sp.treesGeneration != grid->generation || sp.treesVersion != grid->itemsVersion) collectTrees(vm, grid, sp);
  if (sp.trees.empty()) return;
  // which tree kinds each emitter takes
  std::vector<std::vector<char>> takes(emitters.size());
  float range = 0;
  for (size_t k = 0; k < emitters.size(); ++k) {
    ParticleEmitterObj* e = emitters[k];
    range = std::max(range, e->maxDistance > 0 ? e->maxDistance : 200.0f);
    takes[k].assign(sp.treeNames.size(), e->trees.empty() ? 1 : 0);
    for (const std::string& n : e->trees)
      for (size_t t = 0; t < sp.treeNames.size(); ++t)
        if (sp.treeNames[t] == n) takes[k][t] = 1;
  }
  struct Near {
    float d2;
    size_t tree;
  };
  std::vector<Near> near;
  const float r2 = range * range;
  for (size_t i = 0; i < sp.trees.size(); ++i) {
    Vec3 q = sp.trees[i].base - eye;
    float d2 = dot(q, q);
    if (d2 < r2) near.push_back({d2, i});
  }
  if (near.size() > kMaxEmittingTrees) {
    std::partial_sort(near.begin(), near.begin() + kMaxEmittingTrees, near.end(), [](const Near& a, const Near& b) { return a.d2 < b.d2; });
    near.resize(kMaxEmittingTrees);
  }
  Mat4 basis;
  for (const Near& n : near) {
    const SimParticles::Tree& tr = sp.trees[n.tree];
    for (size_t k = 0; k < emitters.size(); ++k) {
      ParticleEmitterObj* e = emitters[k];
      if (!takes[k][tr.kind]) continue;
      float maxD = e->maxDistance > 0 ? e->maxDistance : 200.0f;
      if (n.d2 >= maxD * maxD) continue;
      float want = e->rate(vm) * dt + rnd01();  // a Poisson-like count without a timer per tree
      int count = std::min(int(want), 50);
      for (int i = 0; i < count; ++i) {
        float a = rnd01() * kTwoPi, r = std::sqrt(rnd01()) * 0.7f * tr.radius;
        Vec3 at = tr.base + Vec3{std::cos(a) * r, tr.height * rnd(0.45f, 0.9f), std::sin(a) * r};
        if (!emitParticle(vm, eng, e, at, basis, {})) return;
        eng->particles.back().floorY = tr.base.y + 0.05f + 0.5f * e->particleSize;
      }
    }
  }
}

// test switch REGS_TEST_FIRE=<item>: a fire (template fire-medium, as maybeFire places it: one cell before the
// item's corner) at that item after 60 ticks; put the camera there with REGS_DEBUG_CAMERA=<item>
void testFire(VM& vm, GridObj* grid, SimParticles& sp) {
  static const char* f = std::getenv("REGS_TEST_FIRE");
  if (!f || !*f || sp.fireDone || !grid->data || ++sp.fireTicks < 60) return;
  sp.fireDone = true;
  int item = std::atoi(f);
  if (item < 0 || size_t(item) >= grid->data->items.size()) return;
  const GridData::Item& it = grid->data->items[size_t(item)];
  gridEditBegin(vm, grid);
  int fire = gridEditAddItem(vm, grid, "fire-medium", it.x - 1, it.y, it.z - 1, 0, 0x40000000u, -1, false);
  gridEditEnd(vm, grid);
  LOG_INFO("particles: test fire at item %d (%d,%d): new item %d", item, it.x - 1, it.z - 1, fire);
}

}  // namespace

void simParticlesTick(VM& vm, GridObj* grid, SimState& s, float dt) {
  if (particlesOff()) return;
  if (!s.particles) s.particles = std::make_shared<SimParticles>();
  SimParticles& sp = *s.particles;
  testFire(vm, grid, sp);
  if (dt <= 0) return;
  ParticleEngineObj* eng = gridEngine(vm, grid);
  if (!eng || !s.eyeKnown) return;  // the main camera of the last drawn frame
  if (std::getenv("REGS_TEST_FIREWORKS"))  // test switch: the new-year bit (game.script:639-643) stays on
    vm.setMember(Value::heap(VT::Object, grid), u"effectConditions",
                 Value::integer(fieldInt(vm, grid, u"effectConditions", 0) | 1));
  if (debugParticles() && s.followed >= 0 && size_t(s.followed) < s.vehicles.size()) {  // the followed vehicle's rotor
    static int n = 0;
    const VehicleState& v = s.vehicles[size_t(s.followed)];
    if (++n % 30 == 0 && v.mover >= 0 && s.movers[size_t(v.mover)].live) {
      const Mover& m = s.movers[size_t(v.mover)];
      LOG_INFO("particles: followed vehicle %d: halted %d spinUp %d stopTime %.1f rotor %.2f speed %.2f broken %d", s.followed,
               v.halted, v.spinUp, v.stopTime, m.rotorSpin, m.speed, v.brokenHalted);
    }
  }
  auto t0 = std::chrono::steady_clock::now();
  tickVehicles(vm, eng, s, sp, s.eye, dt);
  tickItems(vm, eng, grid, sp, s.eye, dt);
  tickVehicleEmitters(vm, eng, grid, s, sp, s.eye, dt);
  tickTreeEmitters(vm, eng, grid, sp, s.eye, dt);
  stats.simMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void collectParticles(Display& d, VM& vm, Object* scene, const Vec3& eye, const Vec3& forward, const Mat4& viewProj,
                      float halfDiagonal, render::ParticleDrawList& out) {
  ParticleEngineObj* eng = engineOfScene(scene);
  if (!eng || eng->particles.empty() || particlesOff()) return;
  auto t0 = std::chrono::steady_clock::now();
  const float k = 0.25f * halfDiagonal * halfDiagonal;
  Vec3 fwd = render::normalize(forward);
  struct SysInfo {
    int batch = -1;  // −2: hidden
    float maxDistance = 0, stretch = 0;
  };
  std::vector<SysInfo> sys(eng->systems.size());
  std::map<render::GpuMaterial*, uint32_t> batchOf;
  std::vector<std::pair<float, uint32_t>> order;  // (squared distance, sprite)
  out.sprites.reserve(eng->particles.size());
  for (const Particle& p : eng->particles) {
    SysInfo& si = sys[p.system];
    if (si.batch == -1) {
      auto* e = static_cast<ParticleEmitterObj*>(eng->systems[p.system].get());
      if (e->destroyed || !e->visible()) {
        si.batch = -2;
      } else {
        render::GpuMaterial* m = batchMaterial(d, vm, e);
        auto [it, fresh] = batchOf.emplace(m, uint32_t(out.batches.size()));
        if (fresh) out.batches.push_back({m, materialFlags(e->blend) & 0x180u});
        si.batch = int(it->second);
        si.maxDistance = e->maxDistance;
        si.stretch = e->stretch;
      }
    }
    if (si.batch < 0) continue;
    Vec3 dv = p.pos - eye;
    float d2 = dot(dv, dv);
    if (dot(dv, fwd) <= 0) continue;  // behind the camera (the side test of render, L:0x08219360)
    // size over age (L:0x0822cd90): growth from growStart to growEnd
    float size = p.size;
    if (p.age > p.growStart) size = (1 + (std::min(p.age, p.growEnd) - p.growStart) * p.growth) * p.size;
    float s2 = size * size;
    if (!(s2 < k * d2)) continue;  // larger than half the view's half-diagonal
    Vec3 axis = si.stretch != 0 ? p.vel * si.stretch : Vec3{};  // port streaks
    float reach = std::max(size, std::sqrt(dot(axis, axis)));
    // the view frustum with the billboard's radius (port: the original sends every particle to the blend queue)
    float c[4];
    for (int r = 0; r < 4; ++r)
      c[r] = viewProj.at(r, 0) * p.pos.x + viewProj.at(r, 1) * p.pos.y + viewProj.at(r, 2) * p.pos.z + viewProj.at(r, 3);
    float marginW = c[3] + reach * 1.5f * std::max(1.0f, halfDiagonal);
    if (c[0] < -marginW - reach || c[0] > marginW + reach || c[1] < -marginW - reach || c[1] > marginW + reach) continue;
    // alpha (renderParticle L:0x08218ee0): min(1, k·d²/size² − 1), faded over the last tenth of maxDistance
    float a = std::min(1.0f, k * d2 / s2 - 1);
    if (si.maxDistance != 0) {
      float dist = std::sqrt(d2);
      if (dist >= si.maxDistance) continue;
      a = std::min(a, std::min(1.0f, (si.maxDistance - dist) / si.maxDistance * 10.0f));
    }
    if (p.landed) a = std::min(a, 3.0f * (1.0f - p.age / p.life));  // a landed particle fades over its last third
    uint32_t col = multAlpha(lerpArgb(p.c0, p.c1, std::clamp(p.age / p.life, 0.0f, 1.0f)), a);
    if ((col >> 24) == 0) continue;
    static const bool marked = std::getenv("REGS_DEBUG_PARTICLES") && std::atoi(std::getenv("REGS_DEBUG_PARTICLES")) == 2;
    if (marked) col = 0xFFFF00FF, size *= 3;  // debugging: opaque magenta, three times the size
    order.push_back({d2, uint32_t(out.sprites.size())});
    out.sprites.push_back({p.pos, size, p.orient, col, uint32_t(si.batch), axis});
  }
  // the blend queue's order: far to near
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<render::ParticleSprite> sorted;
  sorted.reserve(order.size());
  for (const auto& [d2, i] : order) sorted.push_back(out.sprites[i]);
  out.sprites = std::move(sorted);
  stats.collectMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  stats.drawn += int(out.sprites.size());
  if (debugParticles() && ++stats.frames % 300 == 0) {
    LOG_INFO("particles: %zu live, %zu systems, %d drawn/frame, %d emitters near/frame, sim %.3f ms, collect %.3f ms, "
             "%zu dropped at the cap",
             eng->particles.size(), eng->systems.size(), stats.drawn / 300, stats.emittersNear / 300, stats.simMs / 300,
             stats.collectMs / 300, eng->dropped);
    stats = Stats{};
  }
}

void prepareParticlePrograms(Display& d, VM& vm, render::ParticleDrawList& list, uint32_t env) {
  for (render::ParticleBatch& b : list.batches) {
    uint32_t e = render::ParticleDrawList::envFor(b, env);
    if (b.material && !b.material->hasProgramFor(e)) {
      b.material->setProgram(e, combinerProgram(d, vm, kParticleShader, b.material->flags & ~render::kMatTwoSided, e));
      if (debugParticles())
        LOG_INFO("particles: material 0x%x env 0x%x: program %u, texture %u", b.material->flags, e, b.material->programFor(e),
                 b.material->textures.empty() ? 0 : b.material->textures[0]);
    }
  }
}

void registerParticleNatives(VM& vm) {
  const ClassInfo* emittingClass = vm.findClass("gsEmittingParticleEmitter");
  const ClassInfo* contrailClass = vm.findClass("gsBoatContrain");
  if (ClassInfo* c = vm.findClass("gsParticleEmitter"))
    c->factory = [emittingClass, contrailClass](const ClassInfo* ci) {
      auto* e = new ParticleEmitterObj(ci);
      e->emitting = emittingClass && ci->isA(emittingClass);
      e->contrail = contrailClass && ci->isA(contrailClass);
      return Ref<Object>(e);
    };
  if (ClassInfo* c = vm.findClass("gsParticleEngine")) {
    c->factory = [](const ClassInfo* ci) { return Ref<Object>(new ParticleEngineObj(ci)); };
    FieldInfo& ts = c->fields[u"timeSpeed"];
    ts.get = [](VM&, Object* o) { return Value::real(static_cast<ParticleEngineObj*>(o)->timeSpeed); };
    ts.set = [](VM& v, Object* o, const Value& x) { static_cast<ParticleEngineObj*>(o)->timeSpeed = v.toFloat(v.resolve(x)); };
    FieldInfo& w = c->fields[u"windVelocity"];
    w.get = [](VM&, Object* o) { return vecValue(static_cast<ParticleEngineObj*>(o)->wind); };
    w.set = [](VM& v, Object* o, const Value& x) { static_cast<ParticleEngineObj*>(o)->wind = vecOf(v.resolve(x)); };
  }
  // port: gsGrid.regsVehicleEmitters (tickVehicleEmitters); the listed emitters stop emitting at their own place
  if (ClassInfo* g = vm.findClass("gsGrid")) {
    FieldInfo& f = g->fields[u"regsVehicleEmitters"];
    f.name = u"regsVehicleEmitters";
    f.get = [](VM&, Object* o) {
      auto it = o->fieldValues.find(u"regsVehicleEmitters");
      return it == o->fieldValues.end() ? Value::null() : it->second;
    };
    f.set = [](VM& v, Object* o, const Value& x) {
      Value r = v.resolve(x);
      o->fieldValues[u"regsVehicleEmitters"] = r;
      if (ArrayObj* arr = r.array())
        for (const Value& e : arr->items)
          if (auto* pe = dynamic_cast<ParticleEmitterObj*>(v.resolve(e).object())) pe->vehicleOnly = true;
    };
  }
  // port: gsGrid.regsTreeEmitters (tickTreeEmitters); the listed emitters stop emitting at their own place too
  if (ClassInfo* g = vm.findClass("gsGrid")) {
    FieldInfo& f = g->fields[u"regsTreeEmitters"];
    f.name = u"regsTreeEmitters";
    f.get = [](VM&, Object* o) {
      auto it = o->fieldValues.find(u"regsTreeEmitters");
      return it == o->fieldValues.end() ? Value::null() : it->second;
    };
    f.set = [](VM& v, Object* o, const Value& x) {
      Value r = v.resolve(x);
      o->fieldValues[u"regsTreeEmitters"] = r;
      if (ArrayObj* arr = r.array())
        for (const Value& e : arr->items)
          if (auto* pe = dynamic_cast<ParticleEmitterObj*>(v.resolve(e).object())) pe->vehicleOnly = true;
    };
  }
  ClassInfo* c = vm.findClass("gsParticleEmitter");
  if (!c) return;
  auto E = [](Object* o) { return dynamic_cast<ParticleEmitterObj*>(o); };
  auto floatField = [&](const char16_t* name, float ParticleEmitterObj::*m) {
    FieldInfo& f = c->fields[name];
    f.get = [m, E](VM& v, Object* o) { auto* e = E(o); return e ? Value::real(e->*m) : Value::real(0); };
    f.set = [m, E](VM& v, Object* o, const Value& x) { if (auto* e = E(o)) e->*m = v.toFloat(v.resolve(x)); };
  };
  auto boolField = [&](const char16_t* name, bool ParticleEmitterObj::*m) {
    FieldInfo& f = c->fields[name];
    f.get = [m, E](VM&, Object* o) { auto* e = E(o); return Value::boolean(e && e->*m); };
    f.set = [m, E](VM& v, Object* o, const Value& x) { if (auto* e = E(o)) e->*m = v.truthy(v.resolve(x)); };
  };
  auto vecField = [&](const char16_t* name, Vec3 ParticleEmitterObj::*m, bool normalise) {
    FieldInfo& f = c->fields[name];
    f.get = [m, E](VM&, Object* o) { auto* e = E(o); return e ? vecValue(e->*m) : Value::vector(0, 0, 0); };
    f.set = [m, E, normalise](VM& v, Object* o, const Value& x) {
      if (auto* e = E(o)) e->*m = normalise ? render::normalize(vecOf(v.resolve(x))) : vecOf(v.resolve(x));
    };
  };
  boolField(u"active", &ParticleEmitterObj::active);
  boolField(u"autoemit", &ParticleEmitterObj::autoemit);
  boolField(u"sortParticles", &ParticleEmitterObj::sortParticles);
  floatField(u"planeSize", &ParticleEmitterObj::planeSize);
  floatField(u"particleSize", &ParticleEmitterObj::particleSize);
  floatField(u"particleAirResist", &ParticleEmitterObj::airResist);
  floatField(u"density", &ParticleEmitterObj::density);
  floatField(u"ownerVelocity", &ParticleEmitterObj::ownerVelocity);
  floatField(u"maxDistance", &ParticleEmitterObj::maxDistance);
  floatField(u"minParticleSpeed", &ParticleEmitterObj::minSpeed);
  floatField(u"maxParticleSpeed", &ParticleEmitterObj::maxSpeed);
  floatField(u"minParticleAngle", &ParticleEmitterObj::minAngle);
  floatField(u"maxParticleAngle", &ParticleEmitterObj::maxAngle);
  floatField(u"minParticleOrient", &ParticleEmitterObj::minOrient);
  floatField(u"maxParticleOrient", &ParticleEmitterObj::maxOrient);
  floatField(u"minParticleLife", &ParticleEmitterObj::minLife);
  floatField(u"maxParticleLife", &ParticleEmitterObj::maxLife);
  floatField(u"minOnTime", &ParticleEmitterObj::minOn);
  floatField(u"maxOnTime", &ParticleEmitterObj::maxOn);
  floatField(u"minOffTime", &ParticleEmitterObj::minOff);
  floatField(u"maxOffTime", &ParticleEmitterObj::maxOff);
  vecField(u"gravitation", &ParticleEmitterObj::gravitation, false);
  vecField(u"direction", &ParticleEmitterObj::direction, true);  // setDirection normalises (L:0x08175440)
  vecField(u"regsEmitBox", &ParticleEmitterObj::emitBox, false);  // port fields (see ParticleEmitterObj)
  floatField(u"regsStretch", &ParticleEmitterObj::stretch);
  floatField(u"regsLand", &ParticleEmitterObj::land);
  floatField(u"regsFlutter", &ParticleEmitterObj::flutter);
  {  // regsTrees: an array of tree mesh names (strings)
    FieldInfo& f = c->fields[u"regsTrees"];
    f.name = u"regsTrees";
    f.get = [](VM&, Object* o) {
      auto it = o->fieldValues.find(u"regsTrees");
      return it != o->fieldValues.end() ? it->second : Value::null();
    };
    f.set = [E](VM& v, Object* o, const Value& x) {
      auto* e = E(o);
      if (!e) return;
      Value r = v.resolve(x);
      o->fieldValues[u"regsTrees"] = r;
      e->trees.clear();
      if (ArrayObj* arr = r.array())
        for (const Value& n : arr->items) e->trees.push_back(utf16ToUtf8(v.toStr(v.resolve(n))));
    };
  }
  {
    FieldInfo& f = c->fields[u"particleBlend"];
    f.get = [E](VM&, Object* o) { auto* e = E(o); return Value::integer(e ? e->blend : 1); };
    f.set = [E](VM& v, Object* o, const Value& x) { if (auto* e = E(o)) e->blend = v.toInt(v.resolve(x)); };
    FieldInfo& k = c->fields[u"conditions"];
    k.get = [E](VM&, Object* o) { auto* e = E(o); return Value::integer(int32_t(e ? e->conditions : 0)); };
    k.set = [E](VM& v, Object* o, const Value& x) { if (auto* e = E(o)) e->conditions = uint32_t(v.toInt(v.resolve(x))); };
    // position (relative to the parent) and globalPos (gs3DElement)
    FieldInfo& p = c->fields[u"position"];
    p.name = u"position", p.type = 7;
    p.get = [E](VM&, Object* o) { auto* e = E(o); return e ? vecValue(e->localPos) : Value::vector(0, 0, 0); };
    p.set = [E](VM& v, Object* o, const Value& x) { if (auto* e = E(o)) e->localPos = vecOf(v.resolve(x)); };
    FieldInfo& g = c->fields[u"globalPos"];
    g.name = u"globalPos", g.type = 7;
    g.get = [E](VM& v, Object* o) { auto* e = E(o); return e ? vecValue(e->globalPos(v)) : Value::vector(0, 0, 0); };
    g.set = [E](VM& v, Object* o, const Value& x) {
      if (auto* e = E(o)) e->localPos = vecOf(v.resolve(x)) - e->parentGlobal(v);
    };
  }
  // addColorRange(start, end) (L:0x0822bea0): one of the ranges is picked per particle
  implement(vm, "gsParticleEmitter", u"addColorRange", [E](VM& v, Object* s, int argc, const Value* a) {
    if (auto* e = E(s); e && argc >= 2) e->colors.push_back({uint32_t(v.toInt(a[0])), uint32_t(v.toInt(a[1]))});
    return Value();
  });
  // addEmitPosition(vector, [relative = true]) (L:0x0822c090)
  implement(vm, "gsParticleEmitter", u"addEmitPosition", [E](VM& v, Object* s, int argc, const Value* a) {
    if (auto* e = E(s); e && argc >= 1) e->emitPositions.push_back({vecOf(v.resolve(a[0])), argc < 2 || v.truthy(a[1])});
    return Value();
  });
  // setParticleScale(growthRate, growStart, growEnd) (W:0x1005b9f0)
  implement(vm, "gsParticleEmitter", u"setParticleScale", [E](VM& v, Object* s, int argc, const Value* a) {
    if (auto* e = E(s); e && argc >= 3) e->growth = v.toFloat(a[0]), e->growStart = v.toFloat(a[1]), e->growEnd = v.toFloat(a[2]);
    return Value();
  });
  // add{Density,Speed,Alpha}Term(weight, variable) (L:0x0822c440 ...): weight × the variable's current value is added
  // to the density, to each particle's speed, or multiplies the colours' alpha
  auto term = [E](std::vector<ParticleEmitterObj::Term> ParticleEmitterObj::*list) {
    return [E, list](VM& v, Object* s, int argc, const Value* a) {
      if (auto* e = E(s); e && argc >= 2) (e->*list).push_back({v.toFloat(a[0]), a[1]});
      return Value();
    };
  };
  implement(vm, "gsParticleEmitter", u"addDensityTerm", term(&ParticleEmitterObj::densityTerms));
  implement(vm, "gsParticleEmitter", u"addSpeedTerm", term(&ParticleEmitterObj::speedTerms));
  implement(vm, "gsParticleEmitter", u"addAlphaTerm", term(&ParticleEmitterObj::alphaTerms));
  // instantEmit([count = 1]) (L:0x0822c720)
  implement(vm, "gsParticleEmitter", u"instantEmit", [E](VM& v, Object* s, int argc, const Value* a) {
    if (auto* e = E(s); e && !particlesOff()) instantEmit(v, e, argc >= 1 ? v.toInt(a[0]) : 1);
    return Value();
  });
  // setTexture(path) (L:0x0822df60): the raw path to the texture cache
  implement(vm, "gsParticleEmitter", u"setTexture", [E](VM& v, Object* s, int argc, const Value* a) {
    if (auto* e = E(s); e && argc >= 1) e->texture = getTexture(v, v.toStr(a[0]));
    return Value();
  });
}

}  // namespace regs::engine
