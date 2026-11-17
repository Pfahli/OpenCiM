// AMD FSR 3.1 upscaling on the Vulkan device (render/fsr_vk.h). Compiled into regs_ffx with the FidelityFX SDK's
// Vulkan backend, which calls a few instance-level Vulkan functions directly; they are defined at the bottom of this
// file and forward to the functions the port loaded from vulkan-1.dll at run time (nothing is linked).
#include "render/fsr_vk.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include <cstdlib>
#include <cwchar>
#include <vector>

#include "core/log.h"

namespace regs::render::fsr {

namespace {

struct State {
  VulkanContext vk;
  bool ready = false;
  bool created = false;
  FfxFsr3UpscalerContext context{};
  VkDeviceContext deviceContext{};  // the backend keeps a pointer to it
  FfxInterface backend{};
  // FSR 3.1 writes three resources that it shares with frame generation; the app owns them (a second backend
  // context, as the SDK's FSR 3 component does)
  bool sharedCreated = false;
  FfxUInt32 sharedId = 0;
  FfxResourceInternal shared[3]{};  // dilated depth, dilated motion vectors, reconstructed previous nearest depth
  PFN_vkDeviceWaitIdle waitIdle = nullptr;
  // frame generation (made on first use, with the upscaler's sizes): optical flow, frame interpolation and the optical
  // flow's two shared outputs
  bool fgCreated = false, fgFailed = false;
  bool fgPq = false;
  FfxOpticalflowContext of{};
  FfxFrameInterpolationContext fi{};
  FfxResourceInternal ofShared[2]{};  // optical flow vectors, scene change detection
  VkFormat fgFormat = VK_FORMAT_UNDEFINED;
  // the last upscale's camera and sizes (the interpolation reads the same)
  UpscaleDesc last;
  std::vector<uint8_t> scratch;
  uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
  // the instance-level functions the SDK's backend calls directly
  PFN_vkGetPhysicalDeviceProperties getProperties = nullptr;
  PFN_vkGetPhysicalDeviceProperties2 getProperties2 = nullptr;
  PFN_vkGetPhysicalDeviceFeatures getFeatures = nullptr;
  PFN_vkGetPhysicalDeviceFeatures2 getFeatures2 = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties = nullptr;
  PFN_vkEnumerateDeviceExtensionProperties enumerateExtensions = nullptr;
  PFN_vkCreateBuffer createBuffer = nullptr;
};

// never destroyed: the device can be shut down during static destruction, after a plain static would be gone
State& state() {
  static State* s = new State;
  return *s;
}

// The device's functions for the backend. It asks for some by their extension names (vkGetBufferMemoryRequirements2KHR),
// which a Vulkan 1.3 device that never enabled those extensions does not hand out: then the core name.
PFN_vkVoidFunction VKAPI_CALL deviceProcAddr(VkDevice device, const char* name) {
  PFN_vkVoidFunction f = state().vk.getDeviceProcAddr(device, name);
  if (!f) {
    std::string core = name;
    if (core.size() > 3 && core.compare(core.size() - 3, 3, "KHR") == 0) {
      core.resize(core.size() - 3);
      f = state().vk.getDeviceProcAddr(device, core.c_str());
    }
  }
  return f;
}

void message(FfxMsgType type, const wchar_t* text) {
  char buf[512];
  std::wcstombs(buf, text, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  if (type == FFX_MESSAGE_TYPE_ERROR) LOG_ERROR("fsr: %s", buf);
  else LOG_WARN("fsr: %s", buf);
}

FfxResource resource(const Image& im, const wchar_t* name) {
  if (!im.image) return FfxResource{};
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = im.format;
  ci.extent = {im.width, im.height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.usage = im.usage;
  FfxResourceDescription desc = ffxGetImageResourceDescriptionVK(im.image, ci, FFX_RESOURCE_USAGE_READ_ONLY);
  // every image of the port lives in GENERAL (UNORDERED_ACCESS): the backend moves it for its passes and back
  return ffxGetResourceVK(reinterpret_cast<void*>(im.image), desc, name, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}

// the upscaler, the shared resources, optical flow, frame interpolation
constexpr uint32_t kContexts = FFX_FSR3UPSCALER_CONTEXT_COUNT + 1 + FFX_OPTICALFLOW_CONTEXT_COUNT + FFX_FRAMEINTERPOLATION_CONTEXT_COUNT;

void destroyFrameGen(State& s) {
  if (!s.fgCreated) return;
  for (FfxResourceInternal& r : s.ofShared) s.backend.fpDestroyResource(&s.backend, r, s.sharedId);
  ffxFrameInterpolationContextDestroy(&s.fi);
  ffxOpticalflowContextDestroy(&s.of);
  s.fgCreated = false;
}

void destroyContext(State& s) {
  if (s.created && s.waitIdle) s.waitIdle(s.vk.device);  // frames in flight still use the resources
  destroyFrameGen(s);
  if (s.sharedCreated) {
    for (FfxResourceInternal& r : s.shared) s.backend.fpDestroyResource(&s.backend, r, s.sharedId);
    s.backend.fpDestroyBackendContext(&s.backend, s.sharedId);
    s.sharedCreated = false;
  }
  if (s.created) ffxFsr3UpscalerContextDestroy(&s.context);
  s.created = false;
}

bool createContext(State& s, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, std::string* error) {
  destroyContext(s);
  s.scratch.assign(ffxGetScratchMemorySizeVK(s.vk.physicalDevice, kContexts), 0);
  s.deviceContext = {s.vk.device, s.vk.physicalDevice, &deviceProcAddr};
  FfxFsr3UpscalerContextDescription cd{};
  if (ffxGetInterfaceVK(&cd.backendInterface, ffxGetDeviceVK(&s.deviceContext), s.scratch.data(), s.scratch.size(),
                        kContexts) != FFX_OK) {
    if (error) *error = "ffxGetInterfaceVK failed";
    return false;
  }
  // low-dynamic-range colour, motion vectors at the render size without the jitter, depth 0 near .. 1 far: the
  // permutations ffx-shadergen builds (CMakeLists.txt) are exactly these
  cd.flags = 0;
  cd.maxRenderSize = {rw, rh};
  cd.maxUpscaleSize = {ow, oh};
  cd.fpMessage = &message;
  if (FfxErrorCode e = ffxFsr3UpscalerContextCreate(&s.context, &cd); e != FFX_OK) {
    if (error) *error = "ffxFsr3UpscalerContextCreate failed (" + std::to_string(int(e)) + ")";
    return false;
  }
  s.created = true;
  s.backend = cd.backendInterface;
  FfxFsr3UpscalerSharedResourceDescriptions sd{};
  if (ffxFsr3UpscalerGetSharedResourceDescriptions(&s.context, &sd) != FFX_OK ||
      s.backend.fpCreateBackendContext(&s.backend, FFX_EFFECT_SHAREDRESOURCES, nullptr, &s.sharedId) != FFX_OK) {
    if (error) *error = "FSR's shared resources: no backend context";
    destroyContext(s);
    return false;
  }
  s.sharedCreated = true;
  const FfxCreateResourceDescription* descs[3] = {&sd.dilatedDepth, &sd.dilatedMotionVectors, &sd.reconstructedPrevNearestDepth};
  for (int i = 0; i < 3; ++i)
    if (s.backend.fpCreateResource(&s.backend, descs[i], s.sharedId, &s.shared[i]) != FFX_OK) {
      if (error) *error = "FSR's shared resources could not be created";
      destroyContext(s);
      return false;
    }
  s.renderW = rw, s.renderH = rh, s.outW = ow, s.outH = oh;
  LOG_INFO("fsr: FSR 3.1 upscaler %ux%u -> %ux%u", rw, rh, ow, oh);
  return true;
}

}  // namespace

bool init(const VulkanContext& vk) {
  State& s = state();
  s.vk = vk;
  auto get = [&](const char* n) { return vk.getInstanceProcAddr(vk.instance, n); };
  s.getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get("vkGetPhysicalDeviceProperties"));
  s.getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get("vkGetPhysicalDeviceProperties2"));
  s.getFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(get("vkGetPhysicalDeviceFeatures"));
  s.getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get("vkGetPhysicalDeviceFeatures2"));
  s.getMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get("vkGetPhysicalDeviceMemoryProperties"));
  s.enumerateExtensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get("vkEnumerateDeviceExtensionProperties"));
  s.createBuffer = reinterpret_cast<PFN_vkCreateBuffer>(vk.getDeviceProcAddr(vk.device, "vkCreateBuffer"));
  s.waitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(vk.getDeviceProcAddr(vk.device, "vkDeviceWaitIdle"));
  s.ready = s.getProperties && s.getProperties2 && s.getFeatures && s.getFeatures2 && s.getMemoryProperties &&
            s.enumerateExtensions && s.createBuffer;
  return s.ready;
}

