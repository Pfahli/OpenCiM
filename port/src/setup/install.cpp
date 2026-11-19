#include "setup/install.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "core/game_find.h"

namespace fs = std::filesystem;

namespace regs::setup {

namespace {

const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OpenCiM";

fs::path knownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
  CoTaskMemFree(p);
  return out;
}

std::string hex(const uint8_t* b, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) s += d[b[i] >> 4], s += d[b[i] & 15];
  return s;
}

// --- a small JSON reader for install.json (objects, arrays, strings, numbers, true/false) ---
struct Json {
  enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::map<std::string, Json> o;
  const Json& at(const std::string& k) const {
    static Json none;
    auto it = o.find(k);
    return it == o.end() ? none : it->second;
  }
};

struct JsonReader {
  const std::string& t;
  size_t i = 0;
  void ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
  }
  bool str(std::string& out) {
    if (t[i] != '"') return false;
    ++i;
    while (i < t.size() && t[i] != '"') {
      char c = t[i++];
      if (c == '\\' && i < t.size()) {
        char e = t[i++];
        if (e == 'n') c = '\n';
        else if (e == 't') c = '\t';
        else if (e == 'u' && i + 4 <= t.size()) {
          unsigned v = unsigned(std::stoul(t.substr(i, 4), nullptr, 16));
          i += 4;
          if (v < 0x80) out += char(v);
          else if (v < 0x800) out += char(0xC0 | (v >> 6)), out += char(0x80 | (v & 63));
          else out += char(0xE0 | (v >> 12)), out += char(0x80 | ((v >> 6) & 63)), out += char(0x80 | (v & 63));
          continue;
        } else c = e;
      }
      out += c;
    }
    if (i >= t.size()) return false;
    ++i;
    return true;
  }
  bool value(Json& v) {
    ws();
    if (i >= t.size()) return false;
    char c = t[i];
    if (c == '{') {
      v.kind = Json::Obj;
      ++i;
      ws();
      if (t[i] == '}') return ++i, true;
      for (;;) {
        ws();
        std::string k;
        if (!str(k)) return false;
        ws();
        if (t[i++] != ':') return false;
        if (!value(v.o[k])) return false;
        ws();
        if (t[i] == ',') { ++i; continue; }
        if (t[i] == '}') return ++i, true;
        return false;
      }
    }
    if (c == '[') {
      v.kind = Json::Arr;
      ++i;
      ws();
      if (t[i] == ']') return ++i, true;
      for (;;) {
        v.a.emplace_back();
        if (!value(v.a.back())) return false;
        ws();
        if (t[i] == ',') { ++i; continue; }
        if (t[i] == ']') return ++i, true;
        return false;
      }
    }
    if (c == '"') return v.kind = Json::Str, str(v.s);
    if (t.compare(i, 4, "true") == 0) return v.kind = Json::Bool, v.b = true, i += 4, true;
    if (t.compare(i, 5, "false") == 0) return v.kind = Json::Bool, i += 5, true;
    if (t.compare(i, 4, "null") == 0) return i += 4, true;
    size_t end = i;
    while (end < t.size() && (std::isdigit(uint8_t(t[end])) || t[end] == '-' || t[end] == '.' || t[end] == 'e' ||
                              t[end] == 'E' || t[end] == '+'))
      ++end;
    if (end == i) return false;
    v.kind = Json::Num;
    v.n = std::stod(t.substr(i, end - i));
    i = end;
    return true;
  }
};

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (c == '\n') o += "\\n";
    else if (uint8_t(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(uint8_t(c)));
      o += buf;
    } else o += c;
  }
  return o;
}

void regSetString(HKEY k, const wchar_t* name, const std::wstring& v) {
  RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
}

void regSetDword(HKEY k, const wchar_t* name, DWORD v) {
  RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
}

