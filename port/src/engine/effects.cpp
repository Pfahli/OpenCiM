// Viewport post effects. Bloom follows docs/renderer/bloom.md: an RGBA16F scene target (MSAA with the window's
// antialiasing, resolved), four 2:1 RGBA16F levels 512×256 … 64×32 for the squared downsample and the separable
// blur, a red-channel RMS of the smallest level as the exposure measurement, the 0.4/0.3/0.2/0.1 sum and the
// tone-mapping composite. Coordinates, quad anchoring and uv are the engine's, including its quirks for viewports
// wider than 2:1 (§4).
#include "engine/effects.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/log.h"
#include "core/text.h"
#include "engine/display.h"
#include "engine/scene3d.h"
#include "engine/settings.h"
#include "render/gl.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

struct BloomState {
  bool failed = false, ready = false;
  RenderTarget scene;
  int samples = 0;
  int requested = 0;  // window.antialiasing the scene target was made for (it steps down when a count fails)
  RenderTarget a[4], b[4];  // A0..A3, B0..B3: 512×256 … 64×32
  uint32_t scale = 0, blur = 0, add = 0, final = 0;
  float measured = 0;  // m of the last frame (bloom.md §3.4)
  Ref<Object> owner;   // kept until the scripts drop or delete the effect (sweepEffects frees the targets)
};

std::map<Object*, BloomState>& states() {
  static std::map<Object*, BloomState> s;
  return s;
}

float fieldFloat(VM& vm, Object* o, const char16_t* name, float def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toFloat(it->second);
}

void clearTarget(const RenderTarget& t) {
  gl::BindFramebuffer(gl::FRAMEBUFFER, t.fbo);
  gl::Disable(gl::SCISSOR_TEST);
  gl::ClearColor(0, 0, 0, 0);
  gl::Clear(gl::COLOR_BUFFER_BIT);
  gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
}

bool setup(Display& d, VM& vm, BloomState& s) {
  if (s.ready || s.failed) return s.ready;
  s.scale = combinerProgram(d, vm, "shaders/bloom/scale.shader", 0, 0);
  s.blur = combinerProgram(d, vm, "shaders/bloom/blur.shader", 0, 0);
  s.add = combinerProgram(d, vm, "shaders/bloom/add.shader", 0, 0);
  s.final = combinerProgram(d, vm, "shaders/bloom/final.shader", 0, 0);
  bool ok = s.scale && s.blur && s.add && s.final;
  for (int i = 0; i < 4 && ok; ++i) {
    int w = 512 >> i, h = 256 >> i;
    ok = d.r3d().makeTarget(s.a[i], w, h, TargetFormat::Rgba16f, false, true) &&
         d.r3d().makeTarget(s.b[i], w, h, TargetFormat::Rgba16f, false, true);
    if (ok) {  // cleared once at creation, never again (bloom.md §3.2)
      clearTarget(s.a[i]);
      clearTarget(s.b[i]);
    }
  }
  if (!ok) {
    LOG_ERROR("Bloom initialization failed!");
    s.failed = true;
    return false;
  }
  s.ready = true;
  return true;
}

// bx, by: the part of the 2:1 targets the passes write (bloom.md §3.2)
void subRect(int vw, int vh, float& bx, float& by) {
  if (2 * vh < vw) {
    bx = 1;
    by = 2.0f * float(vh) / float(vw);
  } else {
    bx = float(vw) / (2.0f * float(vh));
    by = 1;
  }
}

}  // namespace

void initEffectFields(Object* e) {
  if (e->cls->name == "gsBloomEffect_OGL") {
    e->fieldValues[u"scale"] = Value::real(0.5f);
    e->fieldValues[u"saturation"] = Value::real(1.0f);
    e->fieldValues[u"lightness"] = Value::real(0.9f);
    e->fieldValues[u"saturationWeight"] = Value::real(1.0f);
  } else if (e->cls->name == "gsShadowEffect_OGL") {
    e->fieldValues[u"maxDistance"] = Value::real(-1.0f);
  }
}

// ---- port style effects ----

