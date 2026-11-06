#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "script/vm.h"

namespace regs {
class Vfs;
}

namespace regs::engine {

// OpenCiM mods, first step of port/docs/mod-manager.md §8: mod folders with a mod.json manifest, found in the
// port's built-in mods folder (data/mods next to the executable, shipped disabled) and the user mods folder
// (<user root>\mods, i.e. Documents\OpenCiM\mods; --mods <dir> stands in for it). The user folder's mods.json lists
// the enabled mods in load order. Each enabled mod is mounted as VFS layers (its folder under regs/mods/<id>/, its
// files/ over the game and the port's data) and its extend/ scripts chain after the port's own (VM::extendSource).
struct ModInfo {
  std::string id, name, version, saves;
  std::filesystem::path dir;
  bool builtIn = false;
  bool enabled = false;
  std::string vfsRoot() const { return "regs/mods/" + id + "/"; }
};

// The port's user root (settings, saves, mods): host().userFolder once init_userdata ran, before that
// <documents>\OpenCiM (engine/user_folders.h). The mods folder and its mods.json live below it.
std::filesystem::path modsUserRoot();
std::filesystem::path userModsFolder();  // host().modsDir (--mods) or <user root>\mods

// Scans both folders, reads the manifests and mods.json, and mounts the enabled mods in load order (call before the
// first rescan). Problems are logged and skip the mod, never fatal.
void loadMods(Vfs& vfs, script::VM& vm);
const std::vector<ModInfo>& mods();

// regsMods(): the enabled mods in load order, each a map {id, name, version, path (its VFS root), builtIn}.
void registerModNatives(script::VM& vm);

}  // namespace regs::engine
