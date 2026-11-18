#pragma once
// std::getenv with a per-thread cache, for the debug and test switches (REGS_*) read in hot paths. MSVC's getenv
// takes a lock and scans the whole environment block on every call; a stop bubble's camera read REGS_DEBUG_TOPDOWN
// once per bubble per frame (~12 % of a Berlin frame, docs/performance-audit.md A1).
// The environment is fixed once the engine runs (main.cpp sets the start flags before the first frame), so each name
// is read once per thread. Names must be string literals or otherwise outlive the program (entries are keyed by the
// pointer and checked by content); a name built at run time goes through std::getenv directly.
#include <cstdlib>
#include <cstring>

namespace regs {

inline const char* envVar(const char* name) {
  struct Entry {
    const char* name;
    const char* value;
  };
  constexpr int kMax = 512;  // the code reads about 200 distinct names
  thread_local Entry entries[kMax];
  thread_local int count = 0;
  for (int i = 0; i < count; ++i)
    if (entries[i].name == name) return entries[i].value;
  const char* v = nullptr;
  bool found = false;
  for (int i = 0; i < count && !found; ++i)  // the same name through another pointer (another translation unit's literal)
    if (std::strcmp(entries[i].name, name) == 0) v = entries[i].value, found = true;
  if (!found) v = std::getenv(name);
  if (count < kMax) entries[count++] = {name, v};  // this pointer too, so the next call is one compare
  return v;
}

}  // namespace regs
