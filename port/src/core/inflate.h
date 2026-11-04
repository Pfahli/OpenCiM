#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace regs {

// zlib stream (RFC 1950) decoder, as used by GS10 archive entries (tag 0x01120000). Returns false on corrupt input.
// expectedSize, when non-zero, pre-sizes the output; the result is still checked by the caller.
bool zlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t expectedSize = 0);

// Raw DEFLATE (RFC 1951) decoder.
bool rawInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out);

uint32_t adler32(std::span<const uint8_t> data);

}  // namespace regs
