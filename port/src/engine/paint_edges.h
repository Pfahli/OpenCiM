#pragma once
// port: the edges of painted areas (GridData::paintEdges, owner's request 2026-11-14): a painted area with a slanted
// or round outline gets its border cells cut along the outline instead of a saw tooth of whole cells. Each border cell
// keeps one or two straight lines (the best fit to the outline inside the cell, chosen by sampling it); the terrain
// mesh cuts the cell's triangles along them (terrain.cpp), the map file keeps them in their own section
// (kPaintEdgeSectionTag) and the timeline in its own action (timeline.cpp kRegsPaintEdges).
#include <cstdint>
#include <utility>
#include <vector>

#include "engine/grid.h"

namespace regs::engine {

// an outline in corner coordinates (cell (x, z) spans x..x + 1, z..z + 1); several rings are taken even-odd (holes)
using PaintRing = std::vector<std::pair<double, double>>;

struct PaintAreaCell {
  int x = 0, z = 0;
  bool full = true;  // wholly inside: paint the cell; else `edge` (its type left 0 for the caller)
  PaintEdge edge;
};
// the cells of a W × L map that the area covers: whole cells, and the cells its outline crosses with their edge
std::vector<PaintAreaCell> paintAreaCells(int W, int L, const std::vector<PaintRing>& rings);

// map.grid's section (when there are edges)
void writePaintEdgeSection(const GridData& g, std::vector<uint8_t>& out);
void parsePaintEdgeSection(const std::vector<uint8_t>& d, size_t body, size_t stop, GridData& g);

// the timeline action's body: the edges of cells [x0, x0 + w) × [z0, z0 + l) as they are now (i16 x0, i16 z0, u16 w,
// u16 l, u32 count, then per edge u16 dx, u16 dz and the section's 28 bytes after x, z)
std::vector<uint8_t> paintEdgeActionBytes(const GridData& g, int x0, int z0, int w, int l);
// applies such a body: the rect's edges replaced by the recorded ones; returns false for a short body. The rect comes
// back for the caller's rebuild.
bool applyPaintEdgeAction(GridData& g, const std::vector<uint8_t>& raw, int& x0, int& z0, int& w, int& l);
// moves the action's rect by (dx, dz) cells (gsGridTimeline.regsShift)
void shiftPaintEdgeAction(std::vector<uint8_t>& raw, int dx, int dz);

// paints an area into the grid: its whole cells get pavement paint `type` (0..7; flags 0x8 | type), its border cells
// the edge (a border cell already painted in `type` stays whole). Water cells (0x2000) are skipped. Returns the cells
// touched, for the caller's rebuild and timeline (x0, z0, x1, z1 inclusive; x1 < x0 when none).
std::vector<PaintAreaCell> paintArea(GridData& g, const std::vector<PaintRing>& rings, int type, int& x0, int& z0,
                                     int& x1, int& z1);

// the round brush's paints (grid_edit.h gridRoundBrush): brushType 1..8 paints pavement type − 1, 9 erases, over the
// circle of radius R around corner coordinates (cx, cz); the cells the circle crosses are cut along it (their old edge
// joined with the dab), so strokes get round, smooth borders
void paintCircle(GridData& g, int brushType, double cx, double cz, double R);

// painting a cell by hand (or the timeline's brushes) drops its edge
inline void dropPaintEdge(GridData& g, int x, int z) {
  if (!g.paintEdges.empty()) g.paintEdges.erase(paintEdgeKey(x, z));
}

}  // namespace regs::engine
