#include "engine/skinning.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/scene3d.h"
#include "engine/settings.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

constexpr uint32_t kMeshSkinned = 0x100;  // MESH_SKINNED
constexpr uint32_t kDontLoop = 0x20000;

ReadFile vfsReader(VM& vm) {
  return [&vm](const std::string& p, std::vector<uint8_t>& out) { return vm.vfs().read(p, out); };
}

std::string dirOf(const std::string& p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}

std::shared_ptr<const SkeletonData> skeletonFile(VM& vm, const std::string& path) {
  static std::unordered_map<std::string, std::shared_ptr<const SkeletonData>> cache;
  auto it = cache.find(path);
  if (it != cache.end()) return it->second;
  auto data = std::make_shared<SkeletonData>();
  std::string err;
  if (!loadSkeleton(path, vfsReader(vm), *data, &err)) {
    LOG_WARN("skeleton %s: %s", path.c_str(), err.c_str());
    data.reset();
  }
  return cache.emplace(path, data).first->second;
}

std::shared_ptr<const AnimationData> animationFile(VM& vm, const std::string& path) {
  static std::unordered_map<std::string, std::shared_ptr<const AnimationData>> cache;
  auto it = cache.find(path);
  if (it != cache.end()) return it->second;
  auto data = std::make_shared<AnimationData>();
  std::string err;
  if (!loadAnimation(path, vfsReader(vm), *data, &err)) {
    LOG_WARN("animation %s: %s", path.c_str(), err.c_str());
    data.reset();
  }
  return cache.emplace(path, data).first->second;
}

SkeletonObj* asSkeleton(Object* o) { return o && !o->destroyed ? dynamic_cast<SkeletonObj*>(o) : nullptr; }

AnimSlot* slotOf(VM& v, Object* s, const Value& name) {
  auto* sk = asSkeleton(s);
  if (!sk) return nullptr;
  auto it = sk->slots.find(utf16ToUtf8(v.toStr(name)));
  return it == sk->slots.end() ? nullptr : &it->second;
}

// the engine's clamp of crop values: x when 0 < x < max, max when x ≥ max, else 0
float clampCrop(float x, float max) { return x < max ? (x > 0 ? x : 0.0f) : max; }

Quat nlerpQ(const Quat& a, const Quat& b, float t) {
  if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) t = -t;
  Quat r{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z), a.w + t * (b.w - a.w)};
  float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
  return l > 0 ? Quat{r.x / l, r.y / l, r.z / l, r.w / l} : r;
}

struct Active {
  const AnimSlot* slot;
  float weight, pos;
};

// gsSkeleton::animateBones (L:0x080e06d0): per bone, the slots in load order: the first enabled one with a weight
// and a track for the bone sets its local TRS (time = crop start + position), later ones blend towards theirs by
// weight; bones without tracks keep the bind pose. Then the skin palette (interfaces-3d.md §1.5).
Pose posePalette(const SkeletonObj& sk, std::vector<Active> act) {
  const SkeletonData& s = *sk.data;
  std::sort(act.begin(), act.end(), [](const Active& a, const Active& b) { return a.slot->order < b.slot->order; });
  size_t n = s.bones.size();
  std::vector<Vec3> t(n), sc(n);
  std::vector<Quat> q(n);
  for (size_t i = 0; i < n; ++i) {
    t[i] = s.bones[i].t;
    sc[i] = s.bones[i].s;
    q[i] = s.bones[i].q;
    bool first = true;
    for (const Active& a : act) {
      if (!a.slot->enabled || a.weight == 0 || !a.slot->anim || i >= a.slot->trackOfBone.size()) continue;
      int tr = a.slot->trackOfBone[i];
      if (tr < 0) continue;
      Vec3 tt, ss;
      Quat qq;
      sampleTrack(a.slot->anim->tracks[size_t(tr)], a.slot->anim->duration, a.slot->cropStart + a.pos,
                  (a.slot->flags & kDontLoop) != 0, tt, ss, qq);
      if (first) {
        t[i] = tt;
        sc[i] = ss;
        q[i] = qq;
        first = false;
      } else {
        t[i] = t[i] + (tt - t[i]) * a.weight;
        sc[i] = sc[i] + (ss - sc[i]) * a.weight;
        q[i] = nlerpQ(q[i], qq, a.weight);
      }
    }
  }
  auto out = std::make_shared<std::vector<Mat4>>();
  skinPalette(s, t, q, sc, *out);
  return out;
}

