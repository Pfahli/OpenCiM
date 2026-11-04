#include "core/inflate.h"

#include <array>
#include <cstring>

namespace regs {

namespace {

struct BitReader {
  const uint8_t* p;
  const uint8_t* end;
  uint32_t bits = 0;
  int count = 0;
  bool overrun = false;

  uint32_t get(int n) {
    while (count < n) {
      uint32_t b = 0;
      if (p < end) b = *p++;
      else overrun = true;
      bits |= b << count;
      count += 8;
    }
    uint32_t v = bits & ((1u << n) - 1);
    bits >>= n;
    count -= n;
    return v;
  }
  void alignByte() {
    bits >>= count & 7;
    count -= count & 7;
  }
};

// canonical Huffman table: counts per length + symbols sorted by code
struct Huffman {
  std::array<uint16_t, 16> counts{};
  std::array<uint16_t, 320> symbols{};

  bool build(const uint8_t* lengths, int n) {
    counts.fill(0);
    for (int i = 0; i < n; ++i) counts[lengths[i]]++;
    counts[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
      left <<= 1;
      left -= counts[len];
      if (left < 0) return false;  // over-subscribed
    }
    std::array<uint16_t, 16> offs{};
    for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + counts[len]);
    for (int i = 0; i < n; ++i)
      if (lengths[i]) symbols[offs[lengths[i]]++] = uint16_t(i);
    return true;
  }

  int decode(BitReader& br) const {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= int(br.get(1));
      int count = counts[len];
      if (code - count < first) return symbols[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlock(BitReader& br, std::vector<uint8_t>& out, const Huffman& lit, const Huffman& dist) {
  for (;;) {
    int sym = lit.decode(br);
    if (sym < 0 || br.overrun) return false;
    if (sym < 256) {
      out.push_back(uint8_t(sym));
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      size_t len = kLenBase[sym] + br.get(kLenExtra[sym]);
      int ds = dist.decode(br);
      if (ds < 0 || ds >= 30) return false;
      size_t d = kDistBase[ds] + br.get(kDistExtra[ds]);
      if (d > out.size()) return false;
      size_t from = out.size() - d;
      for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
    }
  }
}

}  // namespace

bool rawInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out) {
  BitReader br{in.data(), in.data() + in.size()};
  static Huffman fixedLit, fixedDist;
  static bool fixedReady = false;
  if (!fixedReady) {
    uint8_t l[288];
    std::memset(l, 8, 144);
    std::memset(l + 144, 9, 112);
    std::memset(l + 256, 7, 24);
    std::memset(l + 280, 8, 8);
    fixedLit.build(l, 288);
    uint8_t d[30];
    std::memset(d, 5, 30);
    fixedDist.build(d, 30);
    fixedReady = true;
  }
  int last;
  do {
    last = int(br.get(1));
    int type = int(br.get(2));
    if (type == 0) {
      br.alignByte();
      // the bit buffer is byte aligned now; drain whole bytes it still holds
      while (br.count >= 8) {
        --br.p;
        br.count -= 8;
      }
      br.bits = 0;
      br.count = 0;
      if (br.end - br.p < 4) return false;
      uint16_t len = uint16_t(br.p[0] | (br.p[1] << 8));
      uint16_t nlen = uint16_t(br.p[2] | (br.p[3] << 8));
      if (uint16_t(~nlen) != len) return false;
      br.p += 4;
      if (br.end - br.p < len) return false;
      out.insert(out.end(), br.p, br.p + len);
      br.p += len;
    } else if (type == 1) {
      if (!inflateBlock(br, out, fixedLit, fixedDist)) return false;
    } else if (type == 2) {
      int hlit = int(br.get(5)) + 257, hdist = int(br.get(5)) + 1, hclen = int(br.get(4)) + 4;
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      uint8_t cl[19] = {};
      for (int i = 0; i < hclen; ++i) cl[order[i]] = uint8_t(br.get(3));
      Huffman clh;
      if (!clh.build(cl, 19)) return false;
      uint8_t lengths[320] = {};
      for (int i = 0; i < hlit + hdist;) {
        int sym = clh.decode(br);
        if (sym < 0 || br.overrun) return false;
        if (sym < 16) {
          lengths[i++] = uint8_t(sym);
        } else {
          int rep, val = 0;
          if (sym == 16) {
            if (i == 0) return false;
            val = lengths[i - 1];
            rep = 3 + int(br.get(2));
          } else if (sym == 17) {
            rep = 3 + int(br.get(3));
          } else {
            rep = 11 + int(br.get(7));
          }
          if (i + rep > hlit + hdist) return false;
          while (rep--) lengths[i++] = uint8_t(val);
        }
      }
      Huffman lit, dist;
      if (!lit.build(lengths, hlit) || !dist.build(lengths + hlit, hdist)) return false;
      if (!inflateBlock(br, out, lit, dist)) return false;
    } else {
      return false;
    }
    if (br.overrun) return false;
  } while (!last);
  return true;
}

uint32_t adler32(std::span<const uint8_t> data) {
  uint32_t a = 1, b = 0;
  for (uint8_t c : data) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

bool zlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t expectedSize) {
  if (in.size() < 6) return false;
  uint8_t cmf = in[0], flg = in[1];
  if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return false;
  out.clear();
  if (expectedSize) out.reserve(expectedSize);
  if (!rawInflate(in.subspan(2, in.size() - 6), out)) return false;
  const uint8_t* t = in.data() + in.size() - 4;
  uint32_t want = (uint32_t(t[0]) << 24) | (uint32_t(t[1]) << 16) | (uint32_t(t[2]) << 8) | t[3];
  return adler32(out) == want;
}

}  // namespace regs
