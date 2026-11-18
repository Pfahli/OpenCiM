// Built without the Vulkan backend (CMake REGS_VULKAN off: third_party/vulkan-headers or third_party/glslang missing).
#include "render/gpu.h"

namespace regs::render::gpu {

std::unique_ptr<Device> createVulkanDevice(platform::Window&, bool, std::string* error, const std::filesystem::path&) {
  if (error) *error = "this build has no Vulkan backend (third_party/vulkan-headers and third_party/glslang missing)";
  return nullptr;
}

}  // namespace regs::render::gpu
