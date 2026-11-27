// Crash reports on Linux (docs/crash-reports.md, docs/linux.md): a signal handler on its own stack writes a text report
// to the crash folder (Documents/OpenCiM/crashes) with the signal, the stack as "module+0xoffset" lines (named with
// `addr2line -e regs -f -C <offset>` or regs-tool symbolize), the facts and the last log lines. No minidump. The
// Windows handler is in crash_win32.cpp.
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "build_info.h"
#include "core/crash.h"

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_handling{false};
const auto g_start = std::chrono::steady_clock::now();

std::string frameText(void* addr) {
  Dl_info info{};
  char buf[512];
  if (dladdr(addr, &info) && info.dli_fname) {
    const uintptr_t off = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(info.dli_fbase);
    const char* name = std::strrchr(info.dli_fname, '/');
    std::snprintf(buf, sizeof buf, "  %s+0x%zx%s%s", name ? name + 1 : info.dli_fname, size_t(off),
                  info.dli_sname ? "  " : "", info.dli_sname ? info.dli_sname : "");
  } else {
    std::snprintf(buf, sizeof buf, "  0x%zx", size_t(reinterpret_cast<uintptr_t>(addr)));
  }
  return buf;
}

std::string osVersion() {
  std::string pretty;
  std::ifstream f("/etc/os-release");
  for (std::string l; std::getline(f, l);)
    if (l.rfind("PRETTY_NAME=", 0) == 0) {
      pretty = l.substr(12);
      if (pretty.size() >= 2 && pretty.front() == '"') pretty = pretty.substr(1, pretty.size() - 2);
    }
  utsname u{};
  uname(&u);
  return (pretty.empty() ? std::string(u.sysname) : pretty) + " (" + u.release + ", " + u.machine + ")";
}

std::vector<std::string> tail(const fs::path& p, size_t n) {
  std::ifstream f(p, std::ios::binary);
  std::vector<std::string> lines;
  for (std::string l; std::getline(f, l);) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
    if (lines.size() > n) lines.erase(lines.begin());
  }
  return lines;
}

void report(const char* what, int skip) {
  if (g_handling.exchange(true)) {  // a second thread crashing meanwhile waits for the first report
    for (;;) pause();
  }
  // the report below uses the heap and stdio, which can deadlock when the crash happened inside them (a lock held by
  // this very thread): the stack goes to stderr first with async-signal-safe calls, and the process is ended after
  // 10 seconds whatever happens
  alarm(10);
  void* frames[64];
  const int n = backtrace(frames, 64);
  static const char head[] = "\n*** crash: ";
  ssize_t w = write(2, head, sizeof head - 1);
  w = write(2, what, std::strlen(what));
  w = write(2, "\n", 1);
  (void)w;
  backtrace_symbols_fd(frames + skip, n > skip ? n - skip : 0, 2);
  std::vector<std::string> stack;
  for (int i = skip; i < n; ++i) stack.push_back(frameText(frames[i]));

  const fs::path dir = regs::crashReportDir();
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::time_t now = std::time(nullptr);
  std::tm t{};
  localtime_r(&now, &t);
  char stamp[40], when[40];
  std::strftime(stamp, sizeof stamp, "crash-%Y-%m-%d-%H%M%S", &t);
  std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &t);
  const fs::path txt = dir / (std::string(stamp) + ".txt");
  const long long up = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_start).count();
  struct sysinfo si{};
  sysinfo(&si);
  char self[1024] = {};
  const ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
  if (sl > 0) self[sl] = 0;

  std::ostringstream r;
  r << "OpenCiM crash report\n"
    << "Please attach this file to the bug report.\n\n"
    << "Version:   OpenCiM " REGS_VERSION_STRING " (build " << REGS_BUILD_NUMBER << ", " REGS_BUILD_COMMIT ", " REGS_BUILD_DATE ", Linux)\n"
    << "Time:      " << when << ", after " << up / 3600 << ":" << (up / 60 % 60 < 10 ? "0" : "") << up / 60 % 60 << ":"
    << (up % 60 < 10 ? "0" : "") << up % 60 << " running\n"
    << "Program:   " << fs::path(self).filename().string() << "\n"
    << "System:    " << osVersion() << ", " << sysconf(_SC_NPROCESSORS_ONLN) << " threads, "
    << (unsigned long long)si.totalram * si.mem_unit / (1 << 20) << " MB RAM ("
    << (unsigned long long)si.freeram * si.mem_unit / (1 << 20) << " MB free)\n"
    << "What:      " << what << "\n"
    << "Where:     " << (stack.empty() ? std::string("?") : stack[0].substr(2)) << "\n";
  for (const auto& [k, v] : regs::crashContextSnapshot()) r << "  " << k << ": " << v << "\n";
  r << "\nStack of the crashing thread:\n";
  for (const std::string& l : stack) r << l << "\n";
  r << "\nLast lines of the program's output:\n";
  for (const std::string& l : regs::crashLogSnapshot()) r << "  " << l << "\n";
  const std::vector<std::string> game = tail(dir.parent_path() / "log_metro.txt", 40);
  if (!game.empty()) {
    r << "\nEnd of log_metro.txt:\n";
    for (const std::string& l : game) r << "  " << l << "\n";
  }
  std::ofstream out(txt, std::ios::binary);
  out << r.str();
  out.close();
  std::fprintf(stderr, "*** report: %s\n", txt.string().c_str());
}

