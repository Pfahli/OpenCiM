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
  // A mod's layers (engine/mods.cpp, port/docs/mod-manager.md §3), scanned after the port's data in the order added:
  // the mod folder below keyPrefix ("regs/mods/<id>/"), and its files/ subfolder at the root, where its loose files
  // replace the game's and the port's files of the same path (recorded in overrides()).
  void addModLayer(const std::filesystem::path& dir, const std::string& keyPrefix, const std::string& id);
  struct Override {
    std::string path, mod;
  };
  const std::vector<Override>& overrides() const { return overrides_; }

  // updateFileSystem(): drop every non-memory record, scan the install root, then the userdata root.
  void rescan();
  // init_userdata(root): set the userdata disk folder (created if missing, one level) and scan it.
  void initUserdata(const std::filesystem::path& root);
  // The port's read-only view of another user folder (the original game's Documents\Cities In Motion; port/README
  // "Where data lives"): its saves\*.gs mount below "<prefix>saves/<name>/" and the loose files directly in maps\ as
  // "<prefix>maps/<file>", so the load lists can read a save's game.campaign or a map's preview. Nothing in that
  // folder is ever created, written, renamed or deleted: write() and writableDiskPath() refuse every path outside
  // "userdata/", and the prefix is not "userdata/". Rescans include it.
  void setOriginalRoot(const std::filesystem::path& root, const std::string& prefix);

  bool exists(const std::string& path) const { return records_.count(path) != 0; }
  const Record* find(const std::string& path) const;
  // Both are safe from several threads at once while nothing rescans or writes the file system.
  bool read(const std::string& path, std::vector<uint8_t>& out, std::string* error = nullptr) const;
  // The first n bytes of a file (all of it when shorter), e.g. a header: packed archive entries are inflated only so far.
  bool readHead(const std::string& path, std::vector<uint8_t>& out, size_t n) const;

  // Writes a file: "memory/..." becomes a RAM file; anything else is mapped to disk ("userdata/" -> userdata root,
  // otherwise relative to the install root) and registered as a loose record. Directories are not created.
  bool write(const std::string& path, const std::vector<uint8_t>& data);
  // port: while set, write() puts disk files out on a background thread (the editor's autosave, so the game does not
  // stall on the disk); the record is registered at once. waitForWrites() blocks until they are all out (at exit).
  bool asyncWrites = false;
  static void waitForWrites();

  // Files directly inside dir (names only), optionally filtered by exact extension including the dot.
  std::vector<std::string> listFiles(const std::string& dir, const std::string& ext = "") const;
  // Immediate sub-folders of dir (names only).
  std::vector<std::string> listDirs(const std::string& dir) const;

  std::filesystem::path diskPathFor(const std::string& path) const;
  // The disk path for a native that changes the disk outside write() (delete_file, mkdir, fappend, freset): only
  // below the userdata folder (or anywhere with allowInstallWrites, as write()); false and a warning otherwise.
  bool writableDiskPath(const std::string& path, std::filesystem::path& out) const;

  // The original writes paths outside userdata/ relative to the install folder. Off by default in the port.
  bool allowInstallWrites = false;

  const std::vector<Archive>& archives() const { return archives_; }
  const std::map<std::string, Record>& records() const { return records_; }

 private:
  void scanTree(const std::filesystem::path& root, const std::string& keyPrefix, bool skipUserdata,
                const std::string* overrideBy = nullptr);
  // keepOpen: the install's archives, which do not change while the game runs, keep their file open for reads
  void mountArchive(const std::filesystem::path& file, const std::string& relNoExt, const std::string& keyPrefix,
                    bool plainOnly = false, bool keepOpen = false);
  void scanOriginal();

  std::filesystem::path installRoot_;
  std::optional<std::filesystem::path> userdataRoot_, overlayRoot_, originalRoot_;
  std::string originalPrefix_;
  bool communityContent_ = false;
  struct ModLayer {
    std::filesystem::path dir;
    std::string prefix, id;
  };
  std::vector<ModLayer> modLayers_;
  std::vector<Override> overrides_;
  std::vector<Archive> archives_;
  std::map<std::string, Record> records_;
};

}  // namespace regs
