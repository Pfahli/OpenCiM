#pragma once
// Port: procedural buildings (owner's request 2026-11-15: "custom shaped buildings and some details like windows and
// doors on the front and roof, like big halls"). The game's buildings are fixed models of fixed sizes; a footprint none
// of them fits (a big hall, a town hall, an odd shape) becomes a building of the port's own: its walls raised from the
// footprint outline, a facade texture repeating per floor and bay (windows), doors on the walls facing the street, a
// flat roof with a parapet and roof details (air conditioning boxes, skylight strips on halls) or a gabled roof on a
// rectangular footprint. The textures are drawn by the port at start (procedural, no game files). A building is a
// GridData::ProcBuilding: saved in the port's map.grid section kProcBuildingSectionTag, recorded on the editor's
// timeline (timeline.cpp kRegsProcBuilding) and drawn next to the curved roads (terrain.cpp collectGrid).
// Windows and doors sit in recesses of the walls (reveals, mullions, sills); a share of the windows lights up at dusk in
// three groups (the cycle's night01). Flat roofs: gravel with seams and drains, skylights, solar panels, a stair house,
// air conditioners, vent pipes; halls: metal with glazed roof lanterns; gabled roofs: tiles, roof windows, chimneys.
// Not yet: shadows (the default shader has no shadow-pass variant), picking and the bulldozer, jobs and homes.
#include <cstdint>
#include <vector>

#include "engine/grid.h"
#include "render/mesh.h"
#include "render/renderer3d.h"

namespace regs::engine {

class Display;
class GridObj;

// map.grid: u32 count, then per building u8 roof, u8 style, u16 points, floats y, height, roofHeight, frontX, frontZ,
// then the points (x, z floats)
constexpr uint32_t kProcBuildingSectionTag = 0x07F30000;
void writeProcBuildingSection(const GridData& g, std::vector<uint8_t>& out);
void parseProcBuildingSection(const std::vector<uint8_t>& d, size_t body, size_t stop, GridData& g);
// one building as the bytes of the section's record (the timeline action's raw bytes)
std::vector<uint8_t> procBuildingBytes(const GridData::ProcBuilding& b);
bool procBuildingFromBytes(const std::vector<uint8_t>& raw, GridData::ProcBuilding& b);

// the materials' indices (submesh materials of the mesh): walls, doors, roofs, trim (grey) and frames (white), glass
// dark and in three lit groups (switching on at dusk one after another), solar panels, an air conditioner's grille
enum ProcMaterial { kProcHall, kProcHallDoor, kProcPlaster, kProcBrick, kProcDoor, kProcRoofFlat, kProcRoofTiles, kProcRoofMetal,
                    kProcTrim, kProcFrame, kProcGlass, kProcGlassLit0, kProcGlassLit1, kProcGlassLit2, kProcSolar, kProcGrille,
                    kProcMaterialCount };
// a building's mesh in world space (appended): walls, doors, roof and its details
void buildProcBuildingMesh(const GridData::ProcBuilding& b, render::MeshData& out);
// the map's procedural buildings as draw items (one mesh, rebuilt when GridObj::procVersion moves)
void collectProcBuildings(Display& d, GridObj* grid, std::vector<render::DrawItem>& items);

}  // namespace regs::engine