// the skinned upload of a model, with its materials in the skinning variant
struct SkinnedModel {
  std::shared_ptr<const ObjectData> data;
  GpuMesh mesh[2];
  bool built[2] = {false, false};
  std::vector<GpuMaterial> materials;
};

}  // namespace

SkeletonObj* modelSkeleton(VM& vm, ObjectObj* model) {
  if (!model) return nullptr;
  auto it = model->fieldValues.find(u"skeleton");
  return it == model->fieldValues.end() ? nullptr : asSkeleton(it->second.object());
}

bool isSkinnedModel(VM& vm, ObjectObj* model) {
  SkeletonObj* sk = modelSkeleton(vm, model);
  return sk && sk->data && model->data && !model->data->mesh.skinBones.empty();
}

Pose poseFirstAnimation(VM& vm, ObjectObj* model, float time) {
  SkeletonObj* sk = modelSkeleton(vm, model);
  if (!sk || !sk->data || sk->slots.empty()) return nullptr;
  const AnimSlot& first = sk->slots.begin()->second;  // the map's first node (+0x130): smallest name
  return posePalette(*sk, {{&first, 1.0f, time * first.speed}});
}

bool animateCharacter(VM& vm, ObjectObj* model, CharacterAnimation& a, const std::string& name, float dt, float factor) {
  SkeletonObj* sk = modelSkeleton(vm, model);
  auto slot = [&](const std::string& n) -> const AnimSlot* {
    if (!sk || n.empty()) return nullptr;
    auto it = sk->slots.find(n);
    return it == sk->slots.end() ? nullptr : &it->second;
  };
  if (name != a.current) {  // gsGridCharacter::animate (L:0x081529d0): a new current slot
    if (!slot(name)) {
      a.previous.clear();
      a.current = name;
      a.currentPos = a.previousPos = 0;
      a.fade = 0.001f;
    } else if (name == a.previous) {  // back to the previous one: swap, the fade continues from where it was
      std::swap(a.current, a.previous);
      std::swap(a.currentPos, a.previousPos);
      std::swap(a.currentFactor, a.previousFactor);
      a.fade = 1.0f - a.fade;
    } else {
      a.previous = a.current;
      a.previousPos = a.currentPos;
      a.previousFactor = a.currentFactor;
      a.current = name;
      a.currentPos = 0;
      a.fade = 0.001f;
    }
  }
  // gsGridCharacterAnimations::move (L:0x08151f60)
  auto advance = [&](float& pos, float f, const AnimSlot* s) {
    float step = f * dt * s->speed;
    pos = f >= 0 ? pos + step : pos - step;
    if (pos >= s->length) {
      pos = (f >= 0 && pos - s->length < s->length) ? pos - s->length : s->length;
      return true;
    }
    return false;
  };
  a.currentFactor = factor;
  const AnimSlot* prev = slot(a.previous);
  if (!prev) {
    a.fade = 1;
  } else {
    a.fade += dt + dt;
    if (a.fade >= 1) {
      a.fade = 1;
      a.previous.clear();
    } else {
      advance(a.previousPos, a.previousFactor, prev);
    }
  }
  const AnimSlot* cur = slot(a.current);
  return cur ? advance(a.currentPos, factor, cur) : false;
}

