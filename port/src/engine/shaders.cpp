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
std::u16string portShaderPatch(const std::u16string& path, std::u16string text) {
  static const std::u16string kFrom = u"float y = pos.y;// - gl_MultiTexCoord1.x;";
  static const std::u16string kTo = u"float y = pos.y - gl_MultiTexCoord1.x;";
  if (path.ends_with(u"shaders/trees/alter.vertex")) {
    if (size_t at = text.find(kFrom); at != std::u16string::npos) text.replace(at, kFrom.size(), kTo);
    else LOG_WARN("shader component %s: the tree wind line was not found, trees sway in division space",
                  utf16ToUtf8(path).c_str());
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
