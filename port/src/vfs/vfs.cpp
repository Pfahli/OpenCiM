#include "vfs/vfs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cwctype>
#include <functional>

#include "core/log.h"
#include "core/text.h"

namespace regs {

namespace fs = std::filesystem;

namespace {

std::string pathToUtf8(const fs::path& p) {
  auto w = p.wstring();
  return utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t*>(w.data()), w.size()));
}

// NTFS returns directory entries in upper-cased name order; the engine's scan (FindFirstFileW) inherits it, and the
// order decides which archive is mounted first.
std::vector<fs::directory_entry> sortedEntries(const fs::path& dir) {
  std::vector<fs::directory_entry> v;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) v.push_back(e);
  std::sort(v.begin(), v.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
    auto x = a.path().filename().wstring(), y = b.path().filename().wstring();
    return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(),
                                        [](wchar_t c, wchar_t d) { return std::towupper(c) < std::towupper(d); });
  });
  return v;
}

bool hasGsExtension(const fs::path& p) {
  auto e = p.extension().wstring();
  return e == L".gs";  // the engine compares against L".gs" exactly
}

}  // namespace

const Vfs::Record* Vfs::find(const std::string& path) const {
  auto it = records_.find(path);
  return it == records_.end() ? nullptr : &it->second;
}

void Vfs::rescan() {
  for (auto it = records_.begin(); it != records_.end();) {
    if (it->second.kind != Kind::Memory) it = records_.erase(it);
    else ++it;
  }
  archives_.clear();
  scanTree(installRoot_, "", true);
  if (overlayRoot_) scanTree(*overlayRoot_, "", true);
  overrides_.clear();
  for (const ModLayer& m : modLayers_) {
    scanTree(m.dir, m.prefix, true);
    std::error_code ec;
    if (fs::is_directory(m.dir / "files", ec)) scanTree(m.dir / "files", "", true, &m.id);
  }
  if (userdataRoot_) scanTree(*userdataRoot_, "userdata/", false);
  scanOriginal();
  LOG_INFO("vfs: %zu archives, %zu records", archives_.size(), records_.size());
  if (!overrides_.empty()) {
    std::map<std::string, int> per;
    for (const Override& o : overrides_) ++per[o.mod];
    for (auto& [mod, n] : per) LOG_INFO("vfs: mod %s overrides %d files", mod.c_str(), n);
  }
}

void Vfs::addModLayer(const fs::path& dir, const std::string& keyPrefix, const std::string& id) {
  modLayers_.push_back({dir, keyPrefix, id});
}

void Vfs::initUserdata(const fs::path& root) {
  userdataRoot_ = root;
  std::error_code ec;
  fs::create_directory(root, ec);
  scanTree(root, "userdata/", false);
}

void Vfs::setOriginalRoot(const fs::path& root, const std::string& prefix) {
  originalRoot_ = root;
  originalPrefix_ = prefix;
  scanOriginal();
}

void Vfs::scanOriginal() {
  if (!originalRoot_) return;
  std::error_code ec;
  // saves: only plain packages (no parent prefix, no absolute mount: what writePackage makes), so nothing in that
  // folder can stand in for game files
  for (auto& e : sortedEntries(*originalRoot_ / "saves")) {
    if (!e.is_regular_file(ec) || !hasGsExtension(e.path())) continue;
    mountArchive(e.path(), "saves/" + pathToUtf8(e.path().stem()), originalPrefix_, true);
  }
  for (auto& e : sortedEntries(*originalRoot_ / "maps")) {
    if (!e.is_regular_file(ec)) continue;
    Record r;
    r.kind = Kind::Loose;
    r.disk = e.path();
    records_[originalPrefix_ + "maps/" + pathToUtf8(e.path().filename())] = std::move(r);
  }
}