Pose poseCharacter(VM& vm, ObjectObj* model, const CharacterAnimation& a) {
  SkeletonObj* sk = modelSkeleton(vm, model);
  if (!sk || !sk->data) return nullptr;
  std::vector<Active> act;
  auto add = [&](const std::string& n, float w, float pos) {
    auto it = n.empty() ? sk->slots.end() : sk->slots.find(n);
    if (it != sk->slots.end()) act.push_back({&it->second, w, pos});
  };
  add(a.current, a.fade, a.currentPos);
  add(a.previous, 1.0f - a.fade, a.previousPos);
  return posePalette(*sk, act);
}

void collectSkinned(Display& d, VM& vm, ObjectObj* model, const Mat4& world, const Pose& pose,
                    std::vector<DrawItem>& out, int lod, float alpha) {
  if (!model || !model->data) return;
  static std::unordered_map<const ObjectObj*, SkinnedModel> cache;
  SkinnedModel& sm = cache[model];
  if (sm.data != model->data) {
    for (int k = 0; k < 2; ++k)
      if (sm.built[k]) d.r3d().destroy(sm.mesh[k]);
    sm = SkinnedModel();
    sm.data = model->data;
    bool skinned = !model->data->mesh.skinBones.empty();
    for (const MaterialDef& m : model->data->materials) {
      GpuMaterial g = objectMaterial(d, vm, model, m);
      if (skinned) g.meshFlags = kMeshSkinned;
      sm.materials.push_back(g);
    }
  }
  int k = lod > 0 && model->data->hasLod1 ? 1 : 0;
  if (!sm.built[k]) {
    // the combiner's palette layout follows $indexingInShaders (default.shader: BONEx48 indexed, else BONEx8)
    SkinLayout layout;
    layout.indexed = settingNumber(vm, u"indexingInShaders", 0) != 0;
    layout.paletteSize = layout.indexed ? 48 : 8;
    sm.mesh[k] = d.r3d().upload(k ? model->data->lod1 : model->data->mesh, layout);
    sm.built[k] = true;
  }
  if (sm.mesh[k].parts.empty()) return;
  DrawItem it;
  it.mesh = &sm.mesh[k];
  it.materials = &sm.materials;
  it.world = world;
  it.alpha = alpha;
  it.bones = pose;
  out.push_back(it);
}

