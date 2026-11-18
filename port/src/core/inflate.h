#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace regs {

// zlib stream (RFC 1950) decoder, as used by GS10 archive entries (tag 0x01120000). Returns false on corrupt input.
// expectedSize, when non-zero, pre-sizes the output; the result is still checked by the caller.
bool zlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t expectedSize = 0);

// The first `limit` bytes of a zlib stream, from the whole stream or only its start (no checksum: the rest is not
// decoded). False when the input ends or turns corrupt before that many bytes, or the stream is shorter.
bool zlibInflateHead(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t limit);

// Raw DEFLATE (RFC 1951) decoder.
bool rawInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out);

uint32_t adler32(std::span<const uint8_t> data);

}  // namespace regs
