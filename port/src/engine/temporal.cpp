// TAA and render scale (engine/temporal.h, docs/renderer/vulkan.md §9.1-9.2).
#include "engine/temporal.h"
#include "core/env.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/log.h"
#include "engine/display.h"
#include "engine/hdr.h"
#include "engine/settings.h"
#include "render/gpu.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::render;

namespace {

struct TemporalState {
  bool failed = false, aoFailed = false, fsrFailed = false;
  uint32_t taa = 0, upscale = 0, copy = 0, ao = 0, aoBlur = 0, motion = 0;
  RenderTarget input, history[2], aoRaw, aoSmooth, motionVectors, fsrOutput;
  bool lastFsr = false;
  bool frameGen = false;   // this frame: frame generation on (FSR upscale in the game view)
  bool hudReady = false;   // the HUD-less view was kept this frame
  RenderTarget hudless;
  bool hudFloat = false;
  float jitterPx[2] = {0, 0};  // FSR's: render pixels
  float cameraNear = 0.1f, cameraFar = 1000, fovY = 1;
  std::chrono::steady_clock::time_point lastFinish{};
  Mat4 drawnViewProj;   // the projection the scene was drawn with (jittered), for the occlusion's positions
  int current = 0;      // history[current] holds last frame's result
  bool valid = false;   // the history matches the view (size, a previous frame)
  int frame = 0;
  int outW = 0, outH = 0, inSamples = -1;
  Mat4 viewProj;        // this frame's, without jitter
  Mat4 prevViewProj;
  float jitterUv[2] = {0, 0};
};

TemporalState& state() {
  static TemporalState s;
  return s;
}

float halton(int index, int base) {
  float f = 1, r = 0;
  for (int i = index; i > 0; i /= base) {
    f /= float(base);
    r += f * float(i % base);
  }
  return r;
}

// rays: with the ray-query additions (regs_Scene; Vulkan only)
uint32_t postProgram(Display& d, script::VM& vm, const char* fragment, bool rays = false) {
  std::vector<uint8_t> vs, fs;
  if (!vm.vfs().read("regs/shaders/post.vertex", vs) || !vm.vfs().read(std::string("regs/shaders/") + fragment, fs)) return 0;
  std::string err;
  uint32_t p = d.r3d().buildProgram(std::string(vs.begin(), vs.end()), std::string(fs.begin(), fs.end()), &err, false, rays);
  if (!p) LOG_ERROR("regs/shaders/%s: %s", fragment, err.c_str());
  return p;
}

bool ensure(Display& d, RenderTarget& t, int w, int h, int samples, bool depth, TargetFormat format) {
  if (t.fbo && t.width == w && t.height == h && t.samples == samples) return true;
  d.r3d().destroyTarget(t);
  bool ok = depth ? d.r3d().makeSceneTarget(t, w, h, samples, true, format) : d.r3d().makeTarget(t, w, h, format, false, true);
  if (!ok) d.r3d().destroyTarget(t);
  return ok;
}

}  // namespace

