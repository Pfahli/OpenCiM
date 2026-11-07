#pragma once
// Night lights of the port's day/night cycle (docs/renderer/night-lights.md): lit windows on the buildings and street
// lamps that light the street. Not an engine feature; everything is derived in memory from the shipped assets.
//
// * Windows: each building material's specular map marks its glass (bright texels); the windows found there are
//   split into four random groups, one per channel of a mask texture (mipmaps stay exact: each channel is plain
//   coverage). The patched default shader (engine/shaders.cpp) lights the groups that are on for the building, chosen
//   by a per-building seed (vertex colour alpha of the merged buildings) and the hour.
// * Street lamps: lamp templates are found by name (lamp, light, lantern; not traffic lights); the lamp heads are the
//   downward-facing faces high up on the model. Each lamp adds a soft pool to a light map over the whole map (sampled
//   by the default shader at the world position, so roads, terrain, cars, people and walls below the head are lit)
//   and a glow sprite on its head.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "render/image.h"
#include "render/mesh.h"
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
class GridObj;
struct TerrainCache;

struct WindowMaskInfo {
  int windows = 0;          // window groups (connected glass areas) kept
  int rejected = 0;         // glass areas too large for windows (glass roofs, metal sheets)
  float threshold = 0;      // specular level taken as glass
  float coverage = 0;       // share of the texture that is window
  const char* skipped = nullptr;  // why there is no mask
};

// The window mask of a building texture from its specular map (and its illumination map, which wins: textures whose
// illumination map already lights windows get none). BGRA8 levels with mipmaps, each channel one window group.
bool makeWindowMask(const render::Image& specular, const render::Image* illumination, render::Image& out,
                    WindowMaskInfo* info);

// The GL mask texture of a building material (cached by its texture paths), 0 when it has no windows. Only materials
// of templates under a buildings folder get one.
uint32_t buildingWindowMask(Display& d, script::VM& vm, const render::MaterialDef& m, const std::string& objectPath);
// Makes the masks of these (material, object path) pairs on worker threads (map load: many at once).
void prepareWindowMasks(Display& d, script::VM& vm,
                        const std::vector<std::pair<const render::MaterialDef*, std::string>>& materials);

// A per-building seed (1..255) for the vertex colour alpha of merged building vertices.
uint8_t buildingSeed(int x, int y, int z);

// Street-lamp templates by their mesh path, and their lamp heads in template space (the light points).
bool isLampTemplate(const std::string& objectPath);
std::vector<render::Vec3> findLampHeads(const render::MeshData& mesh);

// Per frame, for a scene with the game sky: the night uniforms of the view (window and lamp light, the lamp map of the
// first grid) and the lamp-head glows (added to the particle list). No-op by day and without the cycle.
void addNightLights(Display& d, script::VM& vm, const std::vector<GridObj*>& grids, script::Object* sky,
                    render::SceneView& v, render::ParticleDrawList& particles);

}  // namespace regs::engine
