#include "vfs/vfs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <functional>
#include <span>

#include <windows.h>

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
  // a package or file still being written would be read half done (or not at all: "size mismatch")
  finishWrites(true);
  waitForWrites();
  scanAll();
}

void Vfs::scanAll() {
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

// ---- packages written in the background (writePackage) ----

struct Vfs::PackageJob {
  fs::path disk;
  std::string path;
  std::vector<std::pair<std::string, Bytes>> files;  // name in the package, bytes (shared with the pending records)
  std::atomic<bool> done{false};
  bool ok = false;
};

namespace {

// one thread, the jobs in order (two saves to one file land in the order made)
struct PackageWriter {
  std::mutex m;
  std::condition_variable work, finished;
  std::deque<std::shared_ptr<Vfs::PackageJob>> queue;
  int open = 0;  // queued or being written
  bool started = false;
};
PackageWriter& packageWriter() {
  static PackageWriter* w = new PackageWriter;  // never destroyed: the detached thread may outlive the statics
  return *w;
}

bool replaceFile(const fs::path& from, const fs::path& to) {
  // a reader of the old package (Archive::read opens the file per read) can hold it for a moment: retry
  for (int i = 0; i < 100; ++i) {
    if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

void writeJob(Vfs::PackageJob& j) {
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::pair<std::string, std::span<const uint8_t>>> views;
  size_t raw = 0;
  for (const auto& [name, data] : j.files) {
    views.emplace_back(name, std::span<const uint8_t>(*data));
    raw += data->size();
  }
  std::vector<uint8_t> package = buildPackage(views);
  auto t1 = std::chrono::steady_clock::now();
  fs::path tmp = j.disk;
  tmp += L".tmp";
  bool ok = false;
  if (FILE* f = _wfopen(tmp.wstring().c_str(), L"wb")) {
    ok = std::fwrite(package.data(), 1, package.size(), f) == package.size();
    ok = std::fclose(f) == 0 && ok;
  }
  if (ok) ok = replaceFile(tmp, j.disk);
  if (!ok) {
    std::error_code ec;
    fs::remove(tmp, ec);
    LOG_ERROR("writePackage: cannot write %s", j.path.c_str());
  }
  auto t2 = std::chrono::steady_clock::now();
  LOG_INFO("writePackage %s: %zu files, %.1f MB packed to %.1f MB in %.0f ms, written in %.0f ms (background)", j.path.c_str(),
           j.files.size(), double(raw) / 1048576.0, double(package.size()) / 1048576.0,
           std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
  j.ok = ok;
}

void writerLoop() {
  PackageWriter& w = packageWriter();
  for (;;) {
    std::shared_ptr<Vfs::PackageJob> j;
    {
      std::unique_lock lock(w.m);
      w.work.wait(lock, [&] { return !w.queue.empty(); });
      j = w.queue.front();
      w.queue.pop_front();
    }
    writeJob(*j);
    {
      std::lock_guard lock(w.m);
      j->done = true;
      --w.open;
    }
    w.finished.notify_all();
  }
}

void queueJob(std::shared_ptr<Vfs::PackageJob> j) {
  PackageWriter& w = packageWriter();
  {
    std::lock_guard lock(w.m);
    if (!w.started) {
      w.started = true;
      std::thread(writerLoop).detach();
    }
    w.queue.push_back(std::move(j));
    ++w.open;
  }
  w.work.notify_one();
}

void waitJob(const Vfs::PackageJob& j) {
  PackageWriter& w = packageWriter();
  std::unique_lock lock(w.m);
  w.finished.wait(lock, [&] { return j.done.load(); });
}

// the same file, as Windows sees it (case and separators do not matter)
bool sameFile(const fs::path& a, const fs::path& b) {
  std::wstring x = a.lexically_normal().wstring(), y = b.lexically_normal().wstring();
  return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), [](wchar_t c, wchar_t d) { return std::towupper(c) == std::towupper(d); });
}

bool endsWith(const std::string& s, const char* suffix) {
  size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

void Vfs::waitForWrites() {
  for (int i = 0; i < 1000 && g_pendingWrites.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  PackageWriter& w = packageWriter();
  std::unique_lock lock(w.m);
  w.finished.wait(lock, [&] { return w.open == 0; });
}

Vfs::Bytes Vfs::share(const std::string& path) const {
  const Record* r = find(path);
  if (r && r->kind == Kind::Memory) return r->memory;
  auto out = std::make_shared<std::vector<uint8_t>>();
  if (!read(path, *out)) return nullptr;
  return out;
}

bool Vfs::writePackage(const std::string& path, std::vector<std::pair<std::string, Bytes>> files) {
  const bool user = path.rfind("userdata/", 0) == 0 && userdataRoot_;
  if (!allowInstallWrites && !user) {
    LOG_WARN("vfs: refused write outside userdata/: %s", path.c_str());
    return false;
  }
  PendingPackage p;
  p.path = path;
  p.disk = diskPathFor(path);
  // as scanTree would mount the file: "<rel minus .gs>/" for a .gs, "<rel>/" and the file for a .cimtpl, the file
  // alone for any other name
  p.keyPrefix = user ? "userdata/" : "";
  p.rel = path.substr(p.keyPrefix.size());
  if (endsWith(path, ".gs")) {
    p.rel.resize(p.rel.size() - 3);
    p.folder = true;
  } else if (endsWith(path, ".cimtpl")) {
    p.folder = p.fileRecord = true;
  } else {
    p.fileRecord = true;
  }
  if (p.folder)
    for (const auto& [name, data] : files) p.entries.emplace_back(p.keyPrefix + p.rel + "/" + packageEntryName(name), data);
  p.job = std::make_shared<PackageJob>();
  p.job->disk = p.disk;
  p.job->path = path;
  p.job->files = std::move(files);
  showPending(p);
  pending_.push_back(p);
  queueJob(p.job);
  return true;
}

void Vfs::dropArchive(const fs::path& disk) {
  // the records of an earlier mount of this file (its directory no longer matches the file being written)
  std::vector<bool> gone(archives_.size(), false);
  bool any = false;
  for (size_t i = 0; i < archives_.size(); ++i)
    if (!archives_[i].entries.empty() && sameFile(archives_[i].file, disk)) gone[i] = any = true;
  if (!any) return;
  for (auto it = records_.begin(); it != records_.end();) {
    if (it->second.kind == Kind::ArchiveEntry && gone[size_t(it->second.archive)]) it = records_.erase(it);
    else ++it;
  }
  for (size_t i = 0; i < archives_.size(); ++i)
    if (gone[i]) archives_[i].entries.clear();  // kept in place: the other records' archive indices stay valid
}

void Vfs::dropPending(const PendingPackage& p) {
  for (const auto& [key, data] : p.entries)
    if (auto it = records_.find(key); it != records_.end() && it->second.kind == Kind::Memory && it->second.memory == data)
      records_.erase(it);
}

void Vfs::showPending(const PendingPackage& p) {
  dropArchive(p.disk);
  for (const PendingPackage& q : pending_)
    if (sameFile(q.disk, p.disk)) dropPending(q);
  for (const auto& [key, data] : p.entries) {
    if (records_.count(key)) continue;  // as mountArchive: loose files and other archives win
    Record r;
    r.kind = Kind::Memory;
    r.memory = data;
    records_.emplace(key, std::move(r));
  }
  if (p.fileRecord) {
    Record r;
    r.kind = Kind::Loose;
    r.disk = p.disk;
    records_[p.path] = std::move(r);
  }
  // the folders on the way, as a rescan lists them (userdata/ folders have marker records, scanTree)
  if (p.keyPrefix == "userdata/") {
    std::error_code ec;
    for (size_t s = p.rel.find('/'); s != std::string::npos; s = p.rel.find('/', s + 1)) {
      std::string dir = p.rel.substr(0, s);
      fs::path d = diskPathFor("userdata/" + dir);
      if (!records_.count("userdata/" + dir + "/") && fs::is_directory(d, ec)) {
        Record r;
        r.kind = Kind::Loose;
        r.disk = d;
        records_["userdata/" + dir + "/"] = std::move(r);
      }
    }
  }
}

void Vfs::finishWrites(bool wait) {
  size_t n = 0;
  for (; n < pending_.size(); ++n) {
    PendingPackage& p = pending_[n];
    if (!p.job->done) {
      if (!wait) break;
      waitJob(*p.job);
    }
    bool later = false;
    for (size_t k = n + 1; k < pending_.size(); ++k) later = later || sameFile(pending_[k].disk, p.disk);
    if (later) continue;  // a newer package for the same file is still on its way and stays shown
    dropPending(p);
    if (p.keyPrefix != "userdata/") {  // outside userdata (allowInstallWrites): as the engine, everything again
      scanAll();
      continue;
    }
    // the written file, or whatever is on disk after a failed write
    std::error_code ec;
    dropArchive(p.disk);
    if (p.folder && fs::is_regular_file(p.disk, ec)) {
      size_t before = archives_.size();
      mountArchive(p.disk, p.rel, p.keyPrefix);
      if (archives_.size() > before) LOG_INFO("vfs: mounted %s (%zu entries)", p.path.c_str(), archives_.back().entries.size());
    }
    if (p.fileRecord && !fs::is_regular_file(p.disk, ec)) records_.erase(p.path);
  }
  pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(n));
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
