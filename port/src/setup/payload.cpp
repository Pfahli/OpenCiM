#include "setup/payload.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "core/deflate.h"
#include "core/game_find.h"
#include "core/inflate.h"

namespace fs = std::filesystem;

namespace regs::setup {

namespace {

constexpr char kFooterMagic[8] = {'O', 'C', 'I', 'M', 'P', 'A', 'Y', '1'};
constexpr char kIndexMagic[8] = {'O', 'C', 'I', 'M', 'I', 'D', 'X', '1'};

template <class T>
void put(std::string& s, T v) {
  s.append(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <class T>
bool get(const std::vector<uint8_t>& b, size_t& at, T& v) {
  if (at + sizeof(T) > b.size()) return false;
  std::memcpy(&v, b.data() + at, sizeof(T));
  at += sizeof(T);
  return true;
}

struct Footer {
  uint64_t start = 0, index = 0;
};

bool readFooter(std::ifstream& in, uint64_t fileSize, Footer& f) {
  if (fileSize < 24) return false;
  char magic[8];
  in.seekg(std::streamoff(fileSize - 24));
  in.read(magic, 8);
  in.read(reinterpret_cast<char*>(&f.start), 8);
  in.read(reinterpret_cast<char*>(&f.index), 8);
  if (!in || std::memcmp(magic, kFooterMagic, 8) != 0) return false;
  return f.start > 0 && f.start + f.index + 24 <= fileSize;
}

}  // namespace

uint64_t stubSize(const fs::path& exe) {
  std::error_code ec;
  uint64_t size = fs::file_size(exe, ec);
  if (ec) return 0;
  std::ifstream in(exe, std::ios::binary);
  Footer f;
  return readFooter(in, size, f) ? f.start : size;
}

bool Payload::open(const fs::path& exe) {
  file = exe;
  start = 0;
  entries.clear();
  std::error_code ec;
  uint64_t size = fs::file_size(exe, ec);
  if (ec) return false;
  std::ifstream in(exe, std::ios::binary);
  Footer f;
  if (!readFooter(in, size, f)) return false;
  uint64_t indexAt = f.start + f.index, indexSize = size - 24 - indexAt;
  std::vector<uint8_t> b(static_cast<size_t>(indexSize));
  in.seekg(std::streamoff(indexAt));
  in.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size()));
  if (!in || b.size() < 12 || std::memcmp(b.data(), kIndexMagic, 8) != 0) return false;
  size_t at = 8;
  uint32_t count = 0;
  if (!get(b, at, count)) return false;
  for (uint32_t i = 0; i < count; ++i) {
    PayloadEntry e;
    uint16_t len = 0;
    if (!get(b, at, len) || at + len > b.size()) return false;
    e.path.assign(reinterpret_cast<const char*>(b.data() + at), len);
    at += len;
    if (!get(b, at, e.offset) || !get(b, at, e.size) || !get(b, at, e.stored) || !get(b, at, e.method)) return false;
    if (e.offset + e.stored > f.index) return false;
    entries.push_back(std::move(e));
  }
  uint16_t vlen = 0;
  if (!get(b, at, vlen) || at + vlen > b.size()) return false;
  version.assign(reinterpret_cast<const char*>(b.data() + at), vlen);
  at += vlen;
  uint32_t bld = 0;
  if (!get(b, at, bld)) return false;
  build = int(bld);
  start = f.start;
  return true;
}

const PayloadEntry* Payload::find(const std::string& path) const {
  for (const PayloadEntry& e : entries)
    if (e.path == path) return &e;
  return nullptr;
}

bool Payload::read(const PayloadEntry& e, std::vector<uint8_t>& out) const {
  std::ifstream in(file, std::ios::binary);
  std::vector<uint8_t> raw(size_t(e.stored));
  in.seekg(std::streamoff(start + e.offset));
  in.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()));
  if (!in) return false;
  if (e.method == 0) {
    out = std::move(raw);
    return out.size() == e.size;
  }
  out.clear();
  return zlibInflate(raw, out, size_t(e.size)) && out.size() == e.size;
}

