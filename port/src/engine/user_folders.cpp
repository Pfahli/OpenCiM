// The port's user folder and the read-only import of the original game's saves and maps (engine/user_folders.h,
// port/README "Where data lives").
//
// Import and clash rule (regsImportOriginal):
// 1. A copy already in OpenCiM is used as it is: an OpenCiM file with the same content (same size and FNV-1a hash;
//    the same name is checked first), or an unchanged copy an earlier import made under another name (the record
//    regs-imports.txt keeps the original's and the copy's size and hash). Such originals are not listed at all.
// 2. Otherwise the original is copied under its own name, or, if that name is taken in OpenCiM by different content,
//    as "<name> (CiM)", "<name> (CiM 2)", ... (first free name). Nothing in OpenCiM is ever overwritten.
//    A save's level.scenario names its own grid ("$levelName = "userdata/saves/<name>/map.grid"", which
//    scenario.script's loadScenario follows), so a renamed save gets those paths rewritten to the new name; the
//    package is rebuilt with buildPackage. A map's files carry no name and are copied byte for byte.
// The original folder is opened for reading only.
#include "engine/user_folders.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <map>
#include <tuple>
#include <vector>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/host.h"
#include "vfs/archive.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
  auto w = p.wstring();
  return utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t*>(w.data()), w.size()));
}
fs::path pathOf(const std::string& s) {
  auto w = utf8ToUtf16(s);
  return fs::path(std::wstring(w.begin(), w.end()));
}
Value deref(const Value& v) { return v.ref() ? v.ref()->get() : v; }

