// GLSL 3.30 to SPIR-V through glslang (docs/renderer/vulkan.md Â§3). The source is the GL path's own text; the only
// edits are mechanical: vertex inputs get the locations GL's glBindAttribLocation would give them, and the vertex
// shader's clip depth is mapped from GL's âˆ’1..1 to Vulkan's 0..1. Loose uniforms go into one uniform block through
// glslang's relaxed Vulkan rules; reflection gives the device every uniform's offset and every sampler's binding.
#include "render/vk_shader.h"

#include "core/log.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <regex>

namespace regs::render::vk {

namespace {

std::once_flag g_init;

// "in <type> <name>;" of a named attribute â†’ "layout(location = N) in <type> <name>;"
std::string bindInputs(std::string src, const std::vector<std::pair<int, const char*>>& attribs) {
  for (const auto& [slot, name] : attribs) {
    std::regex decl("(^|\\n)([ \\t]*)in([ \\t]+\\w+[ \\t]+" + std::string(name) + "[ \\t]*;)");
    src = std::regex_replace(src, decl, "$1$2layout(location = " + std::to_string(slot) + ") in$3",
                             std::regex_constants::format_first_only);
  }
  // the other inputs (attributes no layout supplies, e.g. regs_MultiTexCoord5-7) get locations above the named ones,
  // so automatic locations never land on a named slot; the device feeds them GL's default (0, 0, 0, 1)
  int next = 8;
  for (const auto& [slot, name] : attribs) next = std::max(next, slot + 1);
  static const std::regex other("(^|\\n)([ \\t]*)in([ \\t]+\\w+[ \\t]+\\w+[ \\t]*;)");
  std::string out;
  std::smatch m;
  auto it = src.cbegin();
  while (std::regex_search(it, src.cend(), m, other)) {
    out.append(it, m[0].first);
    out += m[1].str() + m[2].str() + "layout(location = " + std::to_string(next++) + ") in" + m[3].str();
    it = m[0].second;
  }
  out.append(it, src.cend());
  return out;
}

// Varyings matched by name, as GL links them: the game's combiner shaders declare their varyings section by section,
// in a different order in the two stages, and SPIR-V matches by location. Every global "out" of the vertex shader
// and "in" of the fragment shader (one declaration per line, several names allowed) gets layout(location) from one
// table keyed by name; the fragment shader's first output gets location 0. Locations count GL's slots: a mat3 takes
// 3, an array of n takes n.
void matchVaryings(std::string& vs, std::string& fs) {
  static const std::regex decl("(^|\\n)([ \\t]*)(out|in)[ \\t]+(\\w+)[ \\t]+([\\w \\t,\\[\\]]+);");
  std::map<std::string, int> loc;
  int next = 0;
  auto slots = [](const std::string& type) {
    if (type.rfind("mat", 0) == 0) return type.size() > 3 && type[3] >= '2' && type[3] <= '4' ? type[3] - '0' : 4;
    return 1;
  };
  auto rewrite = [&](std::string& src, const char* keyword, bool fragmentOut) {
    std::string out;
    std::smatch m;
    auto it = src.cbegin();
    bool firstOut = true;
    while (std::regex_search(it, src.cend(), m, decl)) {
      out.append(it, m[0].first);
      std::string kw = m[3].str(), type = m[4].str(), names = m[5].str();
      bool mine = kw == keyword;
      bool isOut = fragmentOut && kw == "out";
      if ((mine || isOut) && names.find(',') == std::string::npos) {
        std::string name = names, count = "1";
        name.erase(0, name.find_first_not_of(" \t"));
        size_t lb = name.find('[');
        if (lb != std::string::npos) count = name.substr(lb + 1, name.find(']') - lb - 1), name = name.substr(0, lb);
        name.erase(name.find_last_not_of(" \t") + 1);
        int at;
        if (isOut) {
          at = firstOut ? 0 : -1;
          firstOut = false;
        } else {
          auto f = loc.find(name);
          if (f == loc.end()) {
            f = loc.emplace(name, next).first;
            next += slots(type) * std::max(1, std::atoi(count.c_str()));
          }
          at = f->second;
        }
        if (at >= 0) {
          out += m[1].str() + m[2].str() + "layout(location = " + std::to_string(at) + ") " + m[0].str().substr(m[1].length() + m[2].length());
          it = m[0].second;
          continue;
        }
      }
      out += m[0].str();
      it = m[0].second;
    }
    out.append(it, src.cend());
    src = std::move(out);
  };
  rewrite(vs, "out", false);  // the vertex outputs first: their order sets the locations
  rewrite(fs, "in", true);
  // GLSL 3.30 allows locations on varyings only with this extension
  for (std::string* s : {&vs, &fs}) {
    size_t v = s->find("#version");
    size_t eol = v == std::string::npos ? std::string::npos : s->find('\n', v);
    if (eol != std::string::npos) s->insert(eol + 1, "#extension GL_ARB_separate_shader_objects : enable\n");
  }
}

// GL clip depth âˆ’1..1 â†’ Vulkan 0..1 after the shader's own main
std::string remapDepth(std::string src) {
  static const std::regex mainDecl("void[ \\t]+main[ \\t]*\\([ \\t]*(void)?[ \\t]*\\)");
  if (!std::regex_search(src, mainDecl)) return src;
  src = std::regex_replace(src, mainDecl, "void regs_vk_main()", std::regex_constants::format_first_only);
  src += "\nvoid main() {\n  regs_vk_main();\n  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n}\n";
  return src;
}

bool compileStage(glslang::TShader& sh, const std::string& src, std::string* error, const char* stage) {
  const char* text = src.c_str();
  sh.setStrings(&text, 1);
  EShLanguage lang = sh.getStage();
  sh.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
  sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
  sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
  sh.setEnvInputVulkanRulesRelaxed();
  sh.setGlobalUniformBlockName("regs_Uniforms");
  sh.setGlobalUniformSet(0);
  sh.setGlobalUniformBinding(0);
  sh.setAutoMapBindings(true);
  sh.setAutoMapLocations(true);
  auto msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!sh.parse(GetDefaultResources(), 330, ECoreProfile, false, false, msgs)) {
    if (error) *error = std::string(stage) + ": " + sh.getInfoLog();
    return false;
  }
  return true;
}

}  // namespace

