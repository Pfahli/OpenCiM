#include "render/skeleton.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace regs::render {

namespace {

struct Reader {
  const std::vector<uint8_t>& d;
  uint32_t u32(size_t at) const {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  }
  float f32(size_t at) const {
    float v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  }
  Vec3 vec3(size_t at) const { return {f32(at), f32(at + 4), f32(at + 8)}; }
  Quat quat(size_t at) const { return {f32(at), f32(at + 4), f32(at + 8), f32(at + 12)}; }
  std::string str(size_t at, size_t n) const {
    std::string s;
    for (size_t i = 0; i < n && at + i < d.size() && d[at + i]; ++i) s += char(d[at + i]);
    return s;
  }
  // chunks {u32 tag, u32 size, body[size]} in [from, end)
  template <class F>
  void children(size_t from, size_t end, F&& f) const {
    for (size_t p = from; p + 8 <= end;) {
      uint32_t tag = u32(p), size = u32(p + 4);
      size_t body = p + 8, stop = std::min(end, body + size);
      f(tag, body, stop);
      p = body + size;
    }
  }
};

Mat4 nonUniformScale(const Vec3& s) {
  Mat4 r;
  r.at(0, 0) = s.x;
  r.at(1, 1) = s.y;
  r.at(2, 2) = s.z;
  return r;
}

Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

// FUN_10085da0 (asset-format §1.2): normalize(q0 + t'·(q1 − q0)), t' = −t when the keys lie in opposite hemispheres
Quat nlerp(const Quat& a, const Quat& b, float t) {
  if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) t = -t;
  Quat r{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z), a.w + t * (b.w - a.w)};
  float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
  if (l > 0) r = {r.x / l, r.y / l, r.z / l, r.w / l};
  return r;
}

}  // namespace

int SkeletonData::find(const std::string& name) const {
  for (size_t i = 0; i < bones.size(); ++i)
    if (bones[i].name == name) return int(i);
  return -1;
}

bool loadSkeleton(const std::string& path, const ReadFile& read, SkeletonData& out, std::string* error) {
  std::vector<uint8_t> d;
  if (!read(path, d)) {
    if (error) *error = "file not found";
    return false;
  }
  Reader r{d};
  if (d.size() < 8 + 36 || r.u32(0) != 0x02200000) {
    if (error) *error = "not a .skeleton file";
    return false;
  }
  out = SkeletonData();
  size_t end = std::min(d.size(), size_t(8) + r.u32(4));
  r.children(8 + 36, end, [&](uint32_t tag, size_t body, size_t) {
    if (tag != 0x02201000) return;
    BoneDef b;
    b.name = r.str(body, 32);
    b.parent = int32_t(r.u32(body + 32));
    b.t = r.vec3(body + 36);
    b.s = r.vec3(body + 48);
    b.q = r.quat(body + 60);
    if (b.parent >= int(out.bones.size())) b.parent = -1;  // parents precede their children in all shipped files
    out.bones.push_back(b);
  });
  // bind pose (gsSkeleton_OGL::captureBindPose 0x10050110): world of the stored TRS, root treated as identity
  std::vector<Vec3> t, s;
  std::vector<Quat> q;
  for (const BoneDef& b : out.bones) {
    t.push_back(b.t);
    s.push_back(b.s);
    q.push_back(b.q);
  }
  std::vector<Mat4> world(out.bones.size());
  for (size_t i = 0; i < out.bones.size(); ++i) {
    Mat4 local = boneLocal(t[i], q[i], s[i]);
    world[i] = out.bones[i].parent >= 0 ? world[size_t(out.bones[i].parent)] * local : local;
  }
  out.inverseBind.resize(world.size());
  for (size_t i = 0; i < world.size(); ++i) out.inverseBind[i] = inverseAffine(world[i]);
  return true;
}