std::wstring quoted(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

// the Apps & features entry (HKCU, per user: no admin rights), or its stand-in file in the sandbox
void writeUninstallEntry(const Places& pl, const fs::path& dir, const Manifest& m, uint64_t bytes) {
  fs::path setupExe = dir / "opencim-setup.exe";
  std::wstring version = pathFromUtf8(m.version).wstring();
  if (pl.sandbox) {
    std::ofstream o(pl.sandboxDir / "uninstall-entry.txt", std::ios::binary | std::ios::trunc);
    o << "DisplayName=OpenCiM\nDisplayVersion=" << m.version << "\nInstallLocation=" << pathToUtf8(dir)
      << "\nUninstallString=" << pathToUtf8(setupExe) << " --uninstall\nEstimatedSizeKB=" << bytes / 1024 << "\n";
    return;
  }
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
    return;
  regSetString(k, L"DisplayName", L"OpenCiM");
  regSetString(k, L"DisplayVersion", version);
  regSetString(k, L"Publisher", L"OpenCiM project");
  regSetString(k, L"InstallLocation", dir.wstring());
  regSetString(k, L"DisplayIcon", (dir / "regs.exe").wstring());
  regSetString(k, L"UninstallString", quoted(setupExe) + L" --uninstall");
  regSetString(k, L"QuietUninstallString", quoted(setupExe) + L" --silent-uninstall");
  regSetString(k, L"ModifyPath", quoted(setupExe));
  regSetString(k, L"Comments", L"Unofficial engine for Cities in Motion; needs the original game");
  regSetDword(k, L"EstimatedSize", DWORD(bytes / 1024));
  regSetDword(k, L"NoRepair", 1);  // repair is on the setup's own page (Modify)
  RegCloseKey(k);
}

fs::path uninstallEntryLocation(const Places& pl) {
  if (pl.sandbox) {
    std::ifstream in(pl.sandboxDir / "uninstall-entry.txt", std::ios::binary);
    std::string line;
    while (std::getline(in, line))
      if (line.rfind("InstallLocation=", 0) == 0) return pathFromUtf8(line.substr(16));
    return {};
  }
  wchar_t buf[2048];
  DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
  fs::path out;
  if (RegQueryValueExW(k, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
      type == REG_SZ) {
    buf[size / sizeof(wchar_t)] = 0;
    out = buf;
  }
  RegCloseKey(k);
  return out;
}

void removeUninstallEntry(const Places& pl) {
  if (pl.sandbox) {
    std::error_code ec;
    fs::remove(pl.sandboxDir / "uninstall-entry.txt", ec);
    return;
  }
  RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
}

bool makeShortcut(const fs::path& lnk, const fs::path& target, const fs::path& workDir, const std::wstring& args,
                  const std::wstring& description) {
  std::error_code ec;
  fs::create_directories(lnk.parent_path(), ec);
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
  sl->SetPath(target.c_str());
  sl->SetWorkingDirectory(workDir.c_str());
  if (!args.empty()) sl->SetArguments(args.c_str());
  sl->SetDescription(description.c_str());
  sl->SetIconLocation(target.c_str(), 0);
  IPersistFile* pf = nullptr;
  bool ok = SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
  if (pf) pf->Release();
  sl->Release();
  return ok;
}

void makeShortcuts(const Places& pl, const fs::path& dir, bool startMenu, bool desktop) {
  fs::path game = dir / "regs.exe", setup = dir / "opencim-setup.exe";
  std::error_code ec;
  fs::path menu = pl.startMenu / "OpenCiM";
  if (startMenu) {
    makeShortcut(menu / "OpenCiM.lnk", game, dir, L"", L"Cities in Motion with the OpenCiM engine");
    makeShortcut(menu / "OpenCiM Setup.lnk", setup, dir, L"", L"Update, repair or uninstall OpenCiM");
  } else {
    fs::remove_all(menu, ec);
  }
  fs::path d = pl.desktop / "OpenCiM.lnk";
  if (desktop) makeShortcut(d, game, dir, L"", L"Cities in Motion with the OpenCiM engine");
  else fs::remove(d, ec);
}

void removeShortcuts(const Places& pl) {
  std::error_code ec;
  fs::remove_all(pl.startMenu / "OpenCiM", ec);
  fs::remove(pl.desktop / "OpenCiM.lnk", ec);
}

bool writeFile(const fs::path& p, const std::vector<uint8_t>& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += L".new";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!o) return false;
  }
  if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

// removes the folders above `file` up to (not including) `stop` while they are empty
void pruneEmpty(fs::path dir, const fs::path& stop) {
  std::error_code ec;
  while (!dir.empty() && dir != stop && dir.native().size() > stop.native().size()) {
    if (!fs::is_directory(dir, ec) || !fs::is_empty(dir, ec)) break;
    fs::remove(dir, ec);
    dir = dir.parent_path();
  }
}

bool safeRelative(const std::string& rel) {
  if (rel.empty() || rel[0] == '/' || rel.find(':') != std::string::npos) return false;
  size_t s = 0;
  while (s <= rel.size()) {
    size_t e = rel.find('/', s);
    if (e == std::string::npos) e = rel.size();
    std::string part = rel.substr(s, e - s);
    if (part.empty() || part == "." || part == "..") return false;
    s = e + 1;
  }
  return rel.find('\\') == std::string::npos;
}

}  // namespace

