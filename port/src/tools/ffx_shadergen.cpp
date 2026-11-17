// ffx-shadergen: compiles one AMD FidelityFX GLSL pass (third_party/fidelityfx-fsr) into the permutation header the
// SDK's shader blob accessors include, in place of AMD's prebuilt FidelityFX_SC.exe (docs/renderer/vulkan.md §9.2).
// It uses the port's glslang and reads the bound resources (name, binding, count, set) from the SPIR-V itself:
// constant buffers by their instance name, textures, storage images, buffers, samplers.
//
//   ffx-shadergen --source <pass.glsl> --name <ffx_x_pass> --out <dir> [-I <dir>]... [-D NAME=VALUE]...
//                 --option NAME [--option NAME]... [--fixed NAME=VALUE]...
// Every --option becomes a bit of the permutation key (in the given order). Options without --fixed are compiled with
// 0 and 1; a --fixed option is compiled with its value only, and every key with the other value points at the same
// compiled permutation (the port always creates its contexts with these values). Writes <name>_permutations.h and the
// _wave64, _16bit and _wave64_16bit headers as aliases of it (the port has no wave64- or FP16-specific builds).
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

// #include "..." relative to the including file, then the -I directories
class Includer : public glslang::TShader::Includer {
 public:
  std::vector<fs::path> dirs;
  IncludeResult* includeLocal(const char* name, const char* from, size_t depth) override {
    fs::path base = fs::path(from).parent_path();
    if (IncludeResult* r = tryOpen(base / name)) return r;
    return includeSystem(name, from, depth);
  }
  IncludeResult* includeSystem(const char* name, const char*, size_t) override {
    for (const fs::path& d : dirs)
      if (IncludeResult* r = tryOpen(d / name)) return r;
    return nullptr;
  }
  void releaseInclude(IncludeResult* r) override {
    if (!r) return;
    delete static_cast<std::string*>(r->userData);
    delete r;
  }

 private:
  IncludeResult* tryOpen(const fs::path& p) {
    if (!fs::exists(p)) return nullptr;
    auto* text = new std::string(readFile(p));
    return new IncludeResult(p.generic_string(), text->data(), text->size(), text);
  }
};

struct Binding {
  std::string name;
  uint32_t binding = 0, count = 1, set = 0;
};
struct Reflection {
  std::vector<Binding> cbv, srvTex, uavTex, srvBuf, uavBuf, sampler, accel;
};

