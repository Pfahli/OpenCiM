#pragma once
// Port: curved roads (owner's request 2026-11-07, prototype; the tool works like the road tool of Cities: Skylines:
// straight, curved and freeform modes, drawing on from the last end, snapping to road ends). A curve is a cubic
// Bézier of the small road (GridData::CurveRoad), not a grid item: the map editor's curve tool (tool_curves.cpp,
// REGS_GRIDMODE_CURVE_ROAD) lays it, the renderer draws a mesh of the small road's own materials (terrain.cpp
// collectGrid), the lane graph takes its lanes (sim_graph.cpp rebuildCurveNodes), pedestrians its pavements
// (sim_citizens.cpp), and map.grid keeps it in the port's section 0x07F00000 (grid.cpp / grid_save.cpp); the editor's
// timeline records it as the port action 0x070810A0, so undo and the day switch replay it with the items.
// An end is on a road block (the tool turned the block's dead end into a straight piece; the block's lanes meet the
// curve's), joined to another curve's end (same point and line), or free: a turning circle (a U-turn lane around the
// point 3 units further on, the road's round end of radius 6 around it).
// Cross-section of the small road (metro/objects/roads/small_basic.object): asphalt ±3 at −0.2 under the item height,
// kerbs at ±3, pavements ±3..±6 at the item height, lanes ±1.5 at −0.2, traffic on the right.
#include <cstdint>
#include <vector>

#include "engine/grid.h"
#include "render/math3d.h"
#include "render/mesh.h"

namespace regs::engine {

constexpr uint32_t kCurveSectionTag = 0x07F00000;
constexpr float kCurveHalfWidth = 6.0f;  // world units: the small road's block is 12 wide
constexpr float kCurveCapReach = 3.0f;   // a free end's turning circle: its centre this far beyond the end

struct CurveSample {
  render::Vec3 p;      // centre line at the item height (pavement level)
  render::Vec3 dir;    // unit tangent (with the slope)
  render::Vec3 right;  // unit, horizontal, to the right of dir
  float s = 0;         // arc length from the start
};

struct CurveEnds {
  bool free0 = false, free1 = false;  // the start / end touches neither a road block nor another curve
};
// against the map's curves (the curve itself, by sameCurve, left out)
CurveEnds curveFreeEnds(const GridData& g, const CurveRoad& c);

// samples at most `step` apart (at least 2), ends included
std::vector<CurveSample> sampleCurve(const CurveRoad& c, float step);
// 257 samples evenly in t (arc lengths from 0); with caps: the free ends grown straight by kCurveCapReach, so the
// distance to this centre line (curveNearest clamps at its ends) describes the road with its round ends
std::vector<CurveSample> curveShape(const CurveRoad& c, CurveEnds caps = {});
// the sample at arc length s of a shape (interpolated)
CurveSample curveAt(const std::vector<CurveSample>& shape, float s);
// the nearest point of a shape to (x, z): distance (signed: + right of the line; at the clamped ends the radial one)
// and its arc length
void curveNearest(const std::vector<CurveSample>& shape, float x, float z, float& side, float& s);
// the cells a curve with its caps touches, grown by 3 units (clamped to the map)
void curveCells(const GridData& g, const CurveRoad& c, int& x0, int& z0, int& x1, int& z1);
float curveLength(const CurveRoad& c);
// the smallest radius of the centre line (sampled), for the tool's check
float curveMinRadius(const CurveRoad& c);

// the road mesh of a curve in world space, with its caps: submesh materials asphalt, markings (decal), walkways;
// appended to out
void buildCurveMesh(const CurveRoad& c, CurveEnds caps, int matAsphalt, int matMarks, int matWalk, render::MeshData& out);

// levels the unlocked terrain corners under a curve (with its caps) to just below its asphalt and eases the rim out
// over 3 units; corners next to covered cells (the road blocks) stay. Inside gridEditBegin/End: cells queued as changed.
void levelTerrainUnderCurve(GridObj* grid, const CurveRoad& c);
// the cells of [x0, x1] × [z0, z1] wholly under a curve (its corners and centre within the road) get cell cover 0xF00,
// as items cover theirs, so no terrain is drawn under the asphalt (the road blocks' locked edge corners would poke
// through it). Called at the end of fixGridCover / fixGridCoverArea, which clear and re-stamp the cover.
void applyCurveCover(GridData& g, int x0, int z0, int x1, int z1);

// the section 0x07F00000: u32 count, then per curve 13 floats (the 13th: the end flags) and an i32 kind
void writeCurveSection(const GridData& g, std::vector<uint8_t>& out);
void parseCurveSection(const std::vector<uint8_t>& d, size_t body, size_t stop, GridData& g);

// a curve added to the map (its terrain levelled) or removed (the one with the same ends and handles), inside an edit
// batch of its own if none runs; the renderer rebuilds, and the simulation takes the change at the batch's end
// (gridEditEnd → simCurvesChanged). The tool and the timeline replay both come here.
void applyCurveEdit(script::VM& vm, GridObj* grid, const CurveRoad& c, bool add);
bool sameCurve(const CurveRoad& a, const CurveRoad& b);
// the timeline action's body: u8 add (1) or remove (0), then the curve as in the section (14 × 4 bytes)
std::vector<uint8_t> curveActionBytes(const CurveRoad& c, bool add);
bool curveFromActionBytes(const std::vector<uint8_t>& raw, CurveRoad& c, bool& add);

}  // namespace regs::engine
