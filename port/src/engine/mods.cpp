// OpenCiM mods: the first step of port/docs/mod-manager.md §8 (scan, manifests, enabled list, VFS layers, chained
// script extensions). No UI yet: the enabled list is the user mods folder's mods.json.
#include "engine/mods.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/host.h"
#include "engine/user_folders.h"
#include "render/glsl.h"
#include "render/renderer3d.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
namespace fs = std::filesystem;

namespace {

std::vector<ModInfo>& modList() {
  static std::vector<ModInfo> m;
  return m;
}

std::string u8(const fs::path& p) { return utf16ToUtf8(std::u16string(p.u16string())); }

// ---- a small JSON reader (objects, arrays, strings, numbers, true/false/null; // and /* */ comments allowed, as
// the manifest example in mod-manager.md §2 has them) ----
struct Json {
  enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;
  const Json* get(const std::string& k) const {
    for (auto& [key, v] : o)
      if (key == k) return &v;
    return nullptr;
  }
  std::string str(const std::string& k, const std::string& def = "") const {
    const Json* v = get(k);
    return v && v->t == T::Str ? v->s : def;
  }
};

class JsonReader {
 public:
  explicit JsonReader(const std::string& text) : s_(text) {}
  bool parse(Json& out, std::string& err) {
    try {
      out = value();
      skip();
      if (i_ != s_.size()) fail("text after the value");
      return true;
    } catch (const std::string& e) {
      err = e;
      return false;
    }
  }

