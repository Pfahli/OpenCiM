// The shader combiner (gsShaders_OGL / gsShaderCombiner_OGL / gsTemplate): .shader scripts choose components in
// onProgram, and each component is a template whose '#' lines are GSystem script (docs/renderer/scene-basics.md §6.1,
// interfaces-3d.md §2.12). Templates run through the port's own VM, so every shipped shader variant is generated the
// way the engine generates it.
#include "engine/shaders.h"

#include <cctype>
#include <memory>
#include <set>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

namespace {

std::unordered_map<std::u16string, std::unique_ptr<ShaderCombiner>>& cache() {
  static std::unordered_map<std::u16string, std::unique_ptr<ShaderCombiner>> c;
  return c;
}

std::unordered_map<const Object*, ShaderCombiner*>& byScript() {
  static std::unordered_map<const Object*, ShaderCombiner*> m;
  return m;
}

// output target of the template being run
struct Expansion {
  std::u16string text;
  std::set<std::u16string>* once = nullptr;  // one set per program stage
};
Expansion* g_expansion = nullptr;

int32_t intField(VM& vm, Object* o, const char16_t* name) {
  try {
    return vm.toInt(vm.resolve(vm.getMember(Value::heap(VT::Object, o), name)));
  } catch (const ScriptError&) {
    return 0;
  }
}

Value field(VM& vm, Object* o, const char16_t* name) {
  try {
    return vm.resolve(vm.getMember(Value::heap(VT::Object, o), name));
  } catch (const ScriptError&) {
    return Value();
  }
}

Value copyComponents(const Value& v) {
  // a derived combiner starts from copies of its base's component maps
  if (MapObj* m = v.map()) {
    auto c = make<MapObj>();
    c->entries = m->entries;
    return Value::heap(VT::Map, c.get());
  }
  return v;
}

// gsShaderCombiner_OGL variant setup (FUN_10036b80): the base's onProgram first, then inherit its results
void runProgram(VM& vm, ShaderCombiner* c, uint32_t mat, uint32_t mesh, uint32_t env) {
  Object* s = c->script.get();
  if (c->base) {
    runProgram(vm, c->base, mat, mesh, env);
    Object* b = c->base->script.get();
    for (const char16_t* f : {u"attributes", u"textures", u"parameters"}) s->fieldValues[f] = Value::integer(intField(vm, b, f));
    for (const char16_t* f : {u"vertexComponent", u"fragmentComponent"}) s->fieldValues[f] = copyComponents(field(vm, b, f));
  }
  vm.fireEvent(s, u"onProgram", {Value::integer(int32_t(mat)), Value::integer(int32_t(mesh)), Value::integer(int32_t(env))});
}

// Port adjustments to shipped shader components, applied in memory (the game files stay untouched). The trees' wind
// (metro/shaders/trees/alter.vertex, grid-items.md §3.6) scales the sway with the square of the vertex height above
// the model's origin. The engine draws trees as instances, so pos is model space there; the port merges them into
// division meshes (grid_items.cpp), where pos is division space and the terrain height would join the square. The
// merged tree vertices carry their base height in uv1.x, as the engine's own merged trees do (§3.2.3), and the
// subtraction the shipped line has commented out is switched on.
//
// Night lights (docs/renderer/night-lights.md, engine/night_lights.cpp): the default shader's maps component adds the
// lit windows of building materials (their window-group mask on unit 16, the building's seed in regs_Seed) and its
// colour component adds the street-lamp light (the lamp map on unit 17, sampled at the world position) times the
// surface colour, plus the window glow, before fog. Both are zero by day and without the day/night cycle, and
// shadow passes and additive materials leave them out.
namespace {

const char16_t* kNightMapsDecl =
    u"#$regsNight = $diffusemap && !bool($envFlags & (ENVIROMENT_SHADOWMAP_RENDER | ENVIROMENT_SHADOW_RENDER)) && "
    u"!bool($matFlags & (MATERIAL_ADDITIVE | MATERIAL_ADDITIVE_A));\n"
    u"#once# vec3 regs_windowGlow = vec3(0.0);\n"
    u"#if ($regsNight) {\n"
    u"#once# uniform vec4 regs_Night;\n"
    u"uniform sampler2D regs_WindowMask;\n"
    u"uniform float regs_WindowOn;\n"
    u"uniform float regs_Shop;\n"
    u"vec4 regs_hash4(float s, float k) {\n"
    u"  return fract(sin(vec4(s * 12.9898 + k * 4.1, s * 78.233 + k * 1.3, s * 37.719 + k * 2.9, s * 93.989 + k * 0.7)) * 43758.5453);\n"
    u"}\n"
    u"uniform vec4 regs_Cabin;\n"
    u"float regs_passengers() {\n"
    u"  float H = regs_Cabin.y - regs_Cabin.x;\n"
    u"  if (H < 0.3) return 0.0;\n"
    u"  vec3 p = regs_ModelPos;\n"
    u"  float side = p.x < 0.0 ? 1.0 : 2.0, occ = regs_Cabin.z;\n"
    u"  float slot = floor(p.z / 0.85);\n"
    u"  vec4 h = regs_hash4(slot * 3.0 + side, 7.0);\n"
    u"  float ux = p.z - (slot + 0.5) * 0.85 - (h.y - 0.5) * 0.18;\n"
    u"  float headY = regs_Cabin.x + H * (0.4 + 0.1 * h.z), r = 0.095 + 0.02 * h.w;\n"
    u"  float head = 1.0 - smoothstep(r - 0.02, r + 0.01, length(vec2(ux, (p.y - headY) * 0.9)));\n"
    u"  float body = (1.0 - smoothstep(0.16, 0.2, abs(ux))) * (1.0 - smoothstep(headY - r - 0.08, headY - r - 0.02, p.y));\n"
    u"  float seated = max(head, body) * smoothstep(h.x - 0.05, h.x + 0.05, occ * 1.35);\n"
    u"  float slotS = floor(p.z / 0.85 + 0.5);\n"
    u"  vec4 g = regs_hash4(slotS * 5.0 + side, 9.0);\n"
    u"  float us = p.z - slotS * 0.85 - (g.y - 0.5) * 0.2;\n"
    u"  float headS = regs_Cabin.x + H * (0.8 + 0.12 * g.z), rs = 0.1 + 0.02 * g.w;\n"
    u"  float headSt = 1.0 - smoothstep(rs - 0.02, rs + 0.01, length(vec2(us, (p.y - headS) * 0.9)));\n"
    u"  float bodySt = (1.0 - smoothstep(0.15, 0.19, abs(us))) * (1.0 - smoothstep(headS - rs - 0.08, headS - rs - 0.02, p.y));\n"
    u"  float standing = max(headSt, bodySt) * smoothstep(0.5 + 0.45 * g.x - 0.05, 0.5 + 0.45 * g.x + 0.05, occ);\n"
    u"  return max(seated, standing);\n"
    u"}\n"
    u"vec3 regs_windows(vec2 uv, vec3 albedo) {\n"
    u"  if (regs_WindowOn < 0.5) return vec3(0.0);\n"
    u"  vec4 m = texture2D(regs_WindowMask, uv);\n"
    u"  if (regs_WindowOn > 1.5 && regs_WindowOn < 2.5) {\n"
    u"    float ls = floor(regs_Seed * 255.0 + 0.5);\n"
    u"    float onAt = 0.36 + 0.14 * fract(sin(ls * 12.9898 + 4.1) * 43758.5453);\n"
    u"    float lit = smoothstep(onAt, onAt + 0.04, regs_Night.w);\n"
    u"    return m.r * vec3(2.6, 2.15, 1.5) * lit * (0.65 + 0.7 * dot(albedo, vec3(0.3, 0.59, 0.11)));\n"
    u"  }\n"
    u"  if (regs_WindowOn > 4.5 && regs_WindowOn < 5.5) {\n"
    u"    return m.r * albedo * vec3(1.3, 1.2, 0.95) * smoothstep(0.3, 0.5, regs_Night.w);\n"
    u"  }\n"
    u"  if (regs_WindowOn > 2.5 && regs_WindowOn < 3.5) {\n"
    u"    float ss = floor(regs_Seed * 255.0 + 0.5);\n"
    u"    float pick = fract(sin(ss * 78.233 + 1.7) * 43758.5453), onAt = 0.3 + 0.2 * fract(sin(ss * 37.719) * 43758.5453);\n"
    u"    float lit = pick < 0.6 ? smoothstep(onAt, onAt + 0.04, regs_Night.w) : 0.0;\n"
    u"    return m.r * albedo * 1.7 * lit;\n"
    u"  }\n"
    u"  if (regs_WindowOn > 9.5) {\n"
    u"    float code = floor(regs_WindowOn), on = fract(regs_WindowOn) / 0.9;\n"
    u"    float inside = code > 99.5 ? 1.0 : 0.0;\n"
    u"    code -= 100.0 * inside;\n"
    u"    float fm = floor((code - 10.0) / 4.0), rm = code - 10.0 - fm * 4.0;\n"
    u"    vec3 head = vec3(2.3, 2.15, 1.85), tail = vec3(1.5, 0.1, 0.05);\n"
    u"    vec3 fc = fm < 0.5 ? vec3(0.0) : fm < 1.5 ? head : fm < 2.5 ? tail : tail * 1.8;\n"
    u"    vec3 rc = rm < 0.5 ? vec3(0.0) : rm < 1.5 ? head : rm < 2.5 ? tail : tail * 1.8;\n"
    u"    vec3 cabin = m.b * inside * vec3(0.78, 0.7, 0.56) * (0.55 + 1.2 * dot(albedo, vec3(0.3, 0.59, 0.11)));\n"
    u"    if (inside > 0.5 && m.b > 0.0) cabin *= 1.0 - 0.85 * regs_passengers();\n"
    u"    return ((m.r * fc + m.g * rc) * (0.75 + 0.5 * dot(albedo, vec3(0.3, 0.59, 0.11))) + cabin) * on;\n"
    u"  }\n"
    u"  float s = floor(regs_Seed * 255.0 + 0.5);\n"
    u"  vec4 r = regs_hash4(s, 1.0), t = regs_hash4(s, 2.0), b = regs_hash4(s, 3.0);\n"
    u"  vec4 on = smoothstep(r - 0.03, r + 0.03, vec4(regs_Night.y));\n"
    u"  vec3 sum = vec3(0.0);\n"
    u"  for (int k = 0; k < 3; ++k) {\n"
    u"    vec3 tint = t[k] < 0.06 ? vec3(0.55, 0.64, 0.82) : mix(vec3(1.0, 0.6, 0.3), vec3(1.0, 0.84, 0.62), (t[k] - 0.06) / 0.94);\n"
    u"    sum += m[k] * on[k] * (0.7 + 0.6 * b[k]) * tint;\n"
    u"  }\n"
    u"  float shopOn = smoothstep(r.w - 0.03, r.w + 0.03, regs_Shop);\n"
    u"  sum += m.a * shopOn * 1.6 * mix(vec3(1.0, 0.82, 0.6), vec3(1.0, 0.95, 0.88), t.w);\n"
    u"  return sum * (0.6 + 1.5 * dot(albedo, vec3(0.3, 0.59, 0.11))) * regs_Night.x;\n"
    u"}\n"
    u"#}\n";
const char16_t* kNightMapsCall =
    u"  #if ($regsNight) {\n"
    u"  regs_windowGlow = regs_windows(v_texCoord.xy, diffusemap.rgb);\n"
    u"  #}\n";
const char16_t* kNightColorDecl =
    u"#$regsLamp = !bool($envFlags & (ENVIROMENT_SHADOWMAP_RENDER | ENVIROMENT_SHADOW_RENDER)) && "
    u"!bool($matFlags & (MATERIAL_ADDITIVE | MATERIAL_ADDITIVE_A));\n"
    u"#once# vec3 regs_windowGlow = vec3(0.0);\n"
    u"#if ($regsLamp) {\n"
    u"#once# uniform vec4 regs_Night;\n"
    u"uniform sampler2D regs_LampMap;\n"
    u"uniform vec4 regs_LampMapXf;\n"
    u"uniform vec4 regs_LampHeight;\n"
    u"uniform sampler2D regs_CarMap;\n"
    u"uniform vec4 regs_CarMapXf;\n"
    u"uniform vec4 regs_CarLight;\n"
    u"vec3 regs_carLight() {\n"
    u"  if (regs_CarLight.z <= 0.0) return vec3(0.0);\n"
    u"  vec2 uv = (regs_WorldPos.xz - regs_CarMapXf.xy) * regs_CarMapXf.zw;\n"
    u"  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return vec3(0.0);\n"
    u"  vec4 l = texture2D(regs_CarMap, uv);\n"
    u"  float g = regs_CarLight.x + l.a * regs_CarLight.y;\n"
    u"  float y = regs_WorldPos.y;\n"
    u"  return l.rgb * (smoothstep(g + 3.2, g + 1.0, y) * smoothstep(g - 1.5, g - 0.3, y) * regs_CarLight.z);\n"
    u"}\n"
    u"vec3 regs_lampLight() {\n"
    u"  if (regs_Night.z <= 0.0) return regs_carLight();\n"
    u"  vec2 uv = (regs_WorldPos.xz - regs_LampMapXf.xy) * regs_LampMapXf.zw;\n"
    u"  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return regs_carLight();\n"
    u"  vec4 l = texture2D(regs_LampMap, uv);\n"
    u"  float head = regs_LampHeight.x + l.a * regs_LampHeight.y;\n"
    u"  float y = regs_WorldPos.y;\n"
    u"  return l.rgb * (smoothstep(head + 0.5, head - 1.0, y) * smoothstep(head - 18.0, head - 11.0, y) * regs_Night.z) + regs_carLight();\n"
    u"}\n"
    u"#}\n";
const char16_t* kNightColorStart =
    u"  #if ($regsLamp) {\n"
    u"  vec3 regs_albedo = color.rgb * diffusemap.rgb;\n"
    u"  #}\n";
const char16_t* kNightColorEnd =
    u"  #if ($regsLamp) {\n"
    u"  color.rgb += regs_albedo * regs_lampLight() + regs_windowGlow;\n"
    u"  #}\n";

bool insertBefore(std::u16string& text, const std::u16string& anchor, const std::u16string& what) {
  size_t at = text.find(anchor);
  if (at == std::u16string::npos) return false;
  text.insert(at, what);
  return true;
}
bool insertAfterLine(std::u16string& text, const std::u16string& anchor, const std::u16string& what) {
  size_t at = text.find(anchor);
  if (at == std::u16string::npos) return false;
  size_t eol = text.find(u'\n', at);
  text.insert(eol == std::u16string::npos ? text.size() : eol + 1, what);
  return true;
}

}  // namespace

std::u16string portShaderPatch(const std::u16string& path, std::u16string text) {
  static const std::u16string kFrom = u"float y = pos.y;// - gl_MultiTexCoord1.x;";
  static const std::u16string kTo = u"float y = pos.y - gl_MultiTexCoord1.x;";
  if (path.ends_with(u"shaders/trees/alter.vertex")) {
    if (size_t at = text.find(kFrom); at != std::u16string::npos) text.replace(at, kFrom.size(), kTo);
    else LOG_WARN("shader component %s: the tree wind line was not found, trees sway in division space",
                  utf16ToUtf8(path).c_str());
  }
  if (path.ends_with(u"shaders/default/maps.fragment")) {
    std::u16string t = text;
    if (insertBefore(t, u"void maps_input() {", kNightMapsDecl) && insertBefore(t, u"  geometry_input();", kNightMapsCall))
      text = t;
    else
      LOG_WARN("shader component %s: no anchor for the lit windows", utf16ToUtf8(path).c_str());
  }
  if (path.ends_with(u"shaders/default/color.fragment")) {
    std::u16string t = text;
    size_t last = t.rfind(u'}');
    bool ok = last != std::u16string::npos;
    if (ok) t.insert(last, kNightColorEnd);
    ok = ok && insertAfterLine(t, u"void color_output() {", kNightColorStart) &&
         insertBefore(t, u"void color_input() {", kNightColorDecl);
    if (ok) text = t;
    else LOG_WARN("shader component %s: no anchor for the night lights", utf16ToUtf8(path).c_str());
  }
  return text;
}

bool expandStage(VM& vm, ShaderCombiner* c, const Value& components, uint32_t mat, uint32_t mesh, uint32_t env,
                 std::string& out) {
  std::vector<std::u16string> files;
  if (components.type() == VT::String) files.push_back(components.str());
  else if (MapObj* m = components.map())
    for (auto& [k, v] : m->entries) files.push_back(vm.toStr(v));  // key order: "!base" first, then alphabetical
  if (files.empty()) return false;
  std::set<std::u16string> once;
  Expansion e;
  e.once = &once;
  for (const auto& f : files) {
    std::u16string path = vm.resolvePath(f, c->script.get());
    std::vector<uint8_t> data;
    if (!vm.vfs().read(utf16ToUtf8(path), data)) {
      LOG_WARN("shader %s: component %s not found", utf16ToUtf8(c->path).c_str(), utf16ToUtf8(path).c_str());
      return false;
    }
    std::u16string script = u"$matFlags = " + utf8ToUtf16(std::to_string(mat)) + u";\n$meshFlags = " +
                            utf8ToUtf16(std::to_string(mesh)) + u";\n$envFlags = " + utf8ToUtf16(std::to_string(env)) +
                            u";\n$directionalCount = " + utf8ToUtf16(std::to_string(env >> 8 & 15)) +
                            u";\n$omniCount = " + utf8ToUtf16(std::to_string(env >> 12 & 15)) + u";\n$spotCount = " +
                            utf8ToUtf16(std::to_string(env >> 16 & 15)) + u";\n" +
                            templateToScript(portShaderPatch(path, decodeTextFile(data)));
    std::string u8 = utf16ToUtf8(script);
    const std::string tmp = "memory/regs/shader-template.script";
    vm.vfs().write(tmp, std::vector<uint8_t>(u8.begin(), u8.end()));
    Expansion* outer = g_expansion;
    g_expansion = &e;
    LoadOptions opt;
    opt.cls = vm.findClass("gsTemplate");
    opt.keepInScriptList = false;
    Value r = vm.load(u"/" + utf8ToUtf16(tmp), opt);  // already resolved: absolute, whatever script is running
    g_expansion = outer;
    if (!r.object() || !r.object()->code) {
      LOG_WARN("shader %s: template %s does not compile", utf16ToUtf8(c->path).c_str(), utf16ToUtf8(path).c_str());
      return false;
    }
  }
  out = utf16ToUtf8(e.text);
  return true;
}

}  // namespace

