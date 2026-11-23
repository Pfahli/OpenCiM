#include "core/log.h"

#include "core/crash.h"

#include <chrono>
#include <cstdlib>
#include <mutex>

namespace regs {

namespace {
LogLevel g_level = LogLevel::Info;
FILE* g_file = nullptr;
std::mutex g_mutex;
const char* kNames[] = {"debug", "info", "warn", "error"};
// REGS_LOG_TIME=1: every line starts with the seconds since start (load-time measurements)
const bool g_time = std::getenv("REGS_LOG_TIME") != nullptr;
const auto g_start = std::chrono::steady_clock::now();
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
  char stamp[24] = "";
  if (g_time)
    std::snprintf(stamp, sizeof stamp, "%8.2f ",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count());
  std::fprintf(stderr, "%s[%s] %s\n", stamp, kNames[int(level)], msg.c_str());
  crashLogLine(std::string(stamp) + "[" + kNames[int(level)] + "] " + msg);  // the crash report's last lines
  if (g_file) {
    std::fprintf(g_file, "%s[%s] %s\n", stamp, kNames[int(level)], msg.c_str());
    std::fflush(g_file);
  }
}

std::string logStamp() {
  if (!g_time) return {};
  char stamp[24];
  std::snprintf(stamp, sizeof stamp, "%8.2f ", std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count());
  return stamp;
}

}  // namespace regs
