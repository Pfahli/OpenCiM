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
// * Vehicles: head and tail light glows on every moving car, bus, tram and train, and headlight beams splatted each
//   frame into a light map around the view (lighting road, kerbs, cars and walls ahead, stopped by buildings);
//   road vehicles switch to high beam when no building, lamp-lit street or other vehicle is in the 60-90 m ahead.
#include <algorithm>
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
  int rejected = 0;
  int shops = 0;            // of them shop windows (ground floor, by the mesh)         // glass areas too large for windows (glass roofs, metal sheets)
  float threshold = 0;      // specular level taken as glass
  float coverage = 0;       // share of the texture that is window
  const char* skipped = nullptr;  // why there is no mask
};

// The window mask of a building texture from its specular map (and its illumination map, which wins: textures whose
// illumination map already lights windows get none). BGRA8 levels with mipmaps, each channel one window group.
// With the mesh and material, glass used only by faces within 4.2 m of the ground is a shop window (channel A,
// lit brighter and longer); the other windows then form three groups.
bool makeWindowMask(const render::Image& specular, const render::Image* illumination, render::Image& out,
                    WindowMaskInfo* info, const render::MeshData* mesh = nullptr, int material = -1);

// The GL mask texture of a building material (cached by its texture paths), 0 when it has no windows. Only materials
// of templates under a buildings folder get one.
uint32_t buildingWindowMask(Display& d, script::VM& vm, const render::MaterialDef& m, const std::string& objectPath,
                            const render::MeshData* mesh = nullptr, int material = -1);
// Makes the masks of these objects' materials on worker threads (map load: many at once).
void prepareWindowMasks(Display& d, script::VM& vm,
                        const std::vector<std::pair<const render::ObjectData*, std::string>>& objects);

// Vehicle lamps: on each vehicle material's texture, the texels on the model's front or rear faces at lamp height
// (found by rasterising the mesh in texture space) that look like lamp glass: bright and nearly white at the front,
// clearly red at the rear. Mask channels: R = front lamps, G = rear lamps.
struct VehicleLampInfo {
  int frontLamps = 0, rearLamps = 0;  // lamp areas found
  int windows = 0;                    // window glass areas (channel B: interior light)
  float windowY0 = 0, windowY1 = 0;   // the window band's model height (passenger silhouettes), 0 0 = unknown
  float windowY2 = 0, windowY3 = 0;   // a double-decker's upper deck band, 0 0 = one deck
  int windscreen = 0;                 // texels of windscreen glass (the driver's place)
  // destination displays with a line number (2026-11-24): the number's field on the texture (fractions, v down),
  // its width over height, whether the text reads along -u / -v on the model, the style (0 amber LED)
  struct Sign {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0, aspect = 1;
    bool flipU = false, flipV = false;  // the text reads against its axis / runs bottom to top
    bool rot = false;                   // the text runs along v (the field is turned on the texture)
    int style = 0;                      // 0 LED dots, 1 solid glyphs (a roller blind)
    float colour[3] = {1, 0.6f, 0.15f}; // the original text's hue at full brightness
  };
  Sign signs[3];
  int signCount = 0;
};
// A vehicle material's lamp results into its GPU material: the window bands and the destination displays.
void applyVehicleLamps(render::GpuMaterial& g, const VehicleLampInfo& li);
// texture: the diffuse map's VFS path (a few models' lamps are corrected by it, kLampFixes)
bool makeVehicleLampMask(const render::MeshData& mesh, int material, const render::Image& diffuse,
                         const render::Image* specular, render::Image& out, VehicleLampInfo* info, bool rail = false,
                         const std::string& texture = {});
// The GL lamp mask of a vehicle model's material (cached), 0 when it has no lamps; the lamps of the whole model.
uint32_t vehicleLampMask(Display& d, script::VM& vm, const render::ObjectData& od, int material,
                         VehicleLampInfo* info = nullptr);