namespace {

struct StyleState {
  bool failed = false;
  uint32_t grade = 0, tilt = 0, dof = 0;
  RenderTarget input, blurA, blurB;
};

StyleState& styleState() {
  static StyleState s;
  return s;
}

// colour styles: saturation, vibrance, contrast, brightness, warmth, tint, split toning (index = regsColorStyle)
const float kColorStyles[][7] = {
    {1.00f, 0.00f, 1.00f, 1.00f, 0.00f, 0.00f, 0.0f},   // original
    {1.12f, 0.45f, 1.06f, 1.03f, 0.05f, 0.00f, 0.0f},   // vibrant
    {1.05f, 0.15f, 1.03f, 1.02f, 0.45f, 0.00f, 0.0f},   // warm
    {0.98f, 0.10f, 1.04f, 1.00f, -0.40f, 0.00f, 0.0f},  // cool
    {0.92f, 0.20f, 1.12f, 0.98f, 0.10f, -0.15f, 1.0f},  // cinematic
    {0.00f, 0.00f, 1.15f, 1.02f, 0.00f, 0.00f, 0.0f},   // black and white
};
constexpr int kColorStyleCount = int(sizeof(kColorStyles) / sizeof(kColorStyles[0]));
// lens blur modes (regsLensBlur): off, distance light, distance strong, tilt-shift; max radius in texels and the
// distance blur's strength (circle of confusion per unit of |1/focus − 1/z|)
const float kLensBlurRadius[] = {0, 6, 10, 8};
const float kLensBlurStrength[] = {0, 120, 250, 0};

int colorStyle(VM& vm) { return std::clamp(int(settingNumber(vm, u"regsColorStyle", 0)), 0, kColorStyleCount - 1); }
int lensBlur(VM& vm) { return std::clamp(int(settingNumber(vm, u"regsLensBlur", 0)), 0, 3); }

uint32_t postProgram(Display& d, VM& vm, const char* fragment) {
  std::vector<uint8_t> vs, fs;
  if (!vm.vfs().read("regs/shaders/post.vertex", vs) || !vm.vfs().read(std::string("regs/shaders/") + fragment, fs)) return 0;
  std::string err;
  uint32_t p = d.r3d().buildProgram(std::string(vs.begin(), vs.end()), std::string(fs.begin(), fs.end()), &err);
  if (!p) LOG_ERROR("regs/shaders/%s: %s", fragment, err.c_str());
  return p;
}

bool ensureTarget(Display& d, RenderTarget& t, int w, int h, int samples, bool depthTexture = false) {
  if (t.fbo && t.width == w && t.height == h && t.samples == samples && (t.depthTex != 0) == depthTexture) return true;
  d.r3d().destroyTarget(t);
  bool ok = d.r3d().makeSceneTarget(t, w, h, samples, depthTexture);
  if (!ok) d.r3d().destroyTarget(t);
  return ok;
}

}  // namespace

bool styleActive(VM& vm) { return colorStyle(vm) != 0 || lensBlur(vm) != 0; }

const RenderTarget* styleTarget(Display& d, VM& vm, int w, int h, int samples) {
  StyleState& s = styleState();
  if (s.failed) return nullptr;
  if (!s.grade) {
    s.grade = postProgram(d, vm, "grade.fragment");
    s.tilt = postProgram(d, vm, "tiltshift.fragment");
    s.dof = postProgram(d, vm, "dof.fragment");
    if (!s.grade || !s.tilt || !s.dof) {
      s.failed = true;
      return nullptr;
    }
  }
  for (int n = std::max(0, samples); n >= 0; n -= 2) {  // MSAA steps down by 2 like the engine's targets
    if (ensureTarget(d, s.input, w, h, n, true)) return &s.input;  // with a depth texture for the distance blur
    if (n == 0) break;
  }
  s.failed = true;
  return nullptr;
}

