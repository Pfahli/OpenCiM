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
