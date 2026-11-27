// Platform services on Linux and other non-Windows systems (docs/linux.md): SDL3 for the display modes, the user's
// folders and opening the file manager; libcurl for HTTPS; stb_image for PNG and JPEG. The Windows versions are in
// platform_win32.cpp.
#include "engine/platform.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "build_info.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace regs::engine {

namespace {
bool videoUp() {
  static const bool up = SDL_InitSubSystem(SDL_INIT_VIDEO);
  return up;
}
DisplayMode toMode(const SDL_DisplayMode& m) {
  const int bits = SDL_BITSPERPIXEL(m.format) ? int(SDL_BITSPERPIXEL(m.format)) : 32;
  return {int(std::lround(m.w * m.pixel_density)), int(std::lround(m.h * m.pixel_density)), bits == 24 ? 32 : bits,
          m.refresh_rate > 0 ? int(std::lround(m.refresh_rate)) : 60};
}
}  // namespace

std::vector<DisplayMode> platformDisplayModes() {
  std::vector<DisplayMode> out;
  if (!videoUp()) return out;
  int n = 0;
  SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n);
  for (int i = 0; modes && i < n; ++i) {
    DisplayMode m = toMode(*modes[i]);
    const bool dup = std::any_of(out.begin(), out.end(), [&](const DisplayMode& o) {
      return o.width == m.width && o.height == m.height && o.bits == m.bits && o.freq == m.freq;
    });
    if (!dup) out.push_back(m);
  }
  SDL_free(modes);
  if (out.empty()) out.push_back(platformCurrentDisplayMode());
  return out;
}

DisplayMode platformCurrentDisplayMode() {
  if (videoUp())
    if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) return toMode(*m);
  return {1920, 1080, 32, 60};
}

// the XDG documents folder (~/.config/user-dirs.dirs, e.g. ~/Dokumente), else ~/Documents, else the home folder
std::filesystem::path platformDocumentsFolder() {
  if (const char* d = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) {
    std::filesystem::path p(d);
    if (!p.empty()) return p;
  }
  const char* home = std::getenv("HOME");
  if (!home) return {};
  std::error_code ec;
  std::filesystem::path docs = std::filesystem::path(home) / "Documents";
  return std::filesystem::is_directory(docs, ec) ? docs : std::filesystem::path(home);
}

// the file manager at a folder (for a file: the folder it is in)
void platformShowInExplorer(const std::filesystem::path& path, bool select) {
  std::filesystem::path dir = select ? path.parent_path() : path;
  SDL_OpenURL(("file://" + dir.string()).c_str());
}

namespace {
size_t onData(char* p, size_t size, size_t n, void* user) {
  auto* out = static_cast<std::vector<uint8_t>*>(user);
  out->insert(out->end(), reinterpret_cast<uint8_t*>(p), reinterpret_cast<uint8_t*>(p) + size * n);
  return size * n;
}
}  // namespace

int platformHttpRequest(const std::string& url, const std::string* postBody, std::vector<uint8_t>& out,
                        std::string* error) {
  out.clear();
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CURL* c = curl_easy_init();
  if (!c) {
    if (error) *error = "curl_easy_init failed";
    return 0;
  }
  // OSM's tile and Nominatim policies ask for an identifying User-Agent
  const std::string agent = std::string("OpenCiM/") + REGS_VERSION_STRING + " (Cities in Motion source port; map editor)";
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_USERAGENT, agent.c_str());
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // every encoding curl can decompress
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 200L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);  // worker threads
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &onData);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_slist* headers = nullptr;
  if (postBody) {
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, postBody->data());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(postBody->size()));
  }
  long status = 0;
  const CURLcode r = curl_easy_perform(c);
  if (r == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  else if (error) *error = std::string("request failed (") + curl_easy_strerror(r) + ")";
  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  return int(status);
}

bool platformDecodeImage(const std::vector<uint8_t>& file, int& width, int& height, std::vector<uint8_t>& bgra) {
  if (file.empty()) return false;
  int w = 0, h = 0, comp = 0;
  stbi_uc* px = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &comp, 4);
  if (!px) return false;
  width = w, height = h;
  bgra.assign(px, px + size_t(w) * size_t(h) * 4);
  stbi_image_free(px);
  for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);  // RGBA → BGRA
  return true;
}

// the country of the user's locale (LC_ALL, LC_MESSAGES or LANG, e.g. de_DE.UTF-8), as its centre; no network
bool platformHomeRegion(double& lat, double& lon) {
  const char* loc = nullptr;
  for (const char* v : {"LC_ALL", "LC_MESSAGES", "LANG"})
    if (const char* s = std::getenv(v); s && *s) {
      loc = s;
      break;
    }
  if (!loc) return false;
  const char* u = std::strchr(loc, '_');
  if (!u || !std::isalpha(static_cast<unsigned char>(u[1])) || !std::isalpha(static_cast<unsigned char>(u[2]))) return false;
  const char cc[3] = {char(std::toupper(static_cast<unsigned char>(u[1]))), char(std::toupper(static_cast<unsigned char>(u[2]))), 0};
  struct Country {
    const char* code;
    double lat, lon;
  };
  static const Country countries[] = {
      {"DE", 51.2, 10.4},  {"AT", 47.6, 14.1},  {"CH", 46.8, 8.2},   {"FR", 46.6, 2.4},   {"GB", 54.0, -2.0},
      {"IE", 53.2, -8.2},  {"NL", 52.2, 5.3},   {"BE", 50.6, 4.6},   {"LU", 49.8, 6.1},   {"DK", 56.0, 10.0},
      {"SE", 62.0, 15.0},  {"NO", 62.0, 10.0},  {"FI", 64.0, 26.0},  {"IS", 65.0, -18.0}, {"PL", 52.0, 19.0},
      {"CZ", 49.8, 15.5},  {"SK", 48.7, 19.7},  {"HU", 47.2, 19.5},  {"SI", 46.1, 14.8},  {"HR", 45.1, 15.2},
      {"IT", 42.8, 12.8},  {"ES", 40.4, -3.7},  {"PT", 39.6, -8.0},  {"GR", 39.1, 22.0},  {"RO", 45.9, 24.9},
      {"BG", 42.7, 25.5},  {"RS", 44.0, 20.9},  {"UA", 49.0, 31.4},  {"RU", 61.5, 105.3}, {"TR", 39.0, 35.2},
      {"EE", 58.6, 25.0},  {"LV", 56.9, 24.6},  {"LT", 55.2, 23.9},  {"US", 39.8, -98.6}, {"CA", 56.1, -106.3},
      {"MX", 23.6, -102.6}, {"BR", -14.2, -51.9}, {"AR", -38.4, -63.6}, {"AU", -25.3, 133.8}, {"NZ", -41.5, 172.8},
      {"JP", 36.2, 138.3}, {"CN", 35.9, 104.2}, {"KR", 36.5, 127.9}, {"IN", 21.1, 78.0},  {"ZA", -30.6, 22.9},
  };
  for (const Country& c : countries)
    if (std::strcmp(c.code, cc) == 0) {
      lat = c.lat, lon = c.lon;
      return true;
    }
  return false;
}

}  // namespace regs::engine