void Vfs::scanTree(const fs::path& root, const std::string& keyPrefix, bool skipUserdata, const std::string* overrideBy) {
  // depth-first in directory order
  bool install = root == installRoot_;
  bool overlay = overlayRoot_ && root == *overlayRoot_;
  std::function<void(const fs::path&, const std::string&)> walk = [&](const fs::path& dir, const std::string& rel) {
    for (auto& e : sortedEntries(dir)) {
      std::string name = pathToUtf8(e.path().filename());
      std::string relPath = rel.empty() ? name : rel + "/" + name;
      std::string key = keyPrefix + relPath;
      if (skipUserdata && key.rfind("userdata", 0) == 0) continue;
      if (overlay && relPath == "mods") continue;  // the port's built-in mods mount only when enabled (engine/mods.cpp)
      std::error_code ec;
      if (e.is_directory(ec)) {
        // community content (setCommunityContent): every folder of the install, and userdata/addons
        if (!communityContent_ && (install || key == "userdata/addons")) continue;
        // a folder of the user's own (userdata/…) is listed even while empty (the map manager's new folders): a
        // marker record "<dir>/" that listDirs sees and listFiles skips
        if (key.rfind("userdata/", 0) == 0) {
          Record d;
          d.kind = Kind::Loose;
          d.disk = e.path();
          records_[key + "/"] = std::move(d);
        }
        walk(e.path(), relPath);
      } else if (key.rfind("userdata/", 0) == 0 && e.path().extension() == L".cimtpl") {
        // an OpenCiM template (the editor's templates, regs/ui/mapbrowser.script): a GS10 package shown as the folder
        // "<name>.cimtpl/" (template.grid, preview.dds, template.info); the file itself stays listed for renaming
        Record r;
        r.kind = Kind::Loose;
        r.disk = e.path();
        records_[key] = std::move(r);
        mountArchive(e.path(), relPath, keyPrefix);
      } else if (hasGsExtension(e.path())) {
        std::string relNoExt = relPath.substr(0, relPath.size() - 3);
        mountArchive(e.path(), relNoExt, keyPrefix, false, install);
      } else {
        Record r;
        r.kind = Kind::Loose;
        r.disk = e.path();
        if (overrideBy && records_.count(key)) overrides_.push_back({key, *overrideBy});
        records_[key] = std::move(r);  // loose files replace archive entries
      }
    }
  };
  walk(root, "");
}

void Vfs::mountArchive(const fs::path& file, const std::string& relNoExt, const std::string& keyPrefix, bool plainOnly,
                       bool keepOpen) {
  Archive a;
  a.keepOpen = keepOpen;
  std::string err;
  if (!a.open(file, &err)) {
    LOG_WARN("vfs: skipping archive '%s': %s", pathToUtf8(file).c_str(), err.c_str());
    return;
  }
  if (plainOnly && (a.mountPath || a.parentPrefix)) {
    LOG_WARN("vfs: skipping archive '%s': not a save", pathToUtf8(file).c_str());
    return;
  }
  // archives under userdata mount below "userdata/"; a 0x01140000 absolute mount ignores the location
  std::string prefix = a.mountPath ? *a.mountPath : keyPrefix + a.virtualPrefix(relNoExt);
  int index = int(archives_.size());
  for (int i = 0; i < int(a.entries.size()); ++i) {
    std::string key = prefix + a.entries[i].name;
    if (records_.count(key)) continue;  // first archive wins, loose files are never replaced by archive entries
    Record r;
    r.kind = Kind::ArchiveEntry;
    r.archive = index;
    r.entry = i;
    records_.emplace(std::move(key), std::move(r));
  }
  archives_.push_back(std::move(a));
}

bool Vfs::read(const std::string& path, std::vector<uint8_t>& out, std::string* error) const {
  const Record* r = find(path);
  if (!r) {
    if (error) *error = "not found";
    return false;
  }
  switch (r->kind) {
    case Kind::Memory:
      out = *r->memory;
      return true;
    case Kind::ArchiveEntry: {
      const Archive& a = archives_[r->archive];
      return a.read(a.entries[r->entry], out, error);
    }
    case Kind::Loose: {
      FILE* f = _wfopen(r->disk.wstring().c_str(), L"rb");
      if (!f) {
        if (error) *error = "cannot open";
        return false;
      }
      _fseeki64(f, 0, SEEK_END);
      auto len = _ftelli64(f);
      _fseeki64(f, 0, SEEK_SET);
      out.resize(size_t(len));
      bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
      std::fclose(f);
      return ok;
    }
  }
  return false;
}