bool upscale(VkCommandBuffer cmd, const UpscaleDesc& d, std::string* error) {
  State& s = state();
  if (!s.ready) {
    if (error) *error = "FSR not initialised";
    return false;
  }
  bool fresh = false;
  if (!s.created || s.renderW != d.renderWidth || s.renderH != d.renderHeight || s.outW != d.outputWidth ||
      s.outH != d.outputHeight) {
    if (!createContext(s, d.renderWidth, d.renderHeight, d.outputWidth, d.outputHeight, error)) return false;
    fresh = true;
  }
  FfxFsr3UpscalerDispatchDescription dd{};
  dd.commandList = ffxGetCommandListVK(cmd);
  dd.color = resource(d.color, L"FSR3_Input_Color");
  dd.depth = resource(d.depth, L"FSR3_Input_Depth");
  dd.motionVectors = resource(d.motion, L"FSR3_Input_Motion");
  dd.output = resource(d.output, L"FSR3_Output");
  dd.dilatedDepth = s.backend.fpGetResource(&s.backend, s.shared[0]);
  dd.dilatedMotionVectors = s.backend.fpGetResource(&s.backend, s.shared[1]);
  dd.reconstructedPrevNearestDepth = s.backend.fpGetResource(&s.backend, s.shared[2]);
  s.last = d;
  dd.jitterOffset = {d.jitterX, d.jitterY};
  dd.motionVectorScale = {float(d.renderWidth), float(d.renderHeight)};  // the vectors are in UV units
  dd.renderSize = {d.renderWidth, d.renderHeight};
  dd.upscaleSize = {d.outputWidth, d.outputHeight};
  dd.enableSharpening = d.sharpness > 0;
  dd.sharpness = d.sharpness;
  dd.frameTimeDelta = d.frameTimeMs;
  dd.preExposure = 1.0f;
  dd.reset = d.reset || fresh;
  dd.cameraNear = d.cameraNear;
  dd.cameraFar = d.cameraFar;
  dd.cameraFovAngleVertical = d.fovY;
  dd.viewSpaceToMetersFactor = 1.0f;  // the game's units are metres
  if (FfxErrorCode e = ffxFsr3UpscalerContextDispatch(&s.context, &dd); e != FFX_OK) {
    if (error) *error = "ffxFsr3UpscalerContextDispatch failed (" + std::to_string(int(e)) + ")";
    return false;
  }
  return true;
}

