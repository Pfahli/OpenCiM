#include "core/game_find.h"
#include "core/steam_download.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <sstream>

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

// the archives without which the game's menus, maps or objects cannot load (the port mounts the root .gs files)
const char* const kBaseArchives[] = {"data.gs", "metro.gs", "objects.gs", "maps.gs", "sounds.gs", "location-data.gs"};

struct Dlc {
  const char* file;
  const char* name;
};
const Dlc kDlcs[] = {
    {"london.gs", "London"},           {"paris.gs", "Paris"},
    {"munich.gs", "Munich"},           {"tokyo.gs", "Tokyo"},
    {"ulm.gs", "Ulm"},                 {"stpetersburg.gs", "St. Petersburg"},
    {"usa.gs", "U.S. Cities"},         {"germany.gs", "German Cities"},
    {"mappack01.gs", "Metro Madness"}, {"stationpack01.gs", "Metro Stations"},
    {"vehiclepack01.gs", "Design Classics"}, {"vehiclepack02.gs", "Design Marvels"},
    {"vehiclepack03.gs", "Design Now"}, {"vehiclepack04.gs", "Design Quirks"},
    {"vehiclepack05.gs", "Hop On Hop Off"},
};

fs::path fromUtf8(const std::string& s) {
#if defined(_WIN32)
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return fs::path(w);
#else
  return fs::path(s);
#endif
}

std::string toUtf8(const fs::path& p) {
#if defined(_WIN32)
  const std::wstring& w = p.native();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
#else
  return p.string();
#endif
}

bool isFile(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

bool isDir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

#if defined(_WIN32)
std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view = 0) {
  HKEY h;
  if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &h) != ERROR_SUCCESS) return {};
  wchar_t buf[2048];
  DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
  std::wstring out;
  if (RegQueryValueExW(h, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
      (type == REG_SZ || type == REG_EXPAND_SZ)) {
    buf[size / sizeof(wchar_t)] = 0;
    out = buf;
  }
  RegCloseKey(h);
  return out;
}

// every uninstall entry (both registry views, machine and user) whose DisplayName starts with "Cities in Motion"
void uninstallEntries(std::vector<GameCandidate>& out) {
  struct Root {
    HKEY root;
    REGSAM view;
  };
  const Root roots[] = {{HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY},
                        {HKEY_CURRENT_USER, 0}};
  const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
  for (const Root& r : roots) {
    HKEY h;
    if (RegOpenKeyExW(r.root, base, 0, KEY_READ | r.view, &h) != ERROR_SUCCESS) continue;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
      DWORD len = 512;
      if (RegEnumKeyExW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
      std::wstring key = std::wstring(base) + L"\\" + name;
      std::wstring display = regString(r.root, key.c_str(), L"DisplayName", r.view);
      std::wstring lower = display;
      for (wchar_t& c : lower) c = wchar_t(std::towlower(c));
      if (lower.rfind(L"cities in motion", 0) != 0 || lower.find(L"cities in motion 2") == 0) continue;
      std::wstring dir = regString(r.root, key.c_str(), L"InstallLocation", r.view);
      if (dir.empty()) continue;
      bool gog = std::wstring(name).find(L"_is1") != std::wstring::npos &&
                 regString(r.root, key.c_str(), L"Publisher", r.view).find(L"GOG") != std::wstring::npos;
      out.push_back({fs::path(dir), gog ? "GOG" : "installed"});
    }
    RegCloseKey(h);
  }
}
#endif

// "path" values of a Steam libraryfolders.vdf (keys and values are quoted; backslashes doubled)
std::vector<fs::path> steamLibraries(const fs::path& vdf) {
  std::vector<fs::path> out;
  std::ifstream in(vdf, std::ios::binary);
  if (!in) return out;
  std::stringstream ss;
  ss << in.rdbuf();
  std::string t = ss.str();
  size_t pos = 0;
  while ((pos = t.find("\"path\"", pos)) != std::string::npos) {
    size_t a = t.find('"', pos + 6);
    if (a == std::string::npos) break;
    std::string v;
    size_t i = a + 1;
    for (; i < t.size() && t[i] != '"'; ++i) {
      if (t[i] == '\\' && i + 1 < t.size()) ++i;  // \\ → \ and \" → "
      v += t[i];
    }
    pos = i + 1;
    if (!v.empty()) out.push_back(fromUtf8(v));
  }
  return out;
}

