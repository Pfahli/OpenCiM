// Engine-driven world sounds (docs/audio/README.md §11): vehicle engine / move / tunnel loops, horns, transport bells,
// stop announcements and doors, animal voices, item ambient loops, gsGridVehicleController.playSound. The original
// clones the model's template gsSound under the grid for each play (createInstance); the port plays the template's
// sound as a native world voice (engine/sound.h) and keeps the per-agent slots here. Range: the listener (camera eye)
// distance stands in for the bucket's eyeDist; agents farther than every sound range of their model cost one distance
// check per tick.
#include <algorithm>
#include <cmath>
#include <random>

#include "core/log.h"
#include "engine/api.h"
#include "engine/sim.h"
#include "engine/sim_state.h"
#include "engine/sim_sounds.h"
#include "engine/sound.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::engine::sim;
using render::Vec3;

namespace {

// a model's template sounds (gsGridObject fields, §2.4), resolved on first use
struct ModelSounds {
  bool resolved = false;
  bool engine = false, move = false, tunnel = false, horn = false, bell = false, doorOpen = false, doorClose = false;
  SoundTemplate engineT, moveT, tunnelT, hornT, bellT, doorOpenT, doorCloseT;
  float range = 0;  // the largest maxDistance of the looping and horn sounds
};

struct OneShot {
  int voice = 0;
  SoundTemplate t;
  float pitchMult = 1;
};

struct MoverSound {
  int loop = 0;           // the engine / move slot (+0x38)
  bool loopTunnel = false;  // vehicle flag 0x100: the tunnel variant plays
  const ObjectObj* loopModel = nullptr;
  float accel = 0, lastSpeed = 0;  // accelSmoothed (grid-traffic.md §6.2)
  bool seen = false;
  std::vector<OneShot> shots;     // the agent's sound list (+0x34): one-shots that follow it
};

struct ItemSound {
  uint32_t item = 0;
  Vec3 centre;
  SoundTemplate t;
  int voice = 0;
  float outOfRange = 0;
};

bool isForced(const Object* ctl) {
  if (!ctl) return false;
  auto it = ctl->fieldValues.find(u"forceSounds");
  return it != ctl->fieldValues.end() && it->second.toBool();
}

Object* templateSound(const Object* model, const char16_t* field) {
  if (!model) return nullptr;
  auto it = model->fieldValues.find(field);
  return it == model->fieldValues.end() ? nullptr : it->second.object();
}

}  // namespace

struct SimSounds {
  std::vector<ModelSounds> models;
  std::vector<MoverSound> movers;
  std::vector<OneShot> loose;  // one-shots not following an agent (announcements at stops, animals)
  std::vector<ItemSound> items;
  uint64_t itemsGeneration = ~0ull, itemsVersion = ~0ull;
  std::minstd_rand rng{12345};  // the port's own stream: the simulation's rand() sequence stays untouched
  float rnd(float a, float b) { return a + (b - a) * float(rng() - rng.min()) / float(rng.max() - rng.min()); }

  ~SimSounds() {
    for (MoverSound& m : movers) {
      worldSoundStop(m.loop, "simulation ends");
      for (OneShot& o : m.shots) worldSoundStop(o.voice, "simulation ends");
    }
    for (OneShot& o : loose) worldSoundStop(o.voice, "simulation ends");
    for (ItemSound& it : items) worldSoundStop(it.voice, "simulation ends");
  }

