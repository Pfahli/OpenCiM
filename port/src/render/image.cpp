#include "render/image.h"

#include <algorithm>
#include <cstring>

namespace regs::render {

size_t levelSize(Image::Format f, int w, int h) {
  switch (f) {
    case Image::Format::DXT1: return size_t(std::max(1, (w + 3) / 4)) * size_t(std::max(1, (h + 3) / 4)) * 8;
    case Image::Format::DXT3:
    case Image::Format::DXT5: return size_t(std::max(1, (w + 3) / 4)) * size_t(std::max(1, (h + 3) / 4)) * 16;
    case Image::Format::BGR8: return size_t(w) * size_t(h) * 3;
    default: return size_t(w) * size_t(h) * 4;
  }
}

bool parseDds(const std::vector<uint8_t>& d, Image& out, std::string* error, int mipSkip) {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0) return fail("not a DDS file");
  auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, d.data() + at, 4); return v; };
  int h = int(u32(12)), w = int(u32(16));
  int mips = std::max(1, int(u32(28)));
  uint32_t pfFlags = u32(80), fourCC = u32(84), bits = u32(88), aMask = u32(104), caps2 = u32(112);
  if (w <= 0 || h <= 0) return fail("bad DDS size");
  Image img;
  if (pfFlags & 0x4) {  // DDPF_FOURCC
    switch (fourCC) {
      case 0x31545844: img.format = Image::Format::DXT1; break;  // "DXT1"
      case 0x33545844: img.format = Image::Format::DXT3; break;
      case 0x35545844: img.format = Image::Format::DXT5; break;
      default: return fail("DXTn texture format is not supported!");
    }
  } else if (bits == 32) {
    img.format = Image::Format::BGRA8;
  } else if (bits == 24) {
    img.format = Image::Format::BGR8;
  } else {
    return fail("unsupported DDS pixel format");
  }
  img.cube = (caps2 & 0x200) != 0;
  int faces = img.cube ? 6 : 1;
  int skip = std::clamp(mipSkip, 0, mips - 1);
  img.width = std::max(1, w >> skip);
  img.height = std::max(1, h >> skip);
  img.mipCount = mips - skip;
  size_t at = 128;
  for (int f = 0; f < faces; ++f) {
    for (int m = 0; m < mips; ++m) {
      int lw = std::max(1, w >> m), lh = std::max(1, h >> m);
      size_t n = levelSize(img.format, lw, lh);
      if (at + n > d.size()) {
        // "Warning: invalid image data": keep the levels read so far
        if (m <= skip) return fail("truncated DDS data");
        img.mipCount = std::min(img.mipCount, m - skip);
        break;
      }
      if (m >= skip) {
        img.levels.emplace_back(d.begin() + std::ptrdiff_t(at), d.begin() + std::ptrdiff_t(at + n));
        if (img.format == Image::Format::BGRA8 && aMask == 0)
          for (size_t i = 3; i < n; i += 4) img.levels.back()[i] = 0xFF;  // X8R8G8B8: opaque
      }
      at += n;
    }
  }
  out = std::move(img);
  return true;
}

}  // namespace regs::render

namespace regs::render {

namespace {

void rgb565(uint16_t c, uint8_t out[3]) {
  out[0] = uint8_t((c >> 11 & 31) * 255 / 31);
  out[1] = uint8_t((c >> 5 & 63) * 255 / 63);
  out[2] = uint8_t((c & 31) * 255 / 31);
}

// one 4×4 colour block (8 bytes); dxt1 = the 3-colour mode with transparent black is possible
void colorBlock(const uint8_t* b, bool dxt1, uint8_t px[16][4]) {
  uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
  uint8_t pal[4][4];
  rgb565(c0, pal[0]);
  rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || !dxt1) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2), pal[3][k] = 0;
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
  uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
  for (int i = 0; i < 16; ++i) std::memcpy(px[i], pal[bits >> (2 * i) & 3], 4);
}

}  // namespace

bool decodeRgba(const Image& img, int level, std::vector<uint8_t>& rgba, int& w, int& h) {
  if (img.cube || level < 0 || size_t(level) >= img.levels.size()) return false;
  w = std::max(1, img.width >> level);
  h = std::max(1, img.height >> level);
  const std::vector<uint8_t>& d = img.levels[size_t(level)];
  rgba.assign(size_t(w) * size_t(h) * 4, 0);
  if (d.size() < levelSize(img.format, w, h)) return false;
  if (!img.compressed()) {
    int bpp = img.format == Image::Format::BGR8 ? 3 : 4;
    for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
      const uint8_t* s = d.data() + i * size_t(bpp);
      uint8_t* o = rgba.data() + i * 4;
      o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = bpp == 4 ? s[3] : 255;
    }
    return true;
  }
  const size_t blockBytes = img.format == Image::Format::DXT1 ? 8 : 16;
  int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      const uint8_t* b = d.data() + (size_t(by) * size_t(bw) + size_t(bx)) * blockBytes;
      uint8_t px[16][4];
      if (img.format == Image::Format::DXT1) {
        colorBlock(b, true, px);
      } else {
        colorBlock(b + 8, false, px);
        if (img.format == Image::Format::DXT3) {
          for (int i = 0; i < 16; ++i) px[i][3] = uint8_t((b[i / 2] >> (4 * (i & 1)) & 15) * 17);
        } else {
          uint8_t a[8] = {b[0], b[1]};
          if (a[0] > a[1])
            for (int k = 1; k < 7; ++k) a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1]) / 7);
          else {
            for (int k = 1; k < 5; ++k) a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1]) / 5);
            a[6] = 0, a[7] = 255;
          }
          uint64_t bits = 0;
          for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
          for (int i = 0; i < 16; ++i) px[i][3] = a[bits >> (3 * i) & 7];
        }
      }
      for (int i = 0; i < 16; ++i) {
        int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x < w && y < h) std::memcpy(rgba.data() + (size_t(y) * size_t(w) + size_t(x)) * 4, px[i], 4);
      }
    }
  return true;
}

std::vector<uint8_t> encodePng(const uint8_t* rgba, int w, int h) {
  static const auto crcTable = [] {
    std::vector<uint32_t> t(256);
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  std::vector<uint8_t> out;
  auto be32 = [](std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(uint8_t(v >> 24)), o.push_back(uint8_t(v >> 16)), o.push_back(uint8_t(v >> 8)), o.push_back(uint8_t(v));
  };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    size_t start = out.size();
    be32(out, uint32_t(data.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = start + 4; i < out.size(); ++i) c = crcTable[(c ^ out[i]) & 0xFF] ^ (c >> 8);
    be32(out, c ^ 0xFFFFFFFFu);
  };
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  out.insert(out.end(), sig, sig + 8);
  std::vector<uint8_t> ihdr;
  be32(ihdr, uint32_t(w));
  be32(ihdr, uint32_t(h));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  chunk("IHDR", ihdr);
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + size_t(y) * size_t(w) * 4, rgba + size_t(y + 1) * size_t(w) * 4);
  }
  std::vector<uint8_t> z{0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
  for (size_t at = 0; at < raw.size(); at += 65535) {
    size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
  }
  be32(z, (b << 16) | a);
  chunk("IDAT", z);
  chunk("IEND", {});
  return out;
}

}  // namespace regs::render