TemporalPass temporalBegin(Display& d, script::VM& vm, int w, int h, SceneView& v) {
  TemporalState& s = state();
  TemporalPass t;
  if (s.failed) return t;
  // REGS_TAA=0|1 and REGS_RENDER_SCALE=<percent> override the settings (test runs)
  bool taa = settingNumber(vm, u"regsTaa", 0) > 0;
  float percent = settingNumber(vm, u"regsRenderScale", 100);
  if (const char* e = regs::envVar("REGS_TAA")) taa = std::atoi(e) != 0;
  if (const char* e = regs::envVar("REGS_RENDER_SCALE")) percent = float(std::atof(e));
  float scale = std::clamp(percent, 50.0f, 100.0f) / 100.0f;
  // raytraced ambient occlusion (regsRtAo, REGS_RT_AO=0|1): needs ray queries; runs on the temporal input
  bool ao = false;
  if (gpu::dev().rayTracing() && !s.aoFailed) {
    ao = settingNumber(vm, u"regsRtAo", 0) > 0;
    if (const char* e = regs::envVar("REGS_RT_AO")) ao = std::atoi(e) != 0;
  }
  if (ao && !s.ao) {
    s.ao = postProgram(d, vm, "ao.fragment", true);
    s.aoBlur = postProgram(d, vm, "aoblur.fragment");
    if (!s.ao || !s.aoBlur) {
      LOG_ERROR("temporal: the ambient occlusion programs do not build, it stays off");
      s.aoFailed = true;
      ao = false;
    }
  }
  // the upscaler (regsUpscaler, REGS_UPSCALER): 0 = the port's TAA / spatial upscaler, 1 = AMD FSR 3.1 (Vulkan built
  // with the FidelityFX SDK). FSR antialiases too, so it runs at 100 % as well and takes the place of TAA.
  int upscaler = int(settingNumber(vm, u"regsUpscaler", 0));
  if (const char* e = regs::envVar("REGS_UPSCALER")) upscaler = std::atoi(e);
  bool fsr = upscaler == 1 && gpu::dev().fsrAvailable() && !s.fsrFailed;
  if (fsr && !s.motion) {
    s.motion = postProgram(d, vm, "motion.fragment");
    if (!s.motion) {
      LOG_ERROR("temporal: the motion-vector program does not build, FSR stays off");
      s.fsrFailed = true;
      fsr = false;
    }
  }
  if (fsr) taa = false;
  // FSR 3 frame generation (regsFrameGen, REGS_FRAMEGEN=0|1): with the FSR upscaler only (it reads its dilated depth
  // and motion)
  bool fg = settingNumber(vm, u"regsFrameGen", 0) > 0;
  if (const char* e = regs::envVar("REGS_FRAMEGEN")) fg = std::atoi(e) != 0;
  fg = fg && fsr && gpu::dev().frameGenAvailable();
  gpu::dev().setFrameGen(fg);
  s.frameGen = fg;
  if (fsr != s.lastFsr) s.valid = false;
  s.lastFsr = fsr;
  if (!taa && scale > 0.999f && !ao && !fsr) {
    s.valid = false;
    return t;
  }
  if (!s.taa) {
    s.taa = postProgram(d, vm, "taa.fragment");
    s.upscale = postProgram(d, vm, "upscale.fragment");
    s.copy = postProgram(d, vm, "copy.fragment");
    if (!s.taa || !s.upscale || !s.copy) {
      LOG_ERROR("temporal: the TAA / upscaling programs do not build, both stay off");
      s.failed = true;
      return t;
    }
  }
  t.rw = std::max(1, int(std::lround(float(w) * scale)));
  t.rh = std::max(1, int(std::lround(float(h) * scale)));
  // the input keeps the antialiasing setting's MSAA (TAA on top of it is allowed; it is the player's choice)
  int samples = std::max(0, d.antialiasing);
  if (!ensure(d, s.input, t.rw, t.rh, samples, true, TargetFormat::Rgba16f) &&
      !(samples > 0 && ensure(d, s.input, t.rw, t.rh, samples = 0, true, TargetFormat::Rgba16f))) {
    s.failed = true;
    return t;
  }
  if ((taa || fsr) && (s.outW != w || s.outH != h)) s.valid = false;
  s.outW = w;
  s.outH = h;
  t.input = &s.input;
  t.taa = taa;
  t.ao = ao;
  t.fsr = fsr;
  t.active = true;
  s.viewProj = v.proj * v.view;
  s.jitterUv[0] = s.jitterUv[1] = 0;
  if (fsr) {
    // the camera for FSR's depth reconstruction, from the perspective matrix (GL form: [1][1] = cot(fov/2),
    // [2][2] = -(f+n)/(f-n), [2][3] = -2fn/(f-n))
    float a = v.proj.at(2, 2), b = v.proj.at(2, 3), c = v.proj.at(1, 1);
    if (std::fabs(a - 1) > 1e-6f && std::fabs(a + 1) > 1e-6f && c > 0) {
      s.cameraNear = b / (a - 1);
      s.cameraFar = b / (a + 1);
      s.fovY = 2 * std::atan(1 / c);
    }
    // FSR's own jitter sequence (its phase count follows the scale)
    gpu::dev().fsrJitter(uint32_t(s.frame++), t.rw, w, s.jitterPx[0], s.jitterPx[1]);
    float ox = 2.0f * s.jitterPx[0] / float(t.rw), oy = 2.0f * s.jitterPx[1] / float(t.rh);
    v.proj.at(0, 2) -= ox;
    v.proj.at(1, 2) -= oy;
    s.jitterUv[0] = 0.5f * ox;
    s.jitterUv[1] = 0.5f * oy;
  } else if (taa) {
    // Halton (2, 3) over 8 frames, in render pixels; NDC shift +o needs −o in the projection's third column
    int i = s.frame++ % 8 + 1;
    float ox = 2.0f * (halton(i, 2) - 0.5f) / float(t.rw), oy = 2.0f * (halton(i, 3) - 0.5f) / float(t.rh);
    v.proj.at(0, 2) -= ox;
    v.proj.at(1, 2) -= oy;
    s.jitterUv[0] = 0.5f * ox;
    s.jitterUv[1] = 0.5f * oy;
  } else {
    s.valid = false;
    ++s.frame;  // the occlusion's noise still moves from frame to frame
  }
  s.drawnViewProj = v.proj * v.view;
  return t;
}