namespace {

bool createFrameGen(State& s, VkFormat format, bool pq, std::string* error) {
  destroyFrameGen(s);
  FfxOpticalflowContextDescription od{};
  od.backendInterface = s.backend;
  od.resolution = {s.outW, s.outH};
  if (FfxErrorCode e = ffxOpticalflowContextCreate(&s.of, &od); e != FFX_OK) {
    if (error) *error = "ffxOpticalflowContextCreate failed (" + std::to_string(int(e)) + ")";
    return false;
  }
  FfxFrameInterpolationContextDescription fd{};
  fd.backendInterface = s.backend;
  fd.flags = pq ? FFX_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT : 0;  // vectors at the render size (low resolution)
  fd.maxRenderSize = {s.renderW, s.renderH};
  fd.displaySize = {s.outW, s.outH};
  fd.backBufferFormat = ffxGetSurfaceFormatVK(format);
  fd.previousInterpolationSourceFormat = fd.backBufferFormat;
  if (FfxErrorCode e = ffxFrameInterpolationContextCreate(&s.fi, &fd); e != FFX_OK) {
    ffxOpticalflowContextDestroy(&s.of);
    if (error) *error = "ffxFrameInterpolationContextCreate failed (" + std::to_string(int(e)) + ")";
    return false;
  }
  FfxOpticalflowSharedResourceDescriptions sd{};
  ffxOpticalflowGetSharedResourceDescriptions(&s.of, &sd);
  if (s.backend.fpCreateResource(&s.backend, &sd.opticalFlowVector, s.sharedId, &s.ofShared[0]) != FFX_OK ||
      s.backend.fpCreateResource(&s.backend, &sd.opticalFlowSCD, s.sharedId, &s.ofShared[1]) != FFX_OK) {
    ffxFrameInterpolationContextDestroy(&s.fi);
    ffxOpticalflowContextDestroy(&s.of);
    if (error) *error = "the optical flow's resources could not be created";
    return false;
  }
  s.fgCreated = true;
  s.fgFormat = format;
  s.fgPq = pq;
  LOG_INFO("fsr: FSR 3 frame generation %ux%u (render %ux%u)%s", s.outW, s.outH, s.renderW, s.renderH, pq ? ", HDR10" : "");
  return true;
}

}  // namespace

