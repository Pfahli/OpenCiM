#pragma once
// gsGraphCtrl: the line graphs of the graphs panel and the annual report (docs/ui/graphs.md).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine/gui.h"

namespace regs::engine {

class GraphObj : public ControlObj {
 public:
  static constexpr int kSeries = 10;
  struct Series {
    std::vector<float> data;  // ring buffer of segmentCount samples (+0x1a0 + i·0x10), allocated by setArg
    bool line = false;        // the series' line strip exists (+0x19c + i·0x10): from setArg until reset
    uint32_t color = 0;       // +0x1a8 + i·0x10
  };
  Series series[kSeries];
  int current = 0;            // +0x250: ring index of the newest sample
  int segmentCount = 256;     // +0x254
  int viewOffset = 0;         // +0x258: first ring position drawn (oldest samples hidden)
  float maxValue = 0, minValue = 0;  // +0x25c / +0x260: widened by every sample
  float clock = 0, rate = 10;        // +0x264 / +0x268: automatic update, samples per second
  bool automaticUpdate = true;       // +0x270
  std::u16string textFormat;         // +0x274
  int verticalLines = 0, horizontalLines = 0;  // +0x278 / +0x27c

  GraphObj(const script::ClassInfo* ci);
  void update(script::VM& vm, int target);  // L:0x081d1cd0 / W:0x100ab730
  void tick(script::VM& vm, float dt) override;  // move
};

// What one graph draws, in desktop units: grid lines (pairs), one strip per series and the value labels.
struct GraphGeometry {
  struct Vertex { float x, y; uint32_t argb; };
  std::vector<Vertex> grid;                  // GL_LINES pairs (updateLines)
  std::vector<std::vector<Vertex>> strips;   // one line strip per series (updateLine)
  struct Label { std::u16string text; float x, y; };
  std::vector<Label> labels;                 // zero, max, min and current value (updateTexts)
};
// 'textWidth' measures a label in the control's font (gsIGuiText slot 0x38).
void graphGeometry(script::VM& vm, GraphObj* g, const std::function<float(const std::u16string&)>& textWidth,
                   GraphGeometry& out);

void registerGraphNatives(script::VM& vm);

}  // namespace regs::engine