uint64_t Payload::appBytes() const {
  uint64_t n = 0;
  for (const PayloadEntry& e : entries)
    if (e.path.rfind("app/", 0) == 0) n += e.size;
  return n;
}

bool makeOfflineSetup(const fs::path& stubExe, const fs::path& appDir, const fs::path& showDir, const fs::path& out,
                      const std::string& version, int build, const std::function<void(const std::string&)>& log) {
  uint64_t stub = stubSize(stubExe);
  if (stub == 0) {
    log("no setup program at " + pathToUtf8(stubExe));
    return false;
  }
  struct Src {
    fs::path file;
    std::string path;
  };
  std::vector<Src> files;
  auto collect = [&](const fs::path& dir, const std::string& prefix) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file()) continue;
      std::string rel = pathToUtf8(fs::relative(it->path(), dir));
      std::replace(rel.begin(), rel.end(), '\\', '/');
      files.push_back({it->path(), prefix + rel});
    }
  };
  collect(appDir, "app/");
  collect(showDir, "show/");
  std::sort(files.begin(), files.end(), [](const Src& a, const Src& b) { return a.path < b.path; });
  if (files.empty()) {
    log("nothing to pack");
    return false;
  }
  fs::path tmp = out;
  tmp += ".tmp";
  std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
  {
    std::ifstream s(stubExe, std::ios::binary);
    std::vector<char> buf(static_cast<size_t>(stub));
    s.read(buf.data(), std::streamsize(buf.size()));
    if (!s) {
      log("cannot read " + pathToUtf8(stubExe));
      return false;
    }
    o.write(buf.data(), std::streamsize(buf.size()));
  }
  std::vector<PayloadEntry> entries;
  uint64_t offset = 0, total = 0, packed = 0;
  for (const Src& f : files) {
    std::ifstream in(f.file, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PayloadEntry e;
    e.path = f.path;
    e.offset = offset;
    e.size = data.size();
    std::vector<uint8_t> z;
    std::string ext = pathToUtf8(f.file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
    bool tryZip = data.size() > 64 && ext != ".jpg" && ext != ".png" && ext != ".mp3";
    if (tryZip) z = zlibDeflate(data);
    if (tryZip && z.size() + 16 < data.size()) {
      e.method = 1;
      e.stored = z.size();
      o.write(reinterpret_cast<const char*>(z.data()), std::streamsize(z.size()));
    } else {
      e.method = 0;
      e.stored = data.size();
      o.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    }
    offset += e.stored;
    total += e.size;
    packed += e.stored;
    entries.push_back(std::move(e));
  }
  std::string idx(kIndexMagic, 8);
  put(idx, uint32_t(entries.size()));
  for (const PayloadEntry& e : entries) {
    put(idx, uint16_t(e.path.size()));
    idx += e.path;
    put(idx, e.offset);
    put(idx, e.size);
    put(idx, e.stored);
    put(idx, e.method);
  }
  put(idx, uint16_t(version.size()));
  idx += version;
  put(idx, uint32_t(build));
  o.write(idx.data(), std::streamsize(idx.size()));
  o.write(kFooterMagic, 8);
  o.write(reinterpret_cast<const char*>(&stub), 8);
  o.write(reinterpret_cast<const char*>(&offset), 8);
  o.close();
  if (!o) {
    log("cannot write " + pathToUtf8(tmp));
    return false;
  }
  std::error_code ec;
  fs::rename(tmp, out, ec);
  if (ec) {
    log("cannot rename to " + pathToUtf8(out) + ": " + ec.message());
    return false;
  }
  log(std::to_string(entries.size()) + " files, " + std::to_string(total / 1048576) + " MB packed to " +
      std::to_string(packed / 1048576) + " MB");
  return true;
}

}  // namespace regs::setup
