#pragma once
// Getting the player's own Cities in Motion with Valve's SteamCMD (owner's request 2026-11-23, docs/installer.md §1a),
// for PCs where the Steam client cannot install it (Linux and macOS without Proton, a PC without Steam). SteamCMD is
// fetched from Valve's CDN with the system's curl and tar (Windows 10 and later, Linux and macOS ship both) and runs in
// a console of its own: the account name, password and Steam Guard code are typed into it, never into OpenCiM. The
// account must own the game (app 73010); the Windows depots are fetched on every system
// (+@sSteamCmdForcePlatformType windows), DLCs the account owns come along.
#include <filesystem>

namespace regs {

// Where the downloaded game goes by default (game_find.h looks there too): %LOCALAPPDATA%\OpenCiM\Cities In Motion,
// ~/Library/Application Support/OpenCiM/Cities In Motion, $XDG_DATA_HOME (or ~/.local/share)/OpenCiM/Cities In Motion.
std::filesystem::path steamDownloadDefaultFolder();

// Writes the download script (Windows: a .cmd, elsewhere: an sh script) beside SteamCMD's folder and returns it; empty
// when it cannot be written. SteamCMD lives in <the default folder's parent>/steamcmd.
std::filesystem::path writeSteamDownloadScript(const std::filesystem::path& gameDir);

// Runs the script in this console / terminal and waits; true when the game's archives are in gameDir afterwards.
bool runSteamDownloadHere(const std::filesystem::path& gameDir);

}  // namespace regs
