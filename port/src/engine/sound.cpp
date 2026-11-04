#include "engine/sound.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/host.h"
#include "engine/scene3d.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using render::Vec3;

namespace {

Value plain(const Value& v) { return v.ref() ? v.ref()->get() : v; }

Vec3 vecOf(const Value& v) {
  Value p = plain(v);
  if (p.type() != VT::Vector) return {};
  return {p.vec()[0], p.vec()[1], p.vec()[2]};
}

class SoundObj;
class ListenerObj;

// gsSounds (§3): the enabled flag, the path → sound cache, every live gsSound (for statusUpdated), the listener.
struct CacheEntry {
  audio::ClipRef clip;
  bool threeD = true, music = false;
};
struct WorldVoice {
  Object* grid = nullptr;
  std::string path;
};
struct Sounds {
  bool enabled = false;
  bool debug = false;
  float master = 1;
  std::map<std::u16string, CacheEntry> cache;
  std::set<SoundObj*> all;
  ListenerObj* listener = nullptr;
  Vec3 eye;
  bool eyeKnown = false;
  std::map<int, WorldVoice> world;
  float statClock = 0;
  const ClassInfo* elementClass = nullptr;  // gs3DElement
  const ClassInfo* cameraClass = nullptr;   // gs3DCamera
};

Sounds& snd() {
  static Sounds s;
  return s;
}

// REGS_AUDIO_ONLY=<text>: only sounds whose file name contains the text are heard (the others play at volume 0), so a
// capture measures one kind of sound alone
float solo(const std::string& name) {
  static const char* only = std::getenv("REGS_AUDIO_ONLY");
  return !only || !*only || name.find(only) != std::string::npos ? 1.0f : 0.0f;
}
float solo(const std::u16string& path) { return std::getenv("REGS_AUDIO_ONLY") ? solo(utf16ToUtf8(path)) : 1.0f; }

// effective enabled (1) and audible (4) bits along the owner chain (gsModule status)
uint32_t effectiveStatus(const Object* o) {
  uint32_t bits = 5;
  for (; o && bits; o = o->parent) bits &= o->flags;
  return bits & 5;
}

const char* modeName(bool threeD, bool music) { return music ? (threeD ? "3D compressed" : "2D compressed") : (threeD ? "3D" : "2D"); }

std::string shortPath(const std::u16string& p) {
  std::string s = utf16ToUtf8(p);
  size_t k = s.find_last_of('/');
  return k == std::string::npos ? s : s.substr(k + 1);
}

// gsSounds::getSound (§5.2): enabled only; cached by expanded path, so the first load fixes the mode
audio::ClipRef getSound(VM& vm, const std::u16string& path, bool music, bool threeD, bool* is3D, bool* cached) {
  Sounds& S = snd();
  if (!S.enabled) return nullptr;
  auto it = S.cache.find(path);
  if (it != S.cache.end()) {
    *is3D = it->second.threeD;
    *cached = true;
    return it->second.clip;
  }
  *cached = false;
  std::vector<uint8_t> data;
  std::string err;
  std::string p8 = utf16ToUtf8(path);
  audio::ClipRef clip;
  if (vm.vfs().read(p8, data, &err)) clip = audio::loadClip(std::move(data), music, &err);
  else err = "not found";
  if (!clip) {  // not cached: retried (and reported) on every load
    LOG_WARN("Error loading audio file: %s (%s)", p8.c_str(), err.c_str());
    return nullptr;
  }
  S.cache[path] = {clip, threeD, music};
  *is3D = threeD;
  return clip;
}

// gsSound / gsMusic (§6, §7)
class SoundObj : public Object {
 public:
  explicit SoundObj(const ClassInfo* c) : Object(c) { snd().all.insert(this); }
  ~SoundObj() override {
    snd().all.erase(this);
    if (voice) audio::stop(voice);
    if (parked) audio::stop(parked);
  }

  bool music = false;
  audio::ClipRef clip;
  bool clip3D = true;
  std::u16string path;
  int voice = 0;         // the channel
  bool active = false, loop = false, relative = false;
  float minD = 10, maxD = 100, pitch = 1, pitchMult = 1, volume = 1, volumeMult = 1;
  float outOfRange = 0, length = 0;
  double resumeSec = 0;  // pause(): where the next play starts
  int parked = 0;        // port: the paused voice kept for that resume (streams cannot seek cheaply)
  double parkedAt = -1;
  uint32_t lastStatus = 5;