Places places() {
  Places p;
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"OPENCIM_SETUP_SANDBOX", buf, DWORD(std::size(buf)));
  if (n > 0 && n < std::size(buf)) {
    p.sandbox = true;
    p.sandboxDir = fs::absolute(fs::path(buf));
    p.defaultInstall = p.sandboxDir / "Programs" / "OpenCiM";
    p.startMenu = p.sandboxDir / "StartMenu";
    p.desktop = p.sandboxDir / "Desktop";
    p.documents = p.sandboxDir / "Documents" / "OpenCiM";
    return p;
  }
  p.defaultInstall = knownFolder(FOLDERID_LocalAppData) / "Programs" / "OpenCiM";
  p.startMenu = knownFolder(FOLDERID_Programs);
  p.desktop = knownFolder(FOLDERID_Desktop);
  p.documents = knownFolder(FOLDERID_Documents) / "OpenCiM";
  return p;
}

std::string sha256(const std::vector<uint8_t>& data) {
  uint8_t h[32] = {};
  if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()), ULONG(data.size()), h, 32) != 0)
    return {};
  return hex(h, 32);
}

std::string sha256File(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return {};
  BCRYPT_HASH_HANDLE hh = nullptr;
  if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hh, nullptr, 0, nullptr, 0, 0) != 0) return {};
  std::vector<char> buf(1 << 20);
  while (in) {
    in.read(buf.data(), std::streamsize(buf.size()));
    std::streamsize got = in.gcount();
    if (got > 0) BCryptHashData(hh, reinterpret_cast<PUCHAR>(buf.data()), ULONG(got), 0);
  }
  uint8_t h[32] = {};
  BCryptFinishHash(hh, h, 32, 0);
  BCryptDestroyHash(hh);
  return hex(h, 32);
}

bool Manifest::load(const fs::path& dir) {
  std::ifstream in(dir / "install.json", std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  std::string t = ss.str();
  Json j;
  JsonReader r{t};
  if (!r.value(j) || j.kind != Json::Obj) return false;
  version = j.at("version").s;
  build = int(j.at("build").n);
  channel = j.at("channel").s.empty() ? "stable" : j.at("channel").s;
  game = pathFromUtf8(j.at("game").s);
  startMenu = j.at("startMenu").kind == Json::Bool ? j.at("startMenu").b : true;
  desktop = j.at("desktop").b;
  files.clear();
  for (const Json& f : j.at("files").a) files.push_back({f.at("path").s, f.at("sha256").s, uint64_t(f.at("size").n)});
  return true;
}

bool Manifest::save(const fs::path& dir) const {
  std::string o = "{\n";
  o += "  \"version\": \"" + jsonEscape(version) + "\",\n";
  o += "  \"build\": " + std::to_string(build) + ",\n";
  o += "  \"channel\": \"" + jsonEscape(channel) + "\",\n";
  o += "  \"game\": \"" + jsonEscape(pathToUtf8(game)) + "\",\n";
  o += std::string("  \"startMenu\": ") + (startMenu ? "true" : "false") + ",\n";
  o += std::string("  \"desktop\": ") + (desktop ? "true" : "false") + ",\n";
  o += "  \"files\": [\n";
  for (size_t i = 0; i < files.size(); ++i) {
    const ManifestFile& f = files[i];
    o += "    {\"path\": \"" + jsonEscape(f.path) + "\", \"size\": " + std::to_string(f.size) + ", \"sha256\": \"" +
         f.sha256 + "\"}" + (i + 1 < files.size() ? ",\n" : "\n");
  }
  o += "  ]\n}\n";
  return writeFile(dir / "install.json", std::vector<uint8_t>(o.begin(), o.end()));
}

fs::path findInstall(const fs::path& selfExe) {
  std::error_code ec;
  fs::path own = selfExe.parent_path();
  if (fs::is_regular_file(own / "install.json", ec)) return own;
  fs::path reg = uninstallEntryLocation(places());
  if (!reg.empty() && fs::is_regular_file(reg / "install.json", ec)) return reg;
  return {};
}

bool installFromPayload(const Payload& p, const fs::path& selfExe, const InstallOptions& o, Progress& progress,
                        std::string& err) {
  std::error_code ec;
  const Places pl = places();
  fs::create_directories(o.target, ec);
  if (ec) {
    err = "Cannot create " + pathToUtf8(o.target) + ": " + ec.message();
    return false;
  }
  Manifest old;
  bool hadOld = old.load(o.target);
  Manifest m;
  m.version = p.version;
  m.build = p.build;
  m.game = o.game;
  m.startMenu = o.startMenu;
  m.desktop = o.desktop;
  uint64_t total = std::max<uint64_t>(1, p.appBytes()), done = 0;
  std::vector<uint8_t> data;
  progress.step = 0;
  for (const PayloadEntry& e : p.entries) {
    // the release files, and the maintenance page's picture (the installed setup program has no payload)
    std::string rel = e.path.rfind("app/", 0) == 0 ? e.path.substr(4)
                      : e.path == "show/bg-maintenance.jpg" ? std::string("setup/maintenance.jpg")
                                                             : std::string();
    if (rel.empty()) continue;
    if (!safeRelative(rel)) continue;
    if (!p.read(e, data)) {
      err = "The setup program is damaged (" + rel + "). Download it again.";
      return false;
    }
    fs::path dst = o.target / pathFromUtf8(rel);
    std::string hash = sha256(data);
    // an update or repair leaves files that are already right alone
    bool same = fs::is_regular_file(dst, ec) && fs::file_size(dst, ec) == data.size() && sha256File(dst) == hash;
    if (!same && !writeFile(dst, data)) {
      err = "Cannot write " + pathToUtf8(dst) + ". Is OpenCiM still running?";
      return false;
    }
    m.files.push_back({rel, hash, data.size()});
    done += e.size;
    progress.fraction = 0.9f * float(double(done) / double(total));
  }
  progress.step = 1;
  // the old version's files this one no longer has
  if (hadOld) {
    std::set<std::string> now;
    for (const ManifestFile& f : m.files) now.insert(f.path);
    for (const ManifestFile& f : old.files)
      if (!now.count(f.path) && safeRelative(f.path)) {
        fs::path gone = o.target / pathFromUtf8(f.path);
        fs::remove(gone, ec);
        pruneEmpty(gone.parent_path(), o.target);
      }
  }
  // the setup program itself (without its payload), for Apps & features and the Start menu
  fs::path setupDst = o.target / "opencim-setup.exe";
  if (!fs::equivalent(selfExe, setupDst, ec)) {
    std::ifstream in(selfExe, std::ios::binary);
    std::vector<uint8_t> stub(size_t(stubSize(selfExe)));
    in.read(reinterpret_cast<char*>(stub.data()), std::streamsize(stub.size()));
    if (!in || !writeFile(setupDst, stub)) {
      err = "Cannot write " + pathToUtf8(setupDst);
      return false;
    }
  }
  if (!writeInstallCfg(o.target / "install.cfg", o.game)) {
    err = "Cannot write install.cfg";
    return false;
  }
  if (!m.save(o.target)) {
    err = "Cannot write install.json";
    return false;
  }
  progress.fraction = 0.95f;
  progress.step = 2;
  makeShortcuts(pl, o.target, o.startMenu, o.desktop);
  writeUninstallEntry(pl, o.target, m, total);
  progress.fraction = 1.0f;
  progress.step = 3;
  return true;
}

std::vector<std::string> changedFiles(const fs::path& dir, const Manifest& m) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const ManifestFile& f : m.files) {
    fs::path p = dir / pathFromUtf8(f.path);
    if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) != f.size || sha256File(p) != f.sha256)
      out.push_back(f.path);
  }
  return out;
}

