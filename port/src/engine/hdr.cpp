// HDR output passes (engine/hdr.h, docs/renderer/vulkan.md §9.6).
#include "engine/hdr.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/log.h"
#include "engine/display.h"
#include "engine/settings.h"
#include "render/gpu.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::render;

namespace {

struct HdrState {
  bool failed = false;
  uint32_t expand = 0, encode = 0;
  RenderTarget copy;  // the back buffer (or a view's rect of it) as the passes' input
  RenderTarget encoded;  // hdrEncodeTexture's output (frame generation's HUD-less frame)
  float paper = 200, peak = 1000;
};

HdrState& state() {
  static HdrState s;
  return s;
}

uint32_t postProgram(Display& d, script::VM& vm, const char* fragment) {
  std::vector<uint8_t> vs, fs;
  if (!vm.vfs().read("regs/shaders/post.vertex", vs) || !vm.vfs().read(std::string("regs/shaders/") + fragment, fs)) return 0;
  std::string err;
  uint32_t p = d.r3d().buildProgram(std::string(vs.begin(), vs.end()), std::string(fs.begin(), fs.end()), &err);
  if (!p) LOG_ERROR("regs/shaders/%s: %s", fragment, err.c_str());
  return p;
}

bool ready(Display& d, script::VM& vm) {
  HdrState& s = state();
  if (s.failed || !gpu::dev().hdrActive()) return false;
  if (!s.expand) {
    s.expand = postProgram(d, vm, "hdrexpand.fragment");
    s.encode = postProgram(d, vm, "hdrencode.fragment");
    if (!s.expand || !s.encode) {
      LOG_ERROR("hdr: the HDR programs do not build, the output stays SDR");
      s.failed = true;
      gpu::dev().setHdr(false, s.paper, s.peak);
      return false;
    }
  }
  return true;
}

// the back buffer's rect (GL window coordinates) into the copy target's bottom-left
bool grab(Display& d, int gx, int gy, int w, int h) {
  HdrState& s = state();
  if (!s.copy.fbo || s.copy.width < w || s.copy.height < h) {
    d.r3d().destroyTarget(s.copy);
    if (!d.r3d().makeTarget(s.copy, std::max(w, s.copy.width), std::max(h, s.copy.height), TargetFormat::Rgba16f, false, true)) {
      d.r3d().destroyTarget(s.copy);
      return false;
    }
  }
  gpu::dev().blit(0, s.copy.fbo, gx, gy, gx + w, gy + h, 0, 0, w, h, true, false);
  return true;
}

}  // namespace

void hdrSync(Display& d, script::VM& vm) {
  (void)d;
  HdrState& s = state();
  // REGS_HDR=0|1, REGS_HDR_PAPER=<nits>, REGS_HDR_PEAK=<nits> override the settings (test runs)
  bool on = settingNumber(vm, u"regsHdr", 0) > 0;
  float paper = settingNumber(vm, u"regsHdrPaperWhite", 200), peak = settingNumber(vm, u"regsHdrPeak", 1000);
  if (const char* e = std::getenv("REGS_HDR")) on = std::atoi(e) != 0;
  if (const char* e = std::getenv("REGS_HDR_PAPER")) paper = float(std::atof(e));
  if (const char* e = std::getenv("REGS_HDR_PEAK")) peak = float(std::atof(e));
  s.paper = std::clamp(paper, 80.0f, 500.0f);
  s.peak = std::clamp(peak, std::max(400.0f, s.paper), 10000.0f);
  gpu::dev().setHdr(on && !s.failed, s.paper, s.peak);
}

void hdrExpandView(Display& d, script::VM& vm, int x, int y, int w, int h, int backW, int backH) {
  if (w <= 0 || h <= 0 || !ready(d, vm)) return;
  HdrState& s = state();
  gpu::Device& g = gpu::dev();
  int gy = backH - y - h;
  if (!grab(d, x, gy, w, h)) return;
  // SDR white ends at a third of the way from paper white to the peak; the curve starts at 60 % (linear)
  const float c0[4] = {1.0f + (s.peak / s.paper - 1.0f) / 3.0f, 0.6f, 0, 0};
  const float sx = float(w) / float(s.copy.width), sy = float(h) / float(s.copy.height);
  const float uv[4][4] = {{0, sy, 0, 0}, {sx, sy, 0, 0}, {sx, 0, 0, 0}, {0, 0, 0, 0}};  // TL, TR, BR, BL
  g.bindFramebuffer(0);
  g.viewport(x, gy, w, h);
  g.scissorTest(true);
  g.scissor(x, gy, w, h);
  d.r3d().drawQuad(s.expand, &s.copy.color, 1, c0, uv);
  g.scissorTest(false);
  g.viewport(0, 0, backW, backH);
}

void hdrEncode(Display& d, script::VM& vm, int backW, int backH) {
  if (backW <= 0 || backH <= 0 || !ready(d, vm)) return;
  HdrState& s = state();
  gpu::Device& g = gpu::dev();
  if (!grab(d, 0, 0, backW, backH)) return;
  const float c0[4] = {s.paper, s.peak, 0, 0};
  const float sx = float(backW) / float(s.copy.width), sy = float(backH) / float(s.copy.height);
  const float uv[4][4] = {{0, sy, 0, 0}, {sx, sy, 0, 0}, {sx, 0, 0, 0}, {0, 0, 0, 0}};
  g.bindFramebuffer(0);
  g.scissorTest(false);
  g.viewport(0, 0, backW, backH);
  d.r3d().drawQuad(s.encode, &s.copy.color, 1, c0, uv);
}

uint32_t hdrEncodeTexture(Display& d, script::VM& vm, uint32_t texture, int w, int h) {
  if (!texture || w <= 0 || h <= 0 || !ready(d, vm)) return 0;
  HdrState& s = state();
  if (!s.encoded.fbo || s.encoded.width != w || s.encoded.height != h) {
    d.r3d().destroyTarget(s.encoded);
    if (!d.r3d().makeTarget(s.encoded, w, h, TargetFormat::Rgba16f, false, true)) {
      d.r3d().destroyTarget(s.encoded);
      return 0;
    }
  }
  gpu::Device& g = gpu::dev();
  const float c0[4] = {s.paper, s.peak, 0, 0};
  const float uv[4][4] = {{0, 1, 0, 0}, {1, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}};
  g.bindFramebuffer(s.encoded.fbo);
  g.scissorTest(false);
  g.viewport(0, 0, w, h);
  d.r3d().drawQuad(s.encode, &texture, 1, c0, uv);
  g.bindFramebuffer(0);
  g.viewport(0, 0, w, h);
  return s.encoded.color;
}

void hdrReset(Display& d) {
  d.r3d().destroyTarget(state().copy);
  d.r3d().destroyTarget(state().encoded);
}

}  // namespace regs::engine
