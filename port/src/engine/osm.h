#pragma once
// The map editor's OpenStreetMap import (port, owner's request 2026-11-07): a slippy map (Esri World Imagery or the
// OpenStreetMap map) with a place search (Nominatim), a rectangle the size of the map times a scale factor, and an
// import that puts the area's terrain (AWS Terrarium elevation tiles), water, trees, streets and buildings (Overpass
// API) onto the editor's map, plus the area's satellite picture as an optional overlay on the ground.
// Script side: data/regs/ui/osmwindow.script (the window) and the editor extension (its button). Downloads run on
// worker threads and are cached in <user folder>/cache/osm; the map is changed on the main thread, a slice per frame,
// recorded on the timeline as one group (one undo step).
#include <cstdint>
#include <vector>

#include "script/vm.h"

namespace regs::engine {

class GridObj;

void registerOsmNatives(script::VM& vm);

// The satellite overlay on the ground (terrain.cpp draws it): the picture over the whole map, BGRA rows from the
// map's z = 0 edge; version changes with every new picture or visibility change.
struct GroundOverlay {
  std::vector<uint8_t> bgra;
  int width = 0, height = 0;
  bool visible = false;
  float opacity = 1.0f;
  uint64_t version = 0;
};
GroundOverlay& groundOverlay();

}  // namespace regs::engine
