#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// GLSL 3.30 (the port's translated shaders) to SPIR-V for the Vulkan device (docs/renderer/vulkan.md §3), with the
// reflection the device needs to emulate GL's named uniforms: the loose uniforms live in one uniform block (glslang's
// relaxed Vulkan rules), samplers get their own bindings.
namespace regs::render::vk {

struct UniformInfo {
  std::string name;      // as the GL path would query it: "custom0", "regs_LightSource[2].position", "bones"
  int offset = 0;        // in the default block (std140)
  int arraySize = 1;
  int arrayStride = 0;   // bytes between elements (std140: at least 16)
  int matrixStride = 0;  // bytes between matrix columns (16)
  int columns = 0;       // matrices: 3 or 4; 0 = not a matrix
  int components = 1;    // scalars and vectors: 1-4
  bool isInt = false;    // int, bool, ivec
  // samplers: not in the block; the descriptor binding and kind
  bool sampler = false;
  bool cube = false, shadow = false;
  bool accel = false;    // accelerationStructureEXT (the ray-query scene, regs_Scene): binding, no block data
  int binding = -1;
};

struct ProgramSpirv {
  std::vector<uint32_t> vs, fs;
  std::vector<UniformInfo> uniforms;
  int blockBinding = -1;  // the default uniform block's binding, −1 when the program has no loose uniforms
  int blockSize = 0;
  std::vector<int> inputs;  // vertex input locations the vertex shader reads
  bool bindless = false;    // reads the device's bindless set (set 1: regs_Images, regs_HitSampler; reflections)
};

// False when glslang is not in the build (then error says so) or the source does not compile/link.
bool available();
bool compileProgram(const std::string& vs, const std::string& fs,
                    const std::vector<std::pair<int, const char*>>& attribs, ProgramSpirv& out, std::string* error);

}  // namespace regs::render::vk
