#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace regs {

// The engine works in UTF-16 (wchar_t on Windows). The port keeps script strings as std::u16string and uses
// UTF-8 std::string for paths and logs.

std::string utf16ToUtf8(std::u16string_view s);
std::u16string utf8ToUtf16(std::string_view s);

// Paths from and to UTF-16 / UTF-8 on every system (docs/linux.md): Windows paths are UTF-16 (wchar_t is 2 bytes),
// others are UTF-8 bytes (wchar_t is 4 bytes there, so a path's wstring() is not UTF-16)
std::filesystem::path pathFromUtf16(std::u16string_view s);
std::u16string pathToUtf16(const std::filesystem::path& p);
// (UTF-8: pathToUtf8 / pathFromUtf8 in core/game_find.h)

// Windows-1252 single byte -> UTF-16 code unit (0x80-0x9F map to typographic characters).
char16_t cp1252ToUtf16(uint8_t b);
std::string cp1252ToUtf8(std::string_view s);

// Decode a script/text file as the engine does (gsParser::parseSource): a FF FE byte-order mark means UTF-16LE, an
// EF BB BF mark UTF-8, anything else one byte per code unit, zero-extended (Latin-1).
std::u16string decodeTextFile(std::span<const uint8_t> bytes);

}  // namespace regs
