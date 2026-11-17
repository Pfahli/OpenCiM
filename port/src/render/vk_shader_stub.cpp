// Vulkan device without glslang (bring-up builds only): no program compiles, so nothing but clears reaches the screen.
#include "render/vk_shader.h"

namespace regs::render::vk {

bool available() { return false; }

bool compileProgram(const std::string&, const std::string&, const std::vector<std::pair<int, const char*>>&,
                    ProgramSpirv&, std::string* error) {
  if (error) *error = "built without glslang (third_party/glslang): no GLSL to SPIR-V";
  return false;
}

}  // namespace regs::render::vk