 private:
  [[noreturn]] void fail(const char* what) {
    int line = 1;
    for (size_t k = 0; k < i_ && k < s_.size(); ++k) line += s_[k] == '\n';
    throw std::string(what) + " at line " + std::to_string(line);
  }
  void skip() {
    for (;;) {
      while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
      if (s_.compare(i_, 2, "//") == 0) {
        while (i_ < s_.size() && s_[i_] != '\n') ++i_;
      } else if (s_.compare(i_, 2, "/*") == 0) {
        size_t e = s_.find("*/", i_ + 2);
        i_ = e == std::string::npos ? s_.size() : e + 2;
      } else {
        return;
      }
    }
  }
  std::string string() {
    ++i_;  // the opening quote
    std::string r;
    while (i_ < s_.size() && s_[i_] != '"') {
      char c = s_[i_++];
      if (c != '\\') {
        r += c;
        continue;
      }
      if (i_ >= s_.size()) break;
      char e = s_[i_++];
      switch (e) {
        case 'n': r += '\n'; break;
        case 't': r += '\t'; break;
        case 'r': r += '\r'; break;
        case 'b': r += '\b'; break;
        case 'f': r += '\f'; break;
        case 'u': {
          if (i_ + 4 > s_.size()) fail("bad \\u escape");
          char16_t cu = char16_t(std::stoi(s_.substr(i_, 4), nullptr, 16));
          i_ += 4;
          r += utf16ToUtf8(std::u16string(1, cu));
          break;
        }
        default: r += e;
      }
    }
    if (i_ >= s_.size()) fail("unterminated string");
    ++i_;
    return r;
  }
  Json value() {
    skip();
    if (i_ >= s_.size()) fail("unexpected end");
    Json j;
    char c = s_[i_];
    if (c == '{') {
      j.t = Json::T::Obj;
      ++i_;
      skip();
      if (i_ < s_.size() && s_[i_] == '}') return ++i_, j;
      for (;;) {
        skip();
        if (i_ >= s_.size() || s_[i_] != '"') fail("expected a key");
        std::string k = string();
        skip();
        if (i_ >= s_.size() || s_[i_] != ':') fail("expected ':'");
        ++i_;
        j.o.emplace_back(std::move(k), value());
        skip();
        if (i_ < s_.size() && s_[i_] == ',') { ++i_; skip(); if (i_ < s_.size() && s_[i_] == '}') return ++i_, j; continue; }
        if (i_ < s_.size() && s_[i_] == '}') return ++i_, j;
        fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      j.t = Json::T::Arr;
      ++i_;
      skip();
      if (i_ < s_.size() && s_[i_] == ']') return ++i_, j;
      for (;;) {
        j.a.push_back(value());
        skip();
        if (i_ < s_.size() && s_[i_] == ',') { ++i_; skip(); if (i_ < s_.size() && s_[i_] == ']') return ++i_, j; continue; }
        if (i_ < s_.size() && s_[i_] == ']') return ++i_, j;
        fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      j.t = Json::T::Str;
      j.s = string();
      return j;
    }
    auto word = [&](const char* w) { size_t n = std::char_traits<char>::length(w); if (s_.compare(i_, n, w) == 0) { i_ += n; return true; } return false; };
    if (word("true")) { j.t = Json::T::Bool; j.b = true; return j; }
    if (word("false")) { j.t = Json::T::Bool; return j; }
    if (word("null")) return j;
    size_t start = i_;
    while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || std::strchr("+-.eE", s_[i_]))) ++i_;
    if (start == i_) fail("unexpected character");
    j.t = Json::T::Num;
    j.n = std::atof(s_.substr(start, i_ - start).c_str());
    return j;
  }
  const std::string& s_;
  size_t i_ = 0;
};

bool readJson(const fs::path& file, Json& out, std::string& err) {
  FILE* f = _wfopen(file.wstring().c_str(), L"rb");
  if (!f) {
    err = "cannot open";
    return false;
  }
  std::string text;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
  std::fclose(f);
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // UTF-8 BOM
  return JsonReader(text).parse(out, err);
}

// a mod id: reverse-DNS style, letters, digits, '.', '-', '_' (it becomes a VFS folder name)
bool validId(const std::string& id) {
  if (id.empty() || id.size() > 100) return false;
  for (char c : id)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_')) return false;
  return id.find("..") == std::string::npos;
}

// every mod folder (with a mod.json) directly inside dir
void scanFolder(const fs::path& dir, bool builtIn, std::map<std::string, ModInfo>& found) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> folders;
  for (auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory(ec)) folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  for (const fs::path& f : folders) {
    fs::path manifest = f / "mod.json";
    if (!fs::exists(manifest, ec)) continue;  // not a mod (zip packages: later)
    Json j;
    std::string err;
    if (!readJson(manifest, j, err) || j.t != Json::T::Obj) {
      LOG_WARN("mods: %s: mod.json unreadable (%s), skipped", u8(f).c_str(), err.empty() ? "not an object" : err.c_str());
      continue;
    }
    ModInfo m;
    m.id = j.str("id");
    if (!validId(m.id)) {
      LOG_WARN("mods: %s: missing or invalid \"id\" in mod.json, skipped", u8(f).c_str());
      continue;
    }
    m.name = j.str("name", m.id);
    m.version = j.str("version", "0");
    m.saves = j.str("saves", "none");
    m.dir = f;
    m.builtIn = builtIn;
    auto it = found.find(m.id);
    if (it != found.end()) {
      // a user copy of a built-in mod replaces it; two user folders with one id: the first (by name) wins
      if (it->second.builtIn && !builtIn) {
        LOG_INFO("mods: %s in %s replaces the built-in copy", m.id.c_str(), u8(f).c_str());
        it->second = m;
      } else {
        LOG_WARN("mods: %s: id %s already found in %s, skipped", u8(f).c_str(), m.id.c_str(), u8(it->second.dir).c_str());
      }
      continue;
    }
    found.emplace(m.id, m);
  }
}

