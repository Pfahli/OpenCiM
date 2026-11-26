// Stereoscopic 3D (engine/stereo.h, docs/stereo-3d.md).
#include "engine/stereo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/env.h"
#include "core/log.h"
#include "engine/display.h"
#include "engine/leiasr.h"
#include "engine/settings.h"
#include "render/gpu.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::render;

namespace {

struct StereoState {
  int eye = 0;
  int lastMode = 0;
  int lastWanted = 0;  // the mode asked for (before a failed Leia SR start turns it off)
  int testMode = -1;
  bool failed = false;
  uint32_t program = 0;
  RenderTarget eyes[2];
  bool eyesFloat = false;
  // this frame's settings (stereoFrameMode)
  float parallax = 0.025f;    // on-screen distance between the eyes' pictures of a far point, in image widths
  float convergence = 1.0f;   // the screen plane at this many times the camera's distance to its look point
  float miniature = 1.0f;     // the eyes at most this × 13 % of the camera's distance apart (1 = a model on a table)
  bool swap = false;
};

StereoState& state() {
  static StereoState s;
  return s;
}

}  // namespace

int stereoFrameMode(script::VM& vm) {
  StereoState& s = state();
  // no stored choice: glasses-free 3D on its own when the game finds an SR display (owner 2026-11-26: "auto detection")
  int mode = int(settingNumber(vm, u"regsStereo", -1));
  if (mode < 0) mode = leiaSrDetected().usable ? kStereoLeiaSr : kStereoOff;
  float depth = settingNumber(vm, u"regsStereoDepth", 50);
  float conv = settingNumber(vm, u"regsStereoConvergence", 100);
  bool swap = settingNumber(vm, u"regsStereoSwap", 0) > 0;
  float mini = settingNumber(vm, u"regsStereoMiniature", 100);
  // REGS_STEREO=<mode>, REGS_STEREO_DEPTH=<0..100>, REGS_STEREO_CONVERGENCE=<percent>, REGS_STEREO_SWAP=0|1,
  // REGS_STEREO_MINIATURE=<0..200> (tests)
  if (const char* e = regs::envVar("REGS_STEREO_MINIATURE")) mini = float(std::atof(e));
  if (const char* e = regs::envVar("REGS_STEREO")) mode = std::atoi(e);
  if (const char* e = regs::envVar("REGS_STEREO_DEPTH")) depth = float(std::atof(e));
  if (const char* e = regs::envVar("REGS_STEREO_CONVERGENCE")) conv = float(std::atof(e));
  if (const char* e = regs::envVar("REGS_STEREO_SWAP")) swap = std::atoi(e) != 0;
  if (s.testMode >= 0) mode = s.testMode;
  if (mode < 0 || mode >= kStereoModeCount || s.failed) mode = 0;
  // glasses-free 3D that could not start (engine/leiasr.cpp logged why): off until the mode is chosen again
  if (mode != s.lastWanted) {
    if (mode == kStereoLeiaSr) leiaSrRetry();
    s.lastWanted = mode;
  }
  if (mode == kStereoLeiaSr && leiaSrFailed()) mode = 0;
  // depth 100 = 5 % of the image width between the eyes' pictures of the horizon (comfortable stays under ~3 %)
  s.parallax = std::clamp(depth, 0.0f, 100.0f) / 100.0f * 0.05f;
  s.convergence = std::clamp(conv, 25.0f, 400.0f) / 100.0f;
  s.swap = swap;
  s.miniature = std::clamp(mini, 0.0f, 200.0f) / 100.0f;
  if (mode != s.lastMode) {
    LOG_INFO("stereo 3D: mode %d -> %d", s.lastMode, mode);
    // frame generation would make in-between frames from one eye's motion: off while stereo is on (temporalBegin,
    // which is skipped then, switches it back)
    if (mode) gpu::dev().setFrameGen(false);
    s.lastMode = mode;
  }
  return mode;
}

void stereoTestMode(int mode) { state().testMode = mode; }

int stereoEye() { return state().eye; }
void stereoSetEye(int eye) { state().eye = eye; }

