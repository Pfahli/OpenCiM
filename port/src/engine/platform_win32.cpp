#include "engine/platform.h"

#include <algorithm>
#include <cstdlib>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

namespace regs::engine {

std::vector<DisplayMode> platformDisplayModes() {
  std::vector<DisplayMode> out;
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
    DisplayMode m{int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmBitsPerPel), int(dm.dmDisplayFrequency)};
    bool dup = std::any_of(out.begin(), out.end(), [&](const DisplayMode& o) {
      return o.width == m.width && o.height == m.height && o.bits == m.bits && o.freq == m.freq;
    });
    if (!dup) out.push_back(m);
  }
  return out;
}

DisplayMode platformCurrentDisplayMode() {
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) return {};
  return {int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmBitsPerPel), int(dm.dmDisplayFrequency)};
}

std::filesystem::path platformDocumentsFolder() {
  wchar_t buf[MAX_PATH] = {};
  if (SHGetSpecialFolderPathW(nullptr, buf, 0x0c /* CSIDL_MYDOCUMENTS */, FALSE) ||
      SHGetSpecialFolderPathW(nullptr, buf, CSIDL_PERSONAL, FALSE))
    return buf;
  const wchar_t* profile = _wgetenv(L"USERPROFILE");
  return profile ? std::filesystem::path(profile) : std::filesystem::path();
}

}  // namespace regs::engine

namespace regs::engine {

void platformShowInExplorer(const std::filesystem::path& path, bool select) {
  std::wstring quoted = L"\"" + path.wstring() + L"\"";
  std::wstring args = select ? L"/select," + quoted : quoted;
  ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

}  // namespace regs::engine

#include <wincodec.h>
#include <winhttp.h>

#include "build_info.h"

namespace regs::engine {

namespace {
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
}  // namespace

int platformHttpRequest(const std::string& url, const std::string* postBody, std::vector<uint8_t>& out,
                        std::string* error) {
  out.clear();
  std::wstring wurl = widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  wchar_t host[256] = {}, path[4096] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 4096;
  wchar_t extra[4096] = {};
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 4096;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    if (error) *error = "bad url";
    return 0;
  }
  // OSM's tile and Nominatim policies ask for an identifying User-Agent
  std::wstring agent = widen(std::string("OpenCiM/") + REGS_VERSION_STRING + " (Cities in Motion source port; map editor)");
  HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) {
    if (error) *error = "WinHttpOpen failed";
    return 0;
  }
  // TLS 1.2 and 1.3 (some servers, e.g. overpass-api.de, refuse what WinHTTP offers by default)
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x2000 /* WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 */;
  if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
  }
  WinHttpSetTimeouts(s, 10000, 10000, 30000, 200000);
  int status = 0;
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  HINTERNET r = nullptr;
  if (c) {
    std::wstring object = std::wstring(path) + extra;
    r = WinHttpOpenRequest(c, postBody ? L"POST" : L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  }
  if (r) {
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(r, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof decomp);
    const wchar_t* hdr = postBody ? L"Content-Type: application/x-www-form-urlencoded\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    BOOL ok = WinHttpSendRequest(r, hdr, postBody ? DWORD(-1) : 0,
                                 postBody ? LPVOID(postBody->data()) : WINHTTP_NO_REQUEST_DATA,
                                 postBody ? DWORD(postBody->size()) : 0, postBody ? DWORD(postBody->size()) : 0, 0);
    if (ok && WinHttpReceiveResponse(r, nullptr)) {
      DWORD code = 0, len = sizeof code;
      WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                          &len, WINHTTP_NO_HEADER_INDEX);
      status = int(code);
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
        size_t at = out.size();
        out.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(r, out.data() + at, avail, &got)) {
          out.resize(at);
          break;
        }
        out.resize(at + got);
      }
    } else if (error) {
      *error = "request failed (" + std::to_string(GetLastError()) + ")";
    }
    WinHttpCloseHandle(r);
  } else if (error) {
    *error = "connect failed";
  }
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return status;
}

bool platformDecodeImage(const std::vector<uint8_t>& file, int& width, int& height, std::vector<uint8_t>& bgra) {
  if (file.empty()) return false;
  static thread_local bool comReady = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
  (void)comReady;
  IWICImagingFactory* f = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  bool ok = false;
  IWICStream* st = nullptr;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* fr = nullptr;
  IWICFormatConverter* cv = nullptr;
  if (SUCCEEDED(f->CreateStream(&st)) &&
      SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE*>(file.data()), DWORD(file.size()))) &&
      SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
      SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
      SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                               WICBitmapPaletteTypeCustom))) {
    UINT w = 0, h = 0;
    cv->GetSize(&w, &h);
    width = int(w), height = int(h);
    bgra.resize(size_t(w) * size_t(h) * 4);
    ok = w && h && SUCCEEDED(cv->CopyPixels(nullptr, w * 4, UINT(bgra.size()), bgra.data()));
  }
  if (cv) cv->Release();
  if (fr) fr->Release();
  if (dec) dec->Release();
  if (st) st->Release();
  f->Release();
  return ok;
}

}  // namespace regs::engine

namespace regs::engine {

bool platformHomeRegion(double& lat, double& lon) {
  GEOID id = GetUserGeoID(GEOCLASS_NATION);
  if (id == GEOID_NOT_AVAILABLE) return false;
  wchar_t buf[64] = {};
  if (!GetGeoInfoW(id, GEO_LATITUDE, buf, 64, 0)) return false;
  lat = _wtof(buf);
  if (!GetGeoInfoW(id, GEO_LONGITUDE, buf, 64, 0)) return false;
  lon = _wtof(buf);
  return true;
}

}  // namespace regs::engine