void registerSkinningNatives(VM& vm) {
  if (ClassInfo* c = vm.findClass("gsSkeleton")) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new SkeletonObj(ci)); };
  // loadSkeleton(file) (gsObject::loadSkeleton L:0x08113d10): the .skeleton next to the object; the gsSkeleton becomes
  // the object's skeleton field. Returns it, or the existing one (or unset) when the file is missing.
  implement(vm, "gsObject", u"loadSkeleton", [](VM& v, Object* s, int, const Value* a) {
    std::string path = utf16ToUtf8(v.resolvePath(v.toStr(a[0]), s));
    auto existing = s->fieldValues.find(u"skeleton");
    auto data = v.vfs().exists(path) ? skeletonFile(v, path) : nullptr;
    if (!data) return existing == s->fieldValues.end() ? Value() : existing->second;
    Ref<Object> o = v.newObject(v.findClass("gsSkeleton"), s);
    auto* sk = asSkeleton(o.get());
    if (!sk) return Value();
    sk->data = data;
    sk->path = path;
    Value val = Value::heap(VT::Object, o.get());
    s->fieldValues[u"skeleton"] = val;
    return val;
  });
  // loadAnimation(name, file, [flags], [group names...]) (L:0x080e3f30): file relative to the skeleton; tracks bind to
  // the first bone of their name; a slot starts enabled, uncropped, speed 1, weight 0, looping unless DONT_LOOP
  implement(vm, "gsSkeleton", u"loadAnimation", [](VM& v, Object* s, int argc, const Value* a) {
    auto* sk = asSkeleton(s);
    if (!sk || !sk->data) return Value();
    std::string file = utf16ToUtf8(v.toStr(a[1]));
    std::string path = !file.empty() && file[0] == '/' ? file.substr(1) : dirOf(sk->path) + file;
    auto anim = animationFile(v, path);
    if (!anim) return Value();
    AnimSlot slot;
    slot.anim = anim;
    slot.flags = argc > 2 ? uint32_t(v.toInt(a[2])) : 0u;
    slot.length = anim->duration;
    slot.loop = !(slot.flags & kDontLoop);
    slot.order = int(sk->slots.size());
    slot.trackOfBone.assign(sk->data->bones.size(), -1);
    for (size_t t = 0; t < anim->tracks.size(); ++t) {
      int b = sk->data->find(anim->tracks[t].bone);
      if (b >= 0 && slot.trackOfBone[size_t(b)] < 0) slot.trackOfBone[size_t(b)] = int(t);
    }
    std::string name = utf16ToUtf8(v.toStr(a[0]));
    if (auto old = sk->slots.find(name); old != sk->slots.end()) slot.order = old->second.order;
    sk->slots[name] = std::move(slot);
    return Value();
  });
  implement(vm, "gsSkeleton", u"unloadAnimation", [](VM& v, Object* s, int, const Value* a) {
    if (auto* sk = asSkeleton(s)) sk->slots.erase(utf16ToUtf8(v.toStr(a[0])));
    return Value();
  });
  // cropAnimation(name, start, length) (L:0x080dce80): start within the take, length within the rest
  implement(vm, "gsSkeleton", u"cropAnimation", [](VM& v, Object* s, int, const Value* a) {
    if (AnimSlot* sl = slotOf(v, s, a[0]); sl && sl->anim) {
      float dur = sl->anim->duration;
      sl->cropStart = clampCrop(v.toFloat(a[1]), dur);
      sl->length = clampCrop(v.toFloat(a[2]), dur - sl->cropStart);
      sl->pos = clampCrop(sl->pos, sl->length);
    }
    return Value();
  });
  // playAnimation(name, speed, [loop = true], [speed source]) (L:0x080dd1b0): sets the speed and loop flag only
  implement(vm, "gsSkeleton", u"playAnimation", [](VM& v, Object* s, int argc, const Value* a) {
    if (AnimSlot* sl = slotOf(v, s, a[0])) {
      sl->speed = v.toFloat(a[1]);
      sl->loop = argc > 2 ? v.truthy(a[2]) : true;
    }
    return Value();
  });
  implement(vm, "gsSkeleton", u"stopAnimation", [](VM& v, Object* s, int, const Value* a) {
    if (AnimSlot* sl = slotOf(v, s, a[0])) sl->speed = 0;
    return Value();
  });
  implement(vm, "gsSkeleton", u"rewindAnimation", [](VM& v, Object* s, int, const Value* a) {
    if (AnimSlot* sl = slotOf(v, s, a[0])) sl->pos = 0;
    return Value();
  });
  implement(vm, "gsSkeleton", u"setAnimationPos", [](VM& v, Object* s, int, const Value* a) {
    if (AnimSlot* sl = slotOf(v, s, a[0])) sl->pos = clampCrop(v.toFloat(a[1]), sl->length);
    return Value();
  });
  implement(vm, "gsSkeleton", u"enableAnimation", [](VM& v, Object* s, int argc, const Value* a) {
    if (AnimSlot* sl = slotOf(v, s, a[0])) sl->enabled = argc > 1 ? v.truthy(a[1]) : true;
    return Value();
  });
  implement(vm, "gsSkeleton", u"disableAnimation", [](VM& v, Object* s, int, const Value* a) {
    if (AnimSlot* sl = slotOf(v, s, a[0])) sl->enabled = false;
    return Value();
  });
  // blendAnimation(name, weight, ...) / setAnimationBlend(name, weight, ...): the slot's blend weight (the timed fade
  // of the original is not reproduced)
  for (const char16_t* m : {u"blendAnimation", u"setAnimationBlend"})
    implement(vm, "gsSkeleton", m, [](VM& v, Object* s, int, const Value* a) {
      if (AnimSlot* sl = slotOf(v, s, a[0])) sl->blend = v.toFloat(a[1]);
      return Value();
    });
}

}  // namespace regs::engine