std::u16string templateToScript(const std::u16string& source) {
  // parseTemplate (L:0x0826a8f0): '#once#' lines and plain lines become output calls with $vars spliced in,
  // other '#' lines are script; leading bytes 0x01-0x20 are stripped and empty lines dropped
  auto text = [](const std::u16string& t) {
    std::u16string r;
    for (size_t i = 0; i < t.size();) {
      if (t[i] == u'$') {
        size_t j = i + 1;
        while (j < t.size() && (std::isalnum(int(t[j] < 128 ? t[j] : 0)) || t[j] == u'_')) ++j;
        r += u"\" + " + t.substr(i, j - i) + u" + \"";
        i = j;
      } else {
        r += t[i++];
      }
    }
    return r;
  };
  std::u16string out;
  size_t start = 0;
  while (start <= source.size()) {
    size_t end = source.find(u'\n', start);
    if (end == std::u16string::npos) end = source.size();
    std::u16string line = source.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && line.back() == u'\r') line.pop_back();
    size_t k = 0;
    while (k < line.size() && line[k] >= 1 && line[k] <= 0x20) ++k;
    line = line.substr(k);
    if (line.empty()) continue;
    if (line.rfind(u"#once#", 0) == 0) out += u"this.output_once(\"" + text(line.substr(6)) + u"\\n\");\n";
    else if (line[0] == u'#') out += line.substr(1) + u"\n";
    else out += u"this.output(\"" + text(line) + u"\\n\");\n";
  }
  return out;
}

