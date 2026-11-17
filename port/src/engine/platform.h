#pragma once
#include <filesystem>
#include <string>
#include <cstdint>
#include <vector>

namespace regs::engine {

// Platform services the natives need. Windows implementation in platform_win32.cpp; other platforms will provide
// their own (SDL or native) without touching the natives.
struct DisplayMode {
  int width = 0, height = 0, bits = 32, freq = 60;
};

std::vector<DisplayMode> platformDisplayModes();
DisplayMode platformCurrentDisplayMode();

// The user's Documents folder as the original resolves it for getMyDocumentsFolder (gsFunction_getMyDocumentsFolder
// W:0x10095240 → W:0x100a4ee0): SHGetSpecialFolderPathW with CSIDL_MYDOCUMENTS (0x0c), then CSIDL_PERSONAL (5), then
// %USERPROFILE%. Only resolves the path; creates nothing.
std::filesystem::path platformDocumentsFolder();
// Explorer at a file (selected) or a folder (opened): the editor's template export and import
void platformShowInExplorer(const std::filesystem::path& path, bool select);

}  // namespace regs::engine

namespace regs::engine {

// HTTPS GET (or POST when body is given, as application/x-www-form-urlencoded) with the port's User-Agent; blocking,
// safe to call from worker threads. Returns the HTTP status (0 when the connection failed) and fills out.
int platformHttpRequest(const std::string& url, const std::string* postBody, std::vector<uint8_t>& out,
                        std::string* error = nullptr);
// a PNG or JPEG file → top-down BGRA bytes (Windows: WIC). False when it is no image the platform can decode.
bool platformDecodeImage(const std::vector<uint8_t>& file, int& width, int& height, std::vector<uint8_t>& bgra);

}  // namespace regs::engine

namespace regs::engine {

// The user's home region as the system knows it (Windows: the region setting, a country's centre); no network, no
// location service. False when unknown.
bool platformHomeRegion(double& lat, double& lon);

}  // namespace regs::engine
