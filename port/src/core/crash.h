#pragma once
// Crash reports (docs/crash-reports.md). The handler itself is per platform (tools/crash_win32.cpp: a minidump and a
// text report in Documents\OpenCiM\crashes, then a message box pointing to them); this part is what it puts in the
// report besides the stack: the last log lines and a few facts the program sets while it runs (renderer, game folder).
// Nothing is sent anywhere: the player attaches the two files to a bug report.
#include <filesystem>
#include <string>
#include <vector>

namespace regs {

// where the reports go; until it is set (and for regs-tool) %TEMP%\OpenCiM-crashes
void crashSetReportDir(const std::filesystem::path& dir);
std::filesystem::path crashReportDir();

// the message box after the report (on for the game, off for regs-tool, whose crashes happen in builds and scripts)
void crashSetDialog(bool on);
bool crashDialog();

// a fact for the report ("renderer", "Vulkan 1.3.280 / NVIDIA ..."); the same key again replaces it
void crashContext(const std::string& key, const std::string& value);

// a line of the program's output (core/log.cpp and the script echo call it); the report keeps the last 200
void crashLogLine(const std::string& line);

// for the handler: copies taken without waiting long for a lock (a crash inside the logger must not hang the report)
std::vector<std::pair<std::string, std::string>> crashContextSnapshot();
std::vector<std::string> crashLogSnapshot();

}  // namespace regs