void addUnique(std::vector<GameCandidate>& list, GameCandidate c) {
  std::error_code ec;
  if (!isDir(c.dir)) return;
  c.dir = gameDataFolder(c.dir);  // the Mac version: the archives inside its app bundle
  fs::path canon = fs::weakly_canonical(c.dir, ec);
  if (!ec) c.dir = canon;
  for (const GameCandidate& o : list) {
#if defined(_WIN32)
    std::wstring a = o.dir.wstring(), b = c.dir.wstring();
    if (a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
          return std::towlower(x) == std::towlower(y);
        }))
      return;
#else
    if (o.dir == c.dir) return;  // (canonical already; case matters on Linux)
#endif
  }
  list.push_back(std::move(c));
}

}  // namespace

// the Mac version of the game keeps its archives inside the app bundle (Cities In Motion.app/Contents/MacOS, where
// players put addons and DLC files): a folder without data.gs but with an .app holding it means that folder
fs::path gameDataFolder(const fs::path& dir) {
  if (!isDir(dir) || isFile(dir / "data.gs")) return dir;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".app") continue;
    for (const char* sub : {"Contents/MacOS", "Contents/Resources", "Contents/Resources/data"})
      if (isFile(e.path() / sub / "data.gs")) return e.path() / sub;
  }
  return dir;
}

GameCheck checkGameFolder(const fs::path& dir) {
  GameCheck r;
  if (!isDir(dir)) {
    for (const char* a : kBaseArchives) r.missing.push_back(a);
    return r;
  }
  for (const char* a : kBaseArchives)
    if (!isFile(dir / a)) r.missing.push_back(a);
  for (const Dlc& d : kDlcs)
    if (isFile(dir / d.file)) r.dlcs.push_back(d.name);
  r.ok = r.missing.empty();
  return r;
}

std::vector<GameCandidate> findGameCandidates() {
  std::vector<GameCandidate> out;
  std::vector<fs::path> steamRoots;
#if defined(_WIN32)
  std::wstring steam = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
  if (!steam.empty()) steamRoots.push_back(fs::path(steam));
  for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
    std::wstring machine = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", view);
    if (!machine.empty()) steamRoots.push_back(fs::path(machine));
  }
  // Steam's usual folders, for an install whose registry entries are gone (a reinstalled Windows, a copied library)
  for (const wchar_t* var : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) steamRoots.push_back(fs::path(buf) / "Steam");
  }
#elif defined(__APPLE__)
  // macOS: Steam's folder in Application Support (the game's Mac and Windows depots both land in steamapps/common)
  if (const char* home = std::getenv("HOME"); home && *home)
    steamRoots.push_back(fs::path(home) / "Library" / "Application Support" / "Steam");
#else
  // Linux: the native client (~/.steam/steam, ~/.steam/root and ~/.local/share/Steam, or $XDG_DATA_HOME/Steam), the
  // Flatpak and the Snap; the game itself runs there under Proton but its files sit in the same steamapps/common
  if (const char* home = std::getenv("HOME"); home && *home) {
    const fs::path h(home);
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) steamRoots.push_back(fs::path(xdg) / "Steam");
    for (const char* rel : {".steam/steam", ".steam/root", ".local/share/Steam",
                            ".var/app/com.valvesoftware.Steam/.local/share/Steam", "snap/steam/common/.local/share/Steam"})
      steamRoots.push_back(h / rel);
  }
