#pragma once
// Force-included into the FidelityFX SDK's sources on GCC and Clang (CMakeLists.txt, docs/linux.md): the headers and
// MSVC CRT functions the SDK takes for granted on Windows.
#include <bit>
#include <cmath>
#include <codecvt>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <locale>
#include <string>

#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

inline int sprintf_s(char* buf, size_t size, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}
template <size_t N>
inline int sprintf_s(char (&buf)[N], const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, N, fmt, ap);
  va_end(ap);
  return n;
}

inline int wcscpy_s(wchar_t* dst, size_t size, const wchar_t* src) {
  if (!dst || size == 0) return 1;
  std::wcsncpy(dst, src ? src : L"", size - 1);
  dst[size - 1] = 0;
  return 0;
}
template <size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) {
  return wcscpy_s(dst, N, src);
}

inline int strcpy_s(char* dst, size_t size, const char* src) {
  if (!dst || size == 0) return 1;
  std::strncpy(dst, src ? src : "", size - 1);
  dst[size - 1] = 0;
  return 0;
}
template <size_t N>
inline int strcpy_s(char (&dst)[N], const char* src) {
  return strcpy_s(dst, N, src);
}