bool validIdentifier(const std::string& s) {
  if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
  for (char c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  return s.rfind("gl_", 0) != 0 && s.rfind("regs_", 0) != 0;
}

// "shaderHooks": [{"file": "<path in the mod>", "function": "<name>"}] (render/glsl.h, mod-manager.md §4.3)
void loadShaderHooks(const ModInfo& m) {
  Json j;
  std::string err;
  if (!readJson(m.dir / "mod.json", j, err)) return;
  const Json* hooks = j.get("shaderHooks");
  if (!hooks || hooks->t != Json::T::Arr) return;
  for (const Json& h : hooks->a) {
    std::string file = h.str("file"), function = h.str("function");
    if (!validIdentifier(function) || file.empty() || file.find("..") != std::string::npos) {
      LOG_WARN("mods: %s: shader hook needs a \"file\" in the mod and a GLSL \"function\" name, skipped", m.id.c_str());
      continue;
    }
    auto w = utf8ToUtf16(file);
    FILE* f = _wfopen((m.dir / fs::path(std::wstring(w.begin(), w.end()))).wstring().c_str(), L"rb");
    if (!f) {
      LOG_WARN("mods: %s: shader hook %s not found, skipped", m.id.c_str(), file.c_str());
      continue;
    }
    std::string code;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) code.append(buf, n);
    std::fclose(f);
    if (code.find("gl_") != std::string::npos)
      LOG_WARN("mods: %s: shader hook %s uses a gl_ built-in; hooks are GLSL 3.30 core (the programs would not build)",
               m.id.c_str(), file.c_str());
    render::ShaderHook hook{function, code, m.id + "/" + file};
    if (const Json* b = h.get("shadowPass"); b && b->t == Json::T::Bool) hook.shadow = b->b;
    if (const Json* b = h.get("post"); b && b->t == Json::T::Bool) hook.post = b->b;
    render::addShaderHook(hook);
    LOG_INFO("mods: %s: %s hook %s (%s)%s", m.id.c_str(), hook.post ? "post" : "shader", function.c_str(), file.c_str(),
             hook.shadow ? ", also in shadow passes" : "");
  }
}

}  // namespace

fs::path modsUserRoot() {
  if (!host().userFolder.empty()) return host().userFolder;
  return host().documents / kPortFolderName;
}

fs::path userModsFolder() { return host().modsDir.empty() ? modsUserRoot() / "mods" : host().modsDir; }

const std::vector<ModInfo>& mods() { return modList(); }

void loadMods(Vfs& vfs, VM& vm) {
  auto& list = modList();
  list.clear();
  if (host().noMods) {
    LOG_INFO("mods: off (--no-mods)");
    return;
  }
  std::map<std::string, ModInfo> found;
  if (!host().dataDir.empty()) scanFolder(host().dataDir / "mods", true, found);
  fs::path userDir = userModsFolder();
  scanFolder(userDir, false, found);
  // the enabled list in load order: mods.json {"enabled": ["id", ...]} in the user mods folder
  std::vector<std::string> enabled;
  fs::path listFile = userDir / "mods.json";
  std::error_code ec;
  if (fs::exists(listFile, ec)) {
    Json j;
    std::string err;
    if (!readJson(listFile, j, err)) {
      LOG_WARN("mods: %s unreadable (%s): no mods enabled", u8(listFile).c_str(), err.c_str());
    } else if (const Json* e = j.get("enabled"); e && e->t == Json::T::Arr) {
      for (const Json& x : e->a)
        if (x.t == Json::T::Str && std::find(enabled.begin(), enabled.end(), x.s) == enabled.end()) enabled.push_back(x.s);
    }
  }
  for (auto& [id, m] : found)
    LOG_INFO("mods: found %s %s (%s) in %s", id.c_str(), m.version.c_str(), m.builtIn ? "built-in" : "user", u8(m.dir).c_str());
  for (const std::string& id : enabled) {
    auto it = found.find(id);
    if (it == found.end()) {
      LOG_WARN("mods: %s is enabled in mods.json but not installed, skipped", id.c_str());
      continue;
    }
    ModInfo m = it->second;
    m.enabled = true;
    loadShaderHooks(m);
    // layers: the whole folder under regs/mods/<id>/ (its scripts, sounds, textures, extend/), and files/ over the
    // game and the port's data (later mods win)
    vfs.addModLayer(m.dir, m.vfsRoot(), m.id);
    vm.extensionRoots.push_back(m.vfsRoot() + "extend/");
    LOG_INFO("mods: enabled %s %s", m.id.c_str(), m.version.c_str());
    list.push_back(std::move(m));
  }
  LOG_INFO("mods: %zu found, %zu enabled (list %s)", found.size(), list.size(), u8(listFile).c_str());
}

