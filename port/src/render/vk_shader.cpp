// GLSL 3.30 to SPIR-V through glslang (docs/renderer/vulkan.md §3). The source is the GL path's own text; the only
// edits are mechanical: vertex inputs get the locations GL's glBindAttribLocation would give them, and the vertex
// shader's clip depth is mapped from GL's −1..1 to Vulkan's 0..1. Loose uniforms go into one uniform block through
// glslang's relaxed Vulkan rules; reflection gives the device every uniform's offset and every sampler's binding.
#include "render/vk_shader.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <mutex>
#include <regex>

namespace regs::render::vk {

namespace {

std::once_flag g_init;

// "in <type> <name>;" of a named attribute → "layout(location = N) in <type> <name>;"
std::string bindInputs(std::string src, const std::vector<std::pair<int, const char*>>& attribs) {
  for (const auto& [slot, name] : attribs) {
    std::regex decl("(^|\\n)([ \\t]*)in([ \\t]+\\w+[ \\t]+" + std::string(name) + "[ \\t]*;)");
    src = std::regex_replace(src, decl, "$1$2layout(location = " + std::to_string(slot) + ") in$3",
                             std::regex_constants::format_first_only);
  }
  return src;
}

// GL clip depth −1..1 → Vulkan 0..1 after the shader's own main
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
  if (!compileStage(v, remapDepth(bindInputs(vs, attribs)), error, "vertex shader")) return false;
  if (!compileStage(f, fs, error, "fragment shader")) return false;
  glslang::TProgram prog;
  prog.addShader(&v);
  prog.addShader(&f);
  auto msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!prog.link(msgs) || !prog.mapIO()) {
    if (error) *error = std::string("link: ") + prog.getInfoLog();
    return false;
  }
  if (!prog.buildReflection(EShReflectionDefault | EShReflectionSeparateBuffers)) {
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
    const glslang::TType* t = r.getType();
    if (!t) continue;
    UniformInfo u;
    u.name = r.name;
    if (u.name.rfind("regs_Uniforms.", 0) == 0) u.name = u.name.substr(14);
    u.arraySize = std::max(1, r.size);
    if (t->getBasicType() == glslang::EbtSampler) {
      u.sampler = true;
      u.cube = t->getSampler().dim == glslang::EsdCube;
      u.shadow = t->getSampler().shadow;
      u.binding = r.getBinding();
    } else {
      if (r.index < 0 || prog.getUniformBlock(r.index).name != "regs_Uniforms") continue;  // only the default block
      u.offset = r.offset;
      u.arrayStride = r.arrayStride > 0 ? r.arrayStride : 16;
      if (t->isMatrix()) {
        u.columns = t->getMatrixCols();
        u.components = t->getMatrixRows();
        u.matrixStride = 16;
      } else {
        u.components = std::max(1, t->getVectorSize());
      }
      glslang::TBasicType bt = t->getBasicType();
      u.isInt = bt == glslang::EbtInt || bt == glslang::EbtUint || bt == glslang::EbtBool;
    }
    out.uniforms.push_back(u);
  }
  out.inputs.clear();
  for (int i = 0; i < prog.getNumPipeInputs(); ++i) {
    const glslang::TObjectReflection& r = prog.getPipeInput(i);
    if (const glslang::TType* t = r.getType(); t && t->getQualifier().hasLocation())
      out.inputs.push_back(int(t->getQualifier().layoutLocation));
  }
  return true;
}

}  // namespace regs::render::vk