  const ModelSounds& model(const SimState& s, int index) {
    if (index < 0) {
      static const ModelSounds none{true};
      return none;
    }
    if (models.size() < s.models.size()) models.resize(s.models.size());
    ModelSounds& m = models[size_t(index)];
    if (m.resolved) return m;
    m.resolved = true;
    const ObjectObj* o = s.models[size_t(index)].object;
    m.engine = soundTemplate(templateSound(o, u"engineSound"), m.engineT);
    m.move = soundTemplate(templateSound(o, u"moveSound"), m.moveT);
    m.tunnel = soundTemplate(templateSound(o, u"moveTunnelSound"), m.tunnelT);
    m.horn = soundTemplate(templateSound(o, u"hornSound"), m.hornT);
    m.bell = soundTemplate(templateSound(o, u"bellSound"), m.bellT);
    m.doorOpen = soundTemplate(templateSound(o, u"doorOpenSound"), m.doorOpenT);
    m.doorClose = soundTemplate(templateSound(o, u"doorCloseSound"), m.doorCloseT);
    for (auto [has, t] : {std::pair{m.engine, &m.engineT}, {m.move, &m.moveT}, {m.tunnel, &m.tunnelT}, {m.horn, &m.hornT},
                          {m.bell, &m.bellT}, {m.doorOpen, &m.doorOpenT}, {m.doorClose, &m.doorCloseT}})
      if (has) m.range = std::max(m.range, t->maxDistance);
    return m;
  }
};

