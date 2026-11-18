#pragma once
// Port: level-crossing pieces the game does not ship (owner's request 2026-11-18), made from the game's own meshes when
// the road envset registers their sources (engine/crossing_templates.cpp, port/docs/building-tools.md "Level
// crossings"):
//   tram_small_cross          the small-road tram track over the level crossing (track_double_cross_small): the tram
//                             straight piece with its mesh and lanes raised over the crossing's hump; SPECIAL0 (0x1000)
//                             in its key, so the tram tool picks it where the road is a crossing
//   track_double_cross_large  the double track across a large road: 8 cells along the road, 12 along the track (the
//                             road's 8 and 2 cells of track at each end); the crossing's mesh stretched across the road
//                             (its pavement and asphalt widths are half the large road's), the large road's mesh beside
//                             it, the track at the ends, the barriers' arms twice as long on posts moved out to the
//                             large road's pavements (a copy of the crossing's skeleton and animation)
#include "script/vm.h"

namespace regs::engine {

class GridObj;

// called by loadGridObject after each template: builds the port's crossing pieces whose sources are all registered
void portCrossingTemplates(script::VM& vm, GridObj* grid, const std::string& justLoaded);

}  // namespace regs::engine
