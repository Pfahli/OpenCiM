#include "core/log.h"

#include <mutex>

namespace regs {

namespace {
LogLevel g_level = LogLevel::Info;
FILE* g_file = nullptr;
std::mutex g_mutex;
const char* kNames[] = {"debug", "info", "warn", "error"};
}  // namespace

void logSetLevel(LogLevel level) { g_level = level; }

void logSetFile(const std::string& path) {
  std::lock_guard lock(g_mutex);
  if (g_file) std::fclose(g_file);
  g_file = std::fopen(path.c_str(), "w");
}

void logWrite(LogLevel level, const std::string& msg) {
  if (level < g_level) return;
  std::lock_guard lock(g_mutex);
  std::fprintf(stderr, "[%s] %s\n", kNames[int(level)], msg.c_str());
  if (g_file) {
    std::fprintf(g_file, "[%s] %s\n", kNames[int(level)], msg.c_str());
    std::fflush(g_file);
  }
}

}  // namespace regs
