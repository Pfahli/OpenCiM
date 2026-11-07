#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace regs::render {

// A decoded texture file. Only DDS is accepted, as in the engine (FUN_10041cd0 / FUN_10041f30,
// docs/renderer/interfaces-2d-output.md §3.1).
struct Image {
  enum class Format : uint8_t { BGRA8, BGR8, DXT1, DXT3, DXT5 };
  Format format = Format::BGRA8;
  int width = 0, height = 0;
  bool cube = false;
  // levels[face * mipCount + mip]; one face unless cube
  std::vector<std::vector<uint8_t>> levels;
  int mipCount = 1;

  bool compressed() const { return format >= Format::DXT1; }
};

// Parses a DDS file. mipSkip drops that many leading levels (never the last one), like the engine's loader.
bool parseDds(const std::vector<uint8_t>& file, Image& out, std::string* error, int mipSkip = 0);

// Byte size of one level.
size_t levelSize(Image::Format f, int w, int h);

}  // namespace regs::render

namespace regs::render {

// Decodes one level of a 2D image (BGRA8, BGR8 or DXT1/3/5) into RGBA8 rows, top row first. False for cube maps or a
// missing level.
bool decodeRgba(const Image& img, int level, std::vector<uint8_t>& rgba, int& w, int& h);

// A PNG file (8-bit RGBA, stored deflate blocks) of w×h RGBA8 pixels.
std::vector<uint8_t> encodePng(const uint8_t* rgba, int w, int h);

}  // namespace regs::render
