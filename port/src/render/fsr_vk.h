#pragma once
#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

// AMD FSR 3.1 upscaling on the Vulkan device (third_party/fidelityfx-fsr, docs/renderer/vulkan.md §9.2). Built with
// the FidelityFX SDK in regs_ffx; gpu_vk.cpp calls it with its own images and command buffer. Coordinates are the
// port's (rows as GL stores them): FSR sees every input and its output the same way up, so it never notices.
namespace regs::render::fsr {

struct VulkanContext {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage = 0;
  uint32_t width = 0, height = 0;
  bool depth = false;
};

struct UpscaleDesc {
  Image color, depth, motion, output;  // render size, render size, render size (UV deltas prev − cur), output size
  uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
  float jitterX = 0, jitterY = 0;       // in render pixels, as the content moved (FSR's convention and the port's)
  float cameraNear = 0.1f, cameraFar = 1000.0f, fovY = 1.0f;  // fov in radians
  float frameTimeMs = 16.7f;
  float sharpness = 0;                  // 0..1; 0 = no RCAS pass
  bool reset = false;                   // the history does not fit (first frame, a jump)
};

// The Vulkan objects FSR works with; call once after the device exists. False when the library is not built.
bool init(const VulkanContext& vk);
// Records FSR into cmd (outside any rendering). The images live in VK_IMAGE_LAYOUT_GENERAL and stay there. The context
// is (re)created when the sizes change.
bool upscale(VkCommandBuffer cmd, const UpscaleDesc& d, std::string* error);
// FSR's own jitter sequence for a render → output ratio (pixels in [-0.5, 0.5]).
void jitter(uint32_t frameIndex, uint32_t renderWidth, uint32_t outputWidth, float& x, float& y);
// Frees the context (the device must be idle).
void shutdown();

}  // namespace regs::render::fsr