void styleFinish(Display& d, VM& vm, int x, int y, int w, int h, int backW, int backH, float nearZ, float farZ, float focus) {
  StyleState& s = styleState();
  Renderer3D& r = d.r3d();
  r.resolve(s.input);
  const float uv[4][4] = {{0, 1, 0, 0}, {1, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}};  // TL, TR, BR, BL
  uint32_t image = s.input.color;
  if (int blur = lensBlur(vm); blur > 0 && ensureTarget(d, s.blurA, w, h, 0) && ensureTarget(d, s.blurB, w, h, 0)) {
    const float ch[4] = {1.0f / float(w), 0, kLensBlurRadius[blur], 0}, cv[4] = {0, 1.0f / float(h), kLensBlurRadius[blur], 0};
    // distance blur: thin lens focused on the camera target; tilt-shift: a sharp band at the screen centre
    bool tilt = blur == 3;
    const float lens[4] = {nearZ, farZ, std::max(focus, 1.0f), kLensBlurStrength[blur]}, band[4] = {0.5f, 0.10f, 0.35f, 0};
    uint32_t prog = tilt ? s.tilt : s.dof;
    gl::Disable(gl::SCISSOR_TEST);
    gl::BindFramebuffer(gl::FRAMEBUFFER, s.blurA.fbo);
    gl::Viewport(0, 0, w, h);
    const uint32_t first[2] = {image, s.input.depthTex}, second[2] = {s.blurA.color, s.input.depthTex};
    r.drawQuad(prog, first, tilt ? 1 : 2, ch, uv, nullptr, tilt ? band : lens);
    gl::BindFramebuffer(gl::FRAMEBUFFER, s.blurB.fbo);
    r.drawQuad(prog, second, tilt ? 1 : 2, cv, uv, nullptr, tilt ? band : lens);
    image = s.blurB.color;
  }
  const float* st = kColorStyles[colorStyle(vm)];
  const float c0[4] = {st[0], st[1], st[2], st[3]}, c1[4] = {st[4], st[5], st[6], 0};
  gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
  gl::Viewport(x, backH - y - h, w, h);
  gl::Enable(gl::SCISSOR_TEST);
  gl::Scissor(x, backH - y - h, w, h);
  r.drawQuad(s.grade, &image, 1, c0, uv, nullptr, c1);
  gl::Viewport(0, 0, backW, backH);
}

void styleTakeDepth(Display& d, const RenderTarget& scene, int w, int h) {
  StyleState& s = styleState();
  if (s.input.depthTex) d.r3d().blitDepth(scene, s.input, w, h);
}

// ---- shadows (docs/renderer/shadows.md) ----

namespace {

struct V2 {
  float x = 0, y = 0;
  V2 operator+(V2 o) const { return {x + o.x, y + o.y}; }
  V2 operator-(V2 o) const { return {x - o.x, y - o.y}; }
  V2 operator*(float s) const { return {x * s, y * s}; }
};
float dot2(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
float cross2(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
V2 norm2(V2 a) {
  float l = std::sqrt(dot2(a, a));
  return l > 0 ? a * (1.0f / l) : a;
}

// gsMatrix ⊗ gsVector: the point through the matrix, divided by w unless w is ~0 (L:0x0810deb0)
V2 projectXY(const Mat4& m, const Vec3& p) {
  float x = m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3);
  float y = m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3);
  float w = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
  if (std::fabs(w) >= 1e-5f) {
    x /= w;
    y /= w;
  }
  return {x, y};
}

// one row of the warp acting on (x, y, ·, 1): x·p.x + y·p.y + c
struct Row {
  float x = 0, y = 0, c = 0;
  float at(V2 p) const { return x * p.x + y * p.y + c; }
  Row operator+(const Row& o) const { return {x + o.x, y + o.y, c + o.c}; }
  Row operator*(float s) const { return {x * s, y * s, c * s}; }
};

// gsTrapezoidToSquare (L:0x081e0f00, shadows.md §3.5): t3 → (−1, 1), t2 → (1, 1), t0 → (−1, −1), t1 → (1, ≈−1);
// identity when any denominator is zero
Mat4 trapezoidToSquare(V2 t0, V2 t1, V2 t2, V2 t3) {
  Mat4 id;
  V2 a = (t2 - t3) * 0.5f;
  Row r0{a.x, a.y, 0}, r1{a.y, -a.x, 0};
  V2 d1 = t3 - t0, d2 = t2 - t1;  // the side lines t0–t3 and t1–t2
  float den = cross2(d1, d2);
  if (den == 0) return id;
  V2 i = t0 + d1 * (cross2(t1 - t0, d2) / den);
  r0.c = -(r0.x * i.x + r0.y * i.y);
  r1.c = -(r1.x * i.x + r1.y * i.y);
  float ky = r1.at(t2) + r1.at(t3);
  if (ky == 0) return id;
  r0 = r0 + r1 * (-(r0.at(t3) + r0.at(t2)) / ky);  // shear: the top edge becomes symmetric
  float x2 = r0.at(t2), y2 = r1.at(t2);
  if (x2 == 0 || y2 == 0) return id;
  r0 = r0 * (1.0f / x2);
  r1 = r1 * (1.0f / y2);
  Row r3 = r1;
  r1.c += 1;
  float w2 = r3.at(t2), w0 = r3.at(t0);
  if (w2 == 0 || w0 == 0) return id;
  r1 = r1 + r3 * (-0.5f * (r1.at(t2) / w2 + r1.at(t0) / w0));  // centre y
  float y0 = r1.at(t0);
  if (y0 == 0) return id;
  r1 = r1 * (-w0 / y0);  // t0 → y = −1
  Mat4 m;  // rows r0, r1, (0,0,1,0), r3 over (x, y, z, w)
  m.at(0, 0) = r0.x; m.at(0, 1) = r0.y; m.at(0, 2) = 0; m.at(0, 3) = r0.c;
  m.at(1, 0) = r1.x; m.at(1, 1) = r1.y; m.at(1, 2) = 0; m.at(1, 3) = r1.c;
  m.at(2, 0) = 0;    m.at(2, 1) = 0;    m.at(2, 2) = 1; m.at(2, 3) = 0;
  m.at(3, 0) = r3.x; m.at(3, 1) = r3.y; m.at(3, 2) = 0; m.at(3, 3) = r3.c;
  return m;
}

struct ShadowState {
  bool failed = false;
  RenderTarget map;
  Ref<Object> owner;  // as BloomState::owner
};

std::map<Object*, ShadowState>& shadowStates() {
  static std::map<Object*, ShadowState> s;
  return s;
}

}  // namespace

