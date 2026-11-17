#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <tuple>

#include "script/vm.h"

namespace regs::engine {

// One generated program of a shader combiner: GLSL 1.10 sources as the engine builds them, the combiner's attribute,
// texture and parameter bits, and the GL program once the renderer compiled it.
struct ShaderVariant {
  std::string vertex, fragment;  // generated GLSL 1.10 (no #version)
  uint32_t attributes = 0, textures = 0, parameters = 0;
  uint32_t program = 0;          // GL program (0 until compiled)
  // the same with rays (Vulkan, docs/renderer/vulkan.md §9.3, §9.7), indexed by kRayShadows | kRayReflections
  uint32_t programRays[4] = {};
  bool raysFailed[4] = {};
  bool failed = false;           // expansion or compilation failed: draw nothing with it
};

// gsShaderCombiner_OGL: a .shader script with masks, an optional base shader and onProgram, plus its variants keyed
// by the masked flags (docs/renderer/interfaces-3d.md §2.12, scene-basics.md §6.1).
struct ShaderCombiner {
  std::u16string path;
  script::Ref<script::Object> script;
  ShaderCombiner* base = nullptr;
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, ShaderVariant> variants;
  uint32_t matMask = 0, meshMask = 0, envMask = 0;
};

// The combiner for a .shader path ("" = shaders/default/default.shader), loaded once; null when it cannot be loaded.
ShaderCombiner* getCombiner(script::VM& vm, const std::u16string& path);

// The variant for these flags (masked with the combiner's masks), generated on first use. Shadow-pass key
// reductions are applied by the caller.
ShaderVariant* combinerVariant(script::VM& vm, ShaderCombiner* c, uint32_t matFlags, uint32_t meshFlags, uint32_t envFlags);

// Converts one template component into a GSystem script (exposed for tests).
std::u16string templateToScript(const std::u16string& source);

void registerShaderNatives(script::VM& vm);  // gsShaderCombiner_OGL.setBaseShader, gsTemplate.output/output_once

}  // namespace regs::engine