namespace {

SimSounds& soundsOf(SimState& s) {
  if (!s.sounds) s.sounds = std::make_shared<SimSounds>();
  return *s.sounds;
}

Object* controllerOf(SimState& s, const Mover& m) {
  if (m.transit) return m.vehicle >= 0 && size_t(m.vehicle) < s.vehicles.size() ? s.vehicles[size_t(m.vehicle)].object.get() : nullptr;
  return m.controller.get();
}

void releaseMover(MoverSound& ms, const char* why) {
  worldSoundStop(ms.loop, why);
  ms.loop = 0;
  ms.loopModel = nullptr;
  for (OneShot& o : ms.shots) worldSoundStop(o.voice, why);
  ms.shots.clear();
  ms.accel = ms.lastSpeed = 0;
  ms.seen = false;
}

// gsGridVehicle::playSound (L:0x081adaa0): only in range (or forced); the clone follows the vehicle
void playAtMover(SimState& s, SimSounds& ss, int id, const SoundTemplate& t, float pitch, const char* what) {
  Mover& m = s.movers[size_t(id)];
  if (!m.live || m.track.nodes.empty() || m.units.empty() || m.units[0].model < 0) return;
  Vec3 eye;
  if (!soundListener(eye)) return;
  bool forced = isForced(controllerOf(s, m));
  PlacedPose p = unitPose(s, m, m.units[0], m.dist);
  Vec3 d = p.position - eye;
  if (!forced && dot(d, d) >= t.maxDistance * t.maxDistance) return;
  Vec3 vel = p.forward * (3.0f * m.speed);
  int v = worldSoundPlay(s.grid, t, p.position, vel, forced, false, pitch, 1.0f, what);
  if (!v) return;
  if (ss.movers.size() < s.movers.size()) ss.movers.resize(s.movers.size());
  ss.movers[size_t(id)].shots.push_back({v, t, pitch});
}

void tickMovers(VM& vm, SimState& s, SimSounds& ss, const Vec3& eye, float dt) {
  if (ss.movers.size() < s.movers.size()) ss.movers.resize(s.movers.size());
  const bool underground = fieldInt(vm, s.grid, u"viewMode", 0xfe) == 0x180;
  for (size_t id = 0; id < s.movers.size(); ++id) {
    Mover& m = s.movers[id];
    MoverSound& ms = ss.movers[id];
    if (!m.live || m.parked || m.track.nodes.empty() || m.units.empty() || m.units[0].model < 0) {
      if (ms.seen || ms.loop || !ms.shots.empty()) releaseMover(ms, "vehicle gone");
      continue;
    }
    const ModelSounds& M = ss.model(s, m.units[0].model);
    const ObjectObj* modelObj = s.models[size_t(m.units[0].model)].object;
    if (ms.seen && ms.loopModel && ms.loopModel != modelObj) releaseMover(ms, "mover reused");
    if (!ms.seen) {
      ms.seen = true;
      ms.lastSpeed = m.speed;
    }
    // accelSmoothed += clamp((speed − vOld)/dt − accelSmoothed, −4dt, 4dt) (0x1010de50.c:364-385)
    if (dt > 0) {
      ms.accel += std::clamp((m.speed - ms.lastSpeed) / dt - ms.accel, -4 * dt, 4 * dt);
      ms.lastSpeed = m.speed;
    }
    if (M.range <= 0 && ms.shots.empty()) continue;
    Object* ctl = controllerOf(s, m);
    bool forced = isForced(ctl);
    size_t at = m.track.at(m.dist);
    const LaneNode& node = s.graph.nodes[size_t(m.track.nodes[at])];
    Vec3 q = node.p0 - eye;
    float reach = M.range + node.length + 20.0f;
    bool near = forced || dot(q, q) < reach * reach;
    if (!near) {  // far: no loop; its one-shots go on where they are until they end
      if (ms.loop) worldSoundStop(ms.loop, "out of range"), ms.loop = 0, ms.loopModel = nullptr;
      std::erase_if(ms.shots, [](const OneShot& o) { return !worldSoundAlive(o.voice); });
      continue;
    }
    PlacedPose p = unitPose(s, m, m.units[0], m.dist);
    Vec3 vel = p.forward * (3.0f * m.speed);  // sound.velocity = orient·(0, 0, −3·speed) (L:0x081adca0.c:773-786)
    Vec3 dd = p.position - eye;
    float d = std::sqrt(dot(dd, dd));
    bool tunnelNode = (node.flags & 0x800000) != 0;
    // horn (:470-483): stuck for more than 5 s, in range, rand(0, 50) < dt
    if (M.horn && m.waitTime > 5 && (forced || d < M.hornT.maxDistance) && ss.rnd(0, 50) < dt)
      playAtMover(s, ss, int(id), M.hornT, ss.rnd(0.8f, 1.0f), "horn");
    // the engine or move loop (:487-600)
    const SoundTemplate* want = nullptr;
    bool wantTunnel = false;
    float volM = 1, pitM = 1;
    bool rotor = m.aircraft || (m.vehicle >= 0 && size_t(m.vehicle) < s.vehicles.size() && s.vehicles[size_t(m.vehicle)].line >= 0 &&
                                size_t(s.vehicles[size_t(m.vehicle)].line) < s.lines.size() &&
                                s.lines[size_t(s.vehicles[size_t(m.vehicle)].line)].type == 4);
    if (M.engine) {
      want = &M.engineT;
      if (rotor) {  // the rotor spin-up r (0..12, Mover::rotorSpin, shared with the rotor animation; :604-697)
        float r = std::max(0.0f, m.rotorSpin);
        volM = std::clamp(ms.accel, 0.4f, std::max(0.4f, std::min(1.0f, 0.2f * r)));
        pitM = std::clamp(0.1f * r, 0.4f, 1.0f);
      } else {
        volM = std::clamp(ms.accel, 0.4f, 1.0f);
        pitM = std::clamp(m.speed * 0.2f, 0.4f, 1.0f);
      }
      // hidden by the underground view unless on a tunnel node
      if (underground && !tunnelNode && !forced) want = nullptr;
    } else if (M.move || M.tunnel) {
      wantTunnel = tunnelNode && M.tunnel;
      want = wantTunnel ? &M.tunnelT : M.move ? &M.moveT : nullptr;
      volM = std::min(m.speed * 0.2f, 1.0f);
      pitM = std::min(m.speed * 0.1f + 0.5f, 1.0f);
    }
    if (want && !forced && d >= want->maxDistance) want = nullptr;
    if (ms.loop && (!want || !worldSoundAlive(ms.loop) || wantTunnel != ms.loopTunnel)) {
      worldSoundStop(ms.loop, !want ? "out of range" : "swapped");
      ms.loop = 0;
      ms.loopModel = nullptr;
    }
    if (want) {
      if (!ms.loop) {
        ms.loop = worldSoundPlay(s.grid, *want, p.position, vel, forced, true, pitM, volM, M.engine ? "engine" : wantTunnel ? "tunnel" : "move");
        ms.loopTunnel = wantTunnel;
        ms.loopModel = modelObj;
      } else {
        worldSoundUpdate(ms.loop, *want, p.position, vel, forced, pitM, volM);
      }
    }
    // the sound list (:756-790): finished ones go, the others follow the vehicle
    for (size_t k = 0; k < ms.shots.size();) {
      OneShot& o = ms.shots[k];
      if (!worldSoundAlive(o.voice)) {
        ms.shots.erase(ms.shots.begin() + long(k));
        continue;
      }
      worldSoundUpdate(o.voice, o.t, p.position, vel, forced, o.pitchMult, 1.0f);
      ++k;
    }
  }
}

// gsGridCharacter::move (L:0x08153b90.c:240-270): an idle character whose model has a hornSound (the cow and the pig)
// says something with probability dt/5 per update when in range (or forced)
void tickCharacters(SimState& s, SimSounds& ss, const Vec3& eye, float dt) {
  if (dt <= 0) return;
  for (const Character& ch : s.characters) {
    if (!ch.alive || ch.virtualLeft >= 0 || ch.model < 0 || ch.anim.current != "idle") continue;
    const ModelSounds& M = ss.model(s, ch.model);
    if (!M.horn) continue;
    bool forced = isForced(ch.controller.get());
    Vec3 d = ch.pos - eye;
    if (!forced && dot(d, d) >= M.hornT.maxDistance * M.hornT.maxDistance) continue;
    if (ss.rnd(0, 5) >= dt) continue;
    if (int v = worldSoundPlay(s.grid, M.hornT, ch.pos, Vec3{}, forced, false, ss.rnd(0.8f, 1.0f), 1.0f, "voice"))
      ss.loose.push_back({v, M.hornT, 1});
  }
}

// gsGridItem::move (L:0x08182b10.c:122-140): the template's engineSound loops at the item centre while in range
void tickItems(VM& vm, GridObj* grid, SimSounds& ss, const Vec3& eye, float dt) {
  if (!grid->data) return;
  if (ss.itemsGeneration != grid->generation || ss.itemsVersion != grid->itemsVersion) {
    for (ItemSound& it : ss.items) worldSoundStop(it.voice, "map changed");
    ss.items.clear();
    ss.itemsGeneration = grid->generation;
    ss.itemsVersion = grid->itemsVersion;
    const GridData& g = *grid->data;
    std::vector<int> typeSound(g.types.size(), 0);  // 0 unknown, 1 none, 2 has
    std::vector<SoundTemplate> typeT(g.types.size());
    for (size_t i = 0; i < g.items.size(); ++i) {
      const GridData::Item& it = g.items[i];
      if (it.removed || it.type >= g.types.size()) continue;
      int& known = typeSound[it.type];
      if (!known) {
        auto t = grid->templates.find(g.types[it.type]);
        known = t != grid->templates.end() && soundTemplate(templateSound(t->second.get(), u"engineSound"), typeT[it.type]) ? 2 : 1;
      }
      if (known == 2) ss.items.push_back({uint32_t(i), itemCentre(vm, grid, int(i)), typeT[it.type]});
    }
    if (soundDebug()) LOG_INFO("audio: %zu items with an ambient loop", ss.items.size());
  }
  for (ItemSound& it : ss.items) {
    Vec3 q = it.centre - eye;
    float d = std::sqrt(dot(q, q));
    if (it.voice && !worldSoundAlive(it.voice)) it.voice = 0;
    if (!it.voice) {
      if (d < it.t.maxDistance) {
        it.voice = worldSoundPlay(grid, it.t, it.centre, Vec3{}, false, true, 1.0f, 1.0f, "item loop");
        it.outOfRange = 0;
      }
      continue;
    }
    // the clone's own out-of-range stop (0.5 s, §6.5); far beyond, the bucket is inactive and frees it at once
    if (d <= it.t.maxDistance) it.outOfRange = 0;
    else if ((it.outOfRange += dt) >= 0.5f || d > it.t.maxDistance + 100.0f) worldSoundStop(it.voice, "out of range"), it.voice = 0;
  }
}

}  // namespace