ShaderCombiner* getCombiner(VM& vm, const std::u16string& rawPath) {
  std::u16string path = rawPath.empty() ? u"shaders/default/default.shader" : rawPath;
  while (!path.empty() && path[0] == u'/') path.erase(0, 1);
  auto& c = cache();
  if (auto it = c.find(path); it != c.end()) return it->second->script ? it->second.get() : nullptr;
  auto& slot = c[path];
  slot = std::make_unique<ShaderCombiner>();
  ShaderCombiner* sc = slot.get();
  sc->path = path;
  // the script object exists before its code runs, so setBaseShader can find its combiner
  Ref<Object> obj = vm.newObject(vm.findClass("gsShaderCombiner_OGL"), nullptr);
  byScript()[obj.get()] = sc;
  LoadOptions opt;
  opt.into = obj;
  Value r = vm.load(u"/" + path, opt);  // already resolved: absolute, even when setBaseShader runs inside a script
  if (!r.object() || !obj->code) {
    LOG_WARN("shader %s: cannot load", utf16ToUtf8(path).c_str());
    byScript().erase(obj.get());
    return nullptr;
  }
  sc->script = obj;
  sc->matMask = uint32_t(intField(vm, obj.get(), u"matMask"));
  sc->meshMask = uint32_t(intField(vm, obj.get(), u"meshMask"));
  sc->envMask = uint32_t(intField(vm, obj.get(), u"envMask"));
  return sc;
}