  bool channelValid() {
    if (voice && !audio::isPlaying(voice)) voice = 0;
    return voice != 0;
  }

  Vec3 globalPos(VM& vm) const {
    // the position in the owner chain's 3D elements (orientation ignored: every script-played sound is head-relative
    // or sits on a 2D control, §9.4)
    Vec3 p = vecOf(fieldValue(u"position"));
    for (const Object* o = parent; o; o = o->parent)
      if (snd().elementClass && o->cls->isA(snd().elementClass)) {
        auto g = o->fieldValues.find(u"globalPos");
        if (g != o->fieldValues.end() && g->second.type() == VT::Vector) return p + vecOf(g->second);
        p = p + vecOf(fieldValueOf(o, u"position"));
      }
    (void)vm;
    return p;
  }

  float distance(VM& vm) const {
    if (relative) {
      Vec3 p = vecOf(fieldValue(u"position"));
      return std::sqrt(render::dot(p, p));
    }
    Vec3 p = globalPos(vm);
    if (snd().eyeKnown) p = p - snd().eye;
    return std::sqrt(render::dot(p, p));
  }

  void push3D(VM& vm) {
    if (!voice) return;
    Vec3 p = relative ? vecOf(fieldValue(u"position")) : globalPos(vm);
    float pos[3] = {p.x, p.y, p.z}, vel[3] = {0, 0, 0};
    audio::set3D(voice, relative, pos, vel, minD, maxD);
  }

  void log(VM& vm, const char* what) const {
    if (!snd().debug) return;
    Vec3 p = relative ? vecOf(fieldValue(u"position")) : globalPos(vm);
    LOG_INFO("audio: %s %s [%s%s%s] vol %.2f pitch %.2f pos (%.1f, %.1f, %.1f) dist %.1f (min %.0f max %.0f) voice %d",
             what, shortPath(path).c_str(), modeName(clip3D, music), relative ? ", relative" : "", loop ? ", loop" : "",
             volume * volumeMult, pitch * pitchMult, p.x, p.y, p.z, distance(vm), minD, maxD, voice);
  }

  void stopParked() {
    if (parked) audio::stop(parked);
    parked = 0;
    parkedAt = -1;
  }

  // setActive (L:0x08129e50)
  void setActive(VM& vm, bool on) {
    on = on && clip && (effectiveStatus(this) & 4);
    if (on == active) return;
    active = on;
    if (on) {
      if (parked && resumeSec == parkedAt && audio::isPlaying(parked)) {  // resume the paused voice
        voice = parked;
        parked = 0;
        parkedAt = -1;
        audio::setVolume(voice, volume * volumeMult * solo(path));
        audio::setPitch(voice, pitch * pitchMult);
        audio::setLooping(voice, loop);
        push3D(vm);
        audio::setPaused(voice, false);
      } else {
        stopParked();
        audio::VoiceParams p;
        p.volume = volume * volumeMult * solo(path);
        p.pitch = pitch * pitchMult;
        p.loop = loop;
        p.spatial = clip3D;
        p.relative = relative;
        Vec3 g = relative ? vecOf(fieldValue(u"position")) : globalPos(vm);
        p.pos[0] = g.x, p.pos[1] = g.y, p.pos[2] = g.z;
        p.minDistance = minD;
        p.maxDistance = maxD;
        p.startSeconds = resumeSec;
        voice = audio::play(clip, p);
      }
      outOfRange = 0;
      if (length == 0) length = float(audio::clipInfo(clip).seconds());
      log(vm, resumeSec > 0 ? "resume" : "play");
    } else {
      if (channelValid()) {
        audio::stop(voice);
        log(vm, "stop");
      }
      voice = 0;
      resumeSec = 0;
      stopParked();
    }
  }

  // setPlaying (L:0x0812a500)
  void setPlaying(VM& vm, bool play, bool lp) {
    lastStatus = effectiveStatus(this);
    if (play) {
      if (loop && lp && channelValid()) return;
      loop = lp;
      setActive(vm, false);
      if (lastStatus == 5 && snd().enabled) {
        bool inRange = distance(vm) < maxD;
        if (!inRange) log(vm, "out of range, not started:");
        setActive(vm, inRange);
      }
    } else {
      loop = lp;
      setActive(vm, false);
    }
  }