std::wstring folded(const fs::path& p) {
  std::error_code ec;
  std::wstring s = fs::absolute(p, ec).lexically_normal().wstring();
  for (auto& c : s) c = wchar_t(std::towlower(c));
  while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
  return s;
}

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
  FILE* f = _wfopen(p.wstring().c_str(), L"rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  auto len = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  out.resize(size_t(len));
  bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// only ever called with paths below host().userFolder (checked by the callers)
bool writeFile(const fs::path& p, const std::vector<uint8_t>& data) {
  FILE* f = _wfopen(p.wstring().c_str(), L"wb");
  if (!f) return false;
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  return std::fclose(f) == 0 && ok;
}

bool insideUserFolder(const fs::path& p) {
  std::wstring root = folded(host().userFolder), x = folded(p);
  return !host().userFolder.empty() && x.size() > root.size() + 1 && x.compare(0, root.size(), root) == 0 &&
         x[root.size()] == L'\\';
}

// ---- content identity: size and FNV-1a 64 ----

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull, kFnvPrime = 0x100000001b3ull;
void fnv(uint64_t& h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * kFnvPrime;
}

struct Id {
  bool ok = false;
  uint64_t size = 0, hash = 0;
  bool operator==(const Id& o) const { return ok && o.ok && size == o.size && hash == o.hash; }
};

// file hashes, cached by path, size and write time (the lists hash the original saves each time they open)
std::map<std::wstring, std::tuple<uint64_t, fs::file_time_type, uint64_t>>& hashCache() {
  static std::map<std::wstring, std::tuple<uint64_t, fs::file_time_type, uint64_t>> c;
  return c;
}

Id fileId(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return {};
  uint64_t size = fs::file_size(p, ec);
  auto time = fs::last_write_time(p, ec);
  auto& cache = hashCache();
  auto key = folded(p);
  if (auto it = cache.find(key); it != cache.end() && std::get<0>(it->second) == size && std::get<1>(it->second) == time)
    return {true, size, std::get<2>(it->second)};
  std::vector<uint8_t> data;
  if (!readFile(p, data)) return {};
  uint64_t h = kFnvBasis;
  fnv(h, data.data(), data.size());
  cache[key] = {uint64_t(data.size()), time, h};
  return {true, uint64_t(data.size()), h};
}

// a user map is up to four loose files of one name (main.script saveMapData)
const wchar_t* const kMapExts[] = {L".grid", L".timeline", L".dds", L".scenario"};

enum class Kind { Save, Map };

fs::path saveFile(const fs::path& folder, const std::string& name) { return folder / L"saves" / pathOf(name + ".gs"); }
fs::path mapFile(const fs::path& folder, const std::string& name, const wchar_t* ext) {
  return folder / L"maps" / fs::path(pathOf(name).wstring() + ext);
}

// a save: the package file; a map: its files in kMapExts order, each as (extension, size, hash); needs the .grid
Id contentId(Kind k, const fs::path& folder, const std::string& name) {
  if (k == Kind::Save) return fileId(saveFile(folder, name));
  Id out;
  uint64_t h = kFnvBasis;
  for (const wchar_t* ext : kMapExts) {
    Id f = fileId(mapFile(folder, name, ext));
    if (!f.ok) {
      if (ext == kMapExts[0]) return {};
      continue;
    }
    fnv(h, reinterpret_cast<const uint8_t*>(ext), std::wcslen(ext) * sizeof(wchar_t));
    fnv(h, reinterpret_cast<const uint8_t*>(&f.size), sizeof f.size);
    fnv(h, reinterpret_cast<const uint8_t*>(&f.hash), sizeof f.hash);
    out.size += f.size;
  }
  out.ok = true;
  out.hash = h;
  return out;
}

bool nameTaken(Kind k, const fs::path& folder, const std::string& name) {
  std::error_code ec;
  if (k == Kind::Save)  // a loose folder of that name would mount at the same place as the package
    return fs::exists(saveFile(folder, name), ec) || fs::exists(folder / L"saves" / pathOf(name), ec);
  for (const wchar_t* ext : kMapExts)
    if (fs::exists(mapFile(folder, name, ext), ec)) return true;
  return false;
}

// names (without extension) of the saves (*.gs) or maps (*.grid) in a user folder, sorted
std::vector<std::string> names(Kind k, const fs::path& folder) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(folder / (k == Kind::Save ? L"saves" : L"maps"), ec)) {
    if (!e.is_regular_file(ec) || e.path().extension().wstring() != (k == Kind::Save ? L".gs" : L".grid")) continue;
    out.push_back(u8(e.path().stem()));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ---- the import record (regs-imports.txt): kind, original name, size, hash, copy name, size, hash; tab separated ----

struct Import {
  std::string kind, from, to;
  Id fromId, toId;
};

fs::path recordFile() { return host().userFolder / kImportRecordName; }

std::vector<Import> readImports() {
  std::vector<Import> out;
  std::vector<uint8_t> data;
  if (!readFile(recordFile(), data)) return out;
  std::string text(data.begin(), data.end());
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? text.size() : end + 1;
    std::vector<std::string> f;
    for (size_t a = 0;;) {
      size_t b = line.find('\t', a);
      f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    if (f.size() != 7) continue;
    Import r;
    r.kind = f[0];
    r.from = f[1];
    r.fromId = {true, std::strtoull(f[2].c_str(), nullptr, 10), std::strtoull(f[3].c_str(), nullptr, 16)};
    r.to = f[4];
    r.toId = {true, std::strtoull(f[5].c_str(), nullptr, 10), std::strtoull(f[6].c_str(), nullptr, 16)};
    out.push_back(std::move(r));
  }
  return out;
}

void appendImport(const Import& r) {
  FILE* f = _wfopen(recordFile().wstring().c_str(), L"ab");
  if (!f) return;
  std::fprintf(f, "%s\t%s\t%llu\t%016llx\t%s\t%llu\t%016llx\n", r.kind.c_str(), r.from.c_str(),
               (unsigned long long)r.fromId.size, (unsigned long long)r.fromId.hash, r.to.c_str(),
               (unsigned long long)r.toId.size, (unsigned long long)r.toId.hash);
  std::fclose(f);
}

const char* kindName(Kind k) { return k == Kind::Save ? "save" : "map"; }

// the OpenCiM name of a copy of the original `name` that is already there (rule 1), or ""
std::string existingCopy(Kind k, const std::string& name) {
  const fs::path& user = host().userFolder;
  Id o = contentId(k, host().originalFolder, name);
  if (!o.ok) return "";
  if (contentId(k, user, name) == o) return name;
  std::error_code ec;
  fs::path origMain = k == Kind::Save ? saveFile(host().originalFolder, name) : mapFile(host().originalFolder, name, L".grid");
  uint64_t mainSize = fs::file_size(origMain, ec);
  for (auto& n : names(k, user)) {
    fs::path p = k == Kind::Save ? saveFile(user, n) : mapFile(user, n, L".grid");
    if (n != name && fs::file_size(p, ec) == mainSize && contentId(k, user, n) == o) return n;
  }
  for (auto& r : readImports())
    if (r.kind == kindName(k) && r.from == name && r.fromId == o && contentId(k, user, r.to) == r.toId) return r.to;
  return "";
}

// Windows-1252 bytes of a name (the scripts' 8-bit text files); false if a character has none
bool toCp1252(const std::string& utf8, std::string& out) {
  out.clear();
  for (char16_t c : utf8ToUtf16(utf8)) {
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) {
      out.push_back(char(c));
      continue;
    }
    bool found = false;
    for (int b = 0x80; b < 0xA0 && !found; ++b)
      if (cp1252ToUtf16(uint8_t(b)) == c) {
        out.push_back(char(b));
        found = true;
      }
    if (!found) return false;
  }
  return true;
}

