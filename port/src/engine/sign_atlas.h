#pragma once
// Destination display texts (port, 2026-11-24): each line's text ("12 Stadtplatz", "Stadtplatz 12", "12") rendered
// once with the game's own fonts into one R8 texture, which the vehicles' displays read (engine/shaders.cpp
// regs_signs): a strip per text, its upper part in Tahoma 8 px at one texel per pixel (an LED display shows each
// pixel as a dot), its lower part in Blair 18 px (the printed roller blinds and number boxes of older vehicles).
#include <cstdint>
#include <string>

namespace regs::script {
class VM;
}

namespace regs::engine {

struct SignText {
  int slot = -1;          // strip in the atlas, -1: none
  float ledWidth = 0;     // texels (= LED dots) of the Tahoma text
  float printWidth = 0;   // texels of the Blair text
};

constexpr int kSignAtlasW = 512, kSignStripH = 48, kSignLedRows = 14, kSignPrintTop = 16, kSignPrintRows = 32;
constexpr int kSignSlots = 192;

// The strip of a text, rendered on first use (needs the GPU; empty text: none).
SignText signText(script::VM& vm, const std::u16string& text);
// The atlas texture (0 before the first text) for kSignUnit.
uint32_t signAtlasTexture();

}  // namespace regs::engine
