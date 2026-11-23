// Crash reports on Windows (core/crash.h, docs/crash-reports.md). On an unhandled exception, abort, std::terminate, a
// pure virtual call or a bad CRT argument: a minidump (crash-<time>.dmp) and a text report (crash-<time>.txt: version,
// system, what happened and where, the stack as module+offset with names when the PDB is next to the exe, the facts
// from crashContext, the last log lines and the end of log_metro.txt) in the report folder, the stack also on stderr,
// then a message box with the path (REGS_NO_DIALOGS=1 leaves it out). The work runs on a thread of its own, so a stack
// overflow still gets its report. regs-tool symbolize puts names on a report's module+offset lines afterwards.
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// our own exception code for the CRT's error paths (abort, terminate, purecall, invalid parameter): crtFailure hands
// it with a reason to the same report as the filter's
constexpr DWORD kCrtFailure = 0xE0C1A001;
constexpr DWORD kCppException = 0xE06D7363;

const ULONGLONG g_start = GetTickCount64();
std::atomic<bool> g_handling{false};

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string hex(DWORD64 v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
  return b;
}

// "regs.exe+0x1a2b3" for an address, the module's full path in *path
std::string moduleOffset(DWORD64 addr, std::wstring* path = nullptr) {
  HMODULE mod = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(addr), &mod) || !mod)
    return hex(addr);
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
  std::wstring full(buf, n);
  if (path) *path = full;
  return narrow(fs::path(full).filename().wstring()) + "+" + hex(addr - reinterpret_cast<DWORD64>(mod));
}

// the C++ exception's first type name and what() into the buffers; plain C (no objects to unwind) for __try
void readCpp(const EXCEPTION_RECORD& er, char* type, size_t typeSize, char* what, size_t whatSize) {
  const ULONG_PTR object = er.ExceptionInformation[1];
  const ULONG_PTR image = er.ExceptionInformation[3];  // x64: the throw info's RVAs are relative to this module
  const auto* throwInfo = reinterpret_cast<const DWORD*>(er.ExceptionInformation[2]);
  __try {
    // ThrowInfo: attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray (RVAs on x64)
    const auto* array = reinterpret_cast<const int*>(image + throwInfo[3]);
    const int n = array[0];
    bool isStd = false;
    for (int i = 0; i < n && i < 16; ++i) {
      // CatchableType: properties, pType (RVA), ...; TypeDescriptor: vftable, spare, name[]
      const auto* ct = reinterpret_cast<const DWORD*>(image + DWORD(array[1 + i]));
      const char* name = reinterpret_cast<const char*>(image + ct[1] + 2 * sizeof(void*));
      if (i == 0) strncpy_s(type, typeSize, name, _TRUNCATE);
      if (std::strcmp(name, ".?AVexception@std@@") == 0) isStd = true;
    }
    if (isStd && object) strncpy_s(what, whatSize, reinterpret_cast<const std::exception*>(object)->what(), _TRUNCATE);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// a C++ exception's type (".?AVruntime_error@std@@" read from the throw info) and, for a std::exception, what()
std::string describeCpp(const EXCEPTION_RECORD& er) {
  if (er.NumberParameters < 4) return "C++ exception";
  char type[256] = "", what[512] = "";
  readCpp(er, type, sizeof type, what, sizeof what);
  std::string out = "C++ exception";
  if (*type) out += std::string(" ") + type;
  if (*what) out += std::string(": ") + what;
  return out;
}

std::string describe(const EXCEPTION_RECORD& er) {
  const DWORD c = er.ExceptionCode;
  switch (c) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR: {
      const char* how = er.NumberParameters >= 2
                            ? (er.ExceptionInformation[0] == 0 ? "reading" : er.ExceptionInformation[0] == 8 ? "executing" : "writing")
                            : "at";
      return std::string(c == EXCEPTION_ACCESS_VIOLATION ? "access violation " : "in-page error ") + how + " address " +
             (er.NumberParameters >= 2 ? hex(er.ExceptionInformation[1]) : std::string("?"));
    }
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case 0xC0000409: return "stack buffer overrun (fail fast)";
    case 0xC0000374: return "heap corruption";
    case kCppException: return describeCpp(er);
    case kCrtFailure:
      return er.NumberParameters >= 1 && er.ExceptionInformation[0]
                 ? reinterpret_cast<const char*>(er.ExceptionInformation[0])
                 : "runtime error";
    default: return "exception";
  }
}

std::string osVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof v;
  if (auto f = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
    f(&v);
  char b[64];
  std::snprintf(b, sizeof b, "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
  return b;
}

std::string cpuName() {
  wchar_t buf[128] = {};
  DWORD size = sizeof buf;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                   RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
    return "?";
  std::string s = narrow(buf);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// the last n lines of a text file
std::vector<std::string> tail(const fs::path& p, size_t n) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  f.seekg(0, std::ios::end);
  const std::streamoff size = f.tellg();
  const std::streamoff from = size > 64 * 1024 ? size - 64 * 1024 : 0;
  f.seekg(from);
  std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::string> lines;
  std::istringstream in(data);
  for (std::string l; std::getline(in, l);) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
  }
  if (from > 0 && !lines.empty()) lines.erase(lines.begin());  // cut in the middle
  if (lines.size() > n) lines.erase(lines.begin(), lines.end() - n);
  return lines;
}

struct Job {
  EXCEPTION_POINTERS* ep;
  DWORD threadId;
  fs::path txt;
};

// the stack of the crashing thread, one "  module+offset  name  file:line" per frame
std::vector<std::string> walkStack(const Job& job) {
  std::vector<std::string> out;
  HANDLE proc = GetCurrentProcess();
  HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, job.threadId);
  if (!thread) return out;
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  SymInitializeW(proc, fs::path(self).parent_path().c_str(), TRUE);  // the PDBs next to the exe (developer builds)
  CONTEXT ctx = *job.ep->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrStack.Offset = ctx.Rsp;
  frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 64; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0)
      break;
    const DWORD64 pc = frame.AddrPC.Offset;
    std::string line = "  " + moduleOffset(pc);
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, pc, &disp, sym)) line += "  " + std::string(sym->Name);
    IMAGEHLP_LINE64 src{};
    src.SizeOfStruct = sizeof src;
    DWORD ldisp = 0;
    if (SymGetLineFromAddr64(proc, pc, &ldisp, &src)) line += "  " + fs::path(src.FileName).filename().string() + ":" + std::to_string(src.LineNumber);
    out.push_back(line);
  }
  SymCleanup(proc);
  CloseHandle(thread);
  return out;
}