void stereoCamera(script::VM& vm, Vec3& eye, Vec3& look, const Vec3& up, Mat4& proj) {
  (void)vm;
  const StereoState& s = state();
  if (!s.eye) return;
  Vec3 fwd = look - eye;
  const float dist = std::sqrt(dot(fwd, fwd));
  const float p00 = proj.at(0, 0);
  if (dist < 1e-4f || std::fabs(p00) < 1e-6f) return;
  fwd = fwd * (1.0f / dist);
  Vec3 right = cross(fwd, up);
  const float rl = std::sqrt(dot(right, right));
  if (rl < 1e-6f) return;
  right = right * (1.0f / rl);
  // Each eye goes sideways by b (half the eyes' distance) and its frustum is shifted back by c = b·P00 / D, so the
  // point at the screen plane distance D does not move. A point at view depth z then lies b·P00·(1/D − 1/z) image
  // widths apart between the eyes' pictures (far: behind the screen, near: in front of it).
  // Plain: b such that the horizon gets the depth setting's distance B (b = B·D / P00), whatever is in view.
  // Miniature (owner 2026-11-23, "treated like a miniature to really have a 3D effect"): the eyes as far apart as a
  // person's looking at a model on a table (6.5 cm eyes 50 cm away: 13 % of the distance, × the miniature setting),
  // so the town looks like a model. What is in view, the ground from the bottom to the top edge of the picture (the
  // camera has no roll, so the top edge is the farthest ground), decides the rest: the screen plane goes to its middle
  // (equal depth in front and behind, × the screen plane setting) and the eyes come no farther apart than keeps either
  // end within 1.5 × B. Looking down on the town that is far apart; toward the horizon (infinitely far) it closes in.
  float screenD = dist * s.convergence;
  float b = s.parallax * screenD / p00;
  if (s.miniature > 0 && s.parallax > 0) {
    const float h = eye.y - look.y;  // above the ground at the look point
    const float p11 = proj.at(1, 1);
    if (h > 0.01f && std::fabs(p11) > 1e-6f) {
      const float half = std::atan(1.0f / std::fabs(p11));
      const float pitch = std::asin(std::clamp(-fwd.y, -1.0f, 1.0f));
      const float top = pitch - half, bottom = std::min(pitch + half, 1.5707f);
      const float farZ = top > 0.01f ? h / std::sin(top) * std::cos(half) : 1e9f;
      if (bottom > 0.01f) {
        // the ground at the bottom edge; things standing on it come nearer: 15 % as a margin
        const float nearZ = h / std::sin(bottom) * std::cos(half) * 0.85f;
        screenD = s.convergence * 2.0f / (1.0f / nearZ + 1.0f / farZ);
        const float behind = 1.0f / screenD - 1.0f / farZ, front = 1.0f / nearZ - 1.0f / screenD;
        b = s.miniature * 0.065f * dist;
        const float most = std::max(behind, front);
        if (most > 1e-7f) b = std::min(b, 1.5f * s.parallax / (p00 * most));
      }
    }
  }
  const float sign = float(s.eye);
  const float c = sign * b * p00 / screenD;
  const Vec3 shift = right * (sign * b);
  eye = eye + shift;
  look = look + shift;
  for (int col = 0; col < 4; ++col) proj.at(0, col) += c * proj.at(3, col);  // x_clip += c · w_clip
}

void stereoKeepEye(Display& d, int eye, int w, int h) {
  StereoState& s = state();
  if (w <= 0 || h <= 0) return;
  RenderTarget& t = s.eyes[eye < 0 ? 0 : 1];
  const bool want16 = gpu::dev().hdrActive();  // the HDR back buffer holds values above 1
  if (t.fbo && (t.width != w || t.height != h || s.eyesFloat != want16)) d.r3d().destroyTarget(t);
  if (!t.fbo) {
    if (!d.r3d().makeTarget(t, w, h, want16 ? TargetFormat::Rgba16f : TargetFormat::Rgba8, false, true)) {
      d.r3d().destroyTarget(t);
      return;
    }
    if (eye > 0) s.eyesFloat = want16;
  }
  gpu::dev().blit(0, t.fbo, 0, 0, w, h, 0, 0, w, h, true, false);
}

bool stereoCompose(Display& d, script::VM& vm, int mode, int w, int h, uint32_t target, bool flipY) {
  StereoState& s = state();
  if (!s.eyes[0].fbo || !s.eyes[1].fbo || s.eyes[0].width != w || s.eyes[1].width != w) return false;
  if (!s.program && !s.failed) {
    std::vector<uint8_t> vs, fs;
    std::string err;
    if (vm.vfs().read("regs/shaders/post.vertex", vs) && vm.vfs().read("regs/shaders/stereo.fragment", fs))
      s.program = d.r3d().buildProgram(std::string(vs.begin(), vs.end()), std::string(fs.begin(), fs.end()), &err);
    else
      err = "regs/shaders/stereo.fragment or post.vertex missing";
    if (!s.program) {
      LOG_ERROR("stereo 3D: %s; stereo stays off", err.c_str());
      s.failed = true;
    }
  }
  if (!s.program) return false;
  gpu::Device& g = gpu::dev();
  const uint32_t tex[2] = {s.eyes[s.swap ? 1 : 0].color, s.eyes[s.swap ? 0 : 1].color};
  const float c0[4] = {float(mode), float(w), float(h), 0};
  float uv[4][4] = {{0, 1, 0, 0}, {1, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}};  // TL, TR, BR, BL
  if (flipY)
    for (auto& c : uv) c[1] = 1.0f - c[1];
  g.bindFramebuffer(target);
  g.scissorTest(false);
  g.viewport(0, 0, w, h);
  d.r3d().drawQuad(s.program, tex, 2, c0, uv);
  return true;
}

void stereoReset(Display& d) {
  leiaSrStop(d);  // its targets go with the others (it starts again on the next frame)
  StereoState& s = state();
  d.r3d().destroyTarget(s.eyes[0]);
  d.r3d().destroyTarget(s.eyes[1]);
}

}  // namespace regs::engine