bool fitShadow(const ShadowFitIn& in, ShadowFitOut& o) {
  // §3.1 main-camera frustum
  Vec3 F = normalize(in.target - in.eye);
  float t = std::tan(in.fov * 0.0174532924f * 0.5f);
  Vec3 R = normalize(cross(F, in.up)) * (t * in.aspect);
  Vec3 U = normalize(cross(R, F)) * t;
  const Vec3 d[4] = {F + R + U, F + R - U, F - R + U, F - R - U};
  // §3.2 frustum points: on the plane y = target.y + yPlane (clipYPlane), else far and near corners
  Vec3 C[8];
  if (in.clipYPlane) {
    float plane = in.target.y + in.yPlane;
    for (int i = 0; i < 4; ++i) {
      Vec3 r = d[i] * in.far;
      if (r.y != 0) r = r * ((plane - in.eye.y) / r.y);
      C[i] = C[i + 4] = in.eye + r;
    }
  } else {
    for (int k = 0; k < 4; ++k) {
      C[2 * k] = in.eye + d[k] * in.far;
      C[2 * k + 1] = in.eye + d[k] * in.near;
    }
  }
  // §3.3 light basis and the three-pass fit
  const Vec3 Ls = in.light;
  Vec3 M{0, 0, 0};
  for (const Vec3& c : C) M = M + c;
  M = M * 0.125f;
  Vec3 Z = normalize(M - Ls), X = normalize(cross(Vec3{1, 0, 0}, Z)), Y = cross(X, Z);
  float umin = 0, umax = 0, vmin = 0, vmax = 0, gmin = 0, gmax = 0;
  for (int iter = 0; iter < 3; ++iter) {
    umin = vmin = 1e5f;
    umax = vmax = -1e5f;
    gmin = 1e6f;
    gmax = 0;
    for (const Vec3& c : C) {
      Vec3 p = c - Ls;  // = α·Y + β·X + γ·Z (orthonormal basis)
      float a = dot(p, Y), b = dot(p, X), g = dot(p, Z);
      float u = a / g, v = b / g;
      umin = std::min(umin, u);
      umax = std::max(umax, u);
      vmin = std::min(vmin, v);
      vmax = std::max(vmax, v);
      gmin = std::min(gmin, g);
      gmax = std::max(gmax, g);
    }
    Z = normalize(Z + Y * ((umin + umax) * 0.5f) + X * ((vmin + vmax) * 0.5f));
    Y = normalize(cross(X, Z));
    X = cross(Y, Z);
  }
  o.axisZ = Z;
  o.axisX = X;
  o.axisY = Y;
  // §3.4 light camera: 1.5× the bounding angle, near/far around the points
  o.lightFov = std::max(3.0f * std::atan(std::max(umax - umin, vmax - vmin) * 0.5f) * 57.29578f, 0.001f);
  o.lightNear = 0.99f * gmin;
  o.lightFar = 1.01f * gmax;
  o.lightView = lookAt(Ls, Ls + Z * gmax, X);
  o.lightProj = perspective(o.lightFov, float(in.mapWidth) / float(std::max(1, in.mapHeight)), o.lightNear, o.lightFar);
  Mat4 vp = o.lightProj * o.lightView;
  // §3.5 warp: the footprint quad, each corner grown toward the projected eye by at most safeMargin
  V2 P[4];
  for (int i = 0; i < 4; ++i) P[i] = projectXY(vp, C[i]);
  V2 E = projectXY(vp, in.eye);
  for (int i = 0; i < 4; ++i) {
    o.projected[i][0] = P[i].x;
    o.projected[i][1] = P[i].y;
  }
  o.projectedEye[0] = E.x;
  o.projectedEye[1] = E.y;
  if (in.clipYPlane) {
    auto k = [&](float x) { return std::clamp(x, 0.0f, in.safeMargin); };
    V2 B = (P[1] + P[3]) * 0.5f, T = (P[0] + P[2]) * 0.5f;
    V2 D = norm2(B - T), W = norm2(P[1] - P[3]);
    V2 D2 = norm2(T - B), W2 = norm2(P[0] - P[2]);
    V2 q[4] = {P[0], P[1], P[2], P[3]};
    q[1] = P[1] + D * k(dot2(D, E - B)) + W * k(dot2(W, E - B));
    q[3] = P[3] + D * k(dot2(D, E - B)) - W * k(dot2(W, B - E));
    q[0] = P[0] + D2 * k(dot2(D2, E - T)) + W2 * k(dot2(W2, E - T));
    q[2] = P[2] + D2 * k(dot2(D2, E - T)) - W2 * k(dot2(W2, T - E));
    for (int i = 0; i < 4; ++i) {
      o.warpedCorners[i][0] = q[i].x;
      o.warpedCorners[i][1] = q[i].y;
    }
    o.warp = trapezoidToSquare(q[3], q[1], q[0], q[2]);
  } else {
    o.warp = Mat4();  // gsFitTrapezoid (clipYPlane = false) is not ported; the game never uses it
  }
  // §3.6 shadowParams: half a texel, fade range, depth bias per unit of eye depth
  float half = 0.5f / float(std::max(1, in.mapWidth));
  if (in.maxDistance == -1.0f) {
    o.params[0] = half;
    o.params[1] = o.params[2] = -o.lightFar;
    o.params[3] = 1.5e-10f * o.lightFar;
  } else {
    o.params[0] = half;
    o.params[1] = -0.7f * in.maxDistance;
    o.params[2] = -in.maxDistance;
    o.params[3] = 1.5e-10f * in.maxDistance;
  }
  return std::isfinite(o.lightFov) && gmax > 0;
}

