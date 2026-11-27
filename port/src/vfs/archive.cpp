#include "vfs/archive.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "core/deflate.h"
#include "core/inflate.h"
#include "core/text.h"
#include "core/file.h"

namespace regs {

namespace {

struct File {
  FILE* f = nullptr;
  explicit File(const std::filesystem::path& p) { f = regs::openFile(p, "rb"); }
  ~File() { if (f) std::fclose(f); }
  bool readAt(uint64_t off, void* dst, size_t n) const {
    return regs::seekFile(f, int64_t(off), SEEK_SET) == 0 && std::fread(dst, 1, n, f) == n;
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
  open_ = keepOpen ? std::make_shared<OpenFile>() : nullptr;
  entries.clear();
  parentPrefix.reset();
  mountPath.reset();
  File f(path);
  if (!f.f) return fail(error, "cannot open");
  regs::seekFile(f.f, 0, SEEK_END);
  uint64_t len = uint64_t(regs::tellFile(f.f));
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

// a Win32 handle (no C runtime stream: an install with many addon archives would exhaust the streams); ReadFile at
// an explicit offset needs no shared file position, so reads from several threads need no lock (elsewhere: a file
// descriptor and pread, for the same reasons)
struct Archive::OpenFile {
  std::mutex m;  // the first open
#if defined(_WIN32)
  HANDLE h = INVALID_HANDLE_VALUE;
#else
  int fd = -1;
#endif
  bool tried = false;
  ~OpenFile() {
#if defined(_WIN32)
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
#else
    if (fd >= 0) close(fd);
#endif
  }
};

bool Archive::readAt(uint64_t offset, void* dst, size_t n, std::string* error) const {
  if (keepOpen) {
    if (!open_) return fail(error, "cannot open archive");  // open() not called or failed
#if !defined(_WIN32)
    int fd;
    {
      std::lock_guard<std::mutex> lock(open_->m);
      if (!open_->tried) {
        open_->tried = true;
        open_->fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
      }
      fd = open_->fd;
    }
    if (fd < 0) return fail(error, "cannot open archive");
    for (auto* p = static_cast<uint8_t*>(dst); n > 0;) {
      const ssize_t got = pread(fd, p, std::min<size_t>(n, 1u << 30), off_t(offset));
      if (got <= 0) return fail(error, "short read");
      p += got, offset += uint64_t(got), n -= size_t(got);
    }
    return true;
#else
    HANDLE h;
    {
      std::lock_guard<std::mutex> lock(open_->m);
      if (!open_->tried) {
        open_->tried = true;
        // others may still read, write, rename or delete the file, as with the C runtime's fopen
        open_->h = CreateFileW(file.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      }
      h = open_->h;
    }
    if (h == INVALID_HANDLE_VALUE) return fail(error, "cannot open archive");
    for (auto* p = static_cast<uint8_t*>(dst); n > 0;) {
      OVERLAPPED ov{};
      ov.Offset = DWORD(offset);
      ov.OffsetHigh = DWORD(offset >> 32);
      DWORD want = DWORD(std::min<size_t>(n, 1u << 30)), got = 0;
      if (!ReadFile(h, p, want, &got, &ov) || got == 0) return fail(error, "short read");
      p += got, offset += got, n -= got;
    }
    return true;
#endif
  }
  File f(file);
  if (!f.f) return fail(error, "cannot open archive");
  if (!f.readAt(offset, dst, n)) return fail(error, "short read");
  return true;
}

bool Archive::read(const ArchiveEntry& e, std::vector<uint8_t>& out, std::string* error) const {
  if (e.tag == kStored) {
    out.resize(e.size);
    return readAt(e.offset, out.data(), e.size, error);
  }
  std::vector<uint8_t> packed(e.packedSize);
  if (!readAt(e.offset, packed.data(), packed.size(), error)) return false;
  if (!zlibInflate(packed, out, e.size) || out.size() != e.size) return fail(error, "corrupted");
  return true;
}

bool Archive::readHead(const ArchiveEntry& e, std::vector<uint8_t>& out, size_t n) const {
  n = std::min<size_t>(n, e.size);
  if (e.tag == kStored) {
    out.resize(n);
    return readAt(e.offset, out.data(), n, nullptr);
  }
  if (n < e.size) {
    // the start of the packed data is usually enough (a few hundred bytes give a DDS header); else all of it
    std::vector<uint8_t> packed(std::min<size_t>(e.packedSize, 4096 + 2 * n));
    if (!readAt(e.offset, packed.data(), packed.size(), nullptr)) return false;
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

std::string packageEntryName(const std::string& name) {
  char fixed[128] = {};
  std::memcpy(fixed, name.data(), std::min<size_t>(name.size(), 127));
  return fixedString(reinterpret_cast<const uint8_t*>(fixed), 128);
}

std::vector<uint8_t> buildPackage(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  std::vector<std::pair<std::string, std::span<const uint8_t>>> views;
  for (const auto& [name, data] : files) views.emplace_back(name, std::span<const uint8_t>(data));
  return buildPackage(views);
}

std::vector<uint8_t> buildPackage(const std::vector<std::pair<std::string, std::span<const uint8_t>>>& files) {
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
  regs::seekFile(f.f, 0, SEEK_END);
  uint64_t len = uint64_t(regs::tellFile(f.f));
  uint8_t hdr[8];
  return len >= 8 && f.readAt(0, hdr, 8) && std::memcmp(hdr, "GS10", 4) == 0 && u32(hdr + 4) == len - 8;
}

}  // namespace regs
