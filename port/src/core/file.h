#pragma once
// C stdio on a std::filesystem::path on every system (docs/linux.md): Windows opens the wide path (_wfopen), others
// the UTF-8 one; 64-bit seek and tell.
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace regs {

inline std::FILE* openFile(const std::filesystem::path& p, const char* mode) {
#if defined(_WIN32)
  wchar_t m[8] = {};
  for (int i = 0; i < 7 && mode[i]; ++i) m[i] = wchar_t(mode[i]);
  return _wfopen(p.c_str(), m);
#else
  return std::fopen(p.c_str(), mode);
#endif
}

inline int seekFile(std::FILE* f, int64_t offset, int origin) {
#if defined(_WIN32)
  return _fseeki64(f, offset, origin);
#else
  return fseeko(f, off_t(offset), origin);
#endif
}

inline int64_t tellFile(std::FILE* f) {
#if defined(_WIN32)
  return _ftelli64(f);
#else
  return int64_t(ftello(f));
#endif
}

}  // namespace regs
