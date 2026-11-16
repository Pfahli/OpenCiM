#pragma once
// port: the edges of painted areas (GridData::paintEdges, owner's request 2026-11-14): a painted area with a slanted
// or round outline gets its border cells cut along the outline instead of a saw tooth of whole cells. Each border cell
// keeps one or two straight lines (the best fit to the outline inside the cell, chosen by sampling it); the terrain
// mesh cuts the cell's triangles along them (terrain.cpp), the map file keeps them in their own section
// (kPaintEdgeSectionTag) and the timeline in its own action (timeline.cpp kRegsPaintEdges).
#include <algorithm>
#include <cmath>
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

// port: the brushes' shapes (owner's request 2026-11-14, the editor's shape buttons beside the sizes): 0 circle,
// 1 square along the grid, 2 diamond (the square turned 45°), 3 line (pavement and field paints: a strip as wide as the
// brush from the press point to the release point; as a single dab and for the height brushes: the circle). R is the
// brush radius: the circle's radius, half the square's side, the diamond's centre-to-corner distance.
// kBrushSweep (internal): a convex outline of its own (poly), the area a shape sweeps between two dabs of a stroke
constexpr int kBrushCircle = 0, kBrushSquare = 1, kBrushDiamond = 2, kBrushLine = 3, kBrushSweep = 4;
struct BrushShape {
  int kind = kBrushCircle;  // circle, square or diamond (a line is laid as circles by its dabs), or a sweep
  double cx = 0, cz = 0, R = 1;
  PaintRing poly;           // kBrushSweep: the convex outline
  // the brush distance of a point: 0 at the centre, 1 on the rim (Euclidean, Chebyshev or Manhattan over R; a sweep:
  // 0 inside, 2 outside)
  double norm(double px, double pz) const {
    if (kind == kBrushSweep) return polyContains(px, pz) ? 0.0 : 2.0;
    double dx = std::fabs(px - cx), dz = std::fabs(pz - cz);
    double d = kind == kBrushSquare ? std::max(dx, dz) : kind == kBrushDiamond ? dx + dz : std::sqrt(dx * dx + dz * dz);
    return d / R;
  }
  bool contains(double px, double pz) const { return norm(px, pz) <= 1.0; }
  // the brush touches cell (x, z): the cell's point nearest the centre (the same for all three norms) lies inside
  bool touches(int x, int z) const {
    if (kind == kBrushSweep) return polyTouches(x, z);
    return norm(std::clamp(cx, double(x), double(x + 1)), std::clamp(cz, double(z), double(z + 1))) < 1.0;
  }
  // the outline (the circle as a 64-gon), counter-clockwise in x, z
  PaintRing outline() const;
  // the area this shape sweeps from (ax, az) to its own centre: the convex hull of the shape at both points
  BrushShape sweptFrom(double ax, double az) const;
  // the corner-coordinate box the shape covers
  void bounds(double& x0, double& z0, double& x1, double& z1) const;

 private:
  bool polyContains(double px, double pz) const;
  bool polyTouches(int x, int z) const;
};
// the editor's current shape (gsGrid's global regsBrushShape([n]))
int brushShape();
void setBrushShape(int shape);

// the round brush's paints (grid_edit.h gridRoundBrush): brushType 1..8 paints pavement type − 1, 9 erases, over the
// brush shape; the cells its rim crosses are cut along it (their old edge joined with the dab), so strokes get smooth
// borders of the brush's shape
void paintShape(GridData& g, int brushType, const BrushShape& shape);
inline void paintCircle(GridData& g, int brushType, double cx, double cz, double R) {
  paintShape(g, brushType, BrushShape{kBrushCircle, cx, cz, R});
}

// painting a cell by hand (or the timeline's brushes) drops its edge
inline void dropPaintEdge(GridData& g, int x, int z) {
  if (!g.paintEdges.empty()) g.paintEdges.erase(paintEdgeKey(x, z));
}

}  // namespace regs::engine