// the descriptor bindings of a SPIR-V module
Reflection reflect(const std::vector<uint32_t>& w) {
  std::map<uint32_t, std::string> names;
  std::map<uint32_t, uint32_t> binding, set, constant;
  std::map<uint32_t, bool> block, bufferBlock, nonWritable, memberNonWritable;
  struct Type {
    uint32_t op = 0, a = 0, b = 0, c = 0;  // per opcode: image (sampled, dim), pointer (storage, pointee), array (elem, len)
  };
  std::map<uint32_t, Type> types;
  struct Var {
    uint32_t id, type, storage;
  };
  std::vector<Var> vars;
  // ids that the function bodies mention: a declared resource no code uses is left out, like AMD's own tool does (the
  // effects' binding tables know only the used ones). A literal that happens to equal an id only keeps one too many.
  std::set<uint32_t> used;
  bool inFunctions = false;
  for (size_t i = 5; i < w.size();) {
    uint32_t op = w[i] & 0xFFFF, n = w[i] >> 16;
    if (n == 0) break;
    const uint32_t* a = &w[i + 1];
    if (op == 54) inFunctions = true;  // OpFunction
    if (inFunctions)
      for (uint32_t k = 1; k < n; ++k) used.insert(w[i + k]);
    switch (op) {
      case 5: names[a[0]] = reinterpret_cast<const char*>(&a[1]); break;                       // OpName
      case 71:                                                                                   // OpDecorate
        if (a[1] == 33) binding[a[0]] = a[2];
        if (a[1] == 34) set[a[0]] = a[2];
        if (a[1] == 2) block[a[0]] = true;
        if (a[1] == 3) bufferBlock[a[0]] = true;
        if (a[1] == 24) nonWritable[a[0]] = true;
        break;
      case 72:                                                                                   // OpMemberDecorate
        if (a[2] == 24) memberNonWritable[a[0]] = true;
        break;
      case 43: constant[a[1]] = a[2]; break;                                                     // OpConstant
      case 25: types[a[0]] = {op, a[6], 0, a[2]}; break;                                         // OpTypeImage: sampled, dim
      case 26: case 27: case 30: case 5341: types[a[0]] = {op, a[1], 0}; break;                  // sampler, sampled image, struct, accel
      case 28: types[a[0]] = {op, a[1], a[2]}; break;                                            // OpTypeArray
      case 29: types[a[0]] = {op, a[1], 0}; break;                                               // OpTypeRuntimeArray
      case 32: types[a[0]] = {op, a[1], a[2]}; break;                                            // OpTypePointer
      case 59: vars.push_back({a[1], a[0], a[2]}); break;                                        // OpVariable
    }
    i += n;
  }
  Reflection r;
  for (const Var& v : vars) {
    if (v.storage != 0 && v.storage != 2 && v.storage != 12) continue;  // UniformConstant, Uniform, StorageBuffer
    if (!binding.count(v.id) || !used.count(v.id)) continue;
    Binding b{names[v.id], binding[v.id], 1, set.count(v.id) ? set[v.id] : 0};
    uint32_t t = types[v.type].b;  // pointee
    while (types[t].op == 28 || types[t].op == 29) {
      b.count = types[t].op == 28 ? constant[types[t].b] : 0;
      t = types[t].a;
    }
    const Type& ty = types[t];
    if (ty.op == 30) {  // a block: constant buffer or storage buffer
      bool storage = v.storage == 12 || bufferBlock[t];
      if (!storage) r.cbv.push_back(b);
      else (nonWritable[v.id] || memberNonWritable[t] ? r.srvBuf : r.uavBuf).push_back(b);
    } else if (ty.op == 25 || ty.op == 27) {  // image or sampled image
      const Type& img = ty.op == 27 ? types[ty.a] : ty;
      bool texelBuffer = img.c == 5;  // Dim Buffer
      bool storageImage = img.a == 2;
      if (texelBuffer) (storageImage ? r.uavBuf : r.srvBuf).push_back(b);
      else (storageImage ? r.uavTex : r.srvTex).push_back(b);
    } else if (ty.op == 26) {
      r.sampler.push_back(b);
    } else if (ty.op == 5341) {
      r.accel.push_back(b);
    }
  }
  return r;
}

void writeGroup(std::ostream& o, const std::string& id, const char* kind, const std::vector<Binding>& v) {
  if (v.empty()) return;
  o << "static const char* " << id << "_" << kind << "Names[] = {";
  for (const Binding& b : v) o << " \"" << b.name << "\",";
  o << " };\nstatic const uint32_t " << id << "_" << kind << "Bindings[] = {";
  for (const Binding& b : v) o << " " << b.binding << ",";
  o << " };\nstatic const uint32_t " << id << "_" << kind << "Counts[] = {";
  for (const Binding& b : v) o << " " << b.count << ",";
  o << " };\nstatic const uint32_t " << id << "_" << kind << "Spaces[] = {";
  for (const Binding& b : v) o << " " << b.set << ",";
  o << " };\n";
}

std::string groupFields(const std::string& id, const char* kind, const std::vector<Binding>& v) {
  if (v.empty()) return "0, nullptr, nullptr, nullptr, nullptr";
  return std::to_string(v.size()) + ", " + id + "_" + kind + "Names, " + id + "_" + kind + "Bindings, " + id + "_" + kind +
         "Counts, " + id + "_" + kind + "Spaces";
}

}  // namespace

