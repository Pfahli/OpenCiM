// What a crash report carries besides the stack (core/crash.h).
#include "core/crash.h"

#include <array>
#include <chrono>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace regs {

namespace {

constexpr size_t kLines = 200;

struct State {
  std::mutex m;
  fs::path dir;
  bool dialog = false;
  std::vector<std::pair<std::string, std::string>> context;
  std::array<std::string, kLines> lines;
  size_t next = 0, count = 0;
};

State& st() {
  static State* s = new State;  // never destroyed: a crash during exit still finds it
  return *s;
}

// the lock, but giving up after ~50 ms (the crashing thread may hold it)
struct TryLock {
  std::mutex& m;
  bool owned = false;
  explicit TryLock(std::mutex& mm) : m(mm) {
    for (int i = 0; i < 50 && !(owned = m.try_lock()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ~TryLock() {
    if (owned) m.unlock();
  }
};

}  // namespace

void crashSetReportDir(const fs::path& dir) {
  std::lock_guard lock(st().m);
  st().dir = dir;
}

fs::path crashReportDir() {
  TryLock lock(st().m);
  if (!st().dir.empty()) return st().dir;
  std::error_code ec;
  fs::path tmp = fs::temp_directory_path(ec);
  return (ec ? fs::path(".") : tmp) / "OpenCiM-crashes";
}

void crashSetDialog(bool on) { st().dialog = on; }
bool crashDialog() { return st().dialog; }

void crashContext(const std::string& key, const std::string& value) {
  std::lock_guard lock(st().m);
  for (auto& [k, v] : st().context)
    if (k == key) {
      v = value;
      return;
    }
  st().context.emplace_back(key, value);
}

void crashLogLine(const std::string& line) {
  State& s = st();
  std::lock_guard lock(s.m);
  s.lines[s.next] = line;
  s.next = (s.next + 1) % kLines;
  if (s.count < kLines) ++s.count;
}

std::vector<std::pair<std::string, std::string>> crashContextSnapshot() {
  TryLock lock(st().m);
  return st().context;  // read even without the lock: better a torn line than no report
}

std::vector<std::string> crashLogSnapshot() {
  State& s = st();
  TryLock lock(s.m);
  std::vector<std::string> out;
  out.reserve(s.count);
  for (size_t i = 0; i < s.count; ++i) out.push_back(s.lines[(s.next + kLines - s.count + i) % kLines]);
  return out;
}

}  // namespace regs