ShaderVariant* combinerVariant(VM& vm, ShaderCombiner* c, uint32_t mat, uint32_t mesh, uint32_t env) {
  // getVariant (0x1003ca40, interfaces-3d.md §3.5): shadow passes reduce the material and environment keys
  uint32_t mk = mat & c->matMask, ek = env & c->envMask;
  if (env & 4) {  // shadow-map render
    mk &= 0x4000 | 0x1 | 0x2;  // DIFFUSEMAP | ALPHABLEND | ALPHATEST
    ek &= 4;
  } else if (env & 8) {  // screen-space shadow render
    mk &= 0x4000;
    ek &= 2 | 8;
  }
  auto key = std::make_tuple(mk, mesh & c->meshMask, ek);
  auto it = c->variants.find(key);
  if (it != c->variants.end()) return &it->second;
  ShaderVariant& v = c->variants[key];
  auto [m, me, e] = key;
  runProgram(vm, c, m, me, e);
  Object* s = c->script.get();
  v.attributes = uint32_t(intField(vm, s, u"attributes"));
  v.textures = uint32_t(intField(vm, s, u"textures"));
  v.parameters = uint32_t(intField(vm, s, u"parameters"));
  bool ok = expandStage(vm, c, field(vm, s, u"vertexComponent"), m, me, e, v.vertex) &&
            expandStage(vm, c, field(vm, s, u"fragmentComponent"), m, me, e, v.fragment);
  v.failed = !ok;
  return &v;
}