void simSoundsTick(VM& vm, GridObj* grid, SimState& s, float dt) {
  if (!soundsEnabled()) return;
  Vec3 eye;
  if (!soundListener(eye)) return;
  SimSounds& ss = soundsOf(s);
  tickMovers(vm, s, ss, eye, dt);
  tickCharacters(s, ss, eye, dt);
  tickItems(vm, grid, ss, eye, dt);
  std::erase_if(ss.loose, [](const OneShot& o) { return !worldSoundAlive(o.voice); });
}

void simSoundVehicleEvent(VM& vm, SimState& s, int vi, SimSoundEvent e) {
  if (!soundsEnabled() || vi < 0 || size_t(vi) >= s.vehicles.size()) return;
  VehicleState& v = s.vehicles[size_t(vi)];
  if (v.mover < 0 || v.units.empty() || v.units[0].model < 0) return;
  SimSounds& ss = soundsOf(s);
  const ModelSounds& M = ss.model(s, v.units[0].model);
  switch (e) {
    case SimSoundEvent::Announce: {  // the stop item's template bellSound, at the stop (transport.md §4.3, :221-242)
      if (v.line < 0 || size_t(v.line) >= s.lines.size() || v.stopB < 0) return;
      const LineState& line = s.lines[size_t(v.line)];
      if (size_t(v.stopB) >= line.stops.size()) return;
      int item = line.stops[size_t(v.stopB)].item;
      if (item < 0 || !s.grid->data || size_t(item) >= s.grid->data->items.size()) return;
      const GridData& g = *s.grid->data;
      auto t = s.grid->templates.find(g.types[g.items[size_t(item)].type]);
      SoundTemplate st;
      if (t == s.grid->templates.end() || !soundTemplate(templateSound(t->second.get(), u"bellSound"), st)) return;
      Vec3 eye, at = itemCentre(vm, s.grid, item);
      bool forced = isForced(v.object.get());
      if (!soundListener(eye)) return;
      Vec3 d = at - eye;
      if (!forced && dot(d, d) >= st.maxDistance * st.maxDistance) return;
      if (int voice = worldSoundPlay(s.grid, st, at, Vec3{}, forced, false, 1.0f, 1.0f, "announcement")) ss.loose.push_back({voice, st, 1});
      return;
    }
    case SimSoundEvent::Bell:
      if (M.bell) playAtMover(s, ss, v.mover, M.bellT, 1.0f, "bell");
      return;
    case SimSoundEvent::DoorOpen:
      if (M.doorOpen) playAtMover(s, ss, v.mover, M.doorOpenT, 1.0f, "door open");
      return;
    case SimSoundEvent::DoorClose:
      if (M.doorClose) playAtMover(s, ss, v.mover, M.doorCloseT, 1.0f, "door close");
      return;
  }
}

void registerSimSoundNatives(VM& vm) {
  // gsGridVehicleController.playSound(s) (W:0x10107b60): gsGridVehicle::playSound(s, false, 1.0)
  implement(vm, "gsGridVehicleController", u"playSound", [](VM& v, Object* self, int argc, const Value* a) {
    if (argc < 1 || !soundsEnabled()) return Value();
    Value x = a[0].ref() ? a[0].ref()->get() : a[0];
    SoundTemplate t;
    if (!soundTemplate(x.object(), t)) return Value();
    SimState* s = nullptr;
    int mover = -1;
    if (VehicleState* vs = vehicleOf(self)) {
      auto* to = transport(self);
      auto sp = to ? to->sim.lock() : nullptr;
      s = sp.get();
      mover = vs->mover;
    } else if (auto* g = dynamic_cast<GridObj*>(self->parent)) {
      s = g->sim.get();
      mover = fieldInt(v, self, u"<mover>", -1);
    }
    if (!s || mover < 0 || size_t(mover) >= s->movers.size()) return Value();
    playAtMover(*s, soundsOf(*s), mover, t, 1.0f, "playSound");
    return Value();
  });
}

}  // namespace regs::engine