const RenderTarget* shadowTarget(Display& d, Object* shadow) {
  ShadowState& s = shadowStates()[shadow];
  if (!s.owner) s.owner = Ref<Object>(shadow);
  if (s.failed) return nullptr;
  if (!s.map.fbo) {
    auto size = [&](const char16_t* n) {
      auto it = shadow->fieldValues.find(n);
      return it == shadow->fieldValues.end() ? 1024 : std::max(1, int(it->second.toInt()));
    };
    if (!d.r3d().makeDepthTarget(s.map, size(u"<width>"), size(u"<height>"))) {
      LOG_ERROR("Shadowmap framebuffer creation failed");
      d.r3d().destroyTarget(s.map);
      s.failed = true;
      return nullptr;
    }
  }
  return &s.map;
}

void bloomTick(VM& vm, Object* bloom, float dt) {
  // E is the script field scale; it approaches last frame's measurement with a half-life of 1 s
  float e = fieldFloat(vm, bloom, u"scale", 0.5f), m = states()[bloom].measured;
  if (e == -1.0f || dt < 0.0001f) {
    e = m;
  } else {
    e += (1.0f - std::pow(0.5f, dt)) * (m - e);
    if (!(e > 0.0f && e <= 100000.0f)) e = 1.0f;  // also catches NaN, which the engine keeps
  }
  bloom->fieldValues[u"scale"] = Value::real(e);
}

