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
// rayShadows (Vulkan with ray queries, docs/renderer/vulkan.md §9.3): the fragment stage's shadow2D lookups are
// multiplied by a ray query toward the sun (uniforms regs_Scene, the frame's acceleration structure, and regs_SunPos,
// the sun in world space with w = 1 while the rays are on); the shadow map then only holds what the rays leave out.
// rays is a bit set: kRayShadows (true converts to it) and kRayReflections (§9.7): the fragment stage's textureCube
// lookups (the game's environment-cube reflections on water and glass) trace a ray along the reflected direction; what
// it hits is shaded from the device's hit table and bindless textures (regs_HitTable, regs_Images), a miss reads the
// cube as before.
constexpr unsigned kRayShadows = 1, kRayReflections = 2;
std::string translateGlsl110(const std::string& src, bool fragment, std::string* unsupported, bool hooks = false,
                             unsigned rays = 0);

// Port shader hooks for mods (port/docs/mod-manager.md §4.3): GLSL functions
//   void <function>(inout vec4 color, RegsSurface s)
// run on the final colour of every combiner program (the game's shaders, after fog, before the alpha test) in the
// colour passes of 3D views, never in shadow passes and never when no hook is registered (then the programs are
// exactly the original translation). RegsSurface: pos (world), normal (world, from screen derivatives, facing the
// eye), view (unit, eye → point), dist (to the eye), fog (0..1 at this distance), surfaceClass (the material's
// GpuMaterial::surfaceClass), time (the effect timer, s), uv / uv1 (the mesh's texture coordinates 0 and 1; merged
// trees carry (base height, a per-tree number 0..1) in uv1), shadow (1 in a shadow-map pass). Also visible: samplerCube
// regs_EnvMap (the game's textures/envmap.dds), regs_texel0(uv) (the material's first texture, unit 0) and the hook's
// own uniforms, set by name from script (regsShaderUniform). A hook with shadow = true also runs in the shadow-map
// passes (only its alpha counts there: alpha at or under the alpha-test reference casts no shadow). Hooks are fixed at
// start-up (enabled mods' manifests), before any program is built.
// Post hooks (post = true): void <function>(inout vec3 color, RegsPost p) run per screen pixel on the finished map view
// (before the port's colour styles) while a scene asks for them (gsScene.regsPostHooks); RegsPost: uv (0..1, bottom
// left), pos (world), view, dist, depth (0..1), sky (1 where nothing was drawn), time, texel (one pixel in uv);
// regs_postColor(uv), regs_postDepth(uv), regs_postWorld(uv) read the scene; the surface hooks' files and helpers
// (regs_surfaceAt ...) and uniforms are there too.
struct ShaderHook {
  std::string function, code, origin;
  bool shadow = false, post = false;
};
void addShaderHook(const ShaderHook& h);
const std::vector<ShaderHook>& shaderHooks();
bool postHooksRegistered();
bool shadowHooksRegistered();
// the post program's fragment shader (GLSL 3.30, for regs/shaders/post.vertex)
std::string postHookFragment();
// regsShaderUniform(name, value): vec4 uniforms of the hooks, applied to every hooked program per pass
std::map<std::string, std::array<float, 4>>& shaderHookUniforms();
// the mods' own textures for their hooks (regsShaderTexture): a sampler2D uniform name, the texture's game path, the
// unit it got (kModUnitFirst.., render/renderer3d.h) and its GL texture once loaded (engine/scene3d.cpp)
struct ShaderHookTexture {
  std::string sampler, path;
  int unit = -1;
  uint32_t texture = 0;
  bool tried = false;
};
std::vector<ShaderHookTexture>& shaderHookTextures();

}  // namespace regs::render