bool frameGenerate(VkCommandBuffer cmd, const FrameGenDesc& d, std::string* error) {
  State& s = state();
  if (!s.ready || !s.created || !s.sharedCreated) {
    if (error) *error = "no FSR upscale this frame";
    return false;
  }
  if (d.displayWidth != s.outW || d.displayHeight != s.outH) {
    if (error) *error = "the display size differs from the upscaler's output";
    return false;
  }
  bool fresh = false;
  if (!s.fgCreated || s.fgFormat != d.backBuffer.format || s.fgPq != d.pq) {
    if (s.fgFailed) return false;
    if (s.fgCreated && s.waitIdle) s.waitIdle(s.vk.device);
    if (!createFrameGen(s, d.backBuffer.format, d.pq, error)) {
      s.fgFailed = true;
      return false;
    }
    fresh = true;
  }
  FfxResource ofVector = s.backend.fpGetResource(&s.backend, s.ofShared[0]);
  FfxResource ofScd = s.backend.fpGetResource(&s.backend, s.ofShared[1]);
  FfxResource hudless = resource(d.hudless, L"FG_HudlessColor");
  FfxOpticalflowDispatchDescription od{};
  od.commandList = ffxGetCommandListVK(cmd);
  od.color = hudless;
  od.opticalFlowVector = ofVector;
  od.opticalFlowSCD = ofScd;
  od.reset = d.reset || fresh;
  od.backbufferTransferFunction = d.pq ? FFX_BACKBUFFER_TRANSFER_FUNCTION_PQ : FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
  od.minMaxLuminance = {0.0f, d.pq ? 10000.0f : 1.0f};
  if (FfxErrorCode e = ffxOpticalflowContextDispatch(&s.of, &od); e != FFX_OK) {
    if (error) *error = "ffxOpticalflowContextDispatch failed (" + std::to_string(int(e)) + ")";
    return false;
  }
  FfxFrameInterpolationDispatchDescription fd{};
  fd.commandList = ffxGetCommandListVK(cmd);
  fd.displaySize = {d.displayWidth, d.displayHeight};
  fd.renderSize = {s.renderW, s.renderH};
  fd.currentBackBuffer = resource(d.backBuffer, L"FG_BackBuffer");
  fd.currentBackBuffer_HUDLess = hudless;
  fd.output = resource(d.output, L"FG_Output");
  fd.interpolationRect = {0, 0, int32_t(d.displayWidth), int32_t(d.displayHeight)};
  fd.opticalFlowVector = ofVector;
  fd.opticalFlowSceneChangeDetection = ofScd;
  fd.opticalFlowBufferSize = {(d.displayWidth + 7) / 8, (d.displayHeight + 7) / 8};
  fd.opticalFlowScale = {1.0f / float(d.displayWidth), 1.0f / float(d.displayHeight)};
  fd.opticalFlowBlockSize = 8;
  fd.cameraNear = s.last.cameraNear;
  fd.cameraFar = s.last.cameraFar;
  fd.cameraFovAngleVertical = s.last.fovY;
  fd.viewSpaceToMetersFactor = 1.0f;
  fd.frameTimeDelta = s.last.frameTimeMs;
  fd.reset = d.reset || fresh;
  fd.backBufferTransferFunction = d.pq ? FFX_BACKBUFFER_TRANSFER_FUNCTION_PQ : FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
  fd.minMaxLuminance[0] = 0.0f;
  fd.minMaxLuminance[1] = d.pq ? 10000.0f : 1.0f;
  fd.frameID = d.frameId;
  fd.dilatedDepth = s.backend.fpGetResource(&s.backend, s.shared[0]);
  fd.dilatedMotionVectors = s.backend.fpGetResource(&s.backend, s.shared[1]);
  fd.reconstructedPrevDepth = s.backend.fpGetResource(&s.backend, s.shared[2]);
  if (FfxErrorCode e = ffxFrameInterpolationDispatch(&s.fi, &fd); e != FFX_OK) {
    if (error) *error = "ffxFrameInterpolationDispatch failed (" + std::to_string(int(e)) + ")";
    return false;
  }
  return true;
}