const RenderTarget* bloomSceneTarget(Display& d, VM& vm, Object* bloom, int w, int h) {
  BloomState& s = states()[bloom];
  if (!s.owner) s.owner = Ref<Object>(bloom);
  if (!setup(d, vm, s)) return nullptr;
  // grow-only, exactly the viewport size when (re)allocated; MSAA with window.antialiasing, −2 per failure. Port: a
  // new antialiasing setting (graphics page, live) or a new window size (resetSizedEffects) makes it again
  if (!s.scene.fbo || w > s.scene.width || h > s.scene.height || s.requested != d.antialiasing) {
    int nw = s.requested == d.antialiasing ? std::max(w, s.scene.width) : w;
    int nh = s.requested == d.antialiasing ? std::max(h, s.scene.height) : h;
    d.r3d().destroyTarget(s.scene);
    s.requested = d.antialiasing;
    bool ok = false;
    for (int samples = d.antialiasing; samples >= 0 && !ok; samples -= 2) {
      ok = d.r3d().makeSceneTarget(s.scene, nw, nh, samples);
      if (!ok) d.r3d().destroyTarget(s.scene);
      else s.samples = samples;
      if (samples == 0) break;
    }
    if (!ok) {
      LOG_ERROR("Framebuffer initialization failed!");
      s.failed = true;
      s.ready = false;
      return nullptr;
    }
  }
  return &s.scene;
}

void bloomFinish(Display& d, VM& vm, Object* bloom, int x, int y, int w, int h, int backW, int backH, const RenderTarget* out) {
  BloomState& s = states()[bloom];
  if (!s.ready) return;
  Renderer3D& r = d.r3d();
  r.resolve(s.scene);
  float bx, by;
  subRect(w, h, bx, by);
  float st = float(w) / float(s.scene.width), tt = float(h) / float(s.scene.height);
  const float rect[4] = {0, 0, bx, by};  // top-anchored quad [0, bx] × [0, by] (bloom.md §3.2)
  auto uvFor = [](float su, float tv, float out[4][4]) {
    const float corners[4][2] = {{0, tv}, {su, tv}, {su, 0}, {0, 0}};  // TL, TR, BR, BL
    for (int k = 0; k < 4; ++k) {
      out[k][0] = corners[k][0];
      out[k][1] = corners[k][1];
      out[k][2] = out[k][3] = 0;
    }
  };
  auto into = [&](const RenderTarget& t) {
    gl::BindFramebuffer(gl::FRAMEBUFFER, t.fbo);
    gl::Disable(gl::SCISSOR_TEST);
    gl::Viewport(0, 0, t.width, t.height);
  };
  float uv[4][4];
  // pass 1: (mean of 4 taps)² of the scene into A0
  into(s.a[0]);
  uvFor(st, tt, uv);
  const float c1[4] = {1.0f / 2048, 1.0f / 1024, 0, 0};
  r.drawQuad(s.scale, &s.scene.color, 1, c1, uv, rect);
  // passes 2-9: horizontal into B_i, vertical into A_i; the first of each level reads the level above
  uvFor(bx, by, uv);
  for (int i = 0; i < 4; ++i) {
    const RenderTarget& src = s.a[i == 0 ? 0 : i - 1];
    float tw = float(s.a[i].width), th = float(s.a[i].height);
    const float ch[4] = {1.0f / tw, 0, bx, by}, cv[4] = {0, 1.0f / th, bx, by};
    into(s.b[i]);
    r.drawQuad(s.blur, &src.color, 1, ch, uv, rect);
    into(s.a[i]);
    r.drawQuad(s.blur, &s.b[i].color, 1, cv, uv, rect);
  }
  // measurement for the next frame: RMS of the red channel over the whole 64×32 level
  std::vector<float> px;
  r.readRed(s.a[3].color, s.a[3].width, s.a[3].height, px);
  double sum = 0;
  for (float v : px) sum += v;
  float m = float(std::sqrt(sum / 2048.0));
  s.measured = (m > 100000.0f || !(m > 0.0f)) ? 1.0f : m;
  // pass 10: weighted sum of the four levels into B0
  into(s.b[0]);
  const uint32_t levels[4] = {s.a[0].color, s.a[1].color, s.a[2].color, s.a[3].color};
  r.drawQuad(s.add, levels, 4, nullptr, uv, rect);
  // pass 11: composite into the back-buffer viewport rect (or the bottom-left of the given target)
  gl::BindFramebuffer(gl::FRAMEBUFFER, out ? out->fbo : 0);
  int oy = out ? 0 : backH - y - h, ox = out ? 0 : x;
  gl::Viewport(ox, oy, w, h);
  gl::Enable(gl::SCISSOR_TEST);
  gl::Scissor(ox, oy, w, h);
  float e = fieldFloat(vm, bloom, u"scale", 0.5f);
  if (e == -1.0f) e = s.measured;
  float saturation = fieldFloat(vm, bloom, u"saturation", 1.0f);
  const float c11[4] = {fieldFloat(vm, bloom, u"lightness", 0.9f) / (e * 3.0f + 0.1f), saturation,
                        (1.0f - saturation) * fieldFloat(vm, bloom, u"saturationWeight", 1.0f), 0};
  const float composite[4][4] = {{0, tt, 0, by}, {st, tt, bx, by}, {st, 0, bx, 0}, {0, 0, 0, 0}};
  const uint32_t inputs[2] = {s.scene.color, s.b[0].color};
  r.drawQuad(s.final, inputs, 2, c11, composite);
  gl::Viewport(0, 0, backW, backH);
}

