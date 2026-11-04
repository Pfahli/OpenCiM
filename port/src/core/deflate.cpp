#include "core/deflate.h"

#include <algorithm>
#include <queue>

#include "core/inflate.h"

namespace regs {

namespace {

constexpr int kWindow = 32768, kMinMatch = 3, kMaxMatch = 258, kMaxChain = 48, kLazy = 32;
constexpr uint32_t kMatchFlag = 0x80000000u;

const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct BitWriter {
  std::vector<uint8_t>& out;
  uint64_t acc = 0;
  int n = 0;
  void bits(uint32_t v, int count) {  // LSB first
    acc |= uint64_t(v) << n;
    n += count;
    while (n >= 8) {
      out.push_back(uint8_t(acc));
      acc >>= 8;
      n -= 8;
    }
  }
  void flush() {
    if (n > 0) out.push_back(uint8_t(acc));
    acc = 0;
    n = 0;
  }
};

int lengthCode(int len) {
  int c = 28;
  while (kLengthBase[c] > len) --c;
  return c;
}

int distCode(int dist) {
  int c = 29;
  while (kDistBase[c] > dist) --c;
  return c;
}

// Huffman code lengths of at most maxBits: plain Huffman, frequencies halved until the tree is shallow enough
std::vector<uint8_t> codeLengths(const std::vector<uint32_t>& freq, int maxBits) {
  std::vector<uint8_t> len(freq.size(), 0);
  std::vector<uint32_t> f = freq;
  for (;;) {
    struct Node {
      uint64_t w;
      int left, right, sym;
    };
    std::vector<Node> nodes;
    using Item = std::pair<uint64_t, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    for (size_t s = 0; s < f.size(); ++s)
      if (f[s]) {
        nodes.push_back({f[s], -1, -1, int(s)});
        pq.push({f[s], int(nodes.size() - 1)});
      }
    if (nodes.empty()) return len;
    if (nodes.size() == 1) {
      len[size_t(nodes[0].sym)] = 1;
      return len;
    }
    while (pq.size() > 1) {
      Item a = pq.top();
      pq.pop();
      Item b = pq.top();
      pq.pop();
      nodes.push_back({a.first + b.first, a.second, b.second, -1});
      pq.push({a.first + b.first, int(nodes.size() - 1)});
    }
    std::vector<std::pair<int, int>> stack{{pq.top().second, 0}};
    int deepest = 0;
    std::fill(len.begin(), len.end(), uint8_t(0));
    while (!stack.empty()) {
      auto [i, depth] = stack.back();
      stack.pop_back();
      if (nodes[size_t(i)].sym >= 0) {
        len[size_t(nodes[size_t(i)].sym)] = uint8_t(depth);
        deepest = std::max(deepest, depth);
      } else {
        stack.push_back({nodes[size_t(i)].left, depth + 1});
        stack.push_back({nodes[size_t(i)].right, depth + 1});
      }
    }
    if (deepest <= maxBits) return len;
    for (uint32_t& x : f)
      if (x) x = (x >> 1) | 1;
  }
}

// canonical codes, bit-reversed for LSB-first output
std::vector<uint16_t> canonicalCodes(const std::vector<uint8_t>& len) {
  int count[16] = {}, next[16] = {};
  for (uint8_t l : len)
    if (l) ++count[l];
  for (int b = 1, code = 0; b < 16; ++b) {  // RFC 1951 3.2.2 (count[0] stays 0)
    code = (code + count[b - 1]) << 1;
    next[b] = code;
  }
  std::vector<uint16_t> codes(len.size(), 0);
  for (size_t s = 0; s < len.size(); ++s) {
    int l = len[s];
    if (!l) continue;
    uint32_t c = uint32_t(next[l]++), r = 0;
    for (int k = 0; k < l; ++k) r |= ((c >> k) & 1u) << (l - 1 - k);
    codes[s] = uint16_t(r);
  }
  return codes;
}

void writeBlock(BitWriter& w, const std::vector<uint32_t>& tokens, bool final) {
  std::vector<uint32_t> litFreq(286, 0), distFreq(30, 0);
  for (uint32_t t : tokens) {
    if (t & kMatchFlag) {
      ++litFreq[size_t(257 + lengthCode(int((t >> 16) & 0x7FFF)))];
      ++distFreq[size_t(distCode(int(t & 0xFFFF)))];
    } else {
      ++litFreq[t];
    }
  }
  litFreq[256] = 1;
  std::vector<uint8_t> litLen = codeLengths(litFreq, 15), distLen = codeLengths(distFreq, 15);
  // a complete distance code of at least two symbols (one used code gets a partner)
  int used = int(std::count_if(distLen.begin(), distLen.end(), [](uint8_t l) { return l != 0; }));
  for (size_t s = 0; used < 2 && s < distLen.size(); ++s)
    if (!distLen[s]) {
      distLen[s] = 1;
      ++used;
    }
  if (used == 2)
    for (uint8_t& l : distLen)
      if (l) l = 1;
  int hlit = 286, hdist = 30;
  while (hlit > 257 && !litLen[size_t(hlit - 1)]) --hlit;
  while (hdist > 1 && !distLen[size_t(hdist - 1)]) --hdist;

  // the code lengths, run-length encoded with 16 (repeat previous 3-6), 17 (zeros 3-10), 18 (zeros 11-138)
  std::vector<uint8_t> all(litLen.begin(), litLen.begin() + hlit);
  all.insert(all.end(), distLen.begin(), distLen.begin() + hdist);
  struct Rle {
    uint8_t sym, extra;
  };
  std::vector<Rle> rle;
  for (size_t i = 0; i < all.size();) {
    size_t run = 1;
    while (i + run < all.size() && all[i + run] == all[i]) ++run;
    if (all[i] == 0 && run >= 3) {
      size_t r = std::min<size_t>(run, 138);
      rle.push_back(r >= 11 ? Rle{18, uint8_t(r - 11)} : Rle{17, uint8_t(r - 3)});
      i += r;
    } else if (all[i] != 0 && run >= 4) {
      rle.push_back({all[i], 0});
      size_t r = std::min<size_t>(run - 1, 6);
      rle.push_back({16, uint8_t(r - 3)});
      i += 1 + r;
    } else {
      rle.push_back({all[i], 0});
      ++i;
    }
  }
  std::vector<uint32_t> clFreq(19, 0);
  for (const Rle& r : rle) ++clFreq[r.sym];
  std::vector<uint8_t> clLen = codeLengths(clFreq, 7);
  std::vector<uint16_t> clCode = canonicalCodes(clLen), litCode = canonicalCodes(litLen), distCodeV = canonicalCodes(distLen);
  int hclen = 19;
  while (hclen > 4 && !clLen[kCodeLengthOrder[hclen - 1]]) --hclen;

  w.bits(final ? 1 : 0, 1);
  w.bits(2, 2);  // dynamic Huffman
  w.bits(uint32_t(hlit - 257), 5);
  w.bits(uint32_t(hdist - 1), 5);
  w.bits(uint32_t(hclen - 4), 4);
  for (int k = 0; k < hclen; ++k) w.bits(clLen[kCodeLengthOrder[k]], 3);
  for (const Rle& r : rle) {
    w.bits(clCode[r.sym], clLen[r.sym]);
    if (r.sym == 16) w.bits(r.extra, 2);
    else if (r.sym == 17) w.bits(r.extra, 3);
    else if (r.sym == 18) w.bits(r.extra, 7);
  }
  for (uint32_t t : tokens) {
    if (t & kMatchFlag) {
      int len = int((t >> 16) & 0x7FFF), dist = int(t & 0xFFFF);
      int lc = lengthCode(len), dc = distCode(dist);
      w.bits(litCode[size_t(257 + lc)], litLen[size_t(257 + lc)]);
      w.bits(uint32_t(len - kLengthBase[lc]), kLengthExtra[lc]);
      w.bits(distCodeV[size_t(dc)], distLen[size_t(dc)]);
      w.bits(uint32_t(dist - kDistBase[dc]), kDistExtra[dc]);
    } else {
      w.bits(litCode[t], litLen[t]);
    }
  }
  w.bits(litCode[256], litLen[256]);
}

}  // namespace

std::vector<uint8_t> zlibDeflate(std::span<const uint8_t> in) {
  std::vector<uint8_t> out{0x78, 0x9C};  // deflate, 32 KB window, default level
  BitWriter w{out};
  const size_t n = in.size();
  if (n == 0) {
    w.bits(1, 1);
    w.bits(1, 2);  // fixed Huffman block with only the end code (7 zero bits)
    w.bits(0, 7);
  } else {
    std::vector<int32_t> head(1 << 15, -1), prev(kWindow, -1);
    auto hash = [&](size_t i) { return ((uint32_t(in[i]) << 10) ^ (uint32_t(in[i + 1]) << 5) ^ in[i + 2]) & 0x7FFF; };
    auto insert = [&](size_t i) {
      if (i + 2 >= n) return;
      uint32_t h = hash(i);
      prev[i & (kWindow - 1)] = head[h];
      head[h] = int32_t(i);
    };
    auto longest = [&](size_t i, int& dist) {
      int best = 0;
      if (i + kMinMatch > n) return 0;
      int maxLen = int(std::min<size_t>(kMaxMatch, n - i));
      int32_t j = head[hash(i)];
      for (int chain = 0; j >= 0 && chain < kMaxChain; ++chain) {
        if (size_t(j) >= i || i - size_t(j) > size_t(kWindow - 1)) break;
        if (in[size_t(j) + size_t(best)] == in[i + size_t(best)]) {
          int l = 0;
          while (l < maxLen && in[size_t(j) + size_t(l)] == in[i + size_t(l)]) ++l;
          if (l > best) {
            best = l;
            dist = int(i - size_t(j));
            if (l == maxLen) break;
          }
        }
        int32_t nj = prev[size_t(j) & (kWindow - 1)];
        if (nj >= j) break;  // an overwritten slot points forward: the chain is stale
        j = nj;
      }
      return best >= kMinMatch ? best : 0;
    };
    std::vector<uint32_t> tokens;
    tokens.reserve(1 << 16);
    size_t i = 0;
    while (i < n) {
      int dist = 0, len = longest(i, dist);
      if (len && len < kLazy && i + 1 < n) {  // lazy: a longer match one byte later wins
        int dist2 = 0;
        insert(i);
        int len2 = longest(i + 1, dist2);
        if (len2 > len) {
          tokens.push_back(in[i]);
          ++i;
          len = len2;
          dist = dist2;
        } else {
          tokens.push_back(kMatchFlag | uint32_t(len) << 16 | uint32_t(dist));
          for (size_t k = i + 1; k < i + size_t(len); ++k) insert(k);
          i += size_t(len);
          goto next;
        }
      }
      if (len) {
        tokens.push_back(kMatchFlag | uint32_t(len) << 16 | uint32_t(dist));
        for (size_t k = i; k < i + size_t(len); ++k) insert(k);
        i += size_t(len);
      } else {
        tokens.push_back(in[i]);
        insert(i);
        ++i;
      }
    next:
      if (tokens.size() >= 65536 && i < n) {
        writeBlock(w, tokens, false);
        tokens.clear();
      }
    }
    writeBlock(w, tokens, true);
  }
  w.flush();
  uint32_t a = adler32(in);
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(a >> s));
  return out;
}

}  // namespace regs
