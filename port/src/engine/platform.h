#pragma once
#include <filesystem>
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
