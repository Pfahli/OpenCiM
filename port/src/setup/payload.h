#pragma once
// The setup program's payload (docs/installer.md §3): the release files appended to opencim-setup.exe, which makes it
// the offline installer. Layout after the program's own bytes (the "stub"):
//   the entries' data (zlib streams, or stored as is for files that do not shrink, e.g. JPEG)
//   the index: "OCIMIDX1", u32 count, per entry u16 path length, path (UTF-8, '/'), u64 offset (from the payload's
//     start), u64 size, u64 stored size, u8 method (0 stored, 1 zlib); then u16 length + version, u32 build
//   the footer, the file's last 24 bytes: "OCIMPAY1", u64 payload start (= stub size), u64 index offset
// Paths: "app/..." are installed into the install folder, "show/..." are the setup's own pictures (backgrounds and
// showcase slides), never installed.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace regs::setup {

struct PayloadEntry {
  std::string path;
  uint64_t offset = 0, size = 0, stored = 0;
  uint8_t method = 0;
};

struct Payload {
  std::filesystem::path file;
  uint64_t start = 0;  // the stub's size; 0 when the exe has no payload
  std::vector<PayloadEntry> entries;
  std::string version;
  int build = 0;

  bool open(const std::filesystem::path& exe);  // false: no payload (or a damaged one)
  bool present() const { return start != 0; }
  const PayloadEntry* find(const std::string& path) const;
  bool read(const PayloadEntry& e, std::vector<uint8_t>& out) const;
  uint64_t appBytes() const;  // the installed size
};

// The stub's size: the whole file when it has no payload.
uint64_t stubSize(const std::filesystem::path& exe);

// stub + every file under appDir as "app/<rel>" + every file under showDir as "show/<rel>" → out
bool makeOfflineSetup(const std::filesystem::path& stubExe, const std::filesystem::path& appDir,
                      const std::filesystem::path& showDir, const std::filesystem::path& out, const std::string& version,
                      int build, const std::function<void(const std::string&)>& log);

}  // namespace regs::setup
