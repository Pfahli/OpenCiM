#pragma once
#include <filesystem>
#include <string>

#include "script/vm.h"

namespace regs {
class Vfs;
}

namespace regs::engine {

// Where OpenCiM keeps player data (port/README "Where data lives"). The game's metro/config.script runs
// init_userdata(getMyDocumentsFolder()+"/Cities In Motion"); the port answers that one call with its own folder
// <documents>\OpenCiM, so settings.script, controls.keys, state.gs, saves\, maps\ and log_metro.txt all live there and
// the first start gets the game's defaults (config.script copies metro/defaults/* into an empty folder). The
// original's <documents>\Cities In Motion is only read: its saves\*.gs and maps\* show up in the load lists with a
// note and are copied into the OpenCiM folder when the player opens one (regsImportOriginal).
inline constexpr wchar_t kPortFolderName[] = L"OpenCiM";
inline constexpr wchar_t kOriginalFolderName[] = L"Cities In Motion";
// VFS prefix of the original's saves and maps (Vfs::setOriginalRoot; read-only, outside "userdata/")
inline constexpr char kOriginalVfsPrefix[] = "regs-cim/";
// the port's record of what it copied from the original folder (in the OpenCiM folder)
inline constexpr wchar_t kImportRecordName[] = L"regs-imports.txt";

// init_userdata(requested): the folder to use instead. When requested is <documents>\Cities In Motion it is
// <documents>\OpenCiM, and host().userFolder / originalFolder and the VFS's original root are set; any other folder
// (a mod's own) is used as asked.
std::filesystem::path redirectUserdata(Vfs& vfs, const std::filesystem::path& requested);

// regsOriginalSaves() / regsOriginalMaps(): the original's saves (names without .gs) and maps (file names with .grid)
// that have no copy in OpenCiM yet; regsImportOriginal("save"|"map", name): copies one into OpenCiM (or finds the
// copy already there) and returns its OpenCiM name ("" on failure). Clash rule in user_folders.cpp.
void registerUserFolderNatives(script::VM& vm);

}  // namespace regs::engine
