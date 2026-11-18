#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace regs::render {

// Material flags set by the loader (docs/asset-format/mesh-material.md §2.11; MATERIAL_* in docs/native-api.md).
enum MaterialFlag : uint32_t {
  kMatAlphaBlend = 0x1, kMatAlphaTest = 0x2, kMatSpecular = 0x4, kMatNormalMap = 0x8, kMatSpecularMap = 0x10,
  kMatSortAlpha = 0x40, kMatTwoSided = 0x1000, kMatReflection = 0x2000, kMatDiffuseMap = 0x4000, kMatDecal = 0x8000,
  kMatColorize = 0x10000, kMatIlluminationMap = 0x20000, kMatAmbientOcclusionMap = 0x100000, kMatUv2Primary = 0x01000000,
  kMatNoShadow = 0x04000000,
};

// One appearance (the 72-byte header of an embedded record or a .material file, plus its texture slots).
struct MaterialDef {
  std::string name;  // the material's own name (header), which gsObject.getMaterial looks up
  std::string file;  // the mesh's reference to it (the .material file stem), e.g. "foliage-final" named "foliage"
  uint32_t flags = 0;
  std::array<float, 4> ambient{}, diffuse{}, specular{}, emissive{};  // RGBA 0..1 as stored, before GL scaling
  uint8_t opacity = 255, specularLevel = 0, glossiness = 0, selfIllumination = 0, reflection = 0;
  std::vector<std::string> textures;  // VFS paths in texture-unit order (diffuse, specular, normal, illum., AO, env)
};

struct Submesh {
  uint32_t firstVertex = 0, vertexCount = 0, firstIndex = 0, indexCount = 0;
  int material = 0;
  uint32_t colorize = 0xFFFFFFFF;  // ARGB tint of merged grid items (colorMap), times the draw item's
};

// A render mesh: 36-byte file vertices expanded into arrays; indices are absolute (into the whole mesh).
struct MeshData {
  std::vector<float> positions, normals, uv0, uv1, tangents;  // 3, 3, 2, 2, 4 floats per vertex
  std::vector<uint32_t> colors;                                // R,G,B,A bytes as stored
  std::vector<uint32_t> indices;
  std::vector<Submesh> submeshes;
  // skin (chunk 0x02011100): 4 influences per vertex, bone = skeleton bone index, weights renormalised; empty when
  // the mesh is not skinned (docs/asset-format/mesh-material.md §2.5)
  std::vector<uint16_t> skinBones;
  std::vector<float> skinWeights;
  float radius = 0;                                            // max |p|
  std::array<float, 3> boundsMin{}, boundsMax{};
  size_t vertexCount() const { return positions.size() / 3; }
};

// A locator of an .object (top-level chunk 0x02600000): name, position and rotation as stored.
struct ObjectLocator {
  std::string name;
  std::array<float, 3> pos{};
  std::array<float, 4> rot{};
};

// A loaded .object: the LOD-0 render mesh (lod1 kept for later), its materials, and the 3ds Max node placement.
struct ObjectData {
  std::string name;
  std::array<float, 3> nodePosition{};
  std::array<float, 4> nodeRotation{0, 0, 0, 1};
  MeshData mesh, lod1;
  bool hasLod1 = false;
  std::vector<MaterialDef> materials;
  std::vector<ObjectLocator> locators;  // in file order (the grid templates' locator paths, engine/sim_graph.cpp)
};

// Reads an .object file; 'read' fetches a VFS file (returns false when missing). Material files are looked up as
// <object dir>/<material name>.material and texture names resolve against that directory.
bool loadObject(const std::string& path, const std::function<bool(const std::string&, std::vector<uint8_t>&)>& read,
                ObjectData& out, std::string* error);

// A standalone .material file (chunk 0x02013100), as loadMaterial() in scripts reads it.
bool loadMaterialFile(const std::string& path, const std::function<bool(const std::string&, std::vector<uint8_t>&)>& read,
                      MaterialDef& out);

// Per-vertex tangents (xyz + handedness w) from positions, normals and uv0.
void computeTangents(MeshData& m);

// "UV2 is primary" materials (kMatUv2Primary, .material header byte 0x21 bit 2): their submeshes swap uv0 and uv1,
// as the engine does when it uploads them (loadObject applies it after the tangents).
void swapPrimaryUv(MeshData& m, const std::vector<MaterialDef>& materials);

}  // namespace regs::render
