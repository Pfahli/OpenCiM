#include "core/inflate.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace regs {

namespace {

// LSB-first bit buffer over the input. Past the end it feeds zero bytes and counts them: a decode that used any of
// them ran over the input (overrun()), however far ahead the table lookups peeked.
struct BitReader {
  const uint8_t* p;
  const uint8_t* end;
  uint64_t bits = 0;
  int count = 0;
  size_t pad = 0;  // zero bytes fed after the end

  void refill() {
    if (end - p >= 8) {  // a whole word: as many bytes as fit, at least 7
      uint64_t w;
      std::memcpy(&w, p, 8);
      bits |= w << count;
      p += (63 - count) >> 3;
      count |= 56;
      return;
    }
    while (count < 56) {  // count stays below 64 (the word path shifts by it)
      uint64_t b = 0;
      if (p < end) b = *p++;
      else ++pad;
      bits |= b << count;
      count += 8;
    }
  }
  uint32_t peek(int n) const { return uint32_t(bits & ((uint64_t(1) << n) - 1)); }
  void drop(int n) {
    bits >>= n;
    count -= n;
  }
  uint32_t get(int n) {  // n <= 32
    if (count < n) refill();
    uint32_t v = peek(n);
    drop(n);
    return v;
  }
  bool overrun() const { return size_t(count) < pad * 8; }
  void alignByte() { drop(count & 7); }
  // after alignByte: the input position of the first byte not consumed, and the buffer emptied (stored blocks)
  const uint8_t* takeBytePosition() {
    size_t buffered = size_t(count) / 8;
    const uint8_t* at = buffered >= pad ? p - (buffered - pad) : end;
    bits = 0, count = 0, pad = 0;
    p = at;
    return at;
  }
};

uint32_t reverseBits(uint32_t v, int n) {
  uint32_t r = 0;
  for (int i = 0; i < n; ++i, v >>= 1) r = (r << 1) | (v & 1);
  return r;
}

// canonical Huffman decoder: codes up to kFastBits long in one table lookup, longer ones by comparing the bit-reversed
// next 16 bits against the per-length code limits (as stb_image's zlib decoder)
constexpr int kFastBits = 10;

struct Huffman {
  std::array<uint16_t, 1 << kFastBits> fast{};  // (length << 9) | symbol, 0 = longer code or none
  std::array<uint32_t, 17> maxCode{};           // first 16-bit left-justified code above length i
  std::array<uint16_t, 16> firstCode{}, firstSymbol{};
  std::array<uint16_t, 320> symbols{};          // in code order

  bool build(const uint8_t* lengths, int n) {
    std::array<int, 16> counts{};
    for (int i = 0; i < n; ++i) counts[lengths[i]]++;
    counts[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
      left <<= 1;
      left -= counts[len];
      if (left < 0) return false;  // over-subscribed (incomplete codes are allowed: their gaps decode as errors)
    }
    std::array<uint16_t, 16> next{};
    int code = 0, k = 0;
    for (int len = 1; len < 16; ++len) {
      next[len] = uint16_t(code);
      firstCode[len] = uint16_t(code);
      firstSymbol[len] = uint16_t(k);
      code += counts[len];
      maxCode[len] = uint32_t(code) << (16 - len);
      code <<= 1;
      k += counts[len];
    }
    maxCode[16] = 0x10000;
    fast.fill(0);
    for (int i = 0; i < n; ++i) {
      int len = lengths[i];
      if (!len) continue;
      int c = next[len] - firstCode[len] + firstSymbol[len];
      symbols[size_t(c)] = uint16_t(i);
      if (len <= kFastBits)
        for (uint32_t j = reverseBits(next[len], len); j < (1u << kFastBits); j += 1u << len)
          fast[j] = uint16_t((len << 9) | i);
      ++next[len];
    }
    return true;
  }

