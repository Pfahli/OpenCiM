#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace regs {

// GS10 archive (docs/REPORT.md §2). All values little-endian u32; every structure is a chunk {tag, size, body}.
struct ArchiveEntry {
  std::string name;        // entry name as stored (UTF-8, from Windows-1252)
  uint32_t tag = 0;        // 0x01110000 stored, 0x01120000 zlib
  uint32_t offset = 0;     // absolute file offset of the entry data
  uint32_t size = 0;       // unpacked size
  uint32_t packedSize = 0;
};

struct Archive {
  std::filesystem::path file;
  std::optional<std::string> parentPrefix;  // chunk 0x01130000
  std::optional<std::string> mountPath;     // chunk 0x01140000 (absolute mount)
  std::vector<ArchiveEntry> entries;
  std::string trailer;                      // 0x00000001 comment, shipped archives only
  // Keep the file open after the first read instead of opening it for every read (set before open(); the install's
  // archives). Safe to read from several threads.
  bool keepOpen = false;

  static constexpr uint32_t kStored = 0x01110000;
  static constexpr uint32_t kZlib = 0x01120000;

  // Reads the header and directory. Returns false (with a message) if the file is not a valid GS10 archive.
  bool open(const std::filesystem::path& path, std::string* error = nullptr);
  // Reads and unpacks one entry.
  bool read(const ArchiveEntry& e, std::vector<uint8_t>& out, std::string* error = nullptr) const;
  // The first min(n, size) bytes of one entry; a packed entry is inflated only that far.
  bool readHead(const ArchiveEntry& e, std::vector<uint8_t>& out, size_t n) const;

  // Virtual path prefix of this archive's entries. relPath is the archive's path relative to the root it was found
  // under, with '/' separators and without the ".gs" extension.
  std::string virtualPrefix(const std::string& relPathNoExt) const;

 private:
  struct OpenFile;
  std::shared_ptr<OpenFile> open_;  // keepOpen: the handle, opened on the first read
  bool readAt(uint64_t offset, void* dst, size_t n, std::string* error) const;
};

// Header check used by validatePackage: magic "GS10" and size == file length - 8.
bool isValidPackage(const std::filesystem::path& path);

// writePackage's archive (gsFunction_writePackage L:0x081058b0): root, directory with one entry per file (magic = the
// file's first 4 bytes, offsets from the root body), the zlib-compressed data back to back, no trailer.
std::vector<uint8_t> buildPackage(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files);

}  // namespace regs
