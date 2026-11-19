#pragma once
// What the setup program does to the PC (docs/installer.md §1, §3a): the install folder with its manifest
// (install.json) and install.cfg, the shortcuts, the Apps & features entry, update/repair from a payload, uninstall.
//
// OPENCIM_SETUP_SANDBOX=<folder> (tests): everything outside the install folder goes into that folder instead of the
// real places: <folder>\Programs\OpenCiM (the default install folder), <folder>\StartMenu, <folder>\Desktop,
// <folder>\Documents\OpenCiM, and the Apps & features entry as <folder>\uninstall-entry.txt (no registry writes).
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "setup/payload.h"

namespace regs::setup {

struct Places {
  bool sandbox = false;
  std::filesystem::path sandboxDir;
  std::filesystem::path defaultInstall;  // %LOCALAPPDATA%\Programs\OpenCiM
  std::filesystem::path startMenu;       // the Start menu's Programs folder (OpenCiM gets a folder in it)
  std::filesystem::path desktop;
  std::filesystem::path documents;       // Documents\OpenCiM (saves, maps, settings: never touched unless asked)
};
Places places();

struct ManifestFile {
  std::string path;  // relative, '/'
  std::string sha256;
  uint64_t size = 0;
};

struct Manifest {
  std::string version, channel = "stable";
  int build = 0;
  std::filesystem::path game;
  bool startMenu = true, desktop = false;
  std::vector<ManifestFile> files;

  bool load(const std::filesystem::path& installDir);
  bool save(const std::filesystem::path& installDir) const;
};

// The install the setup program belongs to or finds: its own folder when install.json is next to it, else the Apps &
// features entry's InstallLocation. Empty when OpenCiM is not installed.
std::filesystem::path findInstall(const std::filesystem::path& selfExe);

struct InstallOptions {
  std::filesystem::path target, game;
  bool startMenu = true, desktop = false;
};

struct Progress {
  std::atomic<float> fraction{0};
  std::atomic<int> step{0};  // 0 unpacking, 1 checking, 2 shortcuts, 3 done
};

// A fresh install or an update / repair over an existing one (the old manifest's files that the payload no longer has
// are removed; files the player added are left alone). Copies the stub of selfExe in as opencim-setup.exe.
bool installFromPayload(const Payload& p, const std::filesystem::path& selfExe, const InstallOptions& o,
                        Progress& progress, std::string& err);

// Files of the manifest that are missing or changed (repair checks these).
std::vector<std::string> changedFiles(const std::filesystem::path& installDir, const Manifest& m);

// Writes install.cfg and the manifest's game folder.
bool changeGameFolder(const std::filesystem::path& installDir, const std::filesystem::path& game, std::string& err);

// Removes exactly the manifest's files, the shortcuts and the Apps & features entry; Documents\OpenCiM only when
// keepDocuments is false. The running program (when it lives in installDir) is deleted by a short-lived cmd after it
// exits; deleteSelfLater says whether that was arranged.
bool uninstall(const std::filesystem::path& installDir, bool keepDocuments, const std::filesystem::path& selfExe,
               bool& deleteSelfLater, std::string& err);

std::string sha256File(const std::filesystem::path& file);
std::string sha256(const std::vector<uint8_t>& data);

}  // namespace regs::setup
