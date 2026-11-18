#include "vfs/archive.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/deflate.h"
#include "core/inflate.h"
#include "core/text.h"

namespace regs {

namespace {

struct File {
  FILE* f = nullptr;
  explicit File(const std::filesystem::path& p) { f = _wfopen(p.wstring().c_str(), L"rb"); }
  ~File() { if (f) std::fclose(f); }
  bool readAt(uint64_t off, void* dst, size_t n) const {
    return _fseeki64(f, int64_t(off), SEEK_SET) == 0 && std::fread(dst, 1, n, f) == n;
  }
};

uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

std::string fixedString(const uint8_t* p, size_t n) {
  size_t len = 0;
  while (len < n && p[len]) ++len;
  return cp1252ToUtf8(std::string_view(reinterpret_cast<const char*>(p), len));
}

bool fail(std::string* error, const std::string& msg) {
  if (error) *error = msg;
  return false;
}

}  // namespace

bool Archive::open(const std::filesystem::path& path, std::string* error) {
  file = path;
  entries.clear();
  parentPrefix.reset();
  mountPath.reset();
  File f(path);
  if (!f.f) return fail(error, "cannot open");
  _fseeki64(f.f, 0, SEEK_END);
  uint64_t len = uint64_t(_ftelli64(f.f));
  uint8_t hdr[16];
  if (len < 16 || !f.readAt(0, hdr, 16)) return fail(error, "too short");
  if (std::memcmp(hdr, "GS10", 4) != 0) return fail(error, "bad magic");
  if (u32(hdr + 4) != len - 8) return fail(error, "size mismatch");
  if (u32(hdr + 8) != 0x01000000) return fail(error, "no root chunk");
  const uint64_t rootBody = 16;
  const uint64_t rootEnd = rootBody + u32(hdr + 12);
  // walk root children: directory 0x01100000, data 0x01200000
  uint64_t pos = rootBody;
  while (pos + 8 <= rootEnd) {
    uint8_t ch[8];
    if (!f.readAt(pos, ch, 8)) return fail(error, "truncated chunk");
    uint32_t tag = u32(ch), size = u32(ch + 4);
    if (tag == 0x01100000) {
      // Like the engine (FUN_1007ef80), read children until the declared size or the file runs out, skip unknown
      // chunks and ignore the entry count. Third-party packers write wrong root/directory sizes (seen in mods).
      uint64_t avail = std::min<uint64_t>(size, len - (pos + 8));
      std::vector<uint8_t> dir(static_cast<size_t>(avail));
      if (avail < 4 || !f.readAt(pos + 8, dir.data(), dir.size())) return fail(error, "truncated directory");
      size_t p = 4;
      while (p + 8 <= dir.size()) {
        uint32_t t = u32(&dir[p]), s = u32(&dir[p + 4]);
        const uint8_t* body = &dir[p + 8];
        if (p + 8 + s > dir.size()) {
          if ((t & 0xFFFF0000) == kStored || (t & 0xFFFF0000) == kZlib) return fail(error, "truncated directory entry");
          break;  // e.g. the data chunk 0x01200000 when the directory size covers the rest of the file
        }
        if (t == 0x01130000) {
          parentPrefix = fixedString(body, s);
        } else if (t == 0x01140000) {
          mountPath = fixedString(body, s);
        } else if ((t & 0xFFFF0000) == kStored || (t & 0xFFFF0000) == kZlib) {
          if (s < 144) return fail(error, "short entry");
          ArchiveEntry e;
          e.tag = t & 0xFFFF0000;
          // body: magic[4] offset size packedSize name[128]; offsets are relative to the root body
          e.offset = uint32_t(rootBody + u32(body + 4));
          e.size = u32(body + 8);
          e.packedSize = u32(body + 12);
          e.name = fixedString(body + 16, 128);
          entries.push_back(std::move(e));
        }
        p += 8 + s;
      }
      break;  // nothing after the directory is needed; the data chunk is addressed by entry offsets
    }
    pos += 8 + uint64_t(size);
  }
  if (rootEnd + 8 <= len) {  // shipped archives end with a comment chunk after the root
    uint8_t ch[8];
    if (f.readAt(rootEnd, ch, 8) && u32(ch) == 1 && rootEnd + 8 + u32(ch + 4) <= len) {
      std::string t(u32(ch + 4), '\0');
      f.readAt(rootEnd + 8, t.data(), t.size());
      trailer = t.c_str();
    }
  }
  return true;
}

bool Archive::read(const ArchiveEntry& e, std::vector<uint8_t>& out, std::string* error) const {
  File f(file);
  if (!f.f) return fail(error, "cannot open archive");
  if (e.tag == kStored) {
    out.resize(e.size);
    if (!f.readAt(e.offset, out.data(), e.size)) return fail(error, "short read");
    return true;
  }
  std::vector<uint8_t> packed(e.packedSize);
  if (!f.readAt(e.offset, packed.data(), packed.size())) return fail(error, "short read");
  if (!zlibInflate(packed, out, e.size) || out.size() != e.size) return fail(error, "corrupted");
  return true;
}

bool Archive::readHead(const ArchiveEntry& e, std::vector<uint8_t>& out, size_t n) const {
  n = std::min<size_t>(n, e.size);
  if (e.tag == kStored) {
    File f(file);
    out.resize(n);
    return f.f && f.readAt(e.offset, out.data(), n);
  }
  if (n < e.size) {
    // the start of the packed data is usually enough (a few hundred bytes give a DDS header); else all of it
    File f(file);
    if (!f.f) return false;
    std::vector<uint8_t> packed(std::min<size_t>(e.packedSize, 4096 + 2 * n));
    if (!f.readAt(e.offset, packed.data(), packed.size())) return false;
    if (zlibInflateHead(packed, out, n)) return true;
  }
  if (!read(e, out)) return false;
  out.resize(n);
  return true;
}

std::string Archive::virtualPrefix(const std::string& relPathNoExt) const {
  if (mountPath) return *mountPath;
  return parentPrefix.value_or("") + relPathNoExt + "/";
}

std::vector<uint8_t> buildPackage(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  std::vector<uint8_t> out;
  auto put = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
  };
  std::vector<std::vector<uint8_t>> packed;  // zlib, as the engine's compress()
  uint32_t dirSize = 4 + uint32_t(files.size()) * (8 + 144), dataSize = 0;
  for (auto& f : files) {
    packed.push_back(zlibDeflate(f.second));
    dataSize += uint32_t(packed.back().size());
  }
  uint32_t rootSize = 8 + dirSize + 8 + dataSize;
  out.insert(out.end(), {'G', 'S', '1', '0'});
  put(8 + rootSize);
  put(0x01000000);
  put(rootSize);
  put(0x01100000);
  put(dirSize);
  put(uint32_t(files.size()));
  uint32_t offset = 8 + dirSize + 8;  // the data chunk's body, relative to the root body
  for (size_t k = 0; k < files.size(); ++k) {
    const auto& [name, data] = files[k];
    put(Archive::kZlib);
    put(144);
    uint32_t magic = 0;
    for (size_t i = 0; i < 4 && i < data.size(); ++i) magic |= uint32_t(data[i]) << (8 * i);
    put(magic);
    put(offset);
    put(uint32_t(data.size()));
    put(uint32_t(packed[k].size()));
    char fixed[128] = {};
    std::memcpy(fixed, name.data(), std::min<size_t>(name.size(), 127));
    out.insert(out.end(), fixed, fixed + 128);
    offset += uint32_t(packed[k].size());
  }
  put(0x01200000);
  put(dataSize);
  for (auto& p : packed) out.insert(out.end(), p.begin(), p.end());
  return out;
}

bool isValidPackage(const std::filesystem::path& path) {
  File f(path);
  if (!f.f) return false;
  _fseeki64(f.f, 0, SEEK_END);
  uint64_t len = uint64_t(_ftelli64(f.f));
  uint8_t hdr[8];
  return len >= 8 && f.readAt(0, hdr, 8) && std::memcmp(hdr, "GS10", 4) == 0 && u32(hdr + 4) == len - 8;
}

}  // namespace regs
