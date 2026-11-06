#include "render/glsl.h"

#include <regex>

namespace regs::render {

namespace {

// Fixed-function state of GLSL 1.10, re-declared as ordinary uniforms with the same layout and member names.
const char* kBuiltins = R"(struct regs_MaterialParameters { vec4 emission; vec4 ambient; vec4 diffuse; vec4 specular; float shininess; };
struct regs_LightSourceParameters { vec4 ambient; vec4 diffuse; vec4 specular; vec4 position; vec4 halfVector;
  vec3 spotDirection; float spotExponent; float spotCutoff; float spotCosCutoff;
  float constantAttenuation; float linearAttenuation; float quadraticAttenuation; };
struct regs_LightProducts { vec4 ambient; vec4 diffuse; vec4 specular; };
struct regs_LightModelParameters { vec4 ambient; };
struct regs_FogParameters { vec4 color; float density; float start; float end; float scale; };
uniform regs_MaterialParameters regs_FrontMaterial;
uniform regs_LightSourceParameters regs_LightSource[8];
uniform regs_LightProducts regs_FrontLightProduct[8];
uniform regs_LightModelParameters regs_LightModel;
uniform regs_FogParameters regs_Fog;
uniform mat4 regs_ModelViewMatrix;
uniform mat4 regs_ProjectionMatrix;
uniform mat4 regs_ModelViewProjectionMatrix;
uniform mat3 regs_NormalMatrix;
float regs_smoothstep(float a, float b, float x) { float t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
vec2 regs_smoothstep(vec2 a, vec2 b, vec2 x) { vec2 t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
vec3 regs_smoothstep(vec3 a, vec3 b, vec3 x) { vec3 t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
vec4 regs_smoothstep(vec4 a, vec4 b, vec4 x) { vec4 t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
vec2 regs_smoothstep(float a, float b, vec2 x) { return regs_smoothstep(vec2(a), vec2(b), x); }
vec3 regs_smoothstep(float a, float b, vec3 x) { return regs_smoothstep(vec3(a), vec3(b), x); }
vec4 regs_smoothstep(float a, float b, vec4 x) { return regs_smoothstep(vec4(a), vec4(b), x); }
)";

std::vector<ShaderHook>& hooks() {
  static std::vector<ShaderHook> h;
  return h;
}

// the fragment side of the hooks: the world position from the interpolated clip position (any vertex transform:
// terrain, merged items, skinning), the surface record, then each hook in registration order
std::string hookFragmentCode() {
  std::string s = R"(
in vec4 regs_HookClip;
uniform float regs_HookOn = 0.0;
uniform int regs_SurfaceClass = 0;
uniform float regs_HookItem = 0.0;
uniform mat4 regs_HookInvViewProj;
uniform vec3 regs_HookEye;
uniform float regs_HookTime;
uniform samplerCube regs_EnvMap;
uniform sampler2D regs_SurfaceMap;
uniform vec4 regs_SurfaceMapRect;
struct RegsSurface { vec3 pos; vec3 normal; vec3 view; float dist; float fog; int surfaceClass; float time; float item; };
// the surface class seen from above at a world point (engine/surface_map.h; 0 where nothing walkable was painted,
// -1 outside the map or while there is no map)
int regs_surfaceAt(vec2 xz) {
  vec2 uv = (xz - regs_SurfaceMapRect.xy) * regs_SurfaceMapRect.zw;
  if (regs_SurfaceMapRect.z <= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return -1;
  return int(texture(regs_SurfaceMap, uv).r * 255.0 + 0.5) & 127;
}
)";
  for (const ShaderHook& h : hooks()) s += "// hook " + h.function + " (" + h.origin + ")\n" + h.code + "\n";
  s += R"(void regs_hooks(inout vec4 c) {
  vec4 w = regs_HookInvViewProj * regs_HookClip;
  RegsSurface s;
  s.pos = w.xyz / w.w;
  vec3 e = s.pos - regs_HookEye;
  s.dist = length(e);
  s.view = e / max(s.dist, 1e-4);
  vec3 n = normalize(cross(dFdx(s.pos), dFdy(s.pos)));
  s.normal = dot(n, s.view) > 0.0 ? -n : n;
  s.fog = clamp((s.dist - regs_Fog.start) * regs_Fog.scale, 0.0, 1.0);
  s.surfaceClass = regs_SurfaceClass;
  s.time = regs_HookTime;
  s.item = regs_HookItem;
)";
  for (const ShaderHook& h : hooks()) s += "  " + h.function + "(c, s);\n";
  s += "}\n";
  return s;
}

}  // namespace

void addShaderHook(const ShaderHook& h) { hooks().push_back(h); }
const std::vector<ShaderHook>& shaderHooks() { return hooks(); }
std::map<std::string, std::array<float, 4>>& shaderHookUniforms() {
  static std::map<std::string, std::array<float, 4>> u;
  return u;
}

std::string translateGlsl110(const std::string& src, bool fragment, std::string* unsupported, bool withHooks) {
  withHooks = withHooks && !hooks().empty();
  std::string s = src;
  auto sub = [&](const char* re, const char* to) { s = std::regex_replace(s, std::regex(re), to); };
  sub(R"(^\s*#version[^\n]*\n)", "");
  sub(R"(\btexture2D\s*\()", "texture(");
  sub(R"(\btextureCube\s*\()", "texture(");
  sub(R"(\bshadow2D\s*\()", "regs_shadow2D(");
  // smoothstep(0.8, 0.0, x) (terrain maps) has edge0 > edge1, undefined in GLSL; the old drivers used the formula
  sub(R"(\bsmoothstep\s*\()", "regs_smoothstep(");
  sub(R"(\bgl_(ModelViewProjectionMatrix|ModelViewMatrix|ProjectionMatrix|NormalMatrix)\b)", "regs_$1");
  sub(R"(\bgl_(FrontMaterial|LightSource|FrontLightProduct|LightModel|Fog)\b)", "regs_$1");
  std::string head = "#version 330 core\n";
  head += kBuiltins;
  // shadow2D as GLSL 1.10 has it (vec4); the particle shader samples the shadow map in its vertex shader too
  head += "vec4 regs_shadow2D(sampler2DShadow s, vec3 c) { return vec4(texture(s, c)); }\n";
  if (!fragment) {
    sub(R"(\battribute\b)", "in");
    sub(R"(\bvarying\b)", "out");
    sub(R"(\bgl_(Vertex|Normal|Color|MultiTexCoord[0-7])\b)", "regs_$1");
    sub(R"(\bgl_FrontColor\b)", "regs_FrontColor");
    head += "in vec4 regs_Vertex;\nin vec3 regs_Normal;\nin vec4 regs_Color;\n";
    for (int i = 0; i < 8; ++i) head += "in vec4 regs_MultiTexCoord" + std::to_string(i) + ";\n";
    head += "out vec4 regs_FrontColor;\n";
    if (withHooks) {  // the clip position for the hooks' world position
      sub(R"(\bvoid\s+main\s*\()", "void regs_vmain(");
      head += "out vec4 regs_HookClip;\n";
      s += "\nvoid main() {\n  regs_vmain();\n  regs_HookClip = gl_Position;\n}\n";
    }
  } else {
    sub(R"(\bvarying\b)", "in");
    sub(R"(\bgl_Color\b)", "regs_FrontColor");
    sub(R"(\bgl_FragColor\b)", "regs_FragColor");
    head += "in vec4 regs_FrontColor;\nout vec4 regs_FragColor;\n";
    // the fixed-function alpha test (glAlphaFunc GREATER ref) runs after the shader's own main
    sub(R"(\bvoid\s+main\s*\()", "void regs_main(");
    if (withHooks)
      s += hookFragmentCode() +
           "\nuniform float regs_AlphaRef = -1.0;\nvoid main() {\n  regs_main();\n  if (regs_HookOn > 0.5) regs_hooks(regs_FragColor);\n"
           "  if (regs_FragColor.a <= regs_AlphaRef) discard;\n}\n";
    else
      s += "\nuniform float regs_AlphaRef = -1.0;\nvoid main() {\n  regs_main();\n  if (regs_FragColor.a <= regs_AlphaRef) discard;\n}\n";
  }
  std::regex builtin(R"(\bgl_[A-Za-z0-9_]+)");
  for (auto it = std::sregex_iterator(s.begin(), s.end(), builtin); it != std::sregex_iterator(); ++it) {
    const std::string n = it->str();
    if (n == "gl_Position" || n == "gl_FragCoord" || n == "gl_FragDepth" || n == "gl_PointSize" || n == "gl_FrontFacing")
      continue;
    if (unsupported) *unsupported = n;
    return "";
  }
  return head + s;
}

}  // namespace regs::render
