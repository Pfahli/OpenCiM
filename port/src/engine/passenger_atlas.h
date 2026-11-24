#pragma once
// Passenger silhouettes for the lit windows of trams, buses and trains at night (port, 2026-11-24; the default
// shader's regs_passengers in engine/shaders.cpp). The game's own citizen models, posed in their idle animation and
// seen from eight angles round (front, 45°, side, …), rasterised into one R8 atlas: a row per model, a column per
// angle. The shader picks a model, an angle and a mirror per seat, so the figures in a window differ.
#include <cstdint>
#include <string>
#include <vector>

namespace regs {
class Vfs;
}

namespace regs::engine {

struct PassengerAtlas {
  static constexpr int kCols = 8;                 // angles: k·45° about the vertical
  static constexpr int kCellW = 44, kCellH = 92;  // texels of one figure (divisible by 4: two mip levels below)
  static constexpr float kUnitsW = 1.1f, kUnitsH = 2.3f;  // the cell in model units (the game's people are ~2.2 tall)
  int rows = 0;                     // one per model
  std::vector<uint8_t> pixels;      // kCols·kCellW × rows·kCellH coverage, row 0 = the top of the first figure
  std::vector<std::string> models;  // the .object of each row
  int width() const { return kCols * kCellW; }
  int height() const { return rows * kCellH; }
};

// every citizen model of the base game (metro/objects/characters/<era>/<type>/<name>.object with the <name>.script
// that loads its skeleton and "idle" animation), posed in the middle of its idle crop
bool buildPassengerAtlas(const Vfs& vfs, PassengerAtlas& out);

// the atlas as a texture (made once, with two mip levels), and its shader parameters: rows, columns, the cell's
// height and width in model units; 0 and info[0] = 0 when there is none
uint32_t passengerAtlasTexture(const Vfs& vfs, float info[4]);

}  // namespace regs::engine