// ---- live graphics settings (port) ----

namespace {

RenderTarget& msaaScene() {
  static RenderTarget t;
  return t;
}

}  // namespace

const RenderTarget* msaaTarget(Display& d, int w, int h, int samples) {
  RenderTarget& t = msaaScene();
  if (t.fbo && t.width == w && t.height == h && t.samples == samples) return &t;
  for (int n = samples; n > 0; n -= 2) {  // MSAA steps down by 2 like the engine's targets
    d.r3d().destroyTarget(t);
    if (d.r3d().makeSceneTarget(t, w, h, n, false, TargetFormat::Rgba8)) {
      if (n != samples) LOG_WARN("antialiasing: %d samples unavailable, using %d", samples, n);
      t.samples = samples;  // the requested count, so a failed step is not retried every frame
      return &t;
    }
  }
  d.r3d().destroyTarget(t);
  return nullptr;
}

void msaaFinish(Display& d, int x, int y, int w, int h, int backH) { d.r3d().blitToBack(msaaScene(), x, y, w, h, backH); }

void sweepEffects(Display& d) {
  auto dropped = [](const Ref<Object>& o) { return o && (o->destroyed || o->refs() == 1); };
  for (auto it = states().begin(); it != states().end();) {
    if (!dropped(it->second.owner)) { ++it; continue; }
    BloomState& s = it->second;
    d.r3d().destroyTarget(s.scene);
    for (int i = 0; i < 4; ++i) {
      d.r3d().destroyTarget(s.a[i]);
      d.r3d().destroyTarget(s.b[i]);
    }
    it = states().erase(it);
  }
  for (auto it = shadowStates().begin(); it != shadowStates().end();) {
    if (!dropped(it->second.owner)) { ++it; continue; }
    d.r3d().destroyTarget(it->second.map);
    it = shadowStates().erase(it);
  }
}

void resetSizedEffects(Display& d) {
  // the grow-only bloom scene targets start again at the new window size; the style and MSAA targets follow the view
  // size by themselves
  for (auto& [o, s] : states()) {
    d.r3d().destroyTarget(s.scene);
    s.requested = -1;
  }
  StyleState& st = styleState();
  d.r3d().destroyTarget(st.input);
  d.r3d().destroyTarget(st.blurA);
  d.r3d().destroyTarget(st.blurB);
  d.r3d().destroyTarget(msaaScene());
}

}  // namespace regs::engine
