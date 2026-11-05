#include "engine/platform.h"

#include <algorithm>
#include <cstdlib>
#include <windows.h>
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
