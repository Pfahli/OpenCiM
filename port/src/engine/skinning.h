#pragma once
// Skeletal animation (docs/renderer/skinning.md): gsSkeleton script objects with their animation slots, the pose
// rules of the grid's animated objects (items, vehicles, characters), and draw items for skinned models.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "render/renderer3d.h"
#include "render/skeleton.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
class ObjectObj;

// one loaded animation of a skeleton (gsSkelAnim, L: loadAnim 0x080e3f30)
struct AnimSlot {
  std::shared_ptr<const render::AnimationData> anim;
  std::vector<int> trackOfBone;  // per bone: index into anim->tracks, −1 = not animated by this slot
  uint32_t flags = 0;            // loadAnimation flags: 0x20000 DONT_LOOP
  float cropStart = 0, length = 0, speed = 1, blend = 0, pos = 0;
  bool enabled = true, loop = true;
  int order = 0;                 // load order: the order slots apply per bone
};

// gsSkeleton: the skeleton a model loaded with loadSkeleton, and its named slots (std::map: name order)
class SkeletonObj : public script::Object {
 public:
  using Object::Object;
  std::shared_ptr<const render::SkeletonData> data;
  std::string path;  // the .skeleton file; animation files resolve against its directory
  std::map<std::string, AnimSlot> slots;
};

void registerSkinningNatives(script::VM& vm);

// the skeleton a model loaded (gsObject.skeleton), or null
SkeletonObj* modelSkeleton(script::VM& vm, ObjectObj* model);
// true when the model's render mesh is skinned and it has a skeleton (drawn with collectSkinned, not merged)
bool isSkinnedModel(script::VM& vm, ObjectObj* model);

using Pose = std::shared_ptr<const std::vector<render::Mat4>>;  // skin matrix per skeleton bone

// grid items and vehicles (gsGridItem::animate L:0x08181ff0, gsGridVehicle::animate L:0x081ab5f0): the first slot in
// name order at weight 1, clip position = time · slot speed (seconds); null when the model has no skeleton or slot
Pose poseFirstAnimation(script::VM& vm, ObjectObj* model, float time);

// characters (gsGridCharacterAnimations L:0x08151ea0-0x08151f60): the named slot fades in over 0.5 s while the
// previous one fades out; positions advance by factor · dt · slot speed
struct CharacterAnimation {
  std::string current, previous;
  float currentPos = 0, previousPos = 0, fade = 1, currentFactor = 1, previousFactor = 1;
};
// gsGridCharacter::move calls it with "walk" (factor = speed in m/s), "idle" or "sitting" (1), "sitdown" or "getup"
// (−1: played once to the end); returns true when the slot completed a cycle (or reached the end)
bool animateCharacter(script::VM& vm, ObjectObj* model, CharacterAnimation& a, const std::string& name, float dt,
                      float factor);
Pose poseCharacter(script::VM& vm, ObjectObj* model, const CharacterAnimation& a);

// Draw items for a model's mesh (lod 0, or 1 when it has one) at a world placement and pose (null = bind pose). The
// skinned mesh and its materials (with the combiner's skinning variant) are created on first use.
void collectSkinned(Display& d, script::VM& vm, ObjectObj* model, const render::Mat4& world, const Pose& pose,
                    std::vector<render::DrawItem>& out, int lod = 0, float alpha = 1);

}  // namespace regs::engine
