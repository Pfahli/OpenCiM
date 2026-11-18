// GLSL 3.30 to SPIR-V through glslang (docs/renderer/vulkan.md Â§3). The source is the GL path's own text; the only
// edits are mechanical: vertex inputs get the locations GL's glBindAttribLocation would give them, and the vertex
// shader's clip depth is mapped from GL's âˆ’1..1 to Vulkan's 0..1. Loose uniforms go into one uniform block through
// glslang's relaxed Vulkan rules; reflection gives the device every uniform's offset and every sampler's binding.
#include "render/vk_shader.h"
#include "core/env.h"

#include "core/log.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

namespace regs::render::vk {

namespace {

std::once_flag g_init;

// ---- plain scans for the declarations the rewrites below edit (they used to be std::regex; same matches) ----
// A declaration starts at a line start: the start of the scan or the character after a '\n' (the regexes'
// "(^|\n)"); "blank" is a space or tab, "word" is [A-Za-z0-9_].

bool isBlank(char c) { return c == ' ' || c == '\t'; }
bool isWord(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
size_t skipBlanks(const std::string& s, size_t i) {
  while (i < s.size() && isBlank(s[i])) ++i;
  return i;
}
size_t skipWord(const std::string& s, size_t i) {
  while (i < s.size() && isWord(s[i])) ++i;
  return i;
}
bool startsWith(const std::string& s, size_t i, const char* lit, size_t n) {
  return s.size() >= i + n && s.compare(i, n, lit, n) == 0;
}
// the next line start at or after `from` (from itself counts: the start of a scan)
size_t nextLineStart(const std::string& s, size_t from, bool fromCounts) {
  if (fromCounts) return from;
  size_t nl = s.find('\n', from);
  return nl == std::string::npos ? std::string::npos : nl + 1;
}

// "[ \t]*in[ \t]+<type>[ \t]+<name>[ \t]*;" at line start p (name null: any word). Sets `in` to the keyword's position
// and `end` past the ';'.
bool inputDeclAt(const std::string& s, size_t p, const char* name, size_t nameLen, size_t& in, size_t& end) {
  size_t k = skipBlanks(s, p);
  if (!startsWith(s, k, "in", 2)) return false;
  size_t j = k + 2, b = skipBlanks(s, j);
  if (b == j) return false;
  j = skipWord(s, b);
  if (j == b) return false;
  b = skipBlanks(s, j);
  if (b == j) return false;
  if (name) {
    if (!startsWith(s, b, name, nameLen)) return false;
    j = b + nameLen;
  } else {
    j = skipWord(s, b);
    if (j == b) return false;
  }
  j = skipBlanks(s, j);
  if (j >= s.size() || s[j] != ';') return false;
  in = k;
  end = j + 1;
  return true;
}

// The first input declaration at a line start of s[from..], the scan starting at `from`.
bool findInputDecl(const std::string& s, size_t from, const char* name, size_t nameLen, size_t& in, size_t& end) {
  for (size_t p = from; p != std::string::npos && p <= s.size(); p = nextLineStart(s, p, false))
    if (inputDeclAt(s, p, name, nameLen, in, end)) return true;
  return false;
}

// "[ \t]*(out|in)[ \t]+(\w+)[ \t]+([\w \t,\[\]]+);" at line start p: the keyword's position, the type, the names and
// the end past the ';'
struct VaryingDecl {
  size_t kw = 0, end = 0;
  bool out = false;
  std::string type, names;
};
bool isNameChar(char c) { return isWord(c) || isBlank(c) || c == ',' || c == '[' || c == ']'; }
bool varyingDeclAt(const std::string& s, size_t p, VaryingDecl& d) {
  size_t k = skipBlanks(s, p), j;
  bool out;
  if (startsWith(s, k, "out", 3)) out = true, j = k + 3;
  else if (startsWith(s, k, "in", 2)) out = false, j = k + 2;
  else return false;
  size_t t0 = skipBlanks(s, j);
  if (t0 == j) return false;
  size_t t1 = skipWord(s, t0);
  if (t1 == t0) return false;
  size_t n0 = skipBlanks(s, t1);
  if (n0 == t1) return false;
  size_t n1 = n0;
  while (n1 < s.size() && isNameChar(s[n1])) ++n1;
  if (n1 > n0) {
    if (n1 >= s.size() || s[n1] != ';') return false;
  } else {
    // nothing but blanks before the ';': the names take the last blank (the regex backtracked into the blanks)
    if (n0 >= s.size() || s[n0] != ';' || n0 - t1 < 2) return false;
    n0 = n0 - 1;
  }
  d.kw = k;
  d.end = n1 + 1;
  d.out = out;
  d.type.assign(s, t0, t1 - t0);
  d.names.assign(s, n0, n1 - n0);
  return true;
}
bool findVaryingDecl(const std::string& s, size_t from, VaryingDecl& d) {
  for (size_t p = from; p != std::string::npos && p <= s.size(); p = nextLineStart(s, p, false))
    if (varyingDeclAt(s, p, d)) return true;
  return false;
}

// "void[ \t]+main[ \t]*\([ \t]*(void)?[ \t]*\)" anywhere: its first occurrence [begin, end)
bool findMainDecl(const std::string& s, size_t& begin, size_t& end) {
  for (size_t v = s.find("void"); v != std::string::npos; v = s.find("void", v + 1)) {
    size_t j = v + 4, b = skipBlanks(s, j);
    if (b == j || !startsWith(s, b, "main", 4)) continue;
    j = skipBlanks(s, b + 4);
    if (j >= s.size() || s[j] != '(') continue;
    j = skipBlanks(s, j + 1);
    size_t close = j;
    if (startsWith(s, j, "void", 4)) {
      size_t k = skipBlanks(s, j + 4);
      if (k < s.size() && s[k] == ')') close = k;
    }
    if (close >= s.size() || s[close] != ')') continue;
    begin = v;
    end = close + 1;
    return true;
  }
  return false;
}

// "in <type> <name>;" of a named attribute → "layout(location = N) in <type> <name>;"
std::string bindInputs(std::string src, const std::vector<std::pair<int, const char*>>& attribs) {
  for (const auto& [slot, name] : attribs) {
    size_t in, end;
    if (findInputDecl(src, 0, name, std::strlen(name), in, end))
      src.insert(in, "layout(location = " + std::to_string(slot) + ") ");
  }
  // the other inputs (attributes no layout supplies, e.g. regs_MultiTexCoord5-7) get locations above the named ones,
  // so automatic locations never land on a named slot; the device feeds them GL's default (0, 0, 0, 1)
  int next = 8;
  for (const auto& [slot, name] : attribs) next = std::max(next, slot + 1);
  std::string out;
  size_t at = 0, in, end;
  while (findInputDecl(src, at, nullptr, 0, in, end)) {
    out.append(src, at, in - at);
    out += "layout(location = " + std::to_string(next++) + ") ";
    out.append(src, in, end - in);
    at = end;
  }
  out.append(src, at, std::string::npos);
  return out;
}

// Varyings matched by name, as GL links them: the game's combiner shaders declare their varyings section by section,
// in a different order in the two stages, and SPIR-V matches by location. Every global "out" of the vertex shader
// and "in" of the fragment shader (one declaration per line, several names allowed) gets layout(location) from one
// table keyed by name; the fragment shader's first output gets location 0. Locations count GL's slots: a mat3 takes
// 3, an array of n takes n.
void matchVaryings(std::string& vs, std::string& fs) {
  std::map<std::string, int> loc;
  int next = 0;
  auto slots = [](const std::string& type) {
    if (type.rfind("mat", 0) == 0) return type.size() > 3 && type[3] >= '2' && type[3] <= '4' ? type[3] - '0' : 4;
    return 1;
  };
  auto rewrite = [&](std::string& src, bool keywordOut, bool fragmentOut) {
    std::string out;
    size_t at = 0;
    bool firstOut = true;
    VaryingDecl d;
    while (findVaryingDecl(src, at, d)) {
      out.append(src, at, d.kw - at);
      bool mine = d.out == keywordOut;
      bool isOut = fragmentOut && d.out;
      int where = -1;
      if ((mine || isOut) && d.names.find(',') == std::string::npos) {
        std::string name = d.names, count = "1";
        name.erase(0, name.find_first_not_of(" \t"));
        size_t lb = name.find('[');
        if (lb != std::string::npos) count = name.substr(lb + 1, name.find(']') - lb - 1), name = name.substr(0, lb);
        name.erase(name.find_last_not_of(" \t") + 1);
        if (isOut) {
          where = firstOut ? 0 : -1;
          firstOut = false;
        } else {
          auto f = loc.find(name);
          if (f == loc.end()) {
            f = loc.emplace(name, next).first;
            next += slots(d.type) * std::max(1, std::atoi(count.c_str()));
          }
          where = f->second;
        }
      }
      if (where >= 0) out += "layout(location = " + std::to_string(where) + ") ";
      out.append(src, d.kw, d.end - d.kw);
      at = d.end;
    }
    out.append(src, at, std::string::npos);
    src = std::move(out);
  };
  rewrite(vs, true, false);  // the vertex outputs first: their order sets the locations
  rewrite(fs, false, true);
  // GLSL 3.30 allows locations on varyings only with this extension
  for (std::string* s : {&vs, &fs}) {
    size_t v = s->find("#version");
    size_t eol = v == std::string::npos ? std::string::npos : s->find('\n', v);
    if (eol != std::string::npos) s->insert(eol + 1, "#extension GL_ARB_separate_shader_objects : enable\n");
  }
}

// GL clip depth −1..1 → Vulkan 0..1 after the shader's own main
std::string remapDepth(std::string src) {
  size_t begin, end;
  if (!findMainDecl(src, begin, end)) return src;
  src.replace(begin, end - begin, "void regs_vk_main()");
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

namespace {

bool compileUncached(const std::string& vs, const std::string& fs,
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
  static const bool debug = regs::envVar("REGS_VK_DEBUG") != nullptr;
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

// ---- the SPIR-V disk cache ----

// Bump when anything above changes what a source compiles to (the rewrites, the glslang options, the reflection) or
// when the file layout below changes: old entries then no longer match their key and are compiled anew.
constexpr uint32_t kCacheVersion = 1;
constexpr uint32_t kCacheMagic = 0x56534752;  // "RGSV"

std::mutex g_cacheMutex;
std::filesystem::path g_cacheFolder;
CacheStats g_stats;

// two independent 64-bit hashes of the key material: the first names the file, the second is checked inside it
struct Key {
  uint64_t a = 1469598103934665603ull, b = 0x243F6A8885A308D3ull;
  void add(const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
      a = (a ^ p[i]) * 1099511628211ull;
      b = (b + p[i] + 1) * 0x9E3779B97F4A7C15ull;
      b ^= b >> 29;
    }
  }
  void add(const std::string& s) {
    uint64_t n = s.size();
    add(&n, sizeof n);
    add(s.data(), s.size());
  }
};

Key keyOf(const std::string& vs, const std::string& fs, const std::vector<std::pair<int, const char*>>& attribs) {
  Key k;
  const glslang::Version gv = glslang::GetVersion();
  const uint32_t version[5] = {kCacheVersion, uint32_t(gv.major), uint32_t(gv.minor), uint32_t(gv.patch),
                               uint32_t(sizeof(UniformInfo))};
  k.add(version, sizeof version);
  k.add(gv.flavor ? std::string(gv.flavor) : std::string());
  k.add(vs);
  k.add(fs);
  for (const auto& [slot, name] : attribs) {
    int32_t s = slot;
    k.add(&s, sizeof s);
    k.add(std::string(name ? name : ""));
  }
  return k;
}

struct Writer {
  std::vector<uint8_t> b;
  void u32(uint32_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); }
  void i32(int v) { u32(uint32_t(v)); }
  void u64(uint64_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 8); }
  void str(const std::string& s) {
    u32(uint32_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
  }
  void words(const std::vector<uint32_t>& w) {
    u32(uint32_t(w.size()));
    const auto* p = reinterpret_cast<const uint8_t*>(w.data());
    b.insert(b.end(), p, p + w.size() * 4);
  }
};

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
  bool take(void* dst, size_t n) {
    if (!ok || size_t(end - p) < n) return ok = false;
    std::memcpy(dst, p, n);
    p += n;
    return true;
  }
  uint32_t u32() {
    uint32_t v = 0;
    take(&v, 4);
    return v;
  }
  int i32() { return int(u32()); }
  uint64_t u64() {
    uint64_t v = 0;
    take(&v, 8);
    return v;
  }
  std::string str() {
    uint32_t n = u32();
    if (!ok || size_t(end - p) < n) return ok = false, std::string();
    std::string s(reinterpret_cast<const char*>(p), n);
    p += n;
    return s;
  }
  std::vector<uint32_t> words() {
    uint32_t n = u32();
    if (!ok || size_t(end - p) / 4 < n) return ok = false, std::vector<uint32_t>();
    std::vector<uint32_t> w(n);
    std::memcpy(w.data(), p, size_t(n) * 4);
    p += size_t(n) * 4;
    return w;
  }
};

uint64_t checksum(const uint8_t* p, size_t n) {
  Key k;
  k.add(p, n);
  return k.a ^ (k.b << 1);
}

std::vector<uint8_t> serialize(const ProgramSpirv& s) {
  Writer w;
  w.words(s.vs);
  w.words(s.fs);
  w.i32(s.blockBinding);
  w.i32(s.blockSize);
  w.u32(s.bindless ? 1 : 0);
  w.u32(uint32_t(s.inputs.size()));
  for (int i : s.inputs) w.i32(i);
  w.u32(uint32_t(s.uniforms.size()));
  for (const UniformInfo& u : s.uniforms) {
    w.str(u.name);
    w.i32(u.offset), w.i32(u.arraySize), w.i32(u.arrayStride), w.i32(u.matrixStride), w.i32(u.columns),
        w.i32(u.components), w.i32(u.binding);
    w.u32((u.isInt ? 1u : 0u) | (u.sampler ? 2u : 0u) | (u.cube ? 4u : 0u) | (u.shadow ? 8u : 0u) | (u.accel ? 16u : 0u));
  }
  return std::move(w.b);
}

bool deserialize(const uint8_t* p, size_t n, ProgramSpirv& s) {
  Reader r{p, p + n};
  s.vs = r.words();
  s.fs = r.words();
  s.blockBinding = r.i32();
  s.blockSize = r.i32();
  s.bindless = r.u32() != 0;
  uint32_t inputs = r.u32();
  if (!r.ok || inputs > 64) return false;
  s.inputs.assign(inputs, 0);
  for (int& i : s.inputs) i = r.i32();
  uint32_t uniforms = r.u32();
  if (!r.ok || uniforms > 100000) return false;
  s.uniforms.assign(uniforms, UniformInfo{});
  for (UniformInfo& u : s.uniforms) {
    u.name = r.str();
    u.offset = r.i32(), u.arraySize = r.i32(), u.arrayStride = r.i32(), u.matrixStride = r.i32(), u.columns = r.i32(),
    u.components = r.i32(), u.binding = r.i32();
    uint32_t f = r.u32();
    u.isInt = f & 1, u.sampler = f & 2, u.cube = f & 4, u.shadow = f & 8, u.accel = f & 16;
  }
  // both stages must be SPIR-V (the magic number) and nothing may be left over
  return r.ok && r.p == r.end && !s.vs.empty() && !s.fs.empty() && s.vs[0] == 0x07230203u && s.fs[0] == 0x07230203u;
}

std::filesystem::path entryPath(const std::filesystem::path& folder, const Key& k) {
  char name[32];
  std::snprintf(name, sizeof name, "%016llx.spv", static_cast<unsigned long long>(k.a));
  return folder / name;
}

// 1 = loaded, 0 = no entry, −1 = an entry that cannot be used
int loadEntry(const std::filesystem::path& file, const Key& k, ProgramSpirv& out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return 0;
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return -1;
  Reader r{data.data(), data.data() + data.size()};
  if (r.u32() != kCacheMagic || r.u32() != kCacheVersion || r.u64() != k.a || r.u64() != k.b) return -1;
  uint64_t size = r.u64(), sum = r.u64();
  if (!r.ok || size != uint64_t(r.end - r.p) || checksum(r.p, size_t(size)) != sum) return -1;
  ProgramSpirv s;
  if (!deserialize(r.p, size_t(size), s)) return -1;
  out = std::move(s);
  return 1;
}

void storeEntry(const std::filesystem::path& folder, const std::filesystem::path& file, const Key& k,
                const ProgramSpirv& s) {
  std::vector<uint8_t> payload = serialize(s);
  Writer w;
  w.u32(kCacheMagic);
  w.u32(kCacheVersion);
  w.u64(k.a);
  w.u64(k.b);
  w.u64(payload.size());
  w.u64(checksum(payload.data(), payload.size()));
  w.b.insert(w.b.end(), payload.begin(), payload.end());
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  // written next to the entry and renamed over it: a run that dies mid-write leaves no half entry under the name
  std::filesystem::path tmp = file;
  tmp += L".tmp";
  bool ok;
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(w.b.data()), std::streamsize(w.b.size()));
    o.close();
    ok = !o.fail();
  }
  if (ok) std::filesystem::rename(tmp, file, ec);
  if (!ok || ec) std::filesystem::remove(tmp, ec);
}

}  // namespace