DWORD WINAPI reportThread(void* arg) {
  Job& job = *static_cast<Job*>(arg);
  const EXCEPTION_RECORD& er = *job.ep->ExceptionRecord;
  const fs::path dir = regs::crashReportDir();
  std::error_code ec;
  fs::create_directories(dir, ec);
  SYSTEMTIME t;
  GetLocalTime(&t);
  char stamp[40];
  std::snprintf(stamp, sizeof stamp, "crash-%04u-%02u-%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                t.wSecond);
  const fs::path dmp = dir / (std::string(stamp) + ".dmp");
  job.txt = dir / (std::string(stamp) + ".txt");

  // the minidump: threads, stacks and the memory they point to (a few MB), opened with the build's PDB in Visual Studio
  // or WinDbg
  bool dumped = false;
  HANDLE f = CreateFileW(dmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{job.threadId, job.ep, FALSE};
    dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                               MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                             MiniDumpWithUnloadedModules | MiniDumpScanMemory),
                               &mei, nullptr, nullptr);
    CloseHandle(f);
    if (!dumped) DeleteFileW(dmp.c_str());
  }

  std::wstring exe;
  std::string where = moduleOffset(reinterpret_cast<DWORD64>(er.ExceptionAddress), &exe);
  const std::vector<std::string> stack = walkStack(job);
  if (!stack.empty()) where = stack[0].substr(stack[0].find_first_not_of(' '));  // with its name when known
  if (er.ExceptionCode == kCrtFailure || er.ExceptionCode == kCppException) {
    // our CRT handlers and C++ throws: "where" is the first frame past them and the runtime (abort, terminate,
    // RaiseException, _CxxThrowException)
    for (const std::string& l : stack) {
      static const char* const skip[] = {"KERNELBASE.dll+", "ucrtbase.dll+", "VCRUNTIME140.dll+", "vcruntime140.dll+",
                                         "crtFailure", "onAbort", "onTerminate", "onPureCall", "onInvalidParameter"};
      bool runtime = false;
      for (const char* s : skip) runtime = runtime || l.find(s) != std::string::npos;
      if (!runtime) {
        where = l.substr(l.find_first_not_of(' '));
        break;
      }
    }
  }
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof mem;
  GlobalMemoryStatusEx(&mem);
  SYSTEM_INFO si;
  GetNativeSystemInfo(&si);
  const ULONGLONG up = (GetTickCount64() - g_start) / 1000;
  char upText[32];
  std::snprintf(upText, sizeof upText, "%llu:%02llu:%02llu", up / 3600, up / 60 % 60, up % 60);

  std::ostringstream r;
  r << "OpenCiM crash report\r\n"
    << "Please attach this file" << (dumped ? " and " + dmp.filename().string() : std::string()) << " to the bug report.\r\n\r\n"
    << "Version:   OpenCiM " REGS_VERSION_STRING " (build " << REGS_BUILD_NUMBER << ", " REGS_BUILD_COMMIT ", " REGS_BUILD_DATE ")\r\n";
  char when[40];
  std::snprintf(when, sizeof when, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  r << "Time:      " << when << ", after " << upText << " running\r\n"
    << "Program:   " << fs::path(self).filename().string() << "\r\n"
    << "System:    " << osVersion() << ", " << cpuName() << ", " << si.dwNumberOfProcessors << " threads, "
    << (mem.ullTotalPhys >> 20) << " MB RAM (" << (mem.ullAvailPhys >> 20) << " MB free)\r\n"
    << "What:      " << describe(er) << " (" << hex(er.ExceptionCode) << ")\r\n"
    << "Where:     " << where << "\r\n";
  for (const auto& [k, v] : regs::crashContextSnapshot()) r << "  " << k << ": " << v << "\r\n";
  r << "\r\nStack of the crashing thread (" << job.threadId << "):\r\n";
  for (const std::string& l : stack) r << l << "\r\n";
  r << "\r\nLast lines of the program's output:\r\n";
  for (const std::string& l : regs::crashLogSnapshot()) r << "  " << l << "\r\n";
  const fs::path gameLog = dir.parent_path() / "log_metro.txt";
  const std::vector<std::string> game = tail(gameLog, 40);
  if (!game.empty()) {
    r << "\r\nEnd of log_metro.txt:\r\n";
    for (const std::string& l : game) r << "  " << l << "\r\n";
  }
  std::ofstream out(job.txt, std::ios::binary);
  out << r.str();
  out.close();

  // developers in a console see the same
  std::fprintf(stderr, "\n*** crash: %s at %s\n", describe(er).c_str(), where.c_str());
  for (const std::string& l : stack) std::fprintf(stderr, "%s\n", l.c_str());
  std::fprintf(stderr, "*** report: %s\n", job.txt.string().c_str());

  if (regs::crashDialog() && !std::getenv("REGS_NO_DIALOGS")) {
    // in Windows' display language (German or English): the game's own strings are not safe to reach from here
    const bool de = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
    const std::wstring msg =
        de ? L"OpenCiM wurde wegen eines Fehlers beendet.\n\nEin Absturzbericht wurde gespeichert:\n" + job.txt.wstring() +
                 L"\n\nBitte hänge diese Datei" + (dumped ? L" und die .dmp-Datei daneben" : L"") +
                 L" an, wenn du den Fehler meldest.\n\nOrdner jetzt öffnen?"
           : L"OpenCiM stopped because of an error.\n\nA crash report was saved:\n" + job.txt.wstring() +
                 L"\n\nWhen you report the problem, please attach this file" +
                 (dumped ? L" and the .dmp file beside it" : L"") + L".\n\nOpen the folder now?";
    if (MessageBoxW(nullptr, msg.c_str(), L"OpenCiM", MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST) == IDYES) {
      const std::wstring args = L"/select,\"" + job.txt.wstring() + L"\"";
      ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
  }
  return 0;
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
  if (g_handling.exchange(true)) {  // a second thread crashing meanwhile waits for the first report
    Sleep(INFINITE);
    return EXCEPTION_EXECUTE_HANDLER;
  }
  Job job{ep, GetCurrentThreadId(), {}};
  // a thread of its own: this one may have no stack left (stack overflow)
  if (HANDLE h = CreateThread(nullptr, 1 << 20, reportThread, &job, 0, nullptr)) {
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
  }
  TerminateProcess(GetCurrentProcess(), 3);
  return EXCEPTION_EXECUTE_HANDLER;
}

// the CRT's error paths report straight away with the context captured here (raising an exception instead would not
// do: terminate() runs its handler inside an __except of its own and then calls abort)
[[noreturn]] __declspec(noinline) void crtFailure(const char* why) {
  CONTEXT ctx{};
  RtlCaptureContext(&ctx);
  EXCEPTION_RECORD er{};
  er.ExceptionCode = kCrtFailure;
  er.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
  er.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
  er.NumberParameters = 1;
  er.ExceptionInformation[0] = reinterpret_cast<ULONG_PTR>(why);
  EXCEPTION_POINTERS ep{&er, &ctx};
  onCrash(&ep);
  std::_Exit(3);
}

void onAbort(int) { crtFailure("abort() was called"); }

void onTerminate() {
  // an uncaught C++ exception normally reaches onCrash itself (code 0xE06D7363); this is terminate() for other reasons
  static thread_local std::string why;
  why = "std::terminate() was called";
  if (std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& x) {
      why += std::string(" with an uncaught exception: ") + x.what();
    } catch (...) {
      why += " with an uncaught exception";
    }
  }
  crtFailure(why.c_str());
}

