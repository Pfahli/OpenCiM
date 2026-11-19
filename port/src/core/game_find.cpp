#include "core/game_find.h"

#include <algorithm>
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
  fs::path canon = fs::weakly_canonical(c.dir, ec);
  if (!ec) c.dir = canon;
  for (const GameCandidate& o : list) {
    std::wstring a = o.dir.wstring(), b = c.dir.wstring();
    if (a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
          return std::towlower(x) == std::towlower(y);
        }))
      return;
  }
  list.push_back(std::move(c));
}

}  // namespace

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
#if defined(_WIN32)
  std::vector<fs::path> steamRoots;
  std::wstring steam = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
  if (!steam.empty()) steamRoots.push_back(fs::path(steam));
  std::wstring steamMachine = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY);
  if (!steamMachine.empty()) steamRoots.push_back(fs::path(steamMachine));
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
  std::vector<GameCandidate> entries;
  uninstallEntries(entries);
  for (GameCandidate& c : entries) addUnique(out, std::move(c));
#endif
  return out;
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
