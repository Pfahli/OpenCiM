// Prints a symbolized stack trace when the process crashes (developer builds; uses DbgHelp and the PDB).
#include <cstdio>
#include <windows.h>
#include <dbghelp.h>

namespace {

LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  SymInitialize(proc, nullptr, TRUE);
  std::fprintf(stderr, "\n*** crash: exception 0x%08lx at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
  CONTEXT ctx = *ep->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrStack.Offset = ctx.Rsp;
  frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 40; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr))
      break;
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof line;
    DWORD ldisp = 0;
    const char* name = SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
    if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &ldisp, &line))
      std::fprintf(stderr, "  %s  %s:%lu\n", name, line.FileName, line.LineNumber);
    else
      std::fprintf(stderr, "  %s\n", name);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

struct Install {
  Install() { SetUnhandledExceptionFilter(onCrash); }
} install;

}  // namespace
