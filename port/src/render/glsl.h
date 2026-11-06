#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>

namespace regs::render {

// Rewrites a shipped GLSL 1.10 shader (written against fixed-function built-ins) as GLSL 3.30 core:
// attribute/varying → in/out, texture2D/textureCube → texture, shadow2D → a vec4 wrapper, gl_FragColor → an output,
// and every built-in the shipped shaders read (gl_Vertex, gl_Normal, gl_Color, gl_MultiTexCoord0-7, gl_FrontColor,
// the matrices, gl_FrontMaterial, gl_LightSource[], gl_FrontLightProduct[], gl_LightModel, gl_Fog) → regs_* inputs
// and uniforms with the same members. Vertex inputs carry no locations; the program builder binds them.
// With hooks, the registered shader hooks are added (below). Returns "" when the source uses another built-in.
std::string translateGlsl110(const std::string& src, bool fragment, std::string* unsupported, bool hooks = false);

// Port shader hooks for mods (port/docs/mod-manager.md §4.3): GLSL functions
//   void <function>(inout vec4 color, RegsSurface s)
// run on the final colour of every combiner program (the game's shaders, after fog, before the alpha test) in the
// colour passes of 3D views, never in shadow passes and never when no hook is registered (then the programs are
// exactly the original translation). RegsSurface: pos (world), normal (world, from screen derivatives, facing the
// eye), view (unit, eye → point), dist (to the eye), fog (0..1 at this distance), surfaceClass (the material's
// GpuMaterial::surfaceClass), time (the effect timer, s). Also visible: samplerCube regs_EnvMap (the game's
// textures/envmap.dds) and the hook's own uniforms, set by name from script (regsShaderUniform). Hooks are fixed at
// start-up (enabled mods' manifests), before any program is built.
struct ShaderHook {
  std::string function, code, origin;
};
void addShaderHook(const ShaderHook& h);
const std::vector<ShaderHook>& shaderHooks();
// regsShaderUniform(name, value): vec4 uniforms of the hooks, applied to every hooked program per pass
std::map<std::string, std::array<float, 4>>& shaderHookUniforms();

}  // namespace regs::render
