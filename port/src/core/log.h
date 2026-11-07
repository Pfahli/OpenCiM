#pragma once
#include <cstdio>
#include <string>

namespace regs {

enum class LogLevel { Debug, Info, Warn, Error };

void logSetLevel(LogLevel level);
void logSetFile(const std::string& path);
void logWrite(LogLevel level, const std::string& msg);
// "  12.34 " (seconds since start) with REGS_LOG_TIME set, else ""
std::string logStamp();

template <typename... A>
std::string fmt(const char* f, A... a) {
  int n = std::snprintf(nullptr, 0, f, a...);
  std::string s(n > 0 ? n : 0, '\0');
  if (n > 0) std::snprintf(s.data(), s.size() + 1, f, a...);
  return s;
}

#define REGS_LOG(level, ...) ::regs::logWrite(level, ::regs::fmt(__VA_ARGS__))
#define LOG_DEBUG(...) REGS_LOG(::regs::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) REGS_LOG(::regs::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) REGS_LOG(::regs::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) REGS_LOG(::regs::LogLevel::Error, __VA_ARGS__)

}  // namespace regs
