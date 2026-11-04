#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vfs/archive.h"

namespace regs {

// The engine's virtual file system (docs/REPORT.md §3, docs/handlers/batch-C.md file functions):
// one case-sensitive map from virtual path to a record. Archives are mounted as folders; loose files always replace
// archive entries; between archives the first one mounted wins; "memory/" files live in RAM and survive rescans;
// "userdata/" maps to a separate disk folder.
class Vfs {
 public:
  enum class Kind { ArchiveEntry, Loose, Memory };
  struct Record {
    Kind kind = Kind::Loose;
    int archive = -1;  // index into archives_
    int entry = -1;    // index into that archive's entries
    std::filesystem::path disk;
    std::shared_ptr<std::vector<uint8_t>> memory;
  };

  void setInstallRoot(const std::filesystem::path& root) { installRoot_ = root; }
  const std::filesystem::path& installRoot() const { return installRoot_; }
  // The port's own files (data/ next to regs.exe): loose files over the install, scanned after it and before
  // userdata, so they replace game files of the same path without touching the install.
  void setOverlayRoot(const std::filesystem::path& root) { overlayRoot_ = root; }
  // Community content: loose folders and archives below the install root (addons/, metro/ overrides) and
  // userdata/addons. The engine mounts them all; the port mounts only the official archives of the install root
  // (game and DLC, the DLC appearing as addons/<pack>/) unless this is on.
  void setCommunityContent(bool on) { communityContent_ = on; }

  // updateFileSystem(): drop every non-memory record, scan the install root, then the userdata root.
  void rescan();
  // init_userdata(root): set the userdata disk folder (created if missing, one level) and scan it.
  void initUserdata(const std::filesystem::path& root);

  bool exists(const std::string& path) const { return records_.count(path) != 0; }
  const Record* find(const std::string& path) const;
  bool read(const std::string& path, std::vector<uint8_t>& out, std::string* error = nullptr) const;

  // Writes a file: "memory/..." becomes a RAM file; anything else is mapped to disk ("userdata/" -> userdata root,
  // otherwise relative to the install root) and registered as a loose record. Directories are not created.
  bool write(const std::string& path, const std::vector<uint8_t>& data);

  // Files directly inside dir (names only), optionally filtered by exact extension including the dot.
  std::vector<std::string> listFiles(const std::string& dir, const std::string& ext = "") const;
  // Immediate sub-folders of dir (names only).
  std::vector<std::string> listDirs(const std::string& dir) const;

  std::filesystem::path diskPathFor(const std::string& path) const;

  // The original writes paths outside userdata/ relative to the install folder. Off by default in the port.
  bool allowInstallWrites = false;

  const std::vector<Archive>& archives() const { return archives_; }
  const std::map<std::string, Record>& records() const { return records_; }

 private:
  void scanTree(const std::filesystem::path& root, const std::string& keyPrefix, bool skipUserdata);
  void mountArchive(const std::filesystem::path& file, const std::string& relNoExt, const std::string& keyPrefix);

  std::filesystem::path installRoot_;
  std::optional<std::filesystem::path> userdataRoot_, overlayRoot_;
  bool communityContent_ = false;
  std::vector<Archive> archives_;
  std::map<std::string, Record> records_;
};

}  // namespace regs