void temporalFinish(Display& d, script::VM& vm, const TemporalPass& t, int x, int y, int w, int h, int backW, int backH,
                    const RenderTarget* out) {
  (void)vm;
  if (!t.active) return;
  TemporalState& s = state();
  Renderer3D& r = d.r3d();
  gpu::Device& g = gpu::dev();
  r.resolve(s.input);
  const float uv[4][4] = {{0, 1, 0, 0}, {1, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}};  // TL, TR, BR, BL
  // raytraced ambient occlusion on the drawn frame: rays, a depth-aware blur, multiplied into the colour
  if (t.ao && ensure(d, s.aoRaw, t.rw, t.rh, 0, false, TargetFormat::Rgba8) &&
      ensure(d, s.aoSmooth, t.rw, t.rh, 0, false, TargetFormat::Rgba8)) {
    Mat4 inv = inverse(s.drawnViewProj);
    const float c0[4] = {1.0f / float(t.rw), 1.0f / float(t.rh), float(s.frame % 64), 2.5f};
    const float c1[4] = {0.85f, 0, 0, 0};
    g.scissorTest(false);
    g.bindFramebuffer(s.aoRaw.fbo);
    g.viewport(0, 0, t.rw, t.rh);
    r.drawQuad(s.ao, &s.input.depthTex, 1, c0, uv, nullptr, c1, inv.m);
    const uint32_t blurIn[2] = {s.aoRaw.color, s.input.depthTex};
    g.bindFramebuffer(s.aoSmooth.fbo);
    r.drawQuad(s.aoBlur, blurIn, 2, c0, uv);
    g.bindFramebuffer(s.input.fbo);
    // REGS_RT_AO=2 (a check): the occlusion alone instead of multiplied into the colour
    static const bool aoOnly = regs::envVar("REGS_RT_AO") && std::atoi(regs::envVar("REGS_RT_AO")) == 2;
    r.drawQuad(s.copy, &s.aoSmooth.color, 1, nullptr, uv, nullptr, nullptr, nullptr, !aoOnly);
  }
  auto into = [&](const RenderTarget* target) {  // a target's bottom-left w×h, or the back-buffer rect
    g.scissorTest(false);
    g.bindFramebuffer(target ? target->fbo : 0);
    if (target) {
      g.viewport(0, 0, w, h);
    } else {
      g.viewport(x, backH - y - h, w, h);
      g.scissorTest(true);
      g.scissor(x, backH - y - h, w, h);
    }
  };
  auto now = std::chrono::steady_clock::now();
  float frameMs = s.lastFinish.time_since_epoch().count() ? std::chrono::duration<float, std::milli>(now - s.lastFinish).count() : 16.7f;
  s.lastFinish = now;
  bool fsrDone = false;
  if (t.fsr && ensure(d, s.motionVectors, t.rw, t.rh, 0, false, TargetFormat::Rgba16f) &&
      ensure(d, s.fsrOutput, w, h, 0, false, TargetFormat::Rgba16f)) {
    // camera motion vectors at the render size, then FSR into its own output, copied to the destination
    Mat4 reproject = s.prevViewProj * inverse(s.viewProj);
    const float c0[4] = {1.0f / float(t.rw), 1.0f / float(t.rh), s.jitterUv[0], s.jitterUv[1]};
    const float c1[4] = {s.valid ? 1.0f : 0.0f, 0, 0, 0};
    g.scissorTest(false);
    g.bindFramebuffer(s.motionVectors.fbo);
    g.viewport(0, 0, t.rw, t.rh);
    r.drawQuad(s.motion, &s.input.depthTex, 1, c0, uv, nullptr, c1, reproject.m);
    gpu::Device::FsrParams p;
    p.color = s.input.color;
    p.depth = s.input.depthTex;
    p.motion = s.motionVectors.color;
    p.output = s.fsrOutput.color;
    p.renderWidth = t.rw, p.renderHeight = t.rh, p.outputWidth = w, p.outputHeight = h;
    p.jitterX = s.jitterPx[0], p.jitterY = s.jitterPx[1];
    p.cameraNear = s.cameraNear, p.cameraFar = s.cameraFar, p.fovY = s.fovY;
    p.frameTimeMs = std::clamp(frameMs, 1.0f, 100.0f);
    p.sharpness = t.rw < w ? 0.4f : 0.2f;
    p.reset = !s.valid;
    if (g.fsrUpscale(p)) {
      s.prevViewProj = s.viewProj;
      s.valid = true;
      into(out);
      r.drawQuad(s.copy, &s.fsrOutput.color, 1, nullptr, uv);
      fsrDone = true;
    } else {
      LOG_ERROR("temporal: FSR failed, back to the port's upscaler");
      s.fsrFailed = true;
      s.valid = false;
    }
  }
  if (fsrDone) {
  } else if (t.taa) {
    RenderTarget& prev = s.history[s.current];
    RenderTarget& next = s.history[s.current ^ 1];
    if (!ensure(d, prev, w, h, 0, false, TargetFormat::Rgba16f) || !ensure(d, next, w, h, 0, false, TargetFormat::Rgba16f)) {
      s.failed = true;
      return;
    }
    Mat4 reproject = s.prevViewProj * inverse(s.viewProj);  // this frame's NDC → last frame's clip space
    const float c0[4] = {1.0f / float(t.rw), 1.0f / float(t.rh), s.jitterUv[0], s.jitterUv[1]};
    const float c1[4] = {1.0f / float(w), 1.0f / float(h), 0.1f, s.valid ? 1.0f : 0.0f};
    const uint32_t tex[3] = {s.input.color, s.input.depthTex, prev.color};
    into(&next);
    r.drawQuad(s.taa, tex, 3, c0, uv, nullptr, c1, reproject.m);
    s.current ^= 1;
    s.prevViewProj = s.viewProj;
    s.valid = true;
    // then contrast-adaptive sharpening at the output size (TAA softens; more when it also upscales)
    const float sc0[4] = {1.0f / float(w), 1.0f / float(h), t.rw < w ? 1.0f : 0.5f, 0};
    const float sc1[4] = {1.0f / float(w), 1.0f / float(h), 0, 0};
    into(out);
    r.drawQuad(s.upscale, &next.color, 1, sc0, uv, nullptr, sc1);
  } else if (t.rw == w && t.rh == h) {  // only the occlusion ran: a plain copy
    into(out);
    r.drawQuad(s.copy, &s.input.color, 1, nullptr, uv);
  } else {  // spatial upscaling straight into its destination
    const float c0[4] = {1.0f / float(t.rw), 1.0f / float(t.rh), 0.5f, 0};
    const float c1[4] = {1.0f / float(w), 1.0f / float(h), 0, 0};
    into(out);
    r.drawQuad(s.upscale, &s.input.color, 1, c0, uv, nullptr, c1);
  }
  if (out) r.blitDepthScaled(s.input, *out, t.rw, t.rh, w, h);  // the lens blur reads the depth
  g.bindFramebuffer(0);
  g.viewport(0, 0, backW, backH);
}