  void pause(VM& vm) {
    double pos = channelValid() ? audio::positionSeconds(voice) : 0.0;
    loop = false;
    if (voice) {  // setActive(false) with the voice kept, paused, for the resume
      audio::setPaused(voice, true);
      stopParked();
      parked = voice;
      parkedAt = pos;
      voice = 0;
      active = false;
      log(vm, "pause");
    } else {
      setActive(vm, false);
    }
    resumeSec = pos;
  }

  // gsSound::move (L:0x0812a110)
  void tick(VM& vm, float dt) override {
    if (snd().enabled) {
      if (!active && loop && distance(vm) < maxD) setActive(vm, true);
      if (channelValid()) {
        push3D(vm);
        if (distance(vm) <= maxD) outOfRange = 0;
        else if ((outOfRange += dt) >= 0.5f) setActive(vm, false);
      }
    }
    Object::tick(vm, dt);
  }

  void onDestroy(VM& vm) override {
    if (active) setActive(vm, false);
    stopParked();
    if (voice) audio::stop(voice), voice = 0;
    snd().all.erase(this);
  }

  // statusUpdated (L:0x0812a440), polled once per frame
  void pollStatus(VM& vm) {
    uint32_t st = effectiveStatus(this);
    if (st == lastStatus) return;
    uint32_t changed = st ^ lastStatus;
    lastStatus = st;
    if (loop && st == 5) setActive(vm, true);
    else if (changed & 5) setActive(vm, false);
  }

 private:
  Value fieldValue(const char16_t* n) const { return fieldValueOf(this, n); }
  static Value fieldValueOf(const Object* o, const char16_t* n) {
    auto it = o->fieldValues.find(n);
    return it == o->fieldValues.end() ? Value() : it->second;
  }
};

class ListenerObj : public Object {
 public:
  explicit ListenerObj(const ClassInfo* c) : Object(c) { snd().listener = this; }  // the last created is the listener
  ~ListenerObj() override {
    if (snd().listener == this) snd().listener = nullptr;
  }
  void onDestroy(VM&) override {
    if (snd().listener == this) snd().listener = nullptr;
  }
};

SoundObj* soundOf(Object* o) { return dynamic_cast<SoundObj*>(o); }

// gsListener::move (L:0x08127e30): position and orientation of the tracked camera's eye
void updateListener(VM& vm) {
  Sounds& S = snd();
  ListenerObj* L = S.listener;
  if (!L || L->destroyed) return;
  Vec3 eye, look, up{0, 1, 0};
  auto tr = L->fieldValues.find(u"tracked");
  Object* tracked = tr == L->fieldValues.end() ? nullptr : plain(tr->second).object();
  if (tracked && !tracked->destroyed && S.cameraClass && tracked->cls->isA(S.cameraClass)) {
    cameraFrame(vm, tracked, eye, look, up);  // the eye as the 3D pass draws it (= tracked.pos + orient · offset)
  } else {
    auto p = L->fieldValues.find(u"position");
    eye = p == L->fieldValues.end() ? Vec3{} : vecOf(p->second);
    if (tracked) {
      auto tp = tracked->fieldValues.find(u"position");
      if (tp != tracked->fieldValues.end()) eye = vecOf(tp->second);
    }
    look = eye + Vec3{0, 0, -1};
  }
  S.eye = eye;
  S.eyeKnown = true;
  Vec3 f = look - eye;
  float pos[3] = {eye.x, eye.y, eye.z}, fwd[3] = {f.x, f.y, f.z}, u[3] = {up.x, up.y, up.z}, vel[3] = {0, 0, 0};
  audio::setListener(pos, fwd, u, vel);
}

FieldInfo& fieldOfClass(ClassInfo* c, const char16_t* name) {
  FieldInfo& f = c->fields[name];
  f.name = name;
  return f;
}

}  // namespace

bool soundsEnabled() { return snd().enabled; }
bool soundDebug() { return snd().debug; }

bool soundListener(Vec3& eye) {
  if (!snd().eyeKnown) return false;
  eye = snd().eye;
  return true;
}

bool soundTemplate(Object* o, SoundTemplate& out) {
  SoundObj* s = o ? soundOf(o) : nullptr;
  if (!s || !s->clip || s->destroyed) return false;
  out.clip = s->clip;
  out.threeD = s->clip3D;
  out.volume = s->volume;
  out.pitch = s->pitch;
  out.minDistance = s->minD;
  out.maxDistance = s->maxD;
  out.path = shortPath(s->path);
  return true;
}