int main(int argc, char** argv) {
  fs::path source, outDir;
  std::string name;
  Includer includer;
  std::vector<std::string> defines, options;
  std::map<std::string, int> fixed;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i], v = argv[i + 1];
    if (a == "--source") source = v;
    else if (a == "--name") name = v;
    else if (a == "--out") outDir = v;
    else if (a == "-I") includer.dirs.push_back(v);
    else if (a == "-D") defines.push_back(v);
    else if (a == "--option") options.push_back(v);
    else if (a == "--fixed") {
      size_t eq = v.find('=');
      fixed[v.substr(0, eq)] = std::atoi(v.c_str() + eq + 1);
    } else {
      std::fprintf(stderr, "ffx-shadergen: unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  if (source.empty() || name.empty() || outDir.empty()) {
    std::fprintf(stderr, "ffx-shadergen: --source, --name and --out are needed\n");
    return 2;
  }
  glslang::InitializeProcess();
  const std::string text = readFile(source);
  const int optionCount = int(options.size());
  // compiled permutations: one per combination of the free options' values
  std::vector<int> freeBits;
  for (int k = 0; k < optionCount; ++k)
    if (!fixed.count(options[size_t(k)])) freeBits.push_back(k);
  struct Compiled {
    std::vector<uint32_t> spirv;
    Reflection refl;
  };
  std::vector<Compiled> compiled;
  for (uint32_t combo = 0; combo < (1u << freeBits.size()); ++combo) {
    std::string preamble;
    for (const std::string& d : defines) {
      size_t eq = d.find('=');
      preamble += "#define " + (eq == std::string::npos ? d : d.substr(0, eq) + " " + d.substr(eq + 1)) + "\n";
    }
    for (int k = 0; k < optionCount; ++k) {
      const std::string& opt = options[size_t(k)];
      int value = fixed.count(opt) ? fixed[opt] : 0;
      for (size_t f = 0; f < freeBits.size(); ++f)
        if (freeBits[f] == k) value = int(combo >> f & 1u);
      preamble += "#define " + opt + " " + std::to_string(value) + "\n";
    }
    glslang::TShader sh(EShLangCompute);
    const char* src = text.c_str();
    const std::string srcNameStr = source.generic_string();
    const char* srcName = srcNameStr.c_str();
    sh.setStringsWithLengthsAndNames(&src, nullptr, &srcName, 1);
    sh.setPreamble(preamble.c_str());
    sh.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    sh.setEntryPoint("main");
    auto msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    if (!sh.parse(GetDefaultResources(), 450, false, msgs, includer)) {
      std::fprintf(stderr, "ffx-shadergen: %s (permutation %u):\n%s\n", source.generic_string().c_str(), combo, sh.getInfoLog());
      return 1;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msgs)) {
      std::fprintf(stderr, "ffx-shadergen: link %s: %s\n", source.generic_string().c_str(), prog.getInfoLog());
      return 1;
    }
    Compiled c;
    glslang::SpvOptions so;
    so.disableOptimizer = true;
    glslang::GlslangToSpv(*prog.getIntermediate(EShLangCompute), c.spirv, &so);
    c.refl = reflect(c.spirv);
    compiled.push_back(std::move(c));
  }
  glslang::FinalizeProcess();

  fs::create_directories(outDir);
  std::ostringstream o;
  o << "// Generated by ffx-shadergen from " << source.filename().generic_string() << " (port/src/tools/ffx_shadergen.cpp)\n";
  o << "#pragma once\n#include <cstdint>\n\n";
  o << "#ifndef REGS_FFX_GEN_PERMUTATION_INFO\n#define REGS_FFX_GEN_PERMUTATION_INFO\n"
       "typedef struct RegsFfxPermutationInfo {\n"
       "  const unsigned char* blobData; uint32_t blobSize;\n"
       "  uint32_t numConstantBuffers; const char** constantBufferNames; const uint32_t* constantBufferBindings; const uint32_t* constantBufferCounts; const uint32_t* constantBufferSpaces;\n"
       "  uint32_t numSRVTextures; const char** srvTextureNames; const uint32_t* srvTextureBindings; const uint32_t* srvTextureCounts; const uint32_t* srvTextureSpaces;\n"
       "  uint32_t numUAVTextures; const char** uavTextureNames; const uint32_t* uavTextureBindings; const uint32_t* uavTextureCounts; const uint32_t* uavTextureSpaces;\n"
       "  uint32_t numSRVBuffers; const char** srvBufferNames; const uint32_t* srvBufferBindings; const uint32_t* srvBufferCounts; const uint32_t* srvBufferSpaces;\n"
       "  uint32_t numUAVBuffers; const char** uavBufferNames; const uint32_t* uavBufferBindings; const uint32_t* uavBufferCounts; const uint32_t* uavBufferSpaces;\n"
       "  uint32_t numSamplers; const char** samplerNames; const uint32_t* samplerBindings; const uint32_t* samplerCounts; const uint32_t* samplerSpaces;\n"
       "  uint32_t numRTAccelerationStructures; const char** rtAccelerationStructureNames; const uint32_t* rtAccelerationStructureBindings; const uint32_t* rtAccelerationStructureCounts; const uint32_t* rtAccelerationStructureSpaces;\n"
       "} RegsFfxPermutationInfo;\n#endif\n\n";
  o << "typedef union " << name << "_PermutationKey {\n  struct {\n";
  for (const std::string& opt : options) o << "    uint32_t " << opt << " : 1;\n";
  o << "  };\n  uint32_t index;\n} " << name << "_PermutationKey;\n\n";
  for (size_t p = 0; p < compiled.size(); ++p) {
    std::string id = "g_" + name + "_p" + std::to_string(p);
    const Compiled& c = compiled[p];
    o << "static const uint32_t " << id << "_spirv[] = {";
    for (size_t k = 0; k < c.spirv.size(); ++k) o << (k % 12 ? " " : "\n  ") << "0x" << std::hex << c.spirv[k] << std::dec << ",";
    o << "\n};\n";
    writeGroup(o, id, "cb", c.refl.cbv);
    writeGroup(o, id, "srvTex", c.refl.srvTex);
    writeGroup(o, id, "uavTex", c.refl.uavTex);
    writeGroup(o, id, "srvBuf", c.refl.srvBuf);
    writeGroup(o, id, "uavBuf", c.refl.uavBuf);
    writeGroup(o, id, "smp", c.refl.sampler);
    writeGroup(o, id, "rt", c.refl.accel);
  }
  o << "\nstatic const RegsFfxPermutationInfo g_" << name << "_PermutationInfo[] = {\n";
  for (size_t p = 0; p < compiled.size(); ++p) {
    std::string id = "g_" + name + "_p" + std::to_string(p);
    const Reflection& r = compiled[p].refl;
    o << "  { reinterpret_cast<const unsigned char*>(" << id << "_spirv), uint32_t(sizeof(" << id << "_spirv)),\n    "
      << groupFields(id, "cb", r.cbv) << ",\n    " << groupFields(id, "srvTex", r.srvTex) << ",\n    "
      << groupFields(id, "uavTex", r.uavTex) << ",\n    " << groupFields(id, "srvBuf", r.srvBuf) << ",\n    "
      << groupFields(id, "uavBuf", r.uavBuf) << ",\n    " << groupFields(id, "smp", r.sampler) << ",\n    "
      << groupFields(id, "rt", r.accel) << " },\n";
  }
  o << "};\n\n// permutation key → compiled permutation\nstatic const int32_t g_" << name << "_IndirectionTable[] = {";
  for (uint32_t key = 0; key < (1u << optionCount); ++key) {
    uint32_t combo = 0;
    for (size_t f = 0; f < freeBits.size(); ++f) combo |= (key >> freeBits[f] & 1u) << f;
    o << (key % 16 ? " " : "\n  ") << combo << ",";
  }
  o << "\n};\n";
  std::ofstream(outDir / (name + "_permutations.h"), std::ios::binary) << o.str();
  for (const char* variant : {"_wave64", "_16bit", "_wave64_16bit"}) {
    std::ostringstream a;
    a << "// Generated by ffx-shadergen: the port builds no wave64 or FP16 variants, these are the plain ones\n#pragma once\n"
      << "#include \"" << name << "_permutations.h\"\n"
      << "#define g_" << name << variant << "_PermutationInfo g_" << name << "_PermutationInfo\n"
      << "#define g_" << name << variant << "_IndirectionTable g_" << name << "_IndirectionTable\n";
    std::ofstream(outDir / (name + variant + "_permutations.h"), std::ios::binary) << a.str();
  }
  std::printf("ffx-shadergen: %s, %zu permutations\n", name.c_str(), compiled.size());
  return 0;
}