void setCacheFolder(const std::filesystem::path& folder) {
  std::lock_guard<std::mutex> lk(g_cacheMutex);
  g_cacheFolder = folder;
}

CacheStats cacheStats() {
  std::lock_guard<std::mutex> lk(g_cacheMutex);
  return g_stats;
}

bool compileProgram(const std::string& vs, const std::string& fs,
                    const std::vector<std::pair<int, const char*>>& attribs, ProgramSpirv& out, std::string* error) {
  std::filesystem::path folder;
  {
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    folder = g_cacheFolder;
  }
  auto t0 = std::chrono::steady_clock::now();
  auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
  Key key;
  std::filesystem::path file;
  int found = 0;
  if (!folder.empty()) {
    key = keyOf(vs, fs, attribs);
    file = entryPath(folder, key);
    found = loadEntry(file, key, out);
    if (found > 0) {
      std::lock_guard<std::mutex> lk(g_cacheMutex);
      ++g_stats.hits;
      g_stats.loadMs += ms();
      return true;
    }
  }
  if (!compileUncached(vs, fs, attribs, out, error)) return false;
  if (!folder.empty()) storeEntry(folder, file, key, out);
  std::lock_guard<std::mutex> lk(g_cacheMutex);
  ++g_stats.compiled;
  if (found < 0) ++g_stats.rejected;
  g_stats.compileMs += ms();
  return true;
}

}  // namespace regs::render::vk