int worldSoundPlay(Object* grid, const SoundTemplate& t, const Vec3& pos, const Vec3& vel, bool forced, bool loop,
                   float pitchMult, float volumeMult, const char* what) {
  Sounds& S = snd();
  if (!S.enabled || !t.clip || effectiveStatus(grid) != 5) return 0;
  audio::VoiceParams p;
  p.volume = t.volume * volumeMult * solo(t.path);
  p.pitch = t.pitch * pitchMult;
  p.loop = loop;
  p.spatial = t.threeD;
  p.relative = forced;
  if (!forced) {
    p.pos[0] = pos.x, p.pos[1] = pos.y, p.pos[2] = pos.z;
    p.vel[0] = vel.x, p.vel[1] = vel.y, p.vel[2] = vel.z;
  }
  p.minDistance = t.minDistance;
  p.maxDistance = t.maxDistance;
  int v = audio::play(t.clip, p);
  if (v) S.world[v] = {grid, t.path};
  if (S.debug) {
    Vec3 d = forced ? Vec3{} : pos - S.eye;
    LOG_INFO("audio: world %s %s [%s%s%s] vol %.2f pitch %.2f pos (%.1f, %.1f, %.1f) dist %.1f (min %.0f max %.0f) voice %d",
             what, t.path.c_str(), t.threeD ? "3D" : "2D", forced ? ", forced" : "", loop ? ", loop" : "", p.volume, p.pitch,
             pos.x, pos.y, pos.z, std::sqrt(render::dot(d, d)), t.minDistance, t.maxDistance, v);
  }
  return v;
}

void worldSoundUpdate(int voice, const SoundTemplate& t, const Vec3& pos, const Vec3& vel, bool forced, float pitchMult,
                      float volumeMult) {
  if (!voice) return;
  float p[3] = {pos.x, pos.y, pos.z}, v[3] = {vel.x, vel.y, vel.z}, zero[3] = {0, 0, 0};
  audio::set3D(voice, forced, forced ? zero : p, forced ? zero : v, t.minDistance, t.maxDistance);
  audio::setVolume(voice, t.volume * volumeMult * solo(t.path));
  audio::setPitch(voice, t.pitch * pitchMult);
}

bool worldSoundAlive(int voice) { return voice && audio::isPlaying(voice); }

void worldSoundStop(int voice, const char* why) {
  if (!voice) return;
  Sounds& S = snd();
  auto it = S.world.find(voice);
  if (S.debug && it != S.world.end()) LOG_INFO("audio: world stop %s (%s) voice %d", it->second.path.c_str(), why, voice);
  if (it != S.world.end()) S.world.erase(it);
  audio::stop(voice);
}

void soundsFrame(VM& vm, float dt) {
  Sounds& S = snd();
  if (!S.enabled) return;
  updateListener(vm);
  // statusUpdated: only sounds that play, loop or wait for a resume can react
  std::vector<SoundObj*> poll;
  for (SoundObj* s : S.all)
    if ((s->active || s->loop || s->parked) && !s->destroyed) poll.push_back(s);
  for (SoundObj* s : poll) {
    Ref<Object> keep(s);
    s->pollStatus(vm);
  }
  // world voices: stopped with a disabled or inaudible grid (the clones' statusUpdated), forgotten when finished
  for (auto it = S.world.begin(); it != S.world.end();) {
    if (!audio::isPlaying(it->first)) {
      it = S.world.erase(it);
    } else if (!it->second.grid || it->second.grid->destroyed || effectiveStatus(it->second.grid) != 5) {
      if (S.debug) LOG_INFO("audio: world stop %s (grid disabled or inaudible) voice %d", it->second.path.c_str(), it->first);
      audio::stop(it->first);
      it = S.world.erase(it);
    } else {
      ++it;
    }
  }
  audio::update();
  if (S.debug && (S.statClock += dt) >= 1.0f) {
    S.statClock = 0;
    audio::Stats st = audio::takeStats();
    std::string rms;
    for (uint32_t c = 0; c < st.channels && c < 8; ++c) rms += fmt(" %.4f", st.rms[c]);
    LOG_INFO("audio: %d voices (%d mixed, %d virtual, %d paused; %zu world), master %.2f, listener (%.1f, %.1f, %.1f), RMS%s",
             st.voices, st.mixed, st.virtualized, st.paused, S.world.size(), S.master, S.eye.x, S.eye.y, S.eye.z, rms.c_str());
    for (const auto& t : st.top) {  // the loudest 3D voices: where they are and how they are panned
      auto w = S.world.find(t.voice);
      std::string gains;
      for (uint32_t c = 0; c < st.channels && c < 8; ++c) gains += fmt(" %.3f", t.ch[c]);
      LOG_INFO("audio:   voice %d %s: gain %.3f at x %.1f z %.1f (dist %.1f), rate %.3f, channel gains%s", t.voice,
               w != S.world.end() ? w->second.path.c_str() : "(script sound)", t.gain, t.x, t.z, t.d, t.rate, gains.c_str());
    }
  }
}

