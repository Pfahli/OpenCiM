#pragma once
// Finding the player's Cities in Motion (docs/installer.md §1), shared by the setup program and regs.exe's start
// without a game path. The game's files are only looked at here, never read further, copied or changed.
#include <filesystem>
#include <string>
#include <vector>

namespace regs {

struct GameCheck {
  bool ok = false;                   // the base archives are all there
  std::vector<std::string> missing;  // base archives not found (file names)
  std::vector<std::string> dlcs;     // DLC archives found, as the player knows them ("London", "Vehicle Pack 1")
};

// The base archives the port needs and the DLCs it knows, checked in `dir` (the folder with Cities In Motion.exe).
// the folder that holds the game's archives: dir itself, or inside a Mac app bundle in it (Contents/MacOS)
std::filesystem::path gameDataFolder(const std::filesystem::path& dir);
GameCheck checkGameFolder(const std::filesystem::path& dir);

struct GameCandidate {
  std::filesystem::path dir;
  std::string source;  // "Steam", "GOG", "installed" (an uninstall entry), "install.cfg"
};

// Every place Cities in Motion may be: Steam (HKCU\Software\Valve\Steam SteamPath, HKLM InstallPath in both registry
// views, Steam's usual Program Files folders; on Linux ~/.steam, ~/.local/share/Steam, the Flatpak and the Snap; every
// library in steamapps\libraryfolders.vdf, app 73010), then (Windows) GOG and retail uninstall entries whose name is
// Cities in Motion, then the folder OpenCiM's SteamCMD helper downloads to (core/steam_download.h).
// Folders that do not exist are left out; the first one that checkGameFolder accepts is the one to use.
std::vector<GameCandidate> findGameCandidates();

// The first candidate that passes checkGameFolder, or an empty path.
std::filesystem::path findGame(std::string* source = nullptr);

// What to tell the player when findGame found nothing: not installed, or found with files missing (each place found
// and what it lacks, with Steam's "verify integrity" hint). UTF-8.
std::string gameNotFoundMessage();
// The same for a folder the player gave (it does not exist, or which archives it lacks).
std::string gameFolderMessage(const std::filesystem::path& dir, const GameCheck& check);

// install.cfg next to regs.exe (written by the setup program): `game=<folder>`, one setting per line, UTF-8.
std::filesystem::path readInstallCfgGame(const std::filesystem::path& cfgFile);
bool writeInstallCfg(const std::filesystem::path& cfgFile, const std::filesystem::path& game);

// UTF-8 <-> path (the files above and the setup program's manifest are UTF-8)
std::filesystem::path pathFromUtf8(const std::string& s);
std::string pathToUtf8(const std::filesystem::path& p);

}  // namespace regs