bool changeGameFolder(const fs::path& dir, const fs::path& game, std::string& err) {
  Manifest m;
  if (!m.load(dir)) {
    err = "No OpenCiM install in " + pathToUtf8(dir);
    return false;
  }
  m.game = game;
  if (!writeInstallCfg(dir / "install.cfg", game) || !m.save(dir)) {
    err = "Cannot write to " + pathToUtf8(dir);
    return false;
  }
  return true;
}

bool uninstall(const fs::path& dir, bool keepDocuments, const fs::path& selfExe, bool& deleteSelfLater,
               std::string& err) {
  deleteSelfLater = false;
  Manifest m;
  if (!m.load(dir)) {
    err = "No OpenCiM install in " + pathToUtf8(dir);
    return false;
  }
  const Places pl = places();
  std::error_code ec;
  std::vector<std::string> left;
  for (const ManifestFile& f : m.files) {
    if (!safeRelative(f.path)) continue;
    fs::path p = dir / pathFromUtf8(f.path);
    fs::remove(p, ec);
    if (ec) left.push_back(f.path);
    pruneEmpty(p.parent_path(), dir);
  }
  fs::remove(dir / "install.cfg", ec);
  removeShortcuts(pl);
  removeUninstallEntry(pl);
  if (!keepDocuments) fs::remove_all(pl.documents, ec);
  fs::path setup = dir / "opencim-setup.exe";
  bool self = fs::equivalent(selfExe, setup, ec);
  fs::remove(dir / "install.json", ec);
  if (self) {
    // a program cannot delete itself: a hidden cmd waits a moment, then removes it and the empty folder
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q " + quoted(setup) + L" & rmdir " + quoted(dir);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      deleteSelfLater = true;
    }
  } else {
    fs::remove(setup, ec);
    pruneEmpty(dir, dir.parent_path());
  }
  if (!left.empty()) {
    err = std::to_string(left.size()) + " files could not be removed (is OpenCiM still running?), e.g. " + left[0];
    return false;
  }
  return true;
}

}  // namespace regs::setup