void soundsShutdown() {
  Sounds& S = snd();
  for (SoundObj* s : S.all) s->voice = s->parked = 0, s->active = false;
  S.world.clear();
  S.cache.clear();
  S.enabled = false;
  audio::shutdown();
}

void registerSoundNatives(VM& vm) {
  Sounds& S = snd();
  S.debug = std::getenv("REGS_DEBUG_AUDIO") && std::atoi(std::getenv("REGS_DEBUG_AUDIO")) != 0;
  S.elementClass = vm.findClass("gs3DElement");
  S.cameraClass = vm.findClass("gs3DCamera");
  if (ClassInfo* c = vm.findClass("gsSound"))
    c->factory = [&vm](const ClassInfo* ci) {
      auto* s = new SoundObj(ci);
      const ClassInfo* m = vm.findClass("gsMusic");
      s->music = m && ci->isA(m);
      return Ref<Object>(s);
    };
  if (ClassInfo* c = vm.findClass("gsListener")) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new ListenerObj(ci)); };

  // ---- global functions (§2.1, §3) ----
  implementGlobal(vm, u"enable_sounds", [](VM& v, Object*, int argc, const Value* a) {
    Sounds& S = snd();
    if (S.enabled) return Value();  // init runs once
    audio::InitOptions o;
    if (argc > 0) o.device = utf16ToUtf8(v.toStr(a[0]));
    if (argc > 1) o.layout = audio::layoutFromName(utf16ToUtf8(v.toStr(a[1])));
    if (const char* cap = std::getenv("REGS_AUDIO_CAPTURE"); cap && *cap) o.capture = cap;
    if (host().headless) {
      LOG_INFO("audio: headless, sounds stay off");
      return Value();
    }
    LOG_INFO("initializing audio (device \"%s\", speakers \"%s\")...", o.device.c_str(), audio::layoutName(o.layout));
    S.enabled = audio::init(o);
    if (S.enabled) audio::setMasterVolume(S.master);
    return Value();
  });
  implementGlobal(vm, u"disable_sounds", [](VM&, Object*, int, const Value*) {
    if (snd().enabled) soundsShutdown();
    return Value();
  });
  implementGlobal(vm, u"set_audio_volume", [](VM& v, Object*, int, const Value* a) {
    Sounds& S = snd();
    if (!S.enabled) return Value();
    S.master = v.toFloat(a[0]);
    audio::setMasterVolume(S.master);
    if (S.debug) LOG_INFO("audio: master volume %.2f", S.master);
    return Value();
  });
  auto strings = [](const std::vector<std::string>& list) {
    auto arr = make<ArrayObj>();
    for (const auto& s : list) arr->items.push_back(Value::string(utf8ToUtf16(s)));
    return Value::heap(VT::Array, arr.get());
  };
  implementGlobal(vm, u"audio_devices", [strings](VM&, Object*, int, const Value*) {
    std::vector<std::string> list{"Default"};
    if (!host().headless)
      for (auto& n : audio::deviceNames()) list.push_back(n);
    return strings(list);
  });
  implementGlobal(vm, u"audio_speakers", [strings](VM&, Object*, int, const Value*) {
    std::vector<std::string> list;
    for (int i = 0; i < int(audio::Layout::Count); ++i) list.push_back(audio::layoutName(audio::Layout(i)));
    return strings(list);  // the original's eight values, then the port's Headphones (§16.3)
  });

  // ---- gsSound / gsMusic ----
  implement(vm, "gsSound", u"load", [](VM& v, Object* self, int argc, const Value* a) {
    SoundObj* s = soundOf(self);
    if (!s) return Value();
    bool threeD = argc > 1 ? plain(a[1]).toBool() : true;
    const Object* base = self;  // gsExpandFileName: relative to the object's directory
    while (base && base->scriptPath.empty()) base = base->parent;
    std::u16string path = v.resolvePath(v.toStr(a[0]), base);
    bool is3D = threeD, cached = false;
    s->clip = getSound(v, path, s->music, threeD, &is3D, &cached);
    s->clip3D = is3D;
    s->path = path;
    if (!s->music) s->length = 0;
    if (snd().debug && s->clip && (!cached || is3D != threeD))  // first loads, and later ones that get another mode
      LOG_INFO("audio: load %s [%s]%s", utf16ToUtf8(path).c_str(), modeName(is3D, s->music),
               cached ? " (cached: the mode of the first load wins)" : "");
    return Value();
  });
  implement(vm, "gsSound", u"play", [](VM& v, Object* self, int argc, const Value* a) {
    if (SoundObj* s = soundOf(self)) s->setPlaying(v, true, argc > 0 && plain(a[0]).toBool());
    return Value();
  });
  implement(vm, "gsSound", u"stop", [](VM& v, Object* self, int, const Value*) {
    if (SoundObj* s = soundOf(self)) s->setPlaying(v, false, false);
    return Value();
  });
  implement(vm, "gsSound", u"pause", [](VM& v, Object* self, int, const Value*) {
    if (SoundObj* s = soundOf(self)) s->pause(v);
    return Value();
  });
  implement(vm, "gsSound", u"clone", [](VM& v, Object* self, int, const Value*) {
    SoundObj* s = soundOf(self);
    if (!s) return Value::null();
    Ref<Object> c = v.newObject(self->cls, self->parent);
    if (SoundObj* d = soundOf(c.get())) {
      d->clip = s->clip;
      d->clip3D = s->clip3D;
      d->path = s->path;
      d->relative = s->relative;
      d->minD = s->minD, d->maxD = s->maxD;
      d->pitch = s->pitch, d->pitchMult = s->pitchMult;
      d->volume = s->volume, d->volumeMult = s->volumeMult;
      d->length = s->length;
    }
    return Value::heap(VT::Object, c.get());
  });
  if (ClassInfo* c = vm.findClass("gsSound")) {
    auto floatField = [c](const char16_t* name, float SoundObj::*m, bool live) {
      FieldInfo& f = fieldOfClass(c, name);
      f.type = 4;
      f.get = [m](VM&, Object* o) {
        SoundObj* s = soundOf(o);
        return Value::real(s ? s->*m : 0.0f);
      };
      f.set = [m, live](VM& v, Object* o, const Value& x) {
        SoundObj* s = soundOf(o);
        if (!s) return;
        s->*m = plain(x).toFloat();
        if (live && s->channelValid()) {  // the setters push to a live channel at once (§6.4)
          audio::setVolume(s->voice, s->volume * s->volumeMult * solo(s->path));
          audio::setPitch(s->voice, s->pitch * s->pitchMult);
          s->push3D(v);
        }
      };
    };
    floatField(u"pitch", &SoundObj::pitch, true);
    floatField(u"volume", &SoundObj::volume, true);
    floatField(u"minDistance", &SoundObj::minD, true);
    floatField(u"maxDistance", &SoundObj::maxD, true);
    {
      FieldInfo& f = fieldOfClass(c, u"relative");
      f.type = 2;
      f.get = [](VM&, Object* o) {
        SoundObj* s = soundOf(o);
        return Value::boolean(s && s->relative);
      };
      f.set = [](VM& v, Object* o, const Value& x) {
        SoundObj* s = soundOf(o);
        if (!s) return;
        s->relative = plain(x).toBool();
        if (s->channelValid()) s->push3D(v);
      };
    }
    {
      FieldInfo& f = fieldOfClass(c, u"currentPos");
      f.type = 4;
      f.readOnly = true;
      f.get = [](VM&, Object* o) {
        SoundObj* s = soundOf(o);
        return Value::real(s && s->channelValid() ? float(audio::positionSeconds(s->voice)) : 0.0f);
      };
      f.set = nullptr;
    }
    {
      FieldInfo& f = fieldOfClass(c, u"length");
      f.type = 4;
      f.readOnly = true;
      f.get = [](VM&, Object* o) {
        SoundObj* s = soundOf(o);
        return Value::real(s ? s->length : 0.0f);
      };
      f.set = nullptr;
    }
  }
}

}  // namespace regs::engine