bool available() { return true; }

bool compileProgram(const std::string& vs, const std::string& fs,
                    const std::vector<std::pair<int, const char*>>& attribs, ProgramSpirv& out, std::string* error) {
  std::call_once(g_init, [] { glslang::InitializeProcess(); });
  glslang::TShader v(EShLangVertex), f(EShLangFragment);
  std::string vsText = bindInputs(vs, attribs), fsText = fs;
  // ray queries (raytraced shadows) need GLSL 4.60; both stages move up together
  if (fsText.find("GL_EXT_ray_query") != std::string::npos)
    for (std::string* s : {&vsText, &fsText})
      if (size_t at = s->find("#version 330 core"); at != std::string::npos) s->replace(at, 17, "#version 460 core");
  matchVaryings(vsText, fsText);
  vsText = remapDepth(vsText);
  static const bool debug = std::getenv("REGS_VK_DEBUG") != nullptr;
  static int dumped = 0;
  if (debug && dumped < 8) {  // REGS_VK_DEBUG: the first programs as Vulkan compiles them, in the working directory
    std::FILE* file = std::fopen(("vkdump-" + std::to_string(dumped++) + ".glsl").c_str(), "wb");
    if (file) {
      std::fprintf(file, "// vertex\n%s\n// fragment\n%s\n", vsText.c_str(), fsText.c_str());
      std::fclose(file);
    }
  }
  if (!compileStage(v, vsText, error, "vertex shader")) return false;
  if (!compileStage(f, fsText, error, "fragment shader")) return false;
  glslang::TProgram prog;
  prog.addShader(&v);
  prog.addShader(&f);
  auto msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!prog.link(msgs) || !prog.mapIO()) {
    if (error) *error = std::string("link: ") + prog.getInfoLog();
    return false;
  }
  if (!prog.buildReflection(EShReflectionDefault | EShReflectionSeparateBuffers | EShReflectionStrictArraySuffix)) {
    if (error) *error = "reflection failed";
    return false;
  }
  glslang::SpvOptions opt;
  opt.disableOptimizer = true;
  glslang::GlslangToSpv(*prog.getIntermediate(EShLangVertex), out.vs, &opt);
  glslang::GlslangToSpv(*prog.getIntermediate(EShLangFragment), out.fs, &opt);

  out.uniforms.clear();
  out.blockBinding = -1;
  out.blockSize = 0;
  for (int i = 0; i < prog.getNumUniformBlocks(); ++i) {
    const glslang::TObjectReflection& b = prog.getUniformBlock(i);
    if (b.name == "regs_Uniforms") {
      out.blockBinding = b.getBinding();
      out.blockSize = b.size;
    }
  }
  for (int i = 0; i < prog.getNumUniformVariables(); ++i) {
    const glslang::TObjectReflection& r = prog.getUniform(i);
    UniformInfo u;
    u.name = r.name;
    if (u.name.rfind("regs_Uniforms.", 0) == 0) u.name = u.name.substr(14);
    u.arraySize = std::max(1, r.size);
    // the type as a GL enum (glDefineType): samplers, matrices, vectors, ints
    const int gt = r.glDefineType;
    // the bindless set (set 1, raytraced reflections) is the device's own, not a per-draw binding of set 0
    if (u.name.rfind("regs_Images", 0) == 0 || u.name.rfind("regs_HitSampler", 0) == 0) continue;
    if (u.name == "regs_Scene") {  // the ray-query scene (render/glsl.h, raytraced shadows)
      u.accel = true;
      u.binding = r.getBinding();
    } else if (gt == 0x8B5E /* SAMPLER_2D */ || gt == 0x8B60 /* SAMPLER_CUBE */ || gt == 0x8B62 /* SAMPLER_2D_SHADOW */ ||
        gt == 0x8DC5 /* SAMPLER_CUBE_SHADOW */ || gt == 0x8B5F /* SAMPLER_3D */) {
      u.sampler = true;
      u.cube = gt == 0x8B60 || gt == 0x8DC5;
      u.shadow = gt == 0x8B62 || gt == 0x8DC5;
      u.binding = r.getBinding();
    } else {
      if (r.index < 0 || prog.getUniformBlock(r.index).name != "regs_Uniforms") continue;  // only the default block
      u.offset = r.offset;
      u.arrayStride = r.arrayStride > 0 ? r.arrayStride : 16;
      switch (gt) {
        case 0x8B5A: u.columns = 2, u.components = 2; break;  // mat2
        case 0x8B5B: u.columns = 3, u.components = 3; break;  // mat3
        case 0x8B5C: u.columns = 4, u.components = 4; break;  // mat4
        case 0x8B50: case 0x8B53: case 0x8B57: u.components = 2; break;
        case 0x8B51: case 0x8B54: case 0x8B58: u.components = 3; break;
        case 0x8B52: case 0x8B55: case 0x8B59: u.components = 4; break;
        case 0x8DC6: u.components = 2; break;  // uvec2 (regs_HitTable)
        case 0x8DC7: u.components = 3; break;
        case 0x8DC8: u.components = 4; break;
        default: u.components = 1; break;
      }
      if (u.columns) u.matrixStride = 16;
      u.isInt = gt == 0x1404 || gt == 0x1405 || (gt >= 0x8B53 && gt <= 0x8B59) || (gt >= 0x8DC6 && gt <= 0x8DC8);
    }
    out.uniforms.push_back(u);
  }
  out.bindless = fsText.find("regs_Images") != std::string::npos;
  out.inputs.clear();
  for (int i = 0; i < prog.getNumPipeInputs(); ++i) {
    unsigned loc = prog.getPipeInput(i).layoutLocation();
    if (loc < 64) out.inputs.push_back(int(loc));
  }
  return true;
}

}  // namespace regs::render::vk
