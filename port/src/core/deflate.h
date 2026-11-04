#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace regs {

// zlib stream (RFC 1950/1951) encoder for writePackage's compressed entries (tag 0x01120000, the engine uses zlib
// compress()): LZ77 over a 32 KB window with hash chains and lazy matching, dynamic Huffman blocks.
std::vector<uint8_t> zlibDeflate(std::span<const uint8_t> in);

}  // namespace regs
