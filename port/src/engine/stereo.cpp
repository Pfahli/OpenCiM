// Stereoscopic 3D (engine/stereo.h, docs/renderer/stereo.md).
#include "engine/stereo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/env.h"
#include "core/log.h"
#include "engine/display.h"
#include "engine/settings.h"
#include "render/gpu.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::render;

namespace {

struct StereoState {
  int eye = 0;
  int lastMode = 0;
  int testMode = -1;
  bool failed = false;
  uint32_t program = 0;
  RenderTarget eyes[2];
  bool eyesFloat = false;
  // this frame's settings (stereoFrameMode)
  float parallax = 0.025f;    // on-screen distance between the eyes' pictures of a far point, in image widths
  float convergence = 1.0f;   // the screen plane at this many times the camera's distance to its look point
  bool swap = false;
};

StereoState& state() {
  static StereoState s;
  return s;
}

}  // namespace

int stereoFrameMode(script::VM& vm) {
  StereoState& s = state();
  int mode = int(settingNumber(vm, u"regsStereo", 0));
  float depth = settingNumber(vm, u"regsStereoDepth", 50);
  float conv = settingNumber(vm, u"regsStereoConvergence", 100);
  bool swap = settingNumber(vm, u"regsStereoSwap", 0) > 0;
  // REGS_STEREO=<mode>, REGS_STEREO_DEPTH=<0..100>, REGS_STEREO_CONVERGENCE=<percent>, REGS_STEREO_SWAP=0|1 (tests)
  if (const char* e = regs::envVar("REGS_STEREO")) mode = std::atoi(e);
  if (const char* e = regs::envVar("REGS_STEREO_DEPTH")) depth = float(std::atof(e));
  if (const char* e = regs::envVar("REGS_STEREO_CONVERGENCE")) conv = float(std::atof(e));
  if (const char* e = regs::envVar("REGS_STEREO_SWAP")) swap = std::atoi(e) != 0;
  if (s.testMode >= 0) mode = s.testMode;
  if (mode < 0 || mode >= kStereoModeCount || s.failed) mode = 0;
  // depth 100 = 5 % of the image width between the eyes' pictures of the horizon (comfortable stays under ~3 %)
  s.parallax = std::clamp(depth, 0.0f, 100.0f) / 100.0f * 0.05f;
  s.convergence = std::clamp(conv, 25.0f, 400.0f) / 100.0f;
  s.swap = swap;
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
  // a far point lands c NDC units (= c image widths apart between the eyes) to the side; the point at the screen plane
  // distance D does not move: the camera goes sideways by c·D / P00 and the frustum is shifted back by c
  const float sign = float(s.eye);
  const float c = sign * s.parallax;
  const float screenD = dist * s.convergence;
  const Vec3 shift = right * (c * screenD / p00);
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

bool stereoCompose(Display& d, script::VM& vm, int mode, int w, int h) {
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
  const float uv[4][4] = {{0, 1, 0, 0}, {1, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}};  // TL, TR, BR, BL
  g.bindFramebuffer(0);
  g.scissorTest(false);
  g.viewport(0, 0, w, h);
  d.r3d().drawQuad(s.program, tex, 2, c0, uv);
  return true;
}

void stereoReset(Display& d) {
  StereoState& s = state();
  d.r3d().destroyTarget(s.eyes[0]);
  d.r3d().destroyTarget(s.eyes[1]);
}

}  // namespace regs::engine
