#pragma once
// Skeletons and animation takes (docs/asset-format/skeleton-animation-scene.md) and the pose maths of the skinning
// runtime (docs/renderer/skinning.md): track sampling, bone matrices, the skin matrix palette.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "render/math3d.h"

namespace regs::render {

using ReadFile = std::function<bool(const std::string&, std::vector<uint8_t>&)>;

// .skeleton (chunk 0x02200000): bones in file order, parent < own index; the bind pose is the stored local TRS
struct BoneDef {
  std::string name;
  int parent = -1;
  Vec3 t, s{1, 1, 1};
  Quat q;  // (x, y, z, w) as stored; applied as conj(q) in column-vector maths (asset-format §1.2)
};
struct SkeletonData {
  std::vector<BoneDef> bones;
  std::vector<Mat4> inverseBind;  // inverse of each bone's bind world (gsSkeleton_OGL::captureBindPose)
  int find(const std::string& name) const;  // first bone of that name, −1 when none
};
bool loadSkeleton(const std::string& path, const ReadFile& read, SkeletonData& out, std::string* error);

// .animation take (chunk 0x02300000): tracks by bone name, keys uniformly spaced over [0, duration]
struct TrackKey {
  Vec3 t, s{1, 1, 1};
  Quat q;
};
struct AnimTrack {
  std::string bone;
  std::vector<TrackKey> keys;
};
struct AnimationData {
  float duration = 0;  // seconds
  std::vector<AnimTrack> tracks;
};
bool loadAnimation(const std::string& path, const ReadFile& read, AnimationData& out, std::string* error);

// gsTrack::getTransforms (L:0x080de180): f = (n−1)·time/duration, keys floor(f) and trunc(f + 0.99999); an index past
// the end is the last key when clamp (DONT_LOOP) is set, else key 0; t and s lerp, q nlerp with the engine's sign rule
void sampleTrack(const AnimTrack& track, float duration, float time, bool clamp, Vec3& t, Vec3& s, Quat& q);

// a bone's local matrix in column-vector form: T(t)·R(conj q)·S(s) (the engine's row-vector S·R·T)
Mat4 boneLocal(const Vec3& t, const Quat& q, const Vec3& s);

// general affine inverse (bind worlds may carry negative or non-uniform scale)
Mat4 inverseAffine(const Mat4& a);

// skin matrices for a pose of the bones' local TRS: world = parentWorld·local in file order (roots under the
// identity, gsSkeleton::animateBones sets the root transform to zero/identity), skin = world·inverseBind
void skinPalette(const SkeletonData& skel, const std::vector<Vec3>& t, const std::vector<Quat>& q,
                 const std::vector<Vec3>& s, std::vector<Mat4>& out);

}  // namespace regs::render
