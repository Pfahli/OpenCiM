#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "render/math3d.h"
#include "render/mesh.h"

namespace regs::render {

// A material as the renderer binds it: flags, textures in unit order and the fixed-function colours the engine
// sets with glMaterial (docs/renderer/interfaces-3d.md §2.10).
constexpr int kHookEnvUnit = 8;      // texture unit of the shader hooks' environment cube (regs_EnvMap)
constexpr int kHookSurfaceUnit = 9;  // and of the map's surface classes (regs_SurfaceMap)
constexpr int kHookLaneUnit = 10, kHookLaneTrafficUnit = 11, kHookTroddenUnit = 12;  // the traffic maps
constexpr int kHookTopUnit = 13;     // the top map (regs_TopMap)
constexpr int kHookFootIndexUnit = 14, kHookFootAtlasUnit = 15;  // the footprint map (regs_footprintAt)

// GpuMaterial::surfaceClass: the map surfaces a shader hook can tell apart (mod-manager.md §4.3)
enum : uint8_t {
  kSurfaceOther = 0,     // buildings, props, vehicles, people, menus
  kSurfaceTerrain = 1,   // the terrain group: grass, earth, sea floor, cliffs (terrain.md §5)
  kSurfaceField = 2,     // field cells
  kSurfacePavement = 3,  // yard (pavement) cells: paved squares and yards
  kSurfaceRoad = 4,      // road, alley and street-tram items (types 1-7, 10-12) and their texture variations
  kSurfacePlatform = 5,  // stations, docks, heliports (types 0x10-0x12) and PAVEMENT-flagged items
  kSurfaceWater = 6,     // the water surface
  kSurfaceVehicle = 7,   // simulated vehicles (per draw: DrawItem::surfaceClass; hookValue 1 = parked)
  kSurfaceOverlay = 8,   // not a surface: route strips, highlights, previews, markers, the data layer, the sky,
                         // people: the hooks never run on it
  kSurfaceFoliage = 9,   // tree and bush foliage: the materials drawn with the trees' wind shader
                         // (metro/shaders/trees/trees.shader), leaves, needles, twigs and bark of one atlas
};

struct GpuMaterial {
  uint32_t flags = 0;
  std::vector<uint32_t> textures;  // GL names, unit i = textures[i]
  float ambient[4] = {0.2f, 0.2f, 0.2f, 1}, diffuse[4] = {0.8f, 0.8f, 0.8f, 1};
  float specular[4] = {0, 0, 0, 1}, emission[4] = {0, 0, 0, 1};
  float shininess = 1;
  float reflection = 0;   // header byte 0x27 / 255
  uint32_t envCube = 0;   // textures/envmap.dds as a cube map (REFLECTION)
  std::vector<uint8_t> cube;  // textures[i] is a cube map
  float custom[8][4] = {};    // shader parameters custom0..7
  float depthOffset = 0;      // glPolygonOffset(2d, 2d) when non-zero
  std::string shader;         // combiner .shader path (empty: the default shader)
  uint32_t meshFlags = 0;     // combiner mesh flags of the meshes using it (MESH_SKINNED 0x100)
  // port: what the surface is, for the mods' shader hooks (render/glsl.h, mod-manager.md §4.3)
  uint8_t surfaceClass = kSurfaceOther;
  // combiner variants per pass environment (env flags of the pass | light count << 12); program 0 = none (the
  // built-in default shader in normal passes, not drawn in shadow passes)
  struct Variant {
    uint32_t env = ~0u, program = 0;
  };
  Variant variants[4];
  bool hasProgramFor(uint32_t env) const {
    for (const Variant& v : variants)
      if (v.env == env) return true;
    return false;
  }
  uint32_t programFor(uint32_t env) const {
    for (const Variant& v : variants)
      if (v.env == env) return v.program;
    return 0;
  }
  void setProgram(uint32_t env, uint32_t program) {  // replaces the oldest slot
    for (int i = 3; i > 0; --i) variants[i] = variants[i - 1];
    variants[0] = {env, program};
  }
};

struct GpuMesh {
  uint32_t vao = 0, vbo = 0, ibo = 0;
  struct Part {
    uint32_t firstIndex = 0, indexCount = 0;
    int material = 0;
    uint32_t colorize = 0xFFFFFFFF;
    Vec3 center;  // bounds of the part's own vertices: the queue sort key (interfaces-3d.md §1.2)
    float radius = 0;
    // skinned parts: triangle groups whose bones fit one shader palette (gsHWSkinSubMesh_OGL::buildIndices)
    struct SkinGroup {
      uint32_t firstIndex = 0, indexCount = 0;
      std::vector<uint16_t> palette;  // skeleton bone per palette slot
    };
    std::vector<SkinGroup> groups;
  };
  std::vector<Part> parts;
  uint32_t skinVbo = 0;  // TEXCOORD3/4: weights · 32767 and palette slots (or 8 weights)
  Vec3 center;
  float radius = 0;
};

struct RenderTarget {
  uint32_t fbo = 0, color = 0, depth = 0;
  int width = 0, height = 0;
  bool depthTexture = false;  // depth is a texture (shadow map) instead of a renderbuffer
  // multisampled: rendering goes to msFbo (colour and depth renderbuffers), resolve() blits it into fbo/color
  uint32_t msFbo = 0, msColor = 0, msDepth = 0;
  int samples = 0;
  uint32_t depthTex = 0;  // sampleable depth of fbo (scene targets made with a depth texture)
};

enum class TargetFormat { Rgba8, Rgba16f };

// One GL light (point light, attenuation 1): position in world space.
struct Light {
  Vec3 position;
  float ambient[4] = {0, 0, 0, 1}, diffuse[4] = {1, 1, 1, 1}, specular[4] = {1, 1, 1, 1};
};

// Particle billboards (docs/renderer/particles.md §4): camera-facing quads built per pass from these sprites, drawn
// after the blended queue in the order given (far to near), consecutive sprites of one batch in one draw.
struct ParticleSprite {
  Vec3 pos;
  float size = 1, angle = 0;  // half extent, rotation in the view plane
  uint32_t argb = 0;          // colour over life with the distance fade in alpha
  uint32_t batch = 0;         // index into ParticleDrawList::batches
  Vec3 axis;                  // port streaks (emitter regsStretch): half length along this world vector; 0 = billboard
};
struct ParticleBatch {
  GpuMaterial* material = nullptr;  // particles.shader variants per pass env, texture unit 0, two-sided
  uint32_t blend = 0;               // material flags 0x80 additive (ONE, ONE), 0x100 additive alpha (SRC_ALPHA, ONE)
};
struct ParticleDrawList {
  std::vector<ParticleBatch> batches;
  std::vector<ParticleSprite> sprites;
  // the pass environment a batch's variant is selected with: additive materials take 0 (W:0x10051150)
  static uint32_t envFor(const ParticleBatch& b, uint32_t passEnv) { return b.blend ? 0u : passEnv; }
};

struct SceneView {
  Mat4 view, proj;
  Vec3 eye;
  std::vector<Light> lights;
  float lightModelAmbient[4] = {0.1f, 0.1f, 0.1f, 1};  // glLightModel ambient set by the viewport's 3D pass
  bool fog = false;  // linear, radial in eye space
  float fogStart = 0, fogEnd = 1, fogColor[4] = {0, 0, 0, 1};
  float timer = 0;  // the effect timer (uniform timer)
  float emissionScale = 1;  // material emission × this (port day/night cycle: brighter illumination maps at night)
  // material ambient and diffuse × surfaceDiffuse, specular × surfaceSpecular (the scene's port atmosphere fields,
  // engine/scene3d.cpp: wet surfaces in the weather mod)
  float surfaceDiffuse = 1, surfaceSpecular = 1;
  // shader hooks (render/glsl.h): run in this view's colour passes; the environment cube they see on unit 8
  bool hooks = false;
  uint32_t hookEnvCube = 0;
  uint32_t hookSurfaceMap = 0;  // the map's surface classes (engine/surface_map.h) on unit 9
  uint32_t hookTopMap = 0;      // and its top map (overhead heights) on unit 13
  float hookSurfaceRect[4] = {0, 0, 0, 0};
  uint32_t hookLaneMap = 0, hookLaneTraffic = 0, hookTrodden = 0;  // engine/traffic_maps.h, units 10-12
  float hookTrafficRect[4] = {0, 0, 0, 0};
  uint32_t hookFootIndex = 0, hookFootAtlas = 0;  // engine/traffic_maps.h footprints, units 14-15
  float hookFootRect[4] = {0, 0, 0, 0};
  // pass environment (ENVIROMENT_* of the pass, interfaces-3d.md §1.4) and the light count << 12 it adds
  uint32_t env = 0;
  // shadows: the map on texture unit 7 (main pass with ENVIROMENT_SHADOWMAP) and the shader uniforms
  uint32_t shadowMap = 0;
  Mat4 shadowTransform, shadowWarp;
  float shadowParams[4] = {0, 0, 0, 0};
  bool flipCull = false;  // env 0x20: cull front faces (shadow pass with useBackSide)
  const ParticleDrawList* particles = nullptr;  // drawn at the end of draw() (engine/particles.cpp collects them)
};

enum : uint32_t { kEnvFog = 0x1, kEnvShadowMap = 0x2, kEnvShadowMapRender = 0x4, kEnvShadowRender = 0x8, kEnvShadow = 0x10 };

struct DrawItem {
  const GpuMesh* mesh = nullptr;
  const std::vector<GpuMaterial>* materials = nullptr;
  Mat4 world;
  float colorize[4] = {1, 1, 1, 1};
  float alpha = 1;
  bool shadowOnly = false;  // a caster outside the view (inside it with the doubled radius, shadows.md §4.3)
  // a per-instance custom4.xy over the material's (vehicles' animated parts: rotor angle and spin-up, which the engine
  // writes into each vehicle's own material copy, L:0x081adca0.c:604-690); z and w stay the material's
  bool hasCustom4 = false;
  float custom4[2] = {0, 0};
  // skin matrices per skeleton bone for skinned meshes (docs/renderer/skinning.md); null = bind pose
  std::shared_ptr<const std::vector<Mat4>> bones;
  // a layer over the mesh (gsIMesh::addLayer(material, layerableOnly = true), terrain.md §5 data layer): only the parts
  // whose own material is MATERIAL_LAYERABLE (0x400000), drawn with (*materials)[layerMaterial] and, when set, these
  // custom0..7 instead of the material's
  int layerMaterial = -1;
  const float (*layerCustom)[4] = nullptr;
  // the mods' shader hooks (render/glsl.h): a surface class for this draw over the materials' (-1 = theirs) and a
  // per-draw value (RegsSurface.item; vehicles: 1 parked, 0 moving)
  int surfaceClass = -1;
  float hookValue = 0;
};

// how skinned meshes are laid out for the combiner's skinning variant (default.shader: $indexingInShaders)
struct SkinLayout {
  int paletteSize = 48;  // BONEx48 / BONEx8
  bool indexed = true;   // WEIGHTx4 | INDEXx4, else WEIGHTx8 in palette order
};

// The 3D path on an OpenGL 3.3 core context: the engine's default material shader re-expressed in GLSL 3.30,
// opaque then alpha-tested queues (far to near), render-to-texture targets.
class Renderer3D {
 public:
  bool init(std::string* error);
  // A combiner variant: GLSL 1.10 as generated, translated to 3.30 (render/glsl.h) and linked with the attribute
  // slots of upload(). 0 on failure (error set).
  // hooks: add the mods' shader hooks (render/glsl.h; the scene combiner variants).
  uint32_t buildProgram(const std::string& vs110, const std::string& fs110, std::string* error, bool hooks = false);
  // The mods' post hooks (render/glsl.h): a program from the post vertex shader (GLSL 1.10) and postHookFragment();
  // postHookState puts it in use with the view's hook uniforms and maps, ready for drawQuad (scene colour on unit 0,
  // its depth on unit 1, custom0 = one texel).
  uint32_t buildPostHookProgram(const std::string& vs110, std::string* error);
  void postHookState(const SceneView& v, uint32_t prog);
  GpuMesh upload(const MeshData& m, const SkinLayout& skin = {});
  void destroy(GpuMesh& m);
  bool makeTarget(RenderTarget& t, int w, int h, TargetFormat format = TargetFormat::Rgba8, bool withDepth = true,
                  bool clampToEdge = false);
  // A depth-only target whose depth texture can be sampled with sampler2DShadow (compare mode LEQUAL, linear).
  bool makeDepthTarget(RenderTarget& t, int w, int h);
  // An RGBA16F (or the given format) scene target with depth, multisampled with the given samples (0 = none),
  // resolved into a texture.
  bool makeSceneTarget(RenderTarget& t, int w, int h, int samples, bool depthTexture = false,
                       TargetFormat format = TargetFormat::Rgba16f);
  void resolve(const RenderTarget& t);  // multisample → texture (NEAREST blit, depth too); no-op without MSAA
  // resolves t and copies its bottom-left w×h colour into the back-buffer rect (x, y top-left origin)
  void blitToBack(const RenderTarget& t, int x, int y, int w, int h, int backH);
  // copies the depth of src (its multisample buffer when it has one) into dst's depth, bottom-left w×h
  void blitDepth(const RenderTarget& src, const RenderTarget& dst, int w, int h);
  void destroyTarget(RenderTarget& t);
  // A post-effect pass into the bound target and viewport: a quad drawn with a combiner program (attributes
  // POSITION and TEXCOORD0 as a vec4 per corner, corners TL, TR, BR, BL), textures on units 0..n−1 and the custom0
  // parameter. rect = (left, top, right, bottom) in glOrtho(0, 1, 1, 0) units of the viewport (y = 0 at the top),
  // null = the whole viewport. Depth test, culling and blending off.
  void drawQuad(uint32_t program, const uint32_t* textures, int count, const float custom0[4], const float uv[4][4],
                const float* rect = nullptr, const float* custom1 = nullptr);
  // The red channel of a float texture (glGetTexImage(GL_RED, GL_FLOAT)), w·h values.
  void readRed(uint32_t texture, int w, int h, std::vector<float>& out);
  // Starts drawing into a target (or the back buffer when null) inside the pixel rect; clears depth, and colour
  // when clearColor is set (to clearArgb with alpha 0 so the GUI can composite the result).
  void begin(const RenderTarget* t, int x, int y, int w, int h, uint32_t clearArgb, bool clearColor, int backW, int backH);
  void draw(const SceneView& v, const std::vector<DrawItem>& items);
  void end(int backW, int backH);

