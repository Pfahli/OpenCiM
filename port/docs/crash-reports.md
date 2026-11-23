# Crash reports

When OpenCiM crashes, it saves a report next to the player's saves and says where it is. Nothing is sent anywhere:
the player attaches the files to a bug report (there is no repository or server yet; an upload can come with the
GitHub releases).

## What the player sees

A message box: "OpenCiM stopped because of an error", the report's path, a request to attach it and the `.dmp` file
beside it, and **Open the folder now?** (Yes opens Explorer with the report selected). German on a German Windows,
otherwise English (the game's own strings are not safe to reach from a crash). `REGS_NO_DIALOGS=1` leaves the box
out (test runs).

## The files

Both in `Documents\OpenCiM\crashes\` (before the Documents folder is known, and for `regs-tool`, in
`%TEMP%\OpenCiM-crashes\`):

* `crash-<yyyy-mm-dd-hhmmss>.txt`, readable:
  * the version, build number, commit and build date;
  * the time and how long the program had been running;
  * Windows version, CPU, threads, RAM;
  * **What** happened: access violation reading/writing/executing an address, stack overflow, division by zero,
    a C++ exception with its type and `what()`, `abort()`, `std::terminate()`, a pure virtual call, an invalid CRT
    argument;
  * **Where**: as `module+offset`;
  * the facts the program set while running (`crashContext`): the renderer and GPU (`Vulkan 1.3.x / <GPU>` or the
    OpenGL string), the game folder and where it was found;
  * the crashing thread's stack, one `module+offset` per frame, with function names and source lines when the PDB
    is next to the exe (developer builds);
  * the last 200 lines of the program's output (log lines, script echo, script errors);
  * the last 40 lines of `log_metro.txt`.
* `crash-<...>.dmp`, a minidump: all threads, their stacks and the memory they point to, thread info, unloaded
  modules; usually a few MB. It opens in Visual Studio or WinDbg with the build's PDBs.

## Reading a report

`make-release.ps1` keeps the exes and PDBs of each release in `OpenCiM-<version>-symbols.zip` (not part of the
download). The report's commit says which build it came from. To put names on a player's stack:

```
regs-tool symbolize crash-2026-11-23-153012.txt <folder with that build's regs.exe and regs.pdb>
```

It prints the report with the function and `file:line` after every `module+0x...` line (frames of Windows and
driver DLLs stay as they are). For the dump: open it in Visual Studio, set the symbol path to that folder, and use
"Debug with Native Only".

The release build has PDBs since 2026-11-23 (`/Z7`, `/DEBUG:FULL /OPT:REF /OPT:ICF`, same code as before). The exe
holds only the PDB's file name (`/PDBALTPATH:%_PDB%`), not the build machine's path.

## How it works

`tools/crash_win32.cpp` (linked into `regs` and `regs-tool`), `core/crash.{h,cpp}` (the context and log ring in the
engine library; `core/log.cpp` feeds it):

* `SetUnhandledExceptionFilter` catches every unhandled structured exception, uncaught C++ exceptions included
  (code `0xE06D7363`; the type is read from the throw info, `what()` when the type derives from `std::exception`).
* The CRT's error paths go to the same report with code `0xE0C1A001` and the reason, from a context captured where
  they are called (raising an exception instead fails: `terminate()` runs its handler inside an `__except` of its own,
  then calls `abort()`): `SIGABRT` (`abort()`), `std::set_terminate` (kept per thread by the CRT: set at start-up and
  again in `main`), `_set_purecall_handler`, `_set_invalid_parameter_handler`. `_set_abort_behavior` switches off the
  CRT's own box and its fail-fast, which would skip the report.
* The filter writes the report on a new thread with 1 MB of stack and waits for it: the crashing thread may have none
  left (stack overflow), and `MiniDumpWriteDump` is meant to be called from another thread. The main thread also has
  64 KB guaranteed for the filter itself (`SetThreadStackGuarantee`).
* A second thread crashing during the report waits; after the report and the box the process ends with exit code 3
  (no Windows Error Reporting box).
* Locks are taken with a short timeout (the crash may have happened inside the logger).

Not caught: heap corruption that Windows ends with a fail-fast (`0xC0000374`, reported straight to Windows Error
Reporting), and a process killed from outside. No handler yet on Linux and macOS (the port builds on Windows only so
far).

## Tests

`REGS_CRASH_TEST=<kind>[:<frame>]` crashes on purpose at that frame (default 60): `access`, `null-call`, `divide`,
`stack`, `abort`, `throw`, `terminate`, `pure`, `param`.

Checked 2026-11-23 (Vulkan, RTX 3080): all nine write a report and a dump (0.3–0.4 MB, 1.5 MB for the stack
overflow) with the right "What" and "Where" (the frame that failed, past the runtime's own frames), the stack with
names, renderer and game folder, and the last log lines. `regs-tool symbolize` named a report with its names
stripped from a separate folder with `regs.exe` and `regs.pdb`. The message box was read and answered with No by a
script; the process ended with exit code 3. Not tried: opening a dump in Visual Studio, a crash in another thread
than the main one, a release package's symbols zip.
