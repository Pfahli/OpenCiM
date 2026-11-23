// Cities in Motion with SteamCMD (core/steam_download.h).
#include "core/steam_download.h"

#include <cstdlib>
#include <fstream>
#include <string>

#include "core/game_find.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace regs {

namespace {

// beside the default game folder (also when the game goes elsewhere: SteamCMD stays in the per-user place)
fs::path steamCmdFolder() { return steamDownloadDefaultFolder().parent_path() / "steamcmd"; }

#if !defined(_WIN32)
std::string shellQuote(const std::string& s) {  // 'it'\''s'
  std::string out = "'";
  for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}
#endif

}  // namespace

fs::path steamDownloadDefaultFolder() {
#if defined(_WIN32)
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  fs::path base = n > 0 && n < MAX_PATH ? fs::path(buf) : fs::temp_directory_path();
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  fs::path base = fs::path(home ? home : ".") / "Library" / "Application Support";
#else
  const char* xdg = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  fs::path base = xdg && *xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".local" / "share";
#endif
  return base / "OpenCiM" / "Cities In Motion";
}

fs::path writeSteamDownloadScript(const fs::path& gameDir) {
  const fs::path sc = steamCmdFolder();
  std::error_code ec;
  fs::create_directories(sc, ec);
  fs::create_directories(gameDir, ec);
#if defined(_WIN32)
  const fs::path script = sc / "get-cities-in-motion.cmd";
  std::ofstream f(script, std::ios::binary | std::ios::trunc);
  if (!f) return {};
  const std::string scU = pathToUtf8(sc), gameU = pathToUtf8(gameDir);
  // (no parentheses in the texts inside the if blocks: cmd would end the block there)
  f << "@echo off\r\n"
       "chcp 65001 >nul\r\n"
       "title OpenCiM - Cities in Motion from Steam\r\n"
       "setlocal\r\n"
       "set \"SC=" << scU << "\"\r\n"
       "set \"GAME=" << gameU << "\"\r\n"
       "echo(\r\n"
       "echo  OpenCiM downloads YOUR copy of Cities in Motion with Valve's SteamCMD.\r\n"
       "echo  The Steam account must own the game. Account name, password and the Steam Guard\r\n"
       "echo  code are typed into SteamCMD here; OpenCiM never sees them.\r\n"
       "echo(\r\n"
       "echo  Game folder: \"%GAME%\"\r\n"
       "echo(\r\n"
       "if not exist \"%SC%\\steamcmd.exe\" (\r\n"
       "  echo  Getting SteamCMD from Valve ...\r\n"
       "  curl -L --fail -o \"%SC%\\steamcmd.zip\" https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip || goto fail\r\n"
       "  tar -xf \"%SC%\\steamcmd.zip\" -C \"%SC%\" || goto fail\r\n"
       "  del \"%SC%\\steamcmd.zip\"\r\n"
       ")\r\n"
       ":ask\r\n"
       "set \"U=\"\r\n"
       "set /p \"U= Steam account name: \"\r\n"
       "if \"%U%\"==\"\" goto ask\r\n"
       "\"%SC%\\steamcmd.exe\" +@sSteamCmdForcePlatformType windows +force_install_dir \"%GAME%\" +login %U% "
       "+app_update 73010 validate +quit\r\n"
       "if exist \"%GAME%\\data.gs\" (\r\n"
       "  echo(\r\n"
       "  echo  Done: Cities in Motion is in \"%GAME%\".\r\n"
       "  timeout /t 5 >nul\r\n"
       "  exit /b 0\r\n"
       ")\r\n"
       ":fail\r\n"
       "echo(\r\n"
       "echo  The download did not finish. Check the account name, that the account owns Cities in Motion,\r\n"
       "echo  and the internet connection, then try again.\r\n"
       "pause\r\n"
       "exit /b 1\r\n";
  return f ? script : fs::path();
#else
  const fs::path script = sc / "get-cities-in-motion.sh";
  std::ofstream f(script, std::ios::binary | std::ios::trunc);
  if (!f) return {};
  f << "#!/bin/sh\n"
       "# written by OpenCiM: downloads the player's own Cities in Motion with Valve's SteamCMD\n"
       "SC=" << shellQuote(sc.string()) << "\n"
       "GAME=" << shellQuote(gameDir.string()) << "\n"
       "echo\n"
       "echo \" OpenCiM downloads YOUR copy of Cities in Motion with Valve's SteamCMD.\"\n"
       "echo \" The Steam account must own the game. Account name, password and the Steam Guard\"\n"
       "echo \" code are typed into SteamCMD here; OpenCiM never sees them.\"\n"
       "echo\n"
       "echo \" Game folder: $GAME\"\n"
       "echo\n"
       "if [ \"$(uname -s)\" = Darwin ]; then\n"
       "  URL=https://steamcdn-a.akamaihd.net/client/installer/steamcmd_osx.tar.gz\n"
       "else\n"
       "  case \"$(uname -m)\" in\n"
       "    x86_64|amd64|i?86) ;;\n"
       "    *) echo \" SteamCMD runs on x86 PCs only. On ARM: Valve's SteamCMD under box86/FEX, or DepotDownloader\"\n"
       "       echo \" (github.com/SteamRE/DepotDownloader) built for this system: app 73010 into the folder above.\"\n"
       "       exit 1 ;;\n"
       "  esac\n"
       "  URL=https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz\n"
       "fi\n"
       "if [ ! -x \"$SC/steamcmd.sh\" ]; then\n"
       "  echo \" Getting SteamCMD from Valve ...\"\n"
       "  mkdir -p \"$SC\" && curl -L --fail -o \"$SC/steamcmd.tar.gz\" \"$URL\" && tar -xzf \"$SC/steamcmd.tar.gz\" -C \"$SC\" &&\n"
       "    rm -f \"$SC/steamcmd.tar.gz\" || { echo \" Could not get SteamCMD (curl and tar needed).\"; exit 1; }\n"
       "fi\n"
       "U=\n"
       "while [ -z \"$U\" ]; do printf ' Steam account name: '; read U || exit 1; done\n"
       "\"$SC/steamcmd.sh\" +@sSteamCmdForcePlatformType windows +force_install_dir \"$GAME\" +login \"$U\" "
       "+app_update 73010 validate +quit\n"
       "if [ -f \"$GAME/data.gs\" ]; then echo; echo \" Done: Cities in Motion is in $GAME\"; exit 0; fi\n"
       "echo; echo \" The download did not finish. Check the account name, that the account owns Cities in Motion,\"\n"
       "echo \" and the internet connection (SteamCMD on 64-bit Linux also needs the 32-bit C libraries).\"\n"
       "exit 1\n";
  f.close();
  fs::permissions(script, fs::perms::owner_all, fs::perm_options::add, ec);
  return script;
#endif
}

bool runSteamDownloadHere(const fs::path& gameDir) {
  const fs::path script = writeSteamDownloadScript(gameDir);
  if (script.empty()) return false;
#if defined(_WIN32)
  const std::wstring cmd = L"cmd /c \"\"" + script.wstring() + L"\"\"";
  _wsystem(cmd.c_str());
#else
  const std::string cmd = "sh " + shellQuote(script.string());
  if (std::system(cmd.c_str()) == -1) return false;
#endif
  return checkGameFolder(gameDir).ok;
}

}  // namespace regs