 private:
  uint32_t program_ = 0;
  uint32_t quadVao_ = 0, quadVbo_ = 0;
  struct {
    int modelView, proj, normal, view, textures, flags, colorize, matAmbient, matDiffuse, matSpecular, matEmission,
        shininess, lightCount, lightPos, lightAmbient, lightDiffuse, lightSpecular, modelAmbient, fog, fogParams,
        fogColor, alphaRef;
  } u_{};
  void drawPart(const SceneView& v, const DrawItem& it, const GpuMesh::Part& p);
  struct ProgramLocs {
    int mv = -1, proj = -1, mvp = -1, nm = -1, matEmission = -1, matAmbient = -1, matDiffuse = -1, matSpecular = -1,
        matShininess = -1, modelAmbient = -1, fogColor = -1, fogStart = -1, fogEnd = -1, fogScale = -1, fogDensity = -1;
    int lightPos[8], lightAmbient[8], lightDiffuse[8], lightSpecular[8], lightConst[8], lightSpotCut[8], lightSpotCos[8];
    int productAmbient[8], productDiffuse[8], productSpecular[8], texture[8], custom[8];
    int matrix = -1, eyepos = -1, timer = -1, reflection = -1, worldup = -1, colorize = -1, vieworient = -1, alphaRef = -1;
    int shadowTransform = -1, shadowWarp = -1, shadowParams = -1, bones = -1;
    // shader hooks (render/glsl.h): -1 in programs without them
    int hookOn = -1, surfaceClass = -1, hookInvViewProj = -1, hookEye = -1, hookTime = -1, hookSurfaceRect = -1,
        hookItem = -1, hookTrafficRect = -1, hookFootRect = -1;
    std::map<std::string, int> hookUniforms;
  };
  std::map<uint32_t, ProgramLocs> locs_;
  const ProgramLocs& locs(uint32_t prog);
  void setupProgram(const SceneView& v, uint32_t prog);
  void hookState(const SceneView& v, uint32_t prog);
  uint32_t linkProgram(const std::string& vs, const std::string& fs, std::string* error);
  void setupFallback(const SceneView& v);
  void drawCombined(const SceneView& v, const DrawItem& it, const GpuMesh::Part& p, const GpuMaterial& m, uint32_t prog,
                    float alphaRef);
  void drawParticles(const SceneView& v, const ParticleDrawList& list);
  GpuMesh particleMesh_;            // stream VBO of the billboards, static quad index buffer
  size_t particleQuads_ = 0;        // quads the index buffer holds
  std::vector<uint8_t> particleVerts_;
};

}  // namespace regs::render
