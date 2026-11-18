#pragma once
// The lane graph (gsGridGraph, docs/simulation/pathfinding.md §1-4): one directed path segment per named path of every
// placed item, linked to the segments that continue it.
#include <cstdint>
#include <string>
#include <vector>

#include "engine/grid.h"
#include "render/math3d.h"
#include "script/vm.h"

namespace regs::engine {

// GRID_PATH_* bits (docs/native-api.md constants)
constexpr uint32_t kPathIntersection = 0x10, kPathLaneChange = 0x20, kPathStop = 0x40, kPathRoad = 0x100,
                   kPathTram = 0x800, kPathTrain = 0x1000, kPathMetro = 0x2000, kPathAir = 0x4000, kPathWater = 0x8000,
                   kPathStart = 0x10000, kPathEnd = 0x20000, kPathAlternativeStop = 0x40000,
                   kPathConnectTo = 0x80000, kPathConnectFrom = 0x100000, kPathIgnoreTraffic = 0x200000,
                   kPathExternal = 0x400000,
                   kPathPark = 0x8000000, kPathWalk = 0x10000000, kPathBlocker = 0x20000000,
                   kPathFindAlternative = 0x40000000;
constexpr uint32_t kPathDeleted = 0x80;  // a node of a removed item (live edits): unlinked, out of its bucket
constexpr uint32_t kPathClassMask = 0x1000ff00;  // medium and road class, kept by setPathFlags without class bits

// A path segment (0x8c-byte node, §1.2): a cubic Bézier from p0 to p1 with the given end directions.
struct LaneNode {
  uint32_t flags = 0;
  int cellX = 0, cellZ = 0;   // bucket key: the cell of the chord midpoint (§2.2)
  render::Vec3 p0, p1;        // world units, map centre at the origin
  render::Vec3 f0, f1;        // unit forward at both ends (q·(0,0,−1))
  float length = 0;           // chord |p1 − p0|
  float speedLimit = 6, curveFactor = 20;
  int item = -1;              // owning network item
  int bucketNext = -1;        // graph bucket list, newest first
  std::vector<int> next;      // successors in insertion order (saved paths index them)
  std::vector<int> crossings; // nodes that cross or touch this one (§2.4): one may only be entered while the others
                              // are not held
  int inDegree = 0;
  bool railTwin = false;      // port: the tram lane laid on a railway lane (trams on railway tracks, addRailTwin)
};

struct LaneGraph {
  std::vector<LaneNode> nodes;
  std::vector<std::vector<int>> itemNodes;  // per item: its node list (item+0x24), the lane numbers of save refs
  int latticeCount = 0;                     // nodes 0 .. latticeCount−1: the generated water lanes (save refs 0x7f…)
  int width = 0, length = 0, bucketsX = 0, bucketsZ = 0;
  float cellSize = 3;
  std::vector<int> buckets;                 // head node per 8×8-cell bucket
  std::vector<int> curveNodes;              // port: the curved roads' lanes (rebuildCurveNodes), after the items'
  std::vector<int> railLinks;               // port: tram connectors between tram tracks and railway tracks (linkTramsToRail)

  // FUN_100e2070: nodes with cellX ∈ [x−3, x+w+3], cellZ ∈ [z−3, z+l+3] and all mask bits, in bucket row,
  // bucket column, bucket list order
  void query(int x, int z, int w, int l, uint32_t mask, std::vector<int>& out) const;
  // FUN_100e21e0 with size 0: the node with all mask bits nearest to p (dx² + 16·dy² + dz² to its chord midpoint)
  int nearest(const render::Vec3& p, uint32_t mask) const;
  // a save path-node reference: (item1 << 8) | lane, or 0x7f000000 | i for the generated water lanes; −1 for none
  // and inline (air) nodes
  int resolve(uint32_t ref) const;
  // FUN_100e5fe0: the goal-biased lane search; the node sequence after start up to the goal, empty when unreachable.
  // cost = Σ length / speedLimit (seconds of travel)
  bool search(int start, int goal, bool ignoreTraffic, std::vector<int>& path, float& cost) const;
};

// Point and tangent of a node's curve at t ∈ [0, 1] (control points p0 + f0·L/3, p1 − f1·L/3).
render::Vec3 laneBezier(const LaneNode& n, float t);

// Builds the graph from the map's network items and the item templates' paths (§2.1-2.3).
// waterFlags: the cells' flags as the file had them (the generated water lanes are laid before the terrain dry-out)
void buildLaneGraph(script::VM& vm, GridObj* grid, const GridData& g, LaneGraph& out,
                    const std::vector<uint16_t>* waterFlags = nullptr);
// gsGrid::updateSpecialSegments: the generated water lanes of the flags in creation order (boats-aircraft.md §2)
void waterLattice(const GridData& g, const std::vector<uint16_t>& flags, std::vector<LaneNode>& out);

// Live map edits: the nodes of new items inserted and linked as at load (crossings with the older nodes); a removed
// item's nodes unlinked from their neighbours and buckets and flagged kPathDeleted. Node indices stay valid.
void addItemNodes(script::VM& vm, GridObj* grid, const GridData& g, LaneGraph& G, const std::vector<uint32_t>& items);
void removeItemNodes(LaneGraph& G, uint32_t item);
// the same for any node list (emptied)
void removeNodeList(LaneGraph& G, std::vector<int>& list);
// a node's length, curveFactor and speed limit from its ends, directions and flags (§2.1 steps 4-6)
void finishLaneNode(LaneNode& n);
// port: the curved roads' lanes out and in again (engine/curve_roads.h), linked as the items' lanes
void rebuildCurveNodes(script::VM& vm, GridObj* grid, const GridData& g, LaneGraph& G);

// The placed height of an item (cells·256) as the item renderer uses it.
int itemHeight(const GridData& g, const GridData::Item& it, uint32_t templateFlags, int templateType);

void registerPathNatives(script::VM& vm);
// loadGridObject: the template's mesh is loaded, its locator paths come before the onLoaded edits
void markTemplateMeshLoaded(script::VM& vm, script::Object* tpl);

// a template's paths in its object space (name order), and the whole path map replaced (port-made templates)
struct TemplatePathDef {
  std::string name;
  uint32_t flags = 0;
  render::Vec3 p0, p1, f0, f1;
};
std::vector<TemplatePathDef> templatePathDefs(script::VM& vm, script::Object* tpl);
void setTemplatePaths(script::VM& vm, script::Object* tpl, const std::vector<TemplatePathDef>& paths);

}  // namespace regs::engine
