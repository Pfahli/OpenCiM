#pragma once
// Port: curved roads (owner's request 2026-11-07, prototype; the tool works like the road tool of Cities: Skylines:
// straight, curved and freeform modes, drawing on from the last end, snapping to road ends). A curve is a cubic
// Bézier of a road kind (GridData::CurveRoad, CurveProfile below), not a grid item: the map editor's curve tool (tool_curves.cpp,
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
#include <string>
#include <vector>

#include "engine/grid.h"
#include "render/math3d.h"
#include "render/mesh.h"

namespace regs::engine {

constexpr uint32_t kCurveSectionTag = 0x07F00000;
constexpr float kCurveHalfWidth = 6.0f;  // world units: the small road's block is 12 wide
constexpr float kCurveCapReach = 3.0f;   // a free end's turning circle: its centre this far beyond the end

// The road kinds a curve can be (CurveRoad::kind). The small road (0) has its own mesh and lanes (buildCurveMesh,
// rebuildCurveNodes); the others bend copies of their straight piece's mesh and paths along the curve (a whole number
// of pieces, stretched to fit) and put their dead-end piece at a free end (owner's request 2026-11-07).
struct CurveProfile {
  int itemType;          // the road items' type: 2 small, 5 large, 6 avenue, 1 alley (footpath)
  int shift;             // the connections byte of that type (roadShift)
  float piece;           // a straight piece's length along the road (world units)
  float halfWidth;       // centre line to the outer edge
  float carriageway;     // centre line to the kerb (pavement beyond); 0: walkable all over (footpaths)
  float capReach;        // how far a free end's shape grows (the dead end's footprint with halfWidth around)
  float minRadius;       // the tightest bend the tool accepts
  const char* straight;  // templates: the straight piece, the dead end, the street tram piece ("" none)
  const char* deadend;
  const char* tram;
  float capOffset;       // the dead-end piece's centre beyond a free end
  const char* pillar;    // elevated tracks: the pillar template set under every joint of their pieces ("" none)
};
constexpr int kCurveKinds = 9;
// tracks (owner's request 2026-11-08): 4 metro on the ground, 5 elevated metro, 6 the editor's railway (double track);
// bent like the road kinds, joined smoothly to their own kind only (no junctions), never under tram tracks or stops
inline bool curveIsTrack(int kind) { return kind >= 4 && kind <= 6; }
inline bool curveIsElevated(int kind) { return kind == 5; }
// which curve ends may meet: roads of all kinds, footpaths, and each track kind on its own
inline int curveJoinClass(int kind) { return kind >= 3 && kind != 8 ? kind : 0; }
// 8: the express road's ramp (2026-11-15, road_ramp_basic: 6 wide, one lane on its centre line used both ways as in the
// game); joins roads at junctions like a road, and an express road at an entry or exit (RampPort below)
inline bool curveIsRamp(int kind) { return kind == 8; }
// an avenue roundabout's ring (owner's request 2026-11-15: no inner carriageway): only the avenue's outer half, right of
// the curve's direction (away from the island): half the middle strip, the outer carriageway's two lanes, the pavement
inline bool curveHalfRing(const GridData::CurveRoad& c) { return c.kind == 2 && (c.ends & 0xA0) == 0xA0; }
// how far across the road a point lies for the curve's footprint (ground, cover, walk map, minimap, picking): |side|,
// for a half ring the side itself (the island side is off the road)
inline float curveAcross(const GridData::CurveRoad& c, float side) {
  return curveHalfRing(c) ? (side >= 0.0f ? side : 1e9f) : (side >= 0.0f ? side : -side);
}
// 7: the express road (2026-11-13), bent like the avenue; joined smoothly to express roads only, no pedestrians on it
inline bool curveIsExpress(int kind) { return kind == 7; }
// kinds whose ends only join smoothly (no junction pieces): tracks and express roads
inline bool curveNoJunctions(int kind) { return kind >= 4 && kind != 8; }
const CurveProfile& curveProfile(int kind);
// a bridge curve's span template bent along it and its pillar, per kind ("" none)
const char* curveBridgePiece(int kind);
const char* curveBridgePillar(int kind);
// a bridge curve's end span (the abutment, on the template's local −x side: the land), per kind ("" none); drawn at a
// bridge's ends that do not continue on another bridge curve (2026-11-15)
const char* curveBridgeEnd(int kind);
// ---- express ramps (2026-11-15) ----
// A ramp curve's end on an express curve's port: that express piece is drawn and laned as the game's entry or exit piece
// (road_express_enter / road_express_exit, bent along the curve), whose slip lane leaves or joins the outer lane of the
// traffic on that side. Every piece has four ports: on each side an entry (where the slip lane begins, half a block before
// the piece's centre in the traffic's direction) and an exit (half a block after it), 18 out from the centre line, the ramp
// meeting the express road at 45 degrees. One ramp per piece.
struct RampPort {
  int express = -1;   // the express curve
  int piece = 0;      // its piece drawn as the entry or exit piece
  int side = 1;       // +1 right of the express curve's direction (its traffic runs with it), -1 left (against it)
  bool enter = true;  // an entry (the ramp's traffic merges in) or an exit
  float sCentre = 0;  // the piece's centre (arc length on the express curve)
  render::Vec3 p;     // the ramp's end there (y: the express road's height)
  render::Vec3 out;   // unit, flat: from the express road into the ramp
  int ramp = -1, rampEnd = 0;  // usedRampPorts: the ramp curve and which of its ends
};
// the ports of an express curve (all pieces, both sides, entry and exit)
std::vector<RampPort> expressRampPorts(const GridData& g, int expressCurve);
// the ports a ramp curve's end lies on (within 0.3)
std::vector<RampPort> usedRampPorts(const GridData& g);
// whether a ramp curve's end lies on an express port
bool curveEndOnRampPort(const GridData& g, const CurveRoad& c, int end);
// the entry / exit piece's template, and the bend that puts it on the express curve (appendBentMesh's side and across)
const char* rampPortPiece(bool enter);
void rampPortBend(const RampPort& rp, float& side, float& across);
// express road to large road or avenue (2026-11-15): an express curve's end met smoothly by a large-road or avenue
// curve's end takes the game's transition piece there (road_express_large_basic / road_express_avenue_basic: the
// express road on its +x side, the other road's lanes on its -x side). Returns the other road's kind, or -1.
int curveExpressTransition(const GridData& g, const CurveRoad& c, int end);
const char* expressTransitionPiece(int kind);
// whether a curve's end (0 start, 1 end) meets another curve of the same structure (CurveRoad::bridge: plain, bridge,
// tunnel)
bool curveEndContinues(const GridData& g, const CurveRoad& c, int end);
// tunnels (CurveRoad::bridge 2, 2026-11-15): the kind's tunnel tube bent along the curve (tunnel_*_gap) and its portal
// (tunnel_*_end: the mouth on the template's local -x side, the walls falling to the open road at +x) on the first and
// last piece where the tunnel does not continue on another tunnel curve ("" none: the kind has no tunnel)
const char* curveTunnelPiece(int kind);
const char* curveTunnelEnd(int kind);
// the street tram dead-end piece (the tracks' end, no lanes) drawn on a large road's or avenue's dead-end piece at a
// free end of a curve with tracks, per kind ("" none: small roads draw their own turning circle)
const char* curveTramDeadend(int kind);
// the driving lanes' offsets right of the centre line (one direction; the other mirrors), and their medium
struct CurveLanes {
  int count;
  float offset[2];
  uint32_t medium;
};
const CurveLanes& curveLanes(int kind);

struct CurveSample {
  render::Vec3 p;      // centre line at the item height (pavement level)
  render::Vec3 dir;    // unit tangent (with the slope)
  render::Vec3 right;  // unit, horizontal, to the right of dir
  float s = 0;         // arc length from the start
};

struct CurveEnds {
  bool free0 = false, free1 = false;  // the start / end touches neither a road block nor another curve
};
// a tunnel's open parts: the portal pieces at its ends (arc lengths within a piece of an end that does not continue on
// a tunnel); the ground there is cut down to the road and covered, over the tube it stays as it is. Any other curve: true.
// s on curveShape(c, caps) (with a free start's cap its arc lengths start a cap reach earlier)
// margin: the open parts grown that far into the tube (the strip behind a portal's mouth)
bool curveTunnelOpen(const GridData& g, const CurveRoad& c, float s, CurveEnds caps = {}, float margin = 0.0f);
// against the map's curves (the curve itself, by sameCurve, left out); an end off the map (curveEndOffMap) is not free
CurveEnds curveFreeEnds(const GridData& g, const CurveRoad& c);
// map-edge ends (2026-11-15, as the game's map-edge pieces road_*_port*, track_double_port): an end on the map's border
// leaving it straight out (within 0.5 units of the edge, its direction within 18° of the outward normal) has no dead end;
// its lanes are the traffic's way into the map (START) and out of it (END), both EXTERNAL (sim_graph.cpp)
bool curveEndOffMap(const GridData& g, const CurveRoad& c, int end);
// the kinds that may lead off the map: roads, the express road and the railway (not footpaths or metro tracks)
bool curveKindLeavesMap(int kind);

// samples at most `step` apart (at least 2), ends included; the range version only over arc lengths [s0, s1]
std::vector<CurveSample> sampleCurve(const CurveRoad& c, float step);
std::vector<CurveSample> sampleCurveRange(const CurveRoad& c, float s0, float s1, float step);

// ---- junctions (owner's request 2026-11-07: crossings at any angle) ----
// Where three or more curve ends of one kind meet (or two at an angle), the curves stop short of the point (their
// trim) and a junction fills the middle: its asphalt, kerbs and rounded pavement corners between the arms, and a
// lane from every arm's incoming lanes into every other arm. The trim grows with the narrowest angle between arms.
struct CurveArm {
  int curve = -1;
  int kind = 0;             // its road kind (a junction may mix road kinds)
  int end = 0;              // which end of the curve meets the junction
  float trim = 0;           // arc length cut off that end
  render::Vec3 mouth;       // the curve's centre line where the junction begins
  render::Vec3 out;         // unit, flat: away from the junction there
  bool tram = false;        // the curve has tram tracks (they run on through the junction to the other tracked arms)
  int flow = 0;             // one-way curves: 1 runs into the junction, 2 out of it (0 two-way)
  bool single = false;      // a one-way curve with a single lane on its centre line
  bool ring = false;        // a roundabout's ring (no zebra crossing over it)
};
struct CurveJunction {
  render::Vec3 centre;
  int kind = 0;
  float radius = 0;
  std::vector<CurveArm> arms;  // counter-clockwise (by the angle of mouth − centre)
};
std::vector<CurveJunction> findCurveJunctions(const GridData& g);
// a curve cut in two at arc length s (exactly the same road); end flags and stops go with their halves. Returns the
// cut point.
render::Vec3 splitCurve(const CurveRoad& c, float s, CurveRoad& first, CurveRoad& second);
// the arc lengths a curve loses at its ends to junctions (0 when none)
void curveTrims(const std::vector<CurveJunction>& js, int curve, float& trim0, float& trim1);
// the junction's outline at `w` right/left of the arms' centre lines: per arm its mouth edge (two points), then
// `perGap` points of the corner to the next arm (rounded inward, straight on a straight side, around the back of
// the centre where the gap passes 180°); y = the centre's
// layer 0: the carriageway's edge (kerb), 1: the outer edge, each arm at its own road kind's width, plus delta
std::vector<render::Vec3> junctionOutline(const CurveJunction& j, int layer, float delta = 0.0f, int perGap = 12);
bool insideOutline(const std::vector<render::Vec3>& poly, float x, float z);
// a planar texture mapping fitted to a submesh: u = a0 + a1·x + a2·z, v = b0 + b1·x + b2·z over the submesh's
// x / z extents (the tile a junction's fill repeats)
struct PlanarUV {
  float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
  float x0 = 0, x1 = 1, z0 = 0, z1 = 1;
  bool ok = false;
};
PlanarUV fitPlanarUV(const render::MeshData& m, int material);
// the junction's mesh: the fill (asphalt, or for footpaths the footpath) tiled with fillUV, the pavement corners and
// kerbs (walk material, the small road's pavement texture region)
// matMarks (roads): zebra crossings across every arm at its mouth and edge lines along the kerbs, as the game's own
// junction pieces have them (small_junction4.object: bars 0.61 wide every 0.93, 2.71 deep, 0.17 inside the edge)
void buildJunctionMesh(const CurveJunction& j, int matFill, const PlanarUV& fillUV, int matWalk, render::MeshData& out,
                       int matMarks = -1, int matTram = -1);
// a one-way curve's ends on a two-way road (a road block, a two-way curve, a free end; not a junction or another one-way
// curve): its lanes and track meet the road's right-hand lane there, on the piece next to the end
void curveOneWayMeets(const GridData& g, const CurveRoad& c, float trim0, float trim1, bool& meet0, bool& meet1);
// the tram lanes' offset right of the centre line on a curve of the kind (the outer lanes of large roads, the median of
// avenues)
float curveTramOffset(int kind);
// where a tram lane meets a junction at an arm's mouth, toward the centre (incoming) or away from it; false when the
// arm has none that way (a one-way arm: only its own way, in the outer lane or on the centre line)
bool junctionTramEnd(const CurveJunction& j, const CurveArm& a, bool incoming, render::Vec3& p);
// levels the unlocked terrain corners under the junctions at a curve's ends; the cells their outlines cover
void levelTerrainUnderJunctions(GridObj* grid, const CurveRoad& c);
void junctionCells(const GridData& g, const CurveJunction& j, int& x0, int& z0, int& x1, int& z1);
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
// matTram: the street tram strip's material (tracks/small_basic), drawn when the curve has tram tracks (ends bit 2)
// trim0 / trim1: arc lengths left out at the start / end (junctions)
void buildCurveMesh(const CurveRoad& c, CurveEnds caps, int matAsphalt, int matMarks, int matWalk, render::MeshData& out,
                    int matTram = -1, float trim0 = 0, float trim1 = 0, bool meet0 = false, bool meet1 = false);
// a template's mesh bent along a curve shape: its local x runs along the curve from arc length s0 (backwards on the left
// side), its local z away from the centre line from offset `across`, side +1 right / −1 left; its submesh materials
// offset by materialBase (the template's first material in TerrainCache::materials); appended to out
// (scale stretches the local x)
void appendBentMesh(const render::MeshData& src, int materialBase, const std::vector<CurveSample>& shape, float s0,
                    float side, float across, render::MeshData& out, float scale = 1.0f, float dy = 0.0f);
// a mesh cut into slabs `step` wide along its local x (so bending follows the curve), attributes interpolated
render::MeshData sliceMeshAlongX(const render::MeshData& src, float step, int axis = 0);
// the triangles of a mesh whose centre lies at local z >= 0 (an avenue ring's outer half)
render::MeshData rightHalfOf(const render::MeshData& src);
// a template's mesh placed rigidly: local x along ax, z along az, y up, origin at o
void appendPlacedMesh(const render::MeshData& src, int materialBase, const render::Vec3& o, const render::Vec3& ax,
                      const render::Vec3& az, render::MeshData& out);
// the copies of a straight piece along a curve of another kind than the small road: count and stretch, and the
// rigid frames of its free ends' dead-end pieces (origin, local x, local z); openPlusX: the dead end's open side is
// at its local +x (else −x)
struct CurvePieces {
  int count = 1;
  float stretch = 1;
};
// over the arc length range kept (the whole curve less its trims)
CurvePieces curvePieces(const CurveRoad& c, float length = -1);
void curveCapFrame(const CurveRoad& c, int end, bool openPlusX, render::Vec3& o, render::Vec3& ax, render::Vec3& az);
// curve stops: the stop item template of a stop, its anchor item's cell and orientation (at the pavement beside s),
// and GridData::curveAnchors rebuilt from the map's items (after loads and curve edits)
// (small roads: bus 01-03 and tram; large roads and avenues: their bus stops, stop_bus0N_large / _avenue)
std::string stopTemplateName(const CurveRoad::Stop& st, int kind = 0);
// the kinds that take stops: small roads, large roads and avenues (bus and tram; a tram stop needs the curve's tracks)
inline bool curveTakesStop(int kind, int stopKind) {
  return (kind >= 0 && kind <= 2 && stopKind >= 0 && stopKind <= 1) || (kind == 6 && stopKind == 1);  // railway: tram stops
}
// where a stop's model stands, out from the road's centre line (its own z = 0 line): the kerb (small 3, large 6,
// avenue 10.5); an avenue's tram stop at the edge of its middle strip (4.5: platform between track and carriageway, its
// crossing over the carriageway to the pavement)
float curveStopOffset(int kind, int stopKind);
void stopAnchor(const GridData& g, const CurveRoad& c, const CurveRoad::Stop& st, int& x, int& z, int& orient);
void refreshCurveAnchors(GridData& g);

// levels the unlocked terrain corners under a curve (with its caps) to just below its asphalt and eases the rim out
// over 3 units; corners next to covered cells (the road blocks) stay. Inside gridEditBegin/End: cells queued as changed.
void levelTerrainUnderCurve(GridObj* grid, const CurveRoad& c);
// the cells of [x0, x1] × [z0, z1] wholly under a curve (its corners and centre within the road) get cell cover 0xF00,
// as items cover theirs, so no terrain is drawn under the asphalt (the road blocks' locked edge corners would poke
// through it). Called at the end of fixGridCover / fixGridCoverArea, which clear and re-stamp the cover.
void applyCurveCover(GridData& g, int x0, int z0, int x1, int z1);

// the section 0x07F00000: u32 count (bits 31 / 30: format 2 / 3), then per curve 13 floats (the 13th: the end flags),
// an i32 kind, the stops and (format 3) the name (curve_roads.cpp writeCurve)
void writeCurveSection(const GridData& g, std::vector<uint8_t>& out);
void parseCurveSection(const std::vector<uint8_t>& d, size_t body, size_t stop, GridData& g);

// a curve added to the map (its terrain levelled) or removed (the one with the same ends and handles), inside an edit
// batch of its own if none runs; the renderer rebuilds, and the simulation takes the change at the batch's end
// (gridEditEnd → simCurvesChanged). The tool and the timeline replay both come here.
void applyCurveEdit(script::VM& vm, GridObj* grid, const CurveRoad& c, bool add);
bool sameCurve(const CurveRoad& a, const CurveRoad& b);
// the timeline action's body: u8 add (1) or remove (0), then the curve as in the section (format 3; older records
// have no name)
std::vector<uint8_t> curveActionBytes(const CurveRoad& c, bool add);
bool curveFromActionBytes(const std::vector<uint8_t>& raw, CurveRoad& c, bool& add);

}  // namespace regs::engine