VehicleLampInfo vehicleLamps(Display& d, script::VM& vm, const render::ObjectData& od);
// How far a vehicle's lights are on (0..1): each switches on at an own point of the dusk (by its seed).
float vehicleLightsOn(uint32_t seed, float night01);
// The cycle's night01 as the last frame's night lights saw it (0 without the cycle or in the underground view).
float& vehicleNight01();
// The day/night cycle is on (and not the underground view): line vehicles' cabin light by day at half strength.
bool& vehicleCycleOn();
// DrawItem::lampCode of a vehicle part: 10 + 4·front + rear (+ 100 with the interior lit) + 0.9·on, front/rear (the
// model's ends) 0 off, 1 head, 2 tail, 3 braking tail; 0 = all off.
inline float vehicleLampCode(int front, int rear, bool interior, float on) {
  return on > 0.01f && (front || rear || interior)
             ? 10.0f + float(4 * front + rear) + (interior ? 100.0f : 0.0f) + 0.9f * std::min(on, 1.0f)
             : 0.0f;
}

// Street-lamp glass: on a lamp model's texture, the texels of its heads (within 0.8 m of a light point, from 1.5 m
// below to 1 m above) that face down or look like glass (light, unsaturated, lighter than the housing, or shiny).
// Channel R. The default shader lights it warm while the lamps are on (per lamp by the seed in the vertex alpha).
bool makeLampGlassMask(const render::MeshData& mesh, int material, const std::vector<render::Vec3>& heads,
                       const render::Image& diffuse, const render::Image* specular, render::Image& out, int* texels);
uint32_t lampGlassMask(Display& d, script::VM& vm, const render::ObjectData& od, int material,
                       const std::string& objectPath);

// Lit signs and advertisements: ad props (not graffiti) and shop/wall signs by name; their panel (the faces along
// the model's main direction, not tiny ones) glows in its own colours like a light box, on about 60 % of them
// (per item by the seed). Materials with an illumination map keep the game's own light.
bool isSignTemplate(const std::string& objectPath);
bool makeSignMask(const render::MeshData& mesh, int material, int w, int h, render::Image& out, int* texels);
uint32_t signMask(Display& d, script::VM& vm, const render::ObjectData& od, int material, const std::string& objectPath);

// Clocks (*clock*): the light, unsaturated dial glows at night (glow only, no light cast around). (The market stands
// get a lamp under the canopy in the vehicles' light map instead.)
int propGlowKind(const std::string& objectPath);  // 5 clock, 0 none
bool makePropGlowMask(const render::MeshData& mesh, int material, int kind, const render::Image& diffuse,
                      render::Image& out, int* texels);
uint32_t propGlowMask(Display& d, script::VM& vm, const render::ObjectData& od, int material,
                      const std::string& objectPath, int& kind);

// A per-building seed (1..255) for the vertex colour alpha of merged building vertices.
uint8_t buildingSeed(int x, int y, int z);

// Street-lamp templates by their mesh path, and their lamp heads in template space (the light points).
bool isLampTemplate(const std::string& objectPath);
std::vector<render::Vec3> findLampHeads(const render::MeshData& mesh);

// Round lights other parts of the port add for the coming frame (the working traffic lights, engine/traffic_lights.h):
// a pool of `radius` metres around pos (y: the ground it lights), `gain` at full night, warm 0 neutral … 1 the market
// lamps' tint. Taken into the vehicles' light map while the lights are on; the adder clears the list each frame.
struct ExtraNightLight {
  render::Vec3 pos;
  float radius = 3.0f, gain = 0.5f, warm = 0.0f;
};
std::vector<ExtraNightLight>& extraNightLights();

// Per frame, for a scene with the game sky: the night uniforms of the view (window and lamp light, the lamp map of the
// first grid), the lamp-head and vehicle-light glows (added to the particle list) and the vehicles' beams on the road
// (one additive mesh added to the draw list). No-op by day and without the cycle.
void addNightLights(Display& d, script::VM& vm, const std::vector<GridObj*>& grids, script::Object* sky,
                    render::SceneView& v, render::ParticleDrawList& particles, std::vector<render::DrawItem>& items);

// The items touched by live edits since `version` (gridItemsTouchedSince, engine/grid_edit.h) for the caches of the
// night and traffic lights, which read the renderer's templates: false also while a type is not registered there yet.
bool renderItemsTouchedSince(const GridObj* grid, uint64_t version, std::vector<uint32_t>& touched);

}  // namespace regs::engine
