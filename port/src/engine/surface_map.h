#pragma once
// The surface map of a loaded map, for the mods' shader hooks (render/glsl.h, port/docs/mod-manager.md §4.3): a
// top-down texture of the surface class (renderer3d.h kSurface*) of the walkable surfaces, 0.5 m per texel, painted
// from the division meshes as they are built (terrain cells first, items over them), so a hook can tell how far a
// point is from the edge of its surface: the kerb between a carriageway and its pavement, a square's border. Built
// only while a hook is registered. Removed items leave their class until their division is painted again (live edits
// repaint the division).
#include <cstdint>
#include <vector>

#include "render/math3d.h"
#include "render/mesh.h"
#include "render/renderer3d.h"

namespace regs::engine {

struct SurfaceMap {
  int w = 0, h = 0;                // texels
  float originX = 0, originZ = 0;  // world position of texel (0, 0)'s corner
  float texel = 0.5f;              // metres per texel
  // per texel: the surface class in the low 7 bits, 0x80 = painted by an item (terrain cells never paint over items;
  // a carriageway never paints over the pavement next to it)
  std::vector<uint8_t> cls;
  uint32_t texture = 0;            // GL R8, NEAREST, the class only
  int dirtyX0 = 0, dirtyZ0 = 0, dirtyX1 = -1, dirtyZ1 = -1;  // texels to upload
};

// covers [originX, originX + sizeX) × [originZ, originZ + sizeZ)
void surfaceMapInit(SurfaceMap& m, float originX, float originZ, float sizeX, float sizeZ);
// paints the upward faces (normal y > 0.7) of a division mesh placed by world, with the classes of materials;
// items: an item mesh (over terrain), else terrain cells (only where no item painted)
void surfaceMapPaint(SurfaceMap& m, const render::MeshData& mesh, const render::Mat4& world,
                     const std::vector<render::GpuMaterial>& materials, bool items);
// uploads the painted texels (creates the texture on first use)
void surfaceMapUpload(SurfaceMap& m);

}  // namespace regs::engine