// the package of a save copied under another name: "userdata/saves/<from>/" becomes "userdata/saves/<to>/" in its
// text files (level.scenario's $levelName and the other scenario parts)
bool renamedSave(const fs::path& file, const std::string& from, const std::string& to, std::vector<uint8_t>& out,
                 int& replaced) {
  Archive a;
  std::string err;
  if (!a.open(file, &err)) {
    LOG_ERROR("import: cannot open %s: %s", u8(file).c_str(), err.c_str());
    return false;
  }
  std::string needle, repl;
  bool rewrite = toCp1252("userdata/saves/" + from + "/", needle) && toCp1252("userdata/saves/" + to + "/", repl);
  if (!rewrite) LOG_WARN("import: save name '%s' has no 8-bit form; its paths are not rewritten", from.c_str());
  std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
  replaced = 0;
  for (auto& e : a.entries) {
    std::vector<uint8_t> data;
    if (!a.read(e, data, &err)) {
      LOG_ERROR("import: cannot read %s in %s: %s", e.name.c_str(), u8(file).c_str(), err.c_str());
      return false;
    }
    auto dot = e.name.rfind('.');
    std::string ext = dot == std::string::npos ? "" : e.name.substr(dot);
    if (rewrite && (ext == ".scenario" || ext == ".campaign" || ext == ".script")) {
      std::string s(data.begin(), data.end());
      for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + repl.size())) {
        s.replace(at, needle.size(), repl);
        ++replaced;
      }
      data.assign(s.begin(), s.end());
    }
    files.emplace_back(e.name, std::move(data));
  }
  out = buildPackage(files);
  return true;
}

bool validName(const std::string& n) {
  return !n.empty() && n.find_first_of("/\\:*?\"<>|") == std::string::npos && n != "." && n != "..";
}

std::string importOriginal(Vfs& vfs, Kind k, const std::string& name) {
  const fs::path &user = host().userFolder, &orig = host().originalFolder;
  if (user.empty() || orig.empty() || !validName(name)) {
    LOG_ERROR("import: no original %s '%s'", kindName(k), name.c_str());
    return "";
  }
  Id o = contentId(k, orig, name);
  if (!o.ok) {
    LOG_ERROR("import: original %s '%s' not found in %s", kindName(k), name.c_str(), u8(orig).c_str());
    return "";
  }
  std::string target;
  if (std::string t = existingCopy(k, name); !t.empty()) {
    LOG_INFO("import: %s '%s' (size %llu, hash %016llx) is already in OpenCiM as '%s'; loading that", kindName(k),
             name.c_str(), (unsigned long long)o.size, (unsigned long long)o.hash, t.c_str());
    return k == Kind::Map ? t + ".grid" : t;
  }
  for (int n = 1; n < 1000; ++n) {
    std::string cand = n == 1 ? name : n == 2 ? name + " (CiM)" : name + " (CiM " + std::to_string(n - 1) + ")";
    if (!nameTaken(k, user, cand)) {
      target = cand;
      break;
    }
  }
  if (target.empty()) return "";
  std::error_code ec;
  fs::path dir = user / (k == Kind::Save ? L"saves" : L"maps");
  fs::create_directories(dir, ec);
  if (!insideUserFolder(dir / L"x")) {
    LOG_ERROR("import: refused to write outside the OpenCiM folder: %s", u8(dir).c_str());
    return "";
  }
  if (k == Kind::Save) {
    std::vector<uint8_t> data;
    int replaced = 0;
    bool ok = target == name ? readFile(saveFile(orig, name), data) : renamedSave(saveFile(orig, name), name, target, data, replaced);
    if (!ok || !writeFile(saveFile(user, target), data)) {
      LOG_ERROR("import: cannot copy save '%s' to '%s'", name.c_str(), u8(saveFile(user, target)).c_str());
      return "";
    }
    if (target != name) LOG_INFO("import: save renamed to '%s', %d path(s) rewritten", target.c_str(), replaced);
  } else {
    for (const wchar_t* ext : kMapExts) {
      std::vector<uint8_t> data;
      if (!readFile(mapFile(orig, name, ext), data)) continue;
      if (!writeFile(mapFile(user, target, ext), data)) {
        LOG_ERROR("import: cannot copy map file to '%s'", u8(mapFile(user, target, ext)).c_str());
        return "";
      }
    }
  }
  Import r{kindName(k), name, target, o, contentId(k, user, target)};
  appendImport(r);
  LOG_INFO("import: copied %s '%s' (size %llu, hash %016llx) from %s to OpenCiM as '%s' (size %llu, hash %016llx)",
           kindName(k), name.c_str(), (unsigned long long)o.size, (unsigned long long)o.hash, u8(orig).c_str(),
           target.c_str(), (unsigned long long)r.toId.size, (unsigned long long)r.toId.hash);
  vfs.rescan();  // the new package mounts as userdata/saves/<target>/, the map files appear in userdata/maps/
  return k == Kind::Map ? target + ".grid" : target;
}

}  // namespace