void onPureCall() { crtFailure("pure virtual function call"); }

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
  crtFailure("invalid argument to a C runtime function");
}

struct Install {
  Install() {
    SetUnhandledExceptionFilter(onCrash);
    ULONG guarantee = 64 * 1024;  // room for the filter itself on a stack overflow
    SetThreadStackGuarantee(&guarantee);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // no CRT box, no fail-fast past our filter
    std::signal(SIGABRT, onAbort);
    std::set_terminate(onTerminate);
    _set_purecall_handler(onPureCall);
    _set_invalid_parameter_handler(onInvalidParameter);
  }
} install;

}  // namespace

namespace regs {

// the terminate handler is kept per thread by the CRT: main (and any thread that wants it) sets it for itself
void crashThreadHandlers() { std::set_terminate(onTerminate); }

// REGS_CRASH_TEST=<kind> (tests, main.cpp): access, null-call, divide, stack, abort, throw, terminate, pure, param
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
  } else if (k == "stack") {
    struct R {
      static int down(volatile int n) {
        volatile char pad[4096];
        pad[0] = char(n);
        return down(n + 1) + pad[0];
      }
    };
    R::down(0);
  } else if (k == "abort") {
    std::abort();
  } else if (k == "throw") {
    throw std::runtime_error("REGS_CRASH_TEST throw");
  } else if (k == "terminate") {
    std::terminate();
  } else if (k == "pure") {
    struct B {
      B() { call(); }
      void call() { f(); }
      virtual void f() = 0;
    };
    struct D : B {
      void f() override {}
    } d;
  } else if (k == "param") {
    std::printf(nullptr);
  }
}