  // the next symbol; the caller refilled (>= 15 bits buffered, padding included). -1 for no code.
  int decode(BitReader& br) const {
    uint16_t e = fast[br.peek(kFastBits)];
    if (e) {
      br.drop(e >> 9);
      return e & 511;
    }
    uint32_t k = reverseBits(br.peek(16), 16);
    int len = kFastBits + 1;
    while (k >= maxCode[size_t(len)]) ++len;
    if (len >= 16) return -1;
    br.drop(len);
    return symbols[size_t((k >> (16 - len)) - firstCode[len] + firstSymbol[len])];
  }
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// the output: written through a pointer with room ahead (a match copies in 8-byte steps and may write up to 7 bytes
// past its end), trimmed to `pos` when done. sizeHint: the expected output size (0 unknown)
struct Output {
  std::vector<uint8_t>& v;
  size_t pos;
  Output(std::vector<uint8_t>& out, size_t sizeHint) : v(out), pos(out.size()) {
    if (sizeHint) v.resize(pos + sizeHint + 8);
  }
  void room(size_t n) {
    size_t need = pos + n + 8;
    if (need > v.size()) v.resize(std::max(need + 4096, v.size() * 2));
  }
  void finish() { v.resize(pos); }
};

// limit: the block stops early once the output holds that many bytes
bool inflateBlock(BitReader& br, Output& out, const Huffman& lit, const Huffman& dist, size_t limit) {
  while (out.pos < limit) {
    br.refill();  // >= 56 bits: a length code, its extra bits, a distance code and its extra bits fit
    int sym = lit.decode(br);
    if (sym < 0 || br.overrun()) return false;
    if (sym < 256) {
      out.room(1);
      out.v[out.pos++] = uint8_t(sym);
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      size_t len = kLenBase[sym] + br.peek(kLenExtra[sym]);
      br.drop(kLenExtra[sym]);
      int ds = dist.decode(br);
      if (ds < 0 || ds >= 30) return false;
      size_t d = kDistBase[ds] + br.peek(kDistExtra[ds]);
      br.drop(kDistExtra[ds]);
      if (d > out.pos || br.overrun()) return false;
      out.room(len);
      uint8_t* dst = out.v.data() + out.pos;
      const uint8_t* src = dst - d;
      if (d >= 8) {  // 8-byte steps read only bytes written at least 8 earlier
        for (size_t i = 0; i < len; i += 8) std::memcpy(dst + i, src + i, 8);
      } else if (d == 1) {
        std::memset(dst, *src, len);
      } else {
        for (size_t i = 0; i < len; ++i) dst[i] = src[i];
      }
      out.pos += len;
    }
  }
  return true;
}

struct FixedTables {
  Huffman lit, dist;
  FixedTables() {
    uint8_t l[288];
    std::memset(l, 8, 144);
    std::memset(l + 144, 9, 112);
    std::memset(l + 256, 7, 24);
    std::memset(l + 280, 8, 8);
    lit.build(l, 288);
    uint8_t d[30];
    std::memset(d, 5, 30);
    dist.build(d, 30);
  }
};

// the stream up to its last block, or until the output holds `limit` bytes (then true, the output possibly longer)
bool inflateStream(std::span<const uint8_t> in, Output& out, size_t limit) {
  static const FixedTables fixed;  // made once, thread-safe (workers inflate in parallel)
  BitReader br{in.data(), in.data() + in.size()};
  int last;
  do {
    if (out.pos >= limit) return true;
    br.refill();
    last = int(br.get(1));
    int type = int(br.get(2));
    if (br.overrun()) return false;
    if (type == 0) {
      br.alignByte();
      const uint8_t* p = br.takeBytePosition();
      if (in.data() + in.size() - p < 4) return false;
      uint16_t len = uint16_t(p[0] | (p[1] << 8));
      uint16_t nlen = uint16_t(p[2] | (p[3] << 8));
      if (uint16_t(~nlen) != len) return false;
      p += 4;
      size_t have = size_t(in.data() + in.size() - p);
      if (have < len) {  // a head may stop inside a stored block
        if (out.pos + have < limit) return false;
        out.room(have);
        std::memcpy(out.v.data() + out.pos, p, have);
        out.pos += have;
        return true;
      }
      out.room(len);
      std::memcpy(out.v.data() + out.pos, p, len);
      out.pos += len;
      br.p = p + len;
    } else if (type == 1) {
      if (!inflateBlock(br, out, fixed.lit, fixed.dist, limit)) return false;
    } else if (type == 2) {
      int hlit = int(br.get(5)) + 257, hdist = int(br.get(5)) + 1, hclen = int(br.get(4)) + 4;
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      uint8_t cl[19] = {};
      for (int i = 0; i < hclen; ++i) cl[order[i]] = uint8_t(br.get(3));
      Huffman clh;
      if (br.overrun() || !clh.build(cl, 19)) return false;
      uint8_t lengths[320] = {};
      for (int i = 0; i < hlit + hdist;) {
        br.refill();
        int sym = clh.decode(br);
        if (sym < 0 || br.overrun()) return false;
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
      if (!inflateBlock(br, out, lit, dist, limit)) return false;
    } else {
      return false;
    }
    if (br.overrun()) return false;
  } while (!last);
  return true;
}

bool zlibHeaderOk(std::span<const uint8_t> in) {
  if (in.size() < 6) return false;
  uint8_t cmf = in[0], flg = in[1];
  return (cmf & 0x0F) == 8 && ((cmf << 8) | flg) % 31 == 0 && !(flg & 0x20);
}

}  // namespace

bool rawInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out) { return rawInflate(in, out, 0); }

bool rawInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t sizeHint) {
  Output o(out, sizeHint);
  bool ok = inflateStream(in, o, SIZE_MAX);
  o.finish();
  return ok;
}

uint32_t adler32(std::span<const uint8_t> data) {
  // the sums reduced every 5552 bytes, the most that cannot overflow 32 bits (as zlib)
  uint32_t a = 1, b = 0;
  const uint8_t* p = data.data();
  for (size_t left = data.size(); left > 0;) {
    size_t n = std::min<size_t>(left, 5552);
    left -= n;
    while (n--) {
      a += *p++;
      b += a;
    }
    a %= 65521;
    b %= 65521;
  }
  return (b << 16) | a;
}

bool zlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t expectedSize) {
  if (!zlibHeaderOk(in)) return false;
  out.clear();
  if (!rawInflate(in.subspan(2, in.size() - 6), out, expectedSize)) return false;
  const uint8_t* t = in.data() + in.size() - 4;
  uint32_t want = (uint32_t(t[0]) << 24) | (uint32_t(t[1]) << 16) | (uint32_t(t[2]) << 8) | t[3];
  return adler32(out) == want;
}

bool zlibInflateHead(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t limit) {
  out.clear();
  if (!zlibHeaderOk(in)) return false;
  Output o(out, 0);
  bool ok = inflateStream(in.subspan(2), o, limit);
  o.finish();
  if (!ok || out.size() < limit) return false;
  out.resize(limit);
  return true;
}

}  // namespace regs