void temporalHudless(Display& d, script::VM& vm, int x, int y, int w, int h, int backW, int backH) {
  (void)vm;
  TemporalState& s = state();
  if (!s.frameGen || x != 0 || y != 0 || w != backW || h != backH) return;
  TargetFormat f = gpu::dev().hdrActive() ? TargetFormat::Rgba16f : TargetFormat::Rgba8;
  bool want16 = f == TargetFormat::Rgba16f;
  if (s.hudless.fbo && (s.hudless.width != w || s.hudless.height != h || s.hudFloat != want16)) d.r3d().destroyTarget(s.hudless);
  if (!s.hudless.fbo && !d.r3d().makeTarget(s.hudless, w, h, f, false, true)) {
    d.r3d().destroyTarget(s.hudless);
    return;
  }
  s.hudFloat = want16;
  gpu::dev().blit(0, s.hudless.fbo, 0, 0, w, h, 0, 0, w, h, true, false);
  s.hudReady = true;
}

void temporalFrameGenSubmit(Display& d, script::VM& vm, int backW, int backH) {
  TemporalState& s = state();
  if (s.hudReady) {
    uint32_t tex = s.hudless.color;
    if (gpu::dev().hdrActive()) tex = hdrEncodeTexture(d, vm, tex, backW, backH);  // in the back buffer's encoding
    if (tex) gpu::dev().frameGenHudless(tex);
  }
  s.hudReady = false;
  s.frameGen = false;
}

void temporalReset(Display& d) {
  TemporalState& s = state();
  d.r3d().destroyTarget(s.hudless);
  d.r3d().destroyTarget(s.input);
  d.r3d().destroyTarget(s.history[0]);
  d.r3d().destroyTarget(s.history[1]);
  d.r3d().destroyTarget(s.aoRaw);
  d.r3d().destroyTarget(s.aoSmooth);
  d.r3d().destroyTarget(s.motionVectors);
  d.r3d().destroyTarget(s.fsrOutput);
  s.valid = false;
}

}  // namespace regs::engine