void registerModNatives(VM& vm) {
  implementGlobal(vm, u"regsMods", [](VM&, Object*, int, const Value*) {
    auto arr = make<ArrayObj>();
    for (const ModInfo& m : modList()) {
      auto map = make<MapObj>();
      map->at(Value::string(u"id")) = Value::string(utf8ToUtf16(m.id));
      map->at(Value::string(u"name")) = Value::string(utf8ToUtf16(m.name));
      map->at(Value::string(u"version")) = Value::string(utf8ToUtf16(m.version));
      map->at(Value::string(u"path")) = Value::string(utf8ToUtf16("/" + m.vfsRoot()));
      map->at(Value::string(u"builtIn")) = Value::boolean(m.builtIn);
      arr->items.push_back(Value::heap(VT::Map, map.get()));
    }
    return Value::heap(VT::Array, arr.get());
  });
  // regsShaderUniform(name, value): a vec4 uniform of the shader hooks (float → (v, 0, 0, 0), vector → (x, y, z, 0),
  // quat → (x, y, z, w)), set in every hooked program from the next frame on
  implementGlobal(vm, u"regsShaderUniform", [](VM& v, Object*, int argc, const Value* a) {
    if (argc < 2) return Value();
    std::string name = utf16ToUtf8(v.toStr(a[0]));
    Value x = v.resolve(a[1]);
    std::array<float, 4> f{0, 0, 0, 0};
    if (x.type() == VT::Vector || x.type() == VT::Quat) {
      for (int k = 0; k < (x.type() == VT::Quat ? 4 : 3); ++k) f[size_t(k)] = x.vec()[k];
    } else {
      f[0] = v.toFloat(x);
    }
    render::shaderHookUniforms()[name] = f;
    return Value();
  });
  // regsShaderTexture(sampler, path): a texture of the mod's own for its shader hooks, read as `uniform sampler2D
  // <sampler>;` (declared in the hook file). It gets one of the texture units kept for mods (render/renderer3d.h:
  // kModUnitFirst, kModUnitCount: all fits the 16 units every GL 3.3 GPU has). Returns true, or false when they are
  // taken (the hook then reads the sampler as unbound: black) and logs which mods hold them. The same sampler again
  // replaces its texture.
  implementGlobal(vm, u"regsShaderTexture", [](VM& v, Object*, int argc, const Value* a) {
    if (argc < 2) return Value::boolean(false);
    std::string sampler = utf16ToUtf8(v.toStr(a[0])), path = utf16ToUtf8(v.toStr(a[1]));
    auto& list = render::shaderHookTextures();
    for (auto& t : list)
      if (t.sampler == sampler) {
        t.path = path, t.texture = 0, t.tried = false;
        return Value::boolean(true);
      }
    if (int(list.size()) >= render::kModUnitCount) {
      std::string held;
      for (const auto& t : list) held += " " + t.sampler;
      LOG_WARN("mods: no texture unit left for %s (%s); all %d taken by:%s", sampler.c_str(), path.c_str(),
               render::kModUnitCount, held.c_str());
      return Value::boolean(false);
    }
    render::ShaderHookTexture t;
    t.sampler = sampler, t.path = path, t.unit = render::kModUnitFirst + int(list.size());
    list.push_back(t);
    LOG_INFO("mods: texture %s for %s on unit %d", path.c_str(), sampler.c_str(), t.unit);
    return Value::boolean(true);
  });
}

}  // namespace regs::engine