fs::path redirectUserdata(Vfs& vfs, const fs::path& requested) {
  fs::path original = host().documents / kOriginalFolderName;
  if (host().documents.empty() || folded(requested) != folded(original)) {
    LOG_INFO("userdata: %s (as the scripts asked)", u8(requested).c_str());
    return requested;
  }
  host().userFolder = host().documents / kPortFolderName;
  host().originalFolder = original;
  LOG_INFO("userdata: %s (OpenCiM's folder; the scripts asked for the original's)", u8(host().userFolder).c_str());
  LOG_INFO("originals: %s (read only: saves and maps are listed and copied in when opened)", u8(original).c_str());
  vfs.setOriginalRoot(original, kOriginalVfsPrefix);
  return host().userFolder;
}

void registerUserFolderNatives(VM& vm) {
  auto stringArray = [](const std::vector<std::string>& v) {
    auto a = make<ArrayObj>();
    for (auto& s : v) a->items.push_back(Value::string(utf8ToUtf16(s)));
    return Value::heap(VT::Array, a.get());
  };
  // the map manager (extend/metro/ui/menus/mainmenu.script, regs/ui/mapbrowser.script): rename or move a file or
  // folder, and delete one (a folder with everything in it), only below userdata/maps; the file system is read again
  // afterwards. Both return true when the disk changed.
  auto mapsPath = [](VM& v, const Value& val, std::filesystem::path& out) {
    std::string p = utf16ToUtf8(v.toStr(val));
    if (p.rfind("userdata/maps/", 0) != 0 || p.find("..") != std::string::npos || p.size() <= 14) return false;
    return v.vfs().writableDiskPath(p, out);
  };
  implementGlobal(vm, u"regsMapsRename", [mapsPath](VM& v, Object*, int, const Value* a) {
    std::filesystem::path from, to;
    std::error_code ec;
    if (!mapsPath(v, a[0], from) || !mapsPath(v, a[1], to) || !std::filesystem::exists(from, ec) || std::filesystem::exists(to, ec))
      return Value::boolean(false);
    std::filesystem::rename(from, to, ec);
    v.vfs().rescan();
    return Value::boolean(!ec);
  }, 2, 2);
  implementGlobal(vm, u"regsMapsRemove", [mapsPath](VM& v, Object*, int, const Value* a) {
    std::filesystem::path p;
    std::error_code ec;
    if (!mapsPath(v, a[0], p) || !std::filesystem::exists(p, ec)) return Value::boolean(false);
    std::filesystem::remove_all(p, ec);
    v.vfs().rescan();
    return Value::boolean(!ec);
  }, 1, 1);
  implementGlobal(vm, u"regsOriginalSaves", [stringArray](VM& v, Object*, int, const Value*) {
    std::vector<std::string> out;
    if (!host().originalFolder.empty())
      for (auto& n : names(Kind::Save, host().originalFolder))
        if (v.vfs().exists(kOriginalVfsPrefix + std::string("saves/") + n + "/game.campaign") && existingCopy(Kind::Save, n).empty())
          out.push_back(n);
    return stringArray(out);
  }, 0, 0);
  implementGlobal(vm, u"regsOriginalMaps", [stringArray](VM&, Object*, int, const Value*) {
    std::vector<std::string> out;
    if (!host().originalFolder.empty())
      for (auto& n : names(Kind::Map, host().originalFolder))
        if (existingCopy(Kind::Map, n).empty()) out.push_back(n + ".grid");
    return stringArray(out);
  }, 0, 0);
  implementGlobal(vm, u"regsImportOriginal", [](VM& v, Object*, int, const Value* a) {
    std::string kind = utf16ToUtf8(deref(a[0]).toString()), name = utf16ToUtf8(deref(a[1]).toString());
    if (kind == "map" && name.size() > 5 && name.compare(name.size() - 5, 5, ".grid") == 0) name.resize(name.size() - 5);
    return Value::string(utf8ToUtf16(importOriginal(v.vfs(), kind == "map" ? Kind::Map : Kind::Save, name)));
  }, 2, 2);
}

}  // namespace regs::engine