void onSignal(int sig, siginfo_t* info, void*) {
  const char* name = sig == SIGSEGV ? "access violation (SIGSEGV)" : sig == SIGBUS ? "bus error (SIGBUS)"
                     : sig == SIGFPE ? "arithmetic error (SIGFPE)" : sig == SIGILL ? "illegal instruction (SIGILL)"
                     : sig == SIGABRT ? "abort() was called (SIGABRT)" : "signal";
  char what[160];
  if (sig == SIGSEGV || sig == SIGBUS)
    std::snprintf(what, sizeof what, "%s at address 0x%zx", name, size_t(reinterpret_cast<uintptr_t>(info->si_addr)));
  else
    std::snprintf(what, sizeof what, "%s", name);
  report(what, 2);
  signal(sig, SIG_DFL);
  _exit(3);
}

void onTerminate() {
  std::string why = "std::terminate() was called";
  if (std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& x) {
      why += std::string(" with an uncaught exception: ") + x.what();
    } catch (...) {
      why += " with an uncaught exception";
    }
  }
  report(why.c_str(), 1);
  _exit(3);
}

struct Install {
  Install() {
    void* warm[4];
    backtrace(warm, 4);  // loads libgcc's unwinder now, not first inside a signal handler
    static std::vector<char> altStack(1 << 16);  // room for the handler on a stack overflow
    stack_t ss{};
    ss.ss_sp = altStack.data();
    ss.ss_size = altStack.size();
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = onSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(s, &sa, nullptr);
    std::set_terminate(onTerminate);
  }
} install;

}  // namespace

namespace regs {

void crashThreadHandlers() { std::set_terminate(onTerminate); }

// REGS_CRASH_TEST=<kind> (tests, main.cpp): access, null-call, divide, abort, throw, terminate
void crashTest(const char* kind) {
  const std::string k = kind;
  std::fprintf(stderr, "[info] REGS_CRASH_TEST=%s\n", kind);
  if (k == "access") {
    volatile int* p = reinterpret_cast<volatile int*>(sizeof(void*) * 2);
    *p = 1;
  } else if (k == "null-call") {
    void (*volatile f)() = nullptr;
    f();
  } else if (k == "divide") {
    volatile int z = 0;
    volatile int x = 1 / z;
    (void)x;
  } else if (k == "abort") {
    std::abort();
  } else if (k == "throw") {
    throw std::runtime_error("REGS_CRASH_TEST throw");
  } else if (k == "terminate") {
    std::terminate();
  }
}

// regs-tool symbolize <report.txt> [<folder with the build's binaries>]: each "module+0x..." stack line named with
// addr2line (binutils), from the binaries of that build
int crashSymbolize(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: regs-tool symbolize <report.txt> [<folder with the build's binaries>]\n");
    return 2;
  }
  fs::path dir = argc >= 4 ? fs::path(argv[3]) : fs::path();
  if (dir.empty()) {
    char self[1024] = {};
    const ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    if (sl > 0) dir = fs::path(std::string(self, size_t(sl))).parent_path();
  }
  std::ifstream in(argv[2], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", argv[2]);
    return 1;
  }
  int named = 0;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::printf("%s", line.c_str());
    const size_t plus = line.find("+0x"), start = line.find_first_not_of(' ');
    if (plus != std::string::npos && start != std::string::npos && start < plus && line.find(' ', start) > plus) {
      const fs::path image = dir / line.substr(start, plus - start);
      std::error_code ec;
      if (fs::exists(image, ec)) {
        const std::string cmd = "addr2line -f -C -s -e '" + image.string() + "' " + line.substr(plus + 1, line.find(' ', plus) - plus - 1);
        if (FILE* p = popen(cmd.c_str(), "r")) {
          char fn[512] = {}, src[512] = {};
          if (std::fgets(fn, sizeof fn, p) && std::fgets(src, sizeof src, p)) {
            fn[std::strcspn(fn, "\n")] = 0;
            src[std::strcspn(src, "\n")] = 0;
            std::printf("  %s  %s", fn, src);
            ++named;
          }
          pclose(p);
        }
      }
    }
    std::printf("\n");
  }
  std::fprintf(stderr, "%d lines named (binaries from %s)\n", named, dir.string().c_str());
  return 0;
}

}  // namespace regs
