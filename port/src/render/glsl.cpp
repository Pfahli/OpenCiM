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

}  // namespace

std::string translateGlsl110(const std::string& src, bool fragment, std::string* unsupported) {
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
    // port night lights (docs/renderer/night-lights.md): every vertex stage passes the world position (from clip
    // space, so skinned and altered vertices are right too) and the vertex colour alpha (merged buildings: a seed)
    head += "uniform mat4 regs_ViewInverse;\nuniform vec4 regs_ProjInverse;\nout vec3 regs_WorldPos;\nout float regs_Seed;\n";
    sub(R"(\bvoid\s+main\s*\()", "void regs_vmain(");
    s += "\nvoid main() {\n  regs_vmain();\n  float ze = -gl_Position.w;\n"
         "  vec3 e = vec3((gl_Position.x - regs_ProjInverse.z * ze) * regs_ProjInverse.x,\n"
         "                (gl_Position.y - regs_ProjInverse.w * ze) * regs_ProjInverse.y, ze);\n"
         "  regs_WorldPos = (regs_ViewInverse * vec4(e, 1.0)).xyz;\n  regs_Seed = regs_Color.a;\n}\n";
  } else {
    sub(R"(\bvarying\b)", "in");
    sub(R"(\bgl_Color\b)", "regs_FrontColor");
    sub(R"(\bgl_FragColor\b)", "regs_FragColor");
    head += "in vec4 regs_FrontColor;\nout vec4 regs_FragColor;\nin vec3 regs_WorldPos;\nin float regs_Seed;\n";
    // the fixed-function alpha test (glAlphaFunc GREATER ref) runs after the shader's own main
    sub(R"(\bvoid\s+main\s*\()", "void regs_main(");
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