bool loadAnimation(const std::string& path, const ReadFile& read, AnimationData& out, std::string* error) {
  std::vector<uint8_t> d;
  if (!read(path, d)) {
    if (error) *error = "file not found";
    return false;
  }
  Reader r{d};
  if (d.size() < 8 + 40 || r.u32(0) != 0x02300000) {
    if (error) *error = "not an .animation file";
    return false;
  }
  out = AnimationData();
  out.duration = r.f32(8 + 36);
  size_t end = std::min(d.size(), size_t(8) + r.u32(4));
  r.children(8 + 40, end, [&](uint32_t tag, size_t body, size_t stop) {
    if (tag != 0x02301000) return;
    AnimTrack tr;
    tr.bone = r.str(body, 32);
    r.children(body + 36, stop, [&](uint32_t t2, size_t k, size_t) {
      if (t2 != 0x02301010) return;
      tr.keys.push_back({r.vec3(k), r.vec3(k + 12), r.quat(k + 24)});
    });
    out.tracks.push_back(std::move(tr));
  });
  return true;
}

void sampleTrack(const AnimTrack& track, float duration, float time, bool clamp, Vec3& t, Vec3& s, Quat& q) {
  int n = int(track.keys.size());
  if (n < 1) {
    t = {};
    s = {1, 1, 1};
    q = {};
    return;
  }
  float f0 = duration <= 0 ? 0.0f : float(n - 1) * (time / duration), f1 = duration <= 0 ? 0.99999f : f0 + 0.99999f;
  // the engine truncates the magnitude (an inlined float → uint conversion)
  auto index = [&](float f) {
    double a = std::trunc(std::fabs(double(f)));
    int i = a >= double(n) ? n : int(a);
    return i < n ? i : clamp ? n - 1 : 0;
  };
  int i0 = index(f0), i1 = index(f1);
  float a = f0 - std::floor(f0);
  const TrackKey &k0 = track.keys[size_t(i0)], &k1 = track.keys[size_t(i1)];
  t = lerp(k0.t, k1.t, a);
  s = lerp(k0.s, k1.s, a);
  q = nlerp(k0.q, k1.q, a);
}

Mat4 boneLocal(const Vec3& t, const Quat& q, const Vec3& s) {
  return translation(t) * rotation(conjugate(q)) * nonUniformScale(s);
}

Mat4 inverseAffine(const Mat4& a) {
  // 3×3 inverse by cofactors, then the translation
  float m00 = a.at(0, 0), m01 = a.at(0, 1), m02 = a.at(0, 2);
  float m10 = a.at(1, 0), m11 = a.at(1, 1), m12 = a.at(1, 2);
  float m20 = a.at(2, 0), m21 = a.at(2, 1), m22 = a.at(2, 2);
  float c00 = m11 * m22 - m12 * m21, c01 = m02 * m21 - m01 * m22, c02 = m01 * m12 - m02 * m11;
  float c10 = m12 * m20 - m10 * m22, c11 = m00 * m22 - m02 * m20, c12 = m02 * m10 - m00 * m12;
  float c20 = m10 * m21 - m11 * m20, c21 = m01 * m20 - m00 * m21, c22 = m00 * m11 - m01 * m10;
  float det = m00 * c00 + m01 * c10 + m02 * c20;
  Mat4 r;
  if (std::fabs(det) < 1e-20f) return r;
  float id = 1.0f / det;
  r.at(0, 0) = c00 * id; r.at(0, 1) = c01 * id; r.at(0, 2) = c02 * id;
  r.at(1, 0) = c10 * id; r.at(1, 1) = c11 * id; r.at(1, 2) = c12 * id;
  r.at(2, 0) = c20 * id; r.at(2, 1) = c21 * id; r.at(2, 2) = c22 * id;
  Vec3 tr{a.at(0, 3), a.at(1, 3), a.at(2, 3)};
  for (int i = 0; i < 3; ++i) r.at(i, 3) = -(r.at(i, 0) * tr.x + r.at(i, 1) * tr.y + r.at(i, 2) * tr.z);
  return r;
}

void skinPalette(const SkeletonData& skel, const std::vector<Vec3>& t, const std::vector<Quat>& q,
                 const std::vector<Vec3>& s, std::vector<Mat4>& out) {
  size_t n = skel.bones.size();
  std::vector<Mat4> world(n);
  out.resize(n);
  for (size_t i = 0; i < n; ++i) {
    Mat4 local = boneLocal(t[i], q[i], s[i]);
    int p = skel.bones[i].parent;
    world[i] = p >= 0 ? world[size_t(p)] * local : local;
    out[i] = world[i] * skel.inverseBind[i];
  }
}

}  // namespace regs::render