bool Vfs::readHead(const std::string& path, std::vector<uint8_t>& out, size_t n) const {
  const Record* r = find(path);
  if (!r) return false;
  switch (r->kind) {
    case Kind::Memory:
      out.assign(r->memory->begin(), r->memory->begin() + std::ptrdiff_t(std::min(n, r->memory->size())));
      return true;
    case Kind::ArchiveEntry: {
      const Archive& a = archives_[r->archive];
      return a.readHead(a.entries[r->entry], out, n);
    }
    case Kind::Loose: {
      FILE* f = _wfopen(r->disk.wstring().c_str(), L"rb");
      if (!f) return false;
      out.resize(n);
      out.resize(std::fread(out.data(), 1, n, f));
      std::fclose(f);
      return true;
    }
  }
  return false;
}

fs::path Vfs::diskPathFor(const std::string& path) const {
  auto w = utf8ToUtf16(path);
  fs::path rel(std::wstring(w.begin(), w.end()));
  if (path.rfind("userdata/", 0) == 0 && userdataRoot_) {
    auto sub = utf8ToUtf16(path.substr(9));
    return *userdataRoot_ / fs::path(std::wstring(sub.begin(), sub.end()));
  }
  return installRoot_ / rel;
}

bool Vfs::writableDiskPath(const std::string& path, fs::path& out) const {
  if (!allowInstallWrites && !(path.rfind("userdata/", 0) == 0 && userdataRoot_)) {
    LOG_WARN("vfs: refused disk change outside userdata/: %s", path.c_str());
    return false;
  }
  out = diskPathFor(path);
  return true;
}

static std::atomic<int> g_pendingWrites{0};  // background writes not out yet (asyncWrites)

bool Vfs::write(const std::string& path, const std::vector<uint8_t>& data) {
  if (path.rfind("memory/", 0) == 0) {
    Record r;
    r.kind = Kind::Memory;
    r.memory = std::make_shared<std::vector<uint8_t>>(data);
    records_[path] = std::move(r);
    return true;
  }
  if (!allowInstallWrites && !(path.rfind("userdata/", 0) == 0 && userdataRoot_)) {
    // the engine would write into the install folder; the port refuses unless explicitly allowed
    LOG_WARN("vfs: refused write outside userdata/: %s", path.c_str());
    return false;
  }
  fs::path disk = diskPathFor(path);
  Record r;
  r.kind = Kind::Loose;
  r.disk = disk;
  records_[path] = r;
  if (asyncWrites) {
    ++g_pendingWrites;
    std::thread([disk, copy = data]() {
      if (FILE* f = _wfopen(disk.wstring().c_str(), L"wb+")) {
        if (std::fwrite(copy.data(), 1, copy.size(), f) != copy.size()) LOG_WARN("vfs: background write of %s failed", disk.string().c_str());
        std::fclose(f);
      } else {
        LOG_WARN("vfs: background write: cannot open %s", disk.string().c_str());
      }
      --g_pendingWrites;
    }).detach();
    return true;
  }
  FILE* f = _wfopen(disk.wstring().c_str(), L"wb+");
  if (!f) return false;  // the engine silently loses the data here
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  return ok;
}

void Vfs::waitForWrites() {
  for (int i = 0; i < 1000 && g_pendingWrites.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

std::vector<std::string> Vfs::listFiles(const std::string& dir, const std::string& ext) const {
  std::string prefix = dir;
  if (!prefix.empty() && prefix.back() != '/') prefix += '/';
  std::vector<std::string> out;
  for (auto it = records_.lower_bound(prefix); it != records_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
    std::string name = it->first.substr(prefix.size());
    if (name.empty() || name.find('/') != std::string::npos) continue;
    if (!ext.empty()) {
      auto dot = name.rfind('.');
      if (dot == std::string::npos || name.substr(dot) != ext) continue;
    }
    out.push_back(std::move(name));
  }
  return out;
}

std::vector<std::string> Vfs::listDirs(const std::string& dir) const {
  std::string prefix = dir;
  if (!prefix.empty() && prefix.back() != '/') prefix += '/';
  std::vector<std::string> out;
  for (auto it = records_.lower_bound(prefix); it != records_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
    std::string rest = it->first.substr(prefix.size());
    auto slash = rest.find('/');
    if (slash == std::string::npos) continue;
    std::string name = rest.substr(0, slash);
    if (out.empty() || out.back() != name) out.push_back(std::move(name));
  }
  return out;
}

}  // namespace regs