#endif
  std::vector<fs::path> libraries;
  for (const fs::path& root : steamRoots) {
    libraries.push_back(root);
    for (const fs::path& lib : steamLibraries(root / "steamapps" / "libraryfolders.vdf")) libraries.push_back(lib);
  }
  for (const fs::path& lib : libraries) {
    fs::path apps = lib / "steamapps";
    fs::path dir = apps / "common" / "Cities In Motion";
    // the app manifest names the folder; the default name when it cannot be read
    std::ifstream acf(apps / "appmanifest_73010.acf", std::ios::binary);
    if (acf) {
      std::stringstream ss;
      ss << acf.rdbuf();
      std::string t = ss.str();
      size_t k = t.find("\"installdir\"");
      if (k != std::string::npos) {
        size_t a = t.find('"', k + 12), b = a == std::string::npos ? a : t.find('"', a + 1);
        if (b != std::string::npos) dir = apps / "common" / fromUtf8(t.substr(a + 1, b - a - 1));
      }
    }
    addUnique(out, {dir, "Steam"});
  }
#if defined(_WIN32)
  std::vector<GameCandidate> entries;
  uninstallEntries(entries);
  for (GameCandidate& c : entries) addUnique(out, std::move(c));
#endif
  addUnique(out, {steamDownloadDefaultFolder(), "SteamCMD"});  // fetched with OpenCiM's SteamCMD helper
  return out;
}

std::string gameNotFoundMessage() {
  const std::vector<GameCandidate> found = findGameCandidates();
  const char* verify =
      "In Steam: right-click Cities in Motion in the library > Properties > Installed Files > Verify integrity of game "
      "files.";
  if (found.empty())
    return "OpenCiM needs Cities in Motion (Steam, GOG or the disc version), but it was not found on this PC.\n\n"
           "Install it, or start OpenCiM with the game's folder (the one with Cities In Motion.exe) as its first "
           "argument, or run the OpenCiM setup again to choose the folder.\n\n"
           "Without the Steam client (or where it cannot install the game): \"regs --download-game\" fetches your own "
           "copy with Valve's SteamCMD (the setup program has the same as DOWNLOAD FROM STEAM).";
  std::string s = "Cities in Motion was found, but files the game needs are missing:\n";
  for (const GameCandidate& c : found) {
    const GameCheck check = checkGameFolder(c.dir);
    s += "\n" + toUtf8(c.dir) + " (" + c.source + "): ";
    for (size_t i = 0; i < check.missing.size(); ++i) s += (i ? ", " : "") + check.missing[i];
  }
  s += "\n\n";
  s += verify;
  return s;
}

std::string gameFolderMessage(const fs::path& dir, const GameCheck& check) {
  if (!isDir(dir)) return "The game folder given to OpenCiM does not exist:\n" + toUtf8(dir);
  std::string s = "The folder given to OpenCiM is not a complete Cities in Motion folder:\n" + toUtf8(dir) +
                  "\n\nMissing: ";
  for (size_t i = 0; i < check.missing.size(); ++i) s += (i ? ", " : "") + check.missing[i];
  return s + "\n\nIt is the folder with Cities In Motion.exe and data.gs.";
}

fs::path findGame(std::string* source) {
  for (const GameCandidate& c : findGameCandidates())
    if (checkGameFolder(c.dir).ok) {
      if (source) *source = c.source;
      return c.dir;
    }
  return {};
}

fs::path readInstallCfgGame(const fs::path& cfgFile) {
  std::ifstream in(cfgFile, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("game=", 0) == 0) return fromUtf8(line.substr(5));
  }
  return {};
}

bool writeInstallCfg(const fs::path& cfgFile, const fs::path& game) {
  std::ofstream out(cfgFile, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  std::string g = toUtf8(game);
  out << "# written by the OpenCiM setup program: the Cities in Motion folder regs.exe uses\r\ngame=" << g << "\r\n";
  return bool(out);
}

}  // namespace regs

namespace regs {
std::filesystem::path pathFromUtf8(const std::string& s) { return fromUtf8(s); }
std::string pathToUtf8(const std::filesystem::path& p) { return toUtf8(p); }
}  // namespace regs
