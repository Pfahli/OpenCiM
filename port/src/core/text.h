#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace regs {

// The engine works in UTF-16 (wchar_t on Windows). The port keeps script strings as std::u16string and uses
// UTF-8 std::string for paths and logs.

std::string utf16ToUtf8(std::u16string_view s);
std::u16string utf8ToUtf16(std::string_view s);

// Windows-1252 single byte -> UTF-16 code unit (0x80-0x9F map to typographic characters).
char16_t cp1252ToUtf16(uint8_t b);
std::string cp1252ToUtf8(std::string_view s);

// Decode a script/text file as the engine does (gsParser::parseSource): a FF FE byte-order mark means UTF-16LE, an
// EF BB BF mark UTF-8, anything else one byte per code unit, zero-extended (Latin-1).
std::u16string decodeTextFile(std::span<const uint8_t> bytes);

}  // namespace regs