void registerShaderNatives(VM& vm) {
  // setBaseShader(path): the base combiner, loaded through the same cache; its masks are copied (0x1003bf40)
  implement(vm, "gsShaderCombiner_OGL", u"setBaseShader", [](VM& v, Object* s, int, const Value* a) {
    auto it = byScript().find(s);
    if (it == byScript().end()) return Value();
    ShaderCombiner* base = getCombiner(v, v.resolvePath(v.toStr(a[0]), s));
    if (!base) return Value();
    it->second->base = base;
    s->fieldValues[u"matMask"] = Value::integer(int32_t(base->matMask));
    s->fieldValues[u"meshMask"] = Value::integer(int32_t(base->meshMask));
    s->fieldValues[u"envMask"] = Value::integer(int32_t(base->envMask));
    return Value();
  });
  implement(vm, "gsTemplate", u"output", [](VM& v, Object*, int, const Value* a) {
    if (g_expansion) g_expansion->text += v.toStr(a[0]);
    return Value();
  });
  implement(vm, "gsTemplate", u"output_once", [](VM& v, Object*, int, const Value* a) {
    if (g_expansion) {
      std::u16string s = v.toStr(a[0]);
      if (g_expansion->once->insert(s).second) g_expansion->text += s;
    }
    return Value();
  });
}

}  // namespace regs::engine