void jitter(uint32_t frameIndex, uint32_t renderWidth, uint32_t outputWidth, float& x, float& y) {
  int32_t phases = ffxFsr3UpscalerGetJitterPhaseCount(int32_t(renderWidth), int32_t(outputWidth));
  ffxFsr3UpscalerGetJitterOffset(&x, &y, int32_t(frameIndex % uint32_t(phases > 0 ? phases : 1)), phases);
}

void shutdown() {
  State& s = state();
  destroyContext(s);
  s.ready = false;
}

}  // namespace regs::render::fsr

// ---- the Vulkan entry points the SDK's backend calls by name (forwarded; vulkan-1.dll is never linked) ----

using regs::render::fsr::state;

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice p, VkPhysicalDeviceProperties* o) {
  state().getProperties(p, o);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2* o) {
  state().getProperties2(p, o);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice p, VkPhysicalDeviceFeatures* o) {
  state().getFeatures(p, o);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2* o) {
  state().getFeatures2(p, o);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties* o) {
  state().getMemoryProperties(p, o);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p, const char* layer, uint32_t* n,
                                                                    VkExtensionProperties* o) {
  return state().enumerateExtensions(p, layer, n, o);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* a,
                                              VkBuffer* b) {
  return state().createBuffer(d, ci, a, b);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d, const char* name) {
  return regs::render::fsr::deviceProcAddr(d, name);
}

#if !REGS_FFX_FRAMEGEN
// the backend's frame-generation hook lives in the SDK's frame-interpolation swapchain, which this build leaves out
FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*) { return FFX_ERROR_INVALID_ARGUMENT; }
#endif