// regs-tool symbolize <report.txt> [<folder with the build's exes and PDBs>]: the report with names and source lines
// after each "module+0x..." stack line, looked up in the PDBs of that build (make-release keeps them in
// OpenCiM-<version>-symbols.zip)
int crashSymbolize(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: regs-tool symbolize <report.txt> [<folder with the build's exe and pdb>]\n");
    return 2;
  }
  const fs::path report = argv[2];
  fs::path dir = argc >= 4 ? fs::path(argv[3]) : fs::path();
  if (dir.empty()) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    dir = fs::path(self).parent_path();
  }
  std::ifstream in(report, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", report.string().c_str());
    return 1;
  }
  HANDLE h = reinterpret_cast<HANDLE>(0x5e5e);  // any value: symbols for files, not for a live process
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
  if (!SymInitializeW(h, dir.c_str(), FALSE)) {
    std::fprintf(stderr, "SymInitialize failed\n");
    return 1;
  }
  struct Mod {
    std::string name;
    DWORD64 base = 0;
  };
  std::vector<Mod> mods;
  DWORD64 nextBase = 0x10000000;
  auto baseOf = [&](const std::string& name) -> DWORD64 {
    for (const Mod& m : mods)
      if (_stricmp(m.name.c_str(), name.c_str()) == 0) return m.base;
    const fs::path image = dir / name;
    DWORD64 base = 0;
    std::error_code ec;
    if (fs::exists(image, ec)) {
      base = SymLoadModuleExW(h, nullptr, image.c_str(), nullptr, nextBase, 0, nullptr, 0);
      if (base) nextBase += 0x10000000;
    }
    mods.push_back({name, base});
    return base;
  };
  int named = 0;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::printf("%s", line.c_str());
    // "  regs.exe+0x1a2b3" (maybe with names already)
    const size_t plus = line.find("+0x");
    const size_t start = line.find_first_not_of(' ');
    if (plus != std::string::npos && start != std::string::npos && start < plus && line.find(' ', start) > plus) {
      const std::string mod = line.substr(start, plus - start);
      const DWORD64 off = std::strtoull(line.c_str() + plus + 3, nullptr, 16);
      if (const DWORD64 base = baseOf(mod)) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (line.find("  ", plus) == std::string::npos && SymFromAddr(h, base + off, &disp, sym)) {
          std::printf("  %s", sym->Name);
          IMAGEHLP_LINE64 src{};
          src.SizeOfStruct = sizeof src;
          DWORD ldisp = 0;
          if (SymGetLineFromAddr64(h, base + off, &ldisp, &src))
            std::printf("  %s:%lu", fs::path(src.FileName).filename().string().c_str(), src.LineNumber);
          ++named;
        }
      }
    }
    std::printf("\n");
  }
  SymCleanup(h);
  std::fprintf(stderr, "%d lines named (symbols from %s)\n", named, dir.string().c_str());
  return 0;
}

}  // namespace regs
