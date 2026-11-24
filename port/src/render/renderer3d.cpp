#include "render/renderer3d.h"
#include "core/env.h"

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "render/gpu.h"
#include "render/glsl.h"

namespace regs::render {

using gpu::dev;

namespace {

// Interleaved vertex: position, normal, uv0, uv1, tangent (xyz + handedness), colour (RGBA8)
struct Vtx {
  float p[3], n[3], uv0[2], uv1[2], t[4];
  uint32_t c;
};

// gsHWSkinSubMesh_OGL::buildIndices (0x100552d0, interfaces-3d.md §1.5): each submesh's triangles in order are put in
// groups whose bones fit one shader palette; vertices are copied per group with their influences as palette slots.
// skinAttr gets 8 floats per output vertex: TEXCOORD3 = weights · 32767 and TEXCOORD4 = palette slots (indexed), or
// 8 weights · 32767 in palette order
void expandSkin(const MeshData& m, const SkinLayout& layout, MeshData& out, std::vector<float>& skinAttr,
                std::vector<std::vector<GpuMesh::Part::SkinGroup>>& groups) {
  const size_t cap = size_t(std::max(1, layout.paletteSize));
  const bool hasUv1 = m.uv1.size() >= m.vertexCount() * 2, hasTan = m.tangents.size() >= m.vertexCount() * 4;
  out.submeshes.clear();
  groups.assign(m.submeshes.size(), {});
  for (size_t si = 0; si < m.submeshes.size(); ++si) {
    const Submesh& sm = m.submeshes[si];
    Submesh ns = sm;
    ns.firstIndex = uint32_t(out.indices.size());
    ns.firstVertex = uint32_t(out.vertexCount());
    GpuMesh::Part::SkinGroup g;
    std::vector<int> slotOf;   // skeleton bone → palette slot in the current group, −1 = none
    std::vector<uint32_t> copyOf(m.vertexCount(), ~0u);
    std::vector<uint32_t> touched;
    auto close = [&] {
      g.indexCount = uint32_t(out.indices.size()) - g.firstIndex;
      if (g.indexCount) groups[si].push_back(g);
      g = {};
      g.firstIndex = uint32_t(out.indices.size());
      for (uint32_t v : touched) copyOf[v] = ~0u;
      touched.clear();
      slotOf.assign(slotOf.size(), -1);
    };
    g.firstIndex = ns.firstIndex;
    for (uint32_t k = sm.firstIndex; k + 2 < sm.firstIndex + sm.indexCount && k + 2 < m.indices.size(); k += 3) {
      // the triangle's bones; a new group starts when the ones missing from the palette do not fit
      uint16_t tri[12];
      size_t nTri = 0, missing = 0;
      for (int c = 0; c < 3; ++c)
        for (int w = 0; w < 4; ++w) {
          uint32_t v = m.indices[k + size_t(c)];
          if (m.skinWeights[size_t(v) * 4 + size_t(w)] <= 0) continue;
          uint16_t b = m.skinBones[size_t(v) * 4 + size_t(w)];
          if (b >= slotOf.size()) slotOf.resize(size_t(b) + 1, -1);
          if (std::find(tri, tri + nTri, b) != tri + nTri) continue;
          tri[nTri++] = b;
          if (slotOf[b] < 0) ++missing;
        }
      if (g.palette.size() + missing > cap && !g.palette.empty()) close();
      for (size_t i = 0; i < nTri; ++i) {
        if (slotOf[tri[i]] >= 0) continue;
        if (g.palette.size() >= cap) break;  // one triangle over the palette: the rest drop out (weight 0)
        slotOf[tri[i]] = int(g.palette.size());
        g.palette.push_back(tri[i]);
      }
      for (int c = 0; c < 3; ++c) {
        uint32_t v = m.indices[k + size_t(c)];
        if (copyOf[v] == ~0u) {
          copyOf[v] = uint32_t(out.vertexCount());
          touched.push_back(v);
          out.positions.insert(out.positions.end(), &m.positions[size_t(v) * 3], &m.positions[size_t(v) * 3] + 3);
          out.normals.insert(out.normals.end(), &m.normals[size_t(v) * 3], &m.normals[size_t(v) * 3] + 3);
          out.uv0.insert(out.uv0.end(), &m.uv0[size_t(v) * 2], &m.uv0[size_t(v) * 2] + 2);
          if (hasUv1) out.uv1.insert(out.uv1.end(), &m.uv1[size_t(v) * 2], &m.uv1[size_t(v) * 2] + 2);
          if (hasTan) out.tangents.insert(out.tangents.end(), &m.tangents[size_t(v) * 4], &m.tangents[size_t(v) * 4] + 4);
          out.colors.push_back(v < m.colors.size() ? m.colors[v] : 0xFFFFFFFFu);
          float a[8] = {};
          for (int w = 0; w < 4; ++w) {
            float wt = m.skinWeights[size_t(v) * 4 + size_t(w)];
            int slot = wt > 0 ? slotOf[m.skinBones[size_t(v) * 4 + size_t(w)]] : -1;
            if (slot < 0) continue;
            if (layout.indexed) {
              a[w] = wt * 32767.0f;
              a[4 + w] = float(slot);
            } else if (slot < 8) {
              a[slot] += wt * 32767.0f;
            }
          }
          skinAttr.insert(skinAttr.end(), a, a + 8);
        }
        out.indices.push_back(copyOf[v]);
      }
    }
    close();
    ns.indexCount = uint32_t(out.indices.size()) - ns.firstIndex;
    ns.vertexCount = uint32_t(out.vertexCount()) - ns.firstVertex;
    out.submeshes.push_back(ns);
  }
  out.radius = m.radius;
  out.boundsMin = m.boundsMin;
  out.boundsMax = m.boundsMax;
}

// The default material shader (shaders/default/*) for meshes without skinning or shadows, with its material-flag
// branches as uniforms. Lighting follows lighting.fragment: per-pixel point lights, specular with reflect(L, N).
const char* kVertex = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv0;
layout(location = 3) in vec2 aUv1;
layout(location = 4) in vec4 aTangent;
layout(location = 5) in vec4 aColor;
uniform mat4 uModelView;
uniform mat4 uProj;
uniform mat3 uNormalMatrix;
out vec3 vView;
out vec3 vNormal;
out vec3 vTangent;
out vec3 vBitangent;
out vec4 vTex;
void main() {
  vec4 p = uModelView * vec4(aPos, 1.0);
  vView = p.xyz;
  vNormal = uNormalMatrix * mix(vec3(0.0, 1.0, 0.0), aNormal, length(aNormal));  // zero normals point up
  vTangent = uNormalMatrix * (aTangent.xyz * 0.00003051850947599719);
  vBitangent = cross(vNormal, vTangent * aTangent.w);
  vTex = vec4(aUv0, aUv1);
  gl_Position = uProj * p;
}
)";

const char* kFragment = R"(#version 330 core
in vec3 vView;
in vec3 vNormal;
in vec3 vTangent;
in vec3 vBitangent;
in vec4 vTex;
out vec4 oColor;
uniform sampler2D uDiffuseMap, uSpecularMap, uNormalMap, uIlluminationMap, uAoMap;
uniform samplerCube uEnvMap;
uniform mat3 uViewToWorld;  // vieworient: eye-space directions to world space
uniform float uReflection;
uniform int uFlags;
uniform vec4 uColorize;
uniform vec4 uMatAmbient, uMatDiffuse, uMatSpecular, uMatEmission;
uniform float uShininess;
uniform int uLightCount;
uniform vec3 uLightPos[8];
uniform vec4 uLightAmbient[8], uLightDiffuse[8], uLightSpecular[8];
uniform vec4 uModelAmbient;
uniform int uFog;
uniform vec4 uFogParams;  // start, 1 / (end - start)
uniform vec4 uFogColor;
uniform float uAlphaRef;
uniform int uDebug;  // REGS_DEBUG_SHADE: 1 diffuse map, 2 normal, 3 lighting, 4 colorize mask
const int ALPHABLEND = 0x1, ALPHATEST = 0x2, SPECULAR = 0x4, NORMALMAP = 0x8, SPECULARMAP = 0x10, REFLECTION = 0x2000,
          DIFFUSEMAP = 0x4000, COLORIZE = 0x10000, ILLUMINATIONMAP = 0x20000, AOMAP = 0x100000;
// shaders/default/*.fragment expanded for these flags (docs/renderer/scene-basics.md §6.4)
void main() {
  bool lights = uLightCount > 0;
  vec4 diffusemap = (uFlags & DIFFUSEMAP) != 0 ? texture(uDiffuseMap, vTex.xy) : vec4(1.0);
  bool specularMapped = (uFlags & SPECULARMAP) != 0 && (lights || (uFlags & REFLECTION) != 0);
  vec4 specularmap = specularMapped ? texture(uSpecularMap, vTex.xy) : vec4(1.0);
  bool normalMapped = (uFlags & NORMALMAP) != 0 && lights;
  vec4 normalmap = normalMapped ? texture(uNormalMap, vTex.xy) : vec4(0.5, 0.5, 1.0, 0.0);
  if ((uFlags & AOMAP) != 0) diffusemap.rgb *= texture(uAoMap, vTex.zw).rgb;
  vec3 n = normalize(vNormal);
  if (normalMapped) n = normalize(mat3(vTangent, vBitangent, vNormal) * (normalmap.rgb - 0.5));
  vec3 view = normalize(vView);
  vec4 reflectionMap = vec4(0.0);
  if ((uFlags & REFLECTION) != 0) {
    float r = uReflection * (2.0 + dot(n, view) * 1.8);
    reflectionMap = texture(uEnvMap, uViewToWorld * reflect(view, n)) * r;
  }
  if ((uFlags & COLORIZE) != 0) {
    if (normalMapped) {
      diffusemap.rgb *= mix(vec3(1.0), uColorize.rgb, normalmap.a);
    } else {
      diffusemap.rgb *= mix(vec3(1.0), uColorize.rgb, diffusemap.a);
      diffusemap.a = 1.0;
    }
  }
  // v_ambient = emission + light-model ambient * material ambient + each light's ambient product
  vec4 ambient = uMatEmission + uModelAmbient * uMatAmbient;
  vec4 diffuse = vec4(0.0), specular = vec4(0.0);
  for (int i = 0; i < uLightCount; ++i) {
    vec3 l = uLightPos[i] - vView;
    ambient += uLightAmbient[i] * uMatAmbient;
    diffuse += clamp(dot(n, normalize(l)), 0.0, 1.0) * uLightDiffuse[i] * uMatDiffuse;
    if ((uFlags & SPECULAR) != 0)
      specular += uLightSpecular[i] * uMatSpecular * pow(clamp(dot(normalize(reflect(l, n)), view), 0.0, 1.0), uShininess);
  }
  vec4 color;
  color.a = diffusemap.a;
  color.rgb = (ambient + diffuse).rgb * diffusemap.rgb;
  color.rgb += (specular * specularmap).rgb;
  if ((uFlags & ILLUMINATIONMAP) != 0) color.rgb += texture(uIlluminationMap, vTex.xy).rgb;
  color.rgb += (reflectionMap * specularmap).rgb;
  if (uFog != 0) color.rgb = mix(color.rgb, uFogColor.rgb, clamp(max(0.0, (length(vView) - uFogParams.x) * uFogParams.y), 0.0, 1.0));
  if ((uFlags & ALPHABLEND) != 0) color.a *= uMatAmbient.a;
  if ((uFlags & (ALPHATEST | ALPHABLEND)) != 0 && color.a <= uAlphaRef) discard;
  oColor = vec4(color.rgb, clamp(color.a, 0.0, 1.0));
  if (uDebug == 1) oColor = vec4(diffusemap.rgb, 1.0);
  if (uDebug == 2) oColor = vec4(n * 0.5 + 0.5, 1.0);
  if (uDebug == 3) oColor = vec4((ambient + diffuse).rgb, 1.0);
  if (uDebug == 4) oColor = vec4(vec3(normalmap.a), 1.0);
}
)";

}  // namespace

bool Renderer3D::init(std::string* error) {
  program_ = dev().createProgram(kVertex, kFragment, {}, error);
  if (!program_) return false;
  auto loc = [&](const char* n) { return dev().uniformLocation(program_, n); };
  u_.modelView = loc("uModelView");
  u_.proj = loc("uProj");
  u_.normal = loc("uNormalMatrix");
  u_.flags = loc("uFlags");
  u_.colorize = loc("uColorize");
  u_.matAmbient = loc("uMatAmbient");
  u_.matDiffuse = loc("uMatDiffuse");
  u_.matSpecular = loc("uMatSpecular");
  u_.matEmission = loc("uMatEmission");
  u_.shininess = loc("uShininess");
  u_.lightCount = loc("uLightCount");
  u_.lightPos = loc("uLightPos");
  u_.lightAmbient = loc("uLightAmbient");
  u_.lightDiffuse = loc("uLightDiffuse");
  u_.lightSpecular = loc("uLightSpecular");
  u_.modelAmbient = loc("uModelAmbient");
  u_.fog = loc("uFog");
  u_.fogParams = loc("uFogParams");
  u_.fogColor = loc("uFogColor");
  u_.alphaRef = loc("uAlphaRef");
  return true;
}

GpuMesh Renderer3D::upload(const MeshData& src, const SkinLayout& layout) {
  GpuMesh g;
  const bool skinned = src.vertexCount() && src.skinBones.size() >= src.vertexCount() * 4 &&
                       src.skinWeights.size() >= src.vertexCount() * 4;
  MeshData expanded;
  std::vector<float> skinAttr;
  std::vector<std::vector<GpuMesh::Part::SkinGroup>> groups;
  if (skinned) expandSkin(src, layout, expanded, skinAttr, groups);
  const MeshData& m = skinned ? expanded : src;
  size_t n = m.vertexCount();
  std::vector<Vtx> v(n);
  for (size_t i = 0; i < n; ++i) {
    std::memcpy(v[i].p, &m.positions[i * 3], 12);
    std::memcpy(v[i].n, &m.normals[i * 3], 12);
    std::memcpy(v[i].uv0, &m.uv0[i * 2], 8);
    if (m.uv1.size() >= (i + 1) * 2) std::memcpy(v[i].uv1, &m.uv1[i * 2], 8);
    else std::memcpy(v[i].uv1, &m.uv0[i * 2], 8);  // TEXCOORD1 falls back to uv0
    if (m.tangents.size() >= (i + 1) * 4) {
      for (int k = 0; k < 3; ++k) v[i].t[k] = m.tangents[i * 4 + k] * 32767.0f;  // TEXCOORD2: raw shorts
      v[i].t[3] = m.tangents[i * 4 + 3];
    }
    else v[i].t[0] = 1, v[i].t[1] = v[i].t[2] = 0, v[i].t[3] = 1;
    v[i].c = m.colors.size() > i ? m.colors[i] : 0xFFFFFFFF;
  }
  gpu::Device& d = dev();
  g.vbo = d.createBuffer();
  d.bufferData(g.vbo, v.size() * sizeof(Vtx), v.data(), false);
  g.ibo = d.createBuffer();
  d.bufferData(g.ibo, m.indices.size() * 4, m.indices.data(), false);
  using gpu::AttribType;
  std::vector<gpu::VertexAttrib> attrs = {
      {0, 3, AttribType::Float, g.vbo, int(sizeof(Vtx)), offsetof(Vtx, p)},
      {1, 3, AttribType::Float, g.vbo, int(sizeof(Vtx)), offsetof(Vtx, n)},
      {2, 2, AttribType::Float, g.vbo, int(sizeof(Vtx)), offsetof(Vtx, uv0)},
      {3, 2, AttribType::Float, g.vbo, int(sizeof(Vtx)), offsetof(Vtx, uv1)},
      {4, 4, AttribType::Float, g.vbo, int(sizeof(Vtx)), offsetof(Vtx, t)},
      {5, 4, AttribType::UByteNorm, g.vbo, int(sizeof(Vtx)), offsetof(Vtx, c)}};
  if (skinned) {  // a second stream at TEXCOORD3/4 (attribute slots 6 and 7)
    g.skinVbo = d.createBuffer();
    d.bufferData(g.skinVbo, skinAttr.size() * 4, skinAttr.data(), false);
    for (int i = 0; i < 2; ++i) attrs.push_back({6 + i, 4, AttribType::Float, g.skinVbo, 32, size_t(16 * i)});
  }
  g.vao = d.createVertexInput(attrs.data(), int(attrs.size()), g.ibo);
  g.vertexCount = uint32_t(n);
  for (size_t si = 0; si < m.submeshes.size(); ++si) {
    const Submesh& s = m.submeshes[si];
    GpuMesh::Part p{s.firstIndex, s.indexCount, s.material, s.colorize};
    if (skinned) p.groups = groups[si];
    // the submesh bound (gsIMesh::createSubMesh): centre of its vertices' box, radius to the farthest of them
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (uint32_t k = s.firstIndex; k < s.firstIndex + s.indexCount && k < m.indices.size(); ++k) {
      const float* q = &m.positions[size_t(m.indices[k]) * 3];
      lo = {std::min(lo.x, q[0]), std::min(lo.y, q[1]), std::min(lo.z, q[2])};
      hi = {std::max(hi.x, q[0]), std::max(hi.y, q[1]), std::max(hi.z, q[2])};
    }
    if (lo.x <= hi.x) {
      p.center = (lo + hi) * 0.5f;
      float r2 = 0;
      for (uint32_t k = s.firstIndex; k < s.firstIndex + s.indexCount && k < m.indices.size(); ++k) {
        const float* q = &m.positions[size_t(m.indices[k]) * 3];
        Vec3 dv = Vec3{q[0], q[1], q[2]} - p.center;
        r2 = std::max(r2, dot(dv, dv));
      }
      p.radius = std::sqrt(r2);
    }
    g.parts.push_back(p);
  }
  // a sphere around the vertices' box centre that holds every vertex (culling of division meshes)
  Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (size_t i = 0; i < n; ++i) {
    const float* q = &m.positions[i * 3];
    lo = {std::min(lo.x, q[0]), std::min(lo.y, q[1]), std::min(lo.z, q[2])};
    hi = {std::max(hi.x, q[0]), std::max(hi.y, q[1]), std::max(hi.z, q[2])};
  }
  if (n) {
    g.center = (lo + hi) * 0.5f;
    float r2 = 0;
    for (size_t i = 0; i < n; ++i) {
      Vec3 dv = Vec3{m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]} - g.center;
      r2 = std::max(r2, dot(dv, dv));
    }
    g.radius = std::sqrt(r2);
  }
  return g;
}

void Renderer3D::destroy(GpuMesh& m) {
  for (auto it = blas_.begin(); it != blas_.end();) {  // its ray-query structures
    if (it->first.first == m.vao) {
      dev().destroyBlas(it->second);
      it = blas_.erase(it);
    } else {
      ++it;
    }
  }
  if (m.vao) dev().destroyVertexInput(m.vao);
  if (m.ibo) dev().destroyBuffer(m.ibo);
  if (m.skinVbo) dev().destroyBuffer(m.skinVbo);
  if (m.vbo) dev().destroyBuffer(m.vbo);
  m = GpuMesh();
}

bool Renderer3D::makeTarget(RenderTarget& t, int w, int h, TargetFormat format, bool withDepth, bool clampToEdge) {
  gpu::Device& d = dev();
  t.width = w;
  t.height = h;
  t.color = d.createTexture(gpu::TexKind::Tex2D, 1);
  d.texImage(t.color, 0, 0, format == TargetFormat::Rgba16f ? gpu::Format::RGBA16F : gpu::Format::RGBA8, w, h,
             gpu::Pixels::None, nullptr);
  gpu::SamplerDesc s;
  s.wrapS = s.wrapT = clampToEdge ? gpu::Wrap::ClampToEdge : gpu::Wrap::Repeat;
  s.maxLevel = 0;
  d.setSampler(t.color, s);
  if (withDepth) t.depth = d.createRenderbuffer(gpu::Format::Depth24, w, h, 0);
  t.fbo = d.createFramebuffer(t.color, t.depth);
  return t.fbo != 0;
}

bool Renderer3D::makeDepthTarget(RenderTarget& t, int w, int h) {
  gpu::Device& d = dev();
  t.width = w;
  t.height = h;
  t.depthTexture = true;
  t.depth = d.createTexture(gpu::TexKind::Tex2D, 1);
  d.texImage(t.depth, 0, 0, gpu::Format::Depth24, w, h, gpu::Pixels::None, nullptr);
  // outside the map counts as lit: clamp to a border of depth 1 (shadows.md §4.1)
  gpu::SamplerDesc s;
  s.wrapS = s.wrapT = gpu::Wrap::ClampToBorder;
  s.borderWhite = true;
  s.maxLevel = 0;
  s.compareLEqual = true;
  d.setSampler(t.depth, s);
  t.fbo = d.createFramebuffer(0, t.depth);
  return t.fbo != 0;
}

bool Renderer3D::makeSceneTarget(RenderTarget& t, int w, int h, int samples, bool depthTexture, TargetFormat format,
                                 bool hookData) {
  gpu::Device& d = dev();
  if (depthTexture) {  // a sampleable depth on the (resolve) framebuffer
    t.width = w;
    t.height = h;
    t.color = d.createTexture(gpu::TexKind::Tex2D, 1);
    d.texImage(t.color, 0, 0, format == TargetFormat::Rgba16f ? gpu::Format::RGBA16F : gpu::Format::RGBA8, w, h,
               gpu::Pixels::None, nullptr);
    gpu::SamplerDesc cs;
    cs.wrapS = cs.wrapT = gpu::Wrap::ClampToEdge;
    cs.maxLevel = 0;
    d.setSampler(t.color, cs);
    t.depthTex = d.createTexture(gpu::TexKind::Tex2D, 1);
    d.texImage(t.depthTex, 0, 0, gpu::Format::Depth24, w, h, gpu::Pixels::None, nullptr);
    gpu::SamplerDesc ds;
    ds.minFilter = ds.magFilter = gpu::Filter::Nearest;
    ds.wrapS = ds.wrapT = gpu::Wrap::ClampToEdge;
    ds.maxLevel = 0;
    d.setSampler(t.depthTex, ds);
    t.fbo = d.createFramebuffer(t.color, t.depthTex);
    if (!t.fbo) return false;
  } else if (!makeTarget(t, w, h, format, samples <= 0, true)) {
    return false;
  }
  if (hookData) {  // the surface hooks' data: location 1 of every translated fragment shader (render/glsl.h)
    t.hookData = d.createTexture(gpu::TexKind::Tex2D, 1);
    std::vector<uint8_t> zero(size_t(w) * size_t(h) * 4, 0);  // stays 0 (nothing to tell) on a backend without it
    d.texImage(t.hookData, 0, 0, gpu::Format::RGBA8, w, h, gpu::Pixels::BGRA8, zero.data());
    gpu::SamplerDesc hs;
    hs.wrapS = hs.wrapT = gpu::Wrap::ClampToEdge;
    hs.maxLevel = 0;
    d.setSampler(t.hookData, hs);
    if (d.hasColor1() && !d.attachColor1(t.fbo, t.hookData)) return false;
  }
  if (samples <= 0) return true;
  t.samples = samples;
  t.msColor = d.createRenderbuffer(format == TargetFormat::Rgba16f ? gpu::Format::RGBA16F : gpu::Format::RGBA8, w, h, samples);
  t.msDepth = d.createRenderbuffer(gpu::Format::Depth24, w, h, samples);
  t.msFbo = d.createFramebuffer(t.msColor, t.msDepth);
  if (!t.msFbo) return false;
  if (t.hookData && d.hasColor1()) {
    t.msHookData = d.createRenderbuffer(gpu::Format::RGBA8, w, h, samples);
    if (!d.attachColor1(t.msFbo, t.msHookData)) return false;
  }
  return true;
}

void Renderer3D::splat(const RenderTarget& t, uint32_t program, const GpuMesh& mesh, int backW, int backH, int regionH) {
  const int h = regionH > 0 ? std::min(regionH, t.height) : t.height;
  dev().bindFramebuffer(t.fbo);
  dev().viewport(0, 0, t.width, h);
  dev().scissorTest(true);
  dev().scissor(0, 0, t.width, h);
  dev().clear(true, false, 0, 0, 0, 0);
  dev().depthTest(false);
  dev().cull(false);
  dev().blend(true);
  dev().blendFunc(gpu::Blend::One, gpu::Blend::One);
  dev().useProgram(program);
  dev().bindVertexInput(mesh.vao);
  for (const GpuMesh::Part& p : mesh.parts)
    dev().drawElements(gpu::Prim::Triangles, int(p.indexCount), p.firstIndex);
  dev().bindVertexInput(0);
  dev().blendFunc(gpu::Blend::SrcAlpha, gpu::Blend::OneMinusSrcAlpha);
  dev().blend(false);
  dev().scissorTest(false);
  dev().depthTest(true);
  dev().cull(true);
  dev().bindFramebuffer(0);
  dev().viewport(0, 0, backW, backH);
}

void Renderer3D::resolve(const RenderTarget& t) {
  if (!t.msFbo) return;
  dev().scissorTest(false);
  dev().blit(t.msFbo, t.fbo, 0, 0, t.width, t.height, 0, 0, t.width, t.height, true, t.depthTex != 0);
  if (t.hookData && t.msHookData) dev().blitColor1(t.msFbo, t.fbo, t.width, t.height);
}

void Renderer3D::blitToBack(const RenderTarget& t, int x, int y, int w, int h, int backH) {
  if (!t.fbo) return;
  resolve(t);  // a multisample source must be resolved at its own size before a blit to another position
  dev().scissorTest(false);
  dev().blit(t.fbo, 0, 0, 0, w, h, x, backH - y - h, x + w, backH - y, true, false);
}

void Renderer3D::blitDepthScaled(const RenderTarget& src, const RenderTarget& dst, int sw, int sh, int dw, int dh) {
  resolve(src);
  dev().scissorTest(false);
  dev().blit(src.fbo, dst.fbo, 0, 0, sw, sh, 0, 0, dw, dh, false, true);
}

void Renderer3D::blitDepth(const RenderTarget& src, const RenderTarget& dst, int w, int h) {
  dev().scissorTest(false);
  dev().blit(src.msFbo ? src.msFbo : src.fbo, dst.fbo, 0, 0, w, h, 0, 0, w, h, false, true);
}

void Renderer3D::blitHookData(const RenderTarget& src, const RenderTarget& dst, int w, int h) {
  if (!src.hookData || !dst.hookData || !dev().hasColor1()) return;
  dev().scissorTest(false);
  dev().blitColor1(src.msFbo ? src.msFbo : src.fbo, dst.fbo, w, h);
}

void Renderer3D::readRed(uint32_t texture, int w, int h, std::vector<float>& out) {
  out.assign(size_t(w) * size_t(h), 0.0f);
  dev().readTextureRed(texture, w, h, out.data());
}

void Renderer3D::queueRed(uint32_t texture, int w, int h) { dev().queueReadRed(texture, w, h); }

bool Renderer3D::takeRed(uint32_t texture, std::vector<float>& out) { return dev().takeReadRed(texture, out); }

void Renderer3D::destroyTarget(RenderTarget& t) {
  gpu::Device& d = dev();
  if (t.msFbo) d.destroyFramebuffer(t.msFbo);
  if (t.msColor) d.destroyRenderbuffer(t.msColor);
  if (t.msDepth) d.destroyRenderbuffer(t.msDepth);
  if (t.msHookData) d.destroyRenderbuffer(t.msHookData);
  if (t.hookData) d.destroyTexture(t.hookData);
  if (t.fbo) d.destroyFramebuffer(t.fbo);
  if (t.depthTex) d.destroyTexture(t.depthTex);
  if (t.depth && t.depthTexture) d.destroyTexture(t.depth);
  else if (t.depth) d.destroyRenderbuffer(t.depth);
  if (t.color) d.destroyTexture(t.color);
  t = RenderTarget{};
}

void Renderer3D::drawQuad(uint32_t program, const uint32_t* textures, int count, const float custom0[4], const float uv[4][4],
                          const float* rect, const float* custom1, const float* matrix, bool multiply,
                          const float* custom2) {
  if (!program) return;
  if (!quadVao_) {
    quadVbo_ = dev().createBuffer();
    dev().bufferData(quadVbo_, 4 * 7 * sizeof(float), nullptr, true);
    const gpu::VertexAttrib attrs[2] = {
        {0, 3, gpu::AttribType::Float, quadVbo_, int(7 * sizeof(float)), 0},                   // regs_Vertex
        {2, 4, gpu::AttribType::Float, quadVbo_, int(7 * sizeof(float)), 3 * sizeof(float)}};  // regs_MultiTexCoord0
    quadVao_ = dev().createVertexInput(attrs, 2, 0);
  }
  // corners TL, TR, BR, BL in clip space (identity matrices), drawn as two triangles; the rect is in
  // glOrtho(0, 1, 1, 0) units, so x_clip = 2x − 1 and y_clip = 1 − 2y
  float x0 = rect ? rect[0] : 0, y0 = rect ? rect[1] : 0, x1 = rect ? rect[2] : 1, y1 = rect ? rect[3] : 1;
  const float pos[4][2] = {{2 * x0 - 1, 1 - 2 * y0}, {2 * x1 - 1, 1 - 2 * y0}, {2 * x1 - 1, 1 - 2 * y1}, {2 * x0 - 1, 1 - 2 * y1}};
  static const int order[6] = {0, 3, 1, 1, 3, 2};
  float v[6][7];
  for (int i = 0; i < 6; ++i) {
    int k = order[i];
    v[i][0] = pos[k][0];
    v[i][1] = pos[k][1];
    v[i][2] = 0;
    for (int c = 0; c < 4; ++c) v[i][3 + c] = uv[k][c];
  }
  dev().bindVertexInput(quadVao_);
  dev().bufferData(quadVbo_, sizeof(v), v, true);
  dev().depthTest(false);
  dev().cull(false);
  dev().blend(false);
  dev().polygonOffset(false, 0, 0);
  dev().depthMask(0);
  dev().useProgram(program);
  const ProgramLocs& l = locs(program);
  static const Mat4 identity;
  dev().uniformMatrix4fv(l.mvp, 1, identity.m);
  dev().uniformMatrix4fv(l.mv, 1, identity.m);
  dev().uniformMatrix4fv(l.proj, 1, identity.m);
  dev().uniform1f(l.alphaRef, -1.0f);
  if (custom0) dev().uniform4fv(l.custom[0], 1, custom0);
  if (custom1) dev().uniform4fv(l.custom[1], 1, custom1);
  if (custom2) dev().uniform4fv(l.custom[2], 1, custom2);
  if (matrix) dev().uniformMatrix4fv(l.matrix, 1, matrix);
  for (int i = 0; i < count && i < 8; ++i) {
    dev().uniform1i(l.texture[i], i);
    dev().bindTexture(i, textures[i]);
  }
  if (multiply) {
    dev().blend(true);
    dev().blendFunc(gpu::Blend::DstColor, gpu::Blend::Zero);
  }
  dev().drawArrays(gpu::Prim::Triangles, 0, 6);
  if (multiply) {
    dev().blend(false);
    dev().blendFunc(gpu::Blend::SrcAlpha, gpu::Blend::OneMinusSrcAlpha);
  }
  dev().depthMask(1);
  dev().bindVertexInput(0);
}

void Renderer3D::begin(const RenderTarget* t, int x, int y, int w, int h, uint32_t clearArgb, bool clearColor, int backW,
                       int backH) {
  dev().bindFramebuffer(t ? (t->msFbo ? t->msFbo : t->fbo) : 0);
  // the back buffer takes a top-left rect; a target holds the view in its bottom-left corner (GL rows from the bottom),
  // which is the part the GUI image crops when the target is larger than the view (scene-basics.md §7.1)
  int vy = t ? y : backH - y - h;
  dev().viewport(x, vy, w, h);
  dev().scissorTest(true);
  dev().scissor(x, vy, w, h);
  dev().depthMask(1);
  dev().clear(clearColor, true, float(clearArgb >> 16 & 0xFF) / 255, float(clearArgb >> 8 & 0xFF) / 255,
              float(clearArgb & 0xFF) / 255, 0, 1.0f);
  if (t && t->hookData) dev().clearColor1();  // the hooks' data starts at 0 (nothing to tell) whatever the clear colour
  dev().depthTest(true);
  dev().depthFunc(gpu::Compare::LEqual);
  dev().cull(true);
  dev().cullFace(gpu::Face::Back);
  dev().blend(false);
  (void)backW;
}

void Renderer3D::end(int backW, int backH) {
  dev().bindFramebuffer(0);
  dev().depthTest(false);
  dev().cull(false);
  dev().depthMask(1);
  dev().viewport(0, 0, backW, backH);
  dev().bindVertexInput(0);
}

void Renderer3D::drawPart(const SceneView& v, const DrawItem& it, const GpuMesh::Part& p) {
  static const GpuMaterial fallback;
  const GpuMaterial& m = it.materials && size_t(p.material) < it.materials->size() ? (*it.materials)[size_t(p.material)] : fallback;
  // texture units in material order: diffuse, specular, normal, illumination, ambient occlusion
  int unit = 0;
  auto bindSlot = [&](uint32_t flag, const char* name) {
    if (!(m.flags & flag)) return;
    if (size_t(unit) < m.textures.size()) {
      dev().bindTexture(unit, m.textures[size_t(unit)]);
    }
    dev().uniform1i(dev().uniformLocation(program_, name), unit);
    ++unit;
  };
  bindSlot(kMatDiffuseMap, "uDiffuseMap");
  bindSlot(kMatSpecularMap, "uSpecularMap");
  bindSlot(kMatNormalMap, "uNormalMap");
  bindSlot(0x20000, "uIlluminationMap");
  bindSlot(0x100000, "uAoMap");
  if ((m.flags & kMatReflection) && m.envCube) {  // the environment cube follows the 2D maps
    dev().bindTexture(unit, m.envCube, true);
    dev().uniform1i(dev().uniformLocation(program_, "uEnvMap"), unit);
  }
  dev().uniform1f(dev().uniformLocation(program_, "uReflection"), m.reflection);
  dev().uniform1i(u_.flags, int((m.flags & kMatReflection) && !m.envCube ? m.flags & ~kMatReflection : m.flags));
  const float sd = v.surfaceDiffuse, ss = v.surfaceSpecular;  // port atmosphere (SceneView)
  float am[4] = {m.ambient[0] * sd, m.ambient[1] * sd, m.ambient[2] * sd, m.ambient[3]};
  dev().uniform4fv(u_.matAmbient, 1, am);
  float d[4] = {m.diffuse[0] * sd, m.diffuse[1] * sd, m.diffuse[2] * sd, m.diffuse[3] * it.alpha};
  dev().uniform4fv(u_.matDiffuse, 1, d);
  float sp[4] = {m.specular[0] * ss, m.specular[1] * ss, m.specular[2] * ss, m.specular[3]};
  dev().uniform4fv(u_.matSpecular, 1, sp);
  float em[4] = {m.emission[0] * v.emissionScale, m.emission[1] * v.emissionScale, m.emission[2] * v.emissionScale, m.emission[3]};
  dev().uniform4fv(u_.matEmission, 1, em);
  dev().uniform1f(u_.shininess, m.shininess);
  if (m.flags & kMatTwoSided) dev().cull(false);
  else dev().cull(true);
  dev().drawElements(gpu::Prim::Triangles, int(p.indexCount), p.firstIndex);
}

uint32_t Renderer3D::buildProgram(const std::string& vs110, const std::string& fs110, std::string* error, bool hooks,
                                  unsigned rays) {
  std::string why;
  std::string vs = translateGlsl110(vs110, false, &why, hooks),
              fs = vs.empty() ? "" : translateGlsl110(fs110, true, &why, hooks, rays);
  if (vs.empty() || fs.empty()) {
    if (error) *error = "unsupported built-in " + why;
    return 0;
  }
  return linkProgram(vs, fs, error);
}

uint32_t Renderer3D::queueProgram(const std::string& vs110, const std::string& fs110, std::string* error, bool hooks,
                                  unsigned rays) {
  auto t0 = std::chrono::steady_clock::now();
  std::string why;
  std::string vs = translateGlsl110(vs110, false, &why, hooks),
              fs = vs.empty() ? "" : translateGlsl110(fs110, true, &why, hooks, rays);
  if (vs.empty() || fs.empty()) {
    if (error) *error = "unsupported built-in " + why;
    return 0;
  }
  uint32_t ticket = queueLink(vs, fs);
  gLinkSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return ticket;
}

uint32_t Renderer3D::finishProgram(uint32_t ticket, std::string* error) {
  auto t0 = std::chrono::steady_clock::now();
  uint32_t prog = finishLink(ticket, error);
  ++gProgramsLinked;
  gLinkSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return prog;
}

uint32_t Renderer3D::buildPostHookProgram(const std::string& vs110, std::string* error) {
  std::string why;
  std::string vs = translateGlsl110(vs110, false, &why, false);
  if (vs.empty()) {
    if (error) *error = "unsupported built-in " + why;
    return 0;
  }
  return linkProgram(vs, postHookFragment(), error);
}

void Renderer3D::postHookState(const SceneView& v, uint32_t prog) {
  dev().useProgram(prog);
  hookState(v, prog);
  Mat4 vp = v.proj * v.view, inv = inverse(vp);
  dev().uniformMatrix4fv(dev().uniformLocation(prog, "regs_PostInvViewProj"), 1, inv.m);
  dev().uniformMatrix4fv(dev().uniformLocation(prog, "regs_PostViewProj"), 1, vp.m);
  // the scene's fog (regs_Fog), so a post hook can tell the colour of the hazy sky and how fogged a point is
  const ProgramLocs& l = locs(prog);
  dev().uniform4fv(l.fogColor, 1, v.fogColor);
  dev().uniform1f(l.fogStart, v.fog ? v.fogStart : 1e30f);
  dev().uniform1f(l.fogEnd, v.fog ? v.fogEnd : 1e30f);
  dev().uniform1f(l.fogScale, v.fog && v.fogEnd > v.fogStart ? 1.0f / (v.fogEnd - v.fogStart) : 0.0f);
  dev().uniform1f(l.fogDensity, 1);
}

// programs linked and the time spent (load-time measurements: Display::renderFrame logs slow frames)
int gProgramsLinked = 0;
double gLinkSeconds = 0;

uint32_t Renderer3D::linkProgram(const std::string& vs, const std::string& fs, std::string* error) {
  struct Timed {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~Timed() {
      ++gProgramsLinked;
      gLinkSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
  } timed;
  return finishLink(queueLink(vs, fs), error);
}

uint32_t Renderer3D::queueLink(const std::string& vs, const std::string& fs) {
  // the vertex layout of upload(): fixed-function arrays become generic attributes at fixed slots
  return dev().queueProgram(vs, fs,
                            {{0, "regs_Vertex"}, {1, "regs_Normal"}, {2, "regs_MultiTexCoord0"}, {3, "regs_MultiTexCoord1"},
                             {4, "regs_MultiTexCoord2"}, {5, "regs_Color"}, {6, "regs_MultiTexCoord3"},
                             {7, "regs_MultiTexCoord4"}});
}

uint32_t Renderer3D::finishLink(uint32_t ticket, std::string* error) {
  uint32_t prog = dev().finishProgram(ticket, error);
  if (!prog) return 0;
  // the hooks' environment cube lives on unit 8 (a sampler left at unit 0 would clash with the 2D map there)
  int env = dev().uniformLocation(prog, "regs_EnvMap"), surf = dev().uniformLocation(prog, "regs_SurfaceMap");
  if (env >= 0 || surf >= 0) {
    uint32_t current = dev().currentProgram();
    dev().useProgram(prog);
    if (env >= 0) dev().uniform1i(env, kHookEnvUnit);
    if (surf >= 0) dev().uniform1i(surf, kHookSurfaceUnit);
    const std::pair<const char*, int> traffic[] = {
        {"regs_LaneTraffic", kHookLaneTrafficUnit},
        {"regs_TopMap", kHookTopUnit}, {"regs_FootAtlas", kHookFootAtlasUnit}};
    for (const auto& [name, unit] : traffic)
      if (int loc = dev().uniformLocation(prog, name); loc >= 0) dev().uniform1i(loc, unit);
    dev().useProgram(current);
  }
  return prog;
}

Renderer3D::ProgramLocs& Renderer3D::locs(uint32_t prog) {
  ProgramLocs* found;
  if (prog < kLocsDirect) {
    if (prog < locs_.size() && locs_[prog]) return *locs_[prog];
    if (prog >= locs_.size()) locs_.resize(size_t(prog) + 1);
    locs_[prog] = std::make_unique<ProgramLocs>();
    found = locs_[prog].get();
  } else {
    auto it = locsFar_.find(prog);
    if (it != locsFar_.end()) return it->second;
    found = &locsFar_[prog];
  }
  ProgramLocs& l = *found;
  auto at = [&](const std::string& n) { return dev().uniformLocation(prog, n.c_str()); };
  l.mv = at("regs_ModelViewMatrix");
  l.proj = at("regs_ProjectionMatrix");
  l.mvp = at("regs_ModelViewProjectionMatrix");
  l.nm = at("regs_NormalMatrix");
  l.matEmission = at("regs_FrontMaterial.emission");
  l.matAmbient = at("regs_FrontMaterial.ambient");
  l.matDiffuse = at("regs_FrontMaterial.diffuse");
  l.matSpecular = at("regs_FrontMaterial.specular");
  l.matShininess = at("regs_FrontMaterial.shininess");
  l.modelAmbient = at("regs_LightModel.ambient");
  l.fogColor = at("regs_Fog.color");
  l.fogStart = at("regs_Fog.start");
  l.fogEnd = at("regs_Fog.end");
  l.fogScale = at("regs_Fog.scale");
  l.fogDensity = at("regs_Fog.density");
  for (int i = 0; i < 8; ++i) {
    std::string ls = "regs_LightSource[" + std::to_string(i) + "].", lp = "regs_FrontLightProduct[" + std::to_string(i) + "].";
    l.lightPos[i] = at(ls + "position");
    l.lightAmbient[i] = at(ls + "ambient");
    l.lightDiffuse[i] = at(ls + "diffuse");
    l.lightSpecular[i] = at(ls + "specular");
    l.lightConst[i] = at(ls + "constantAttenuation");
    l.lightSpotCos[i] = at(ls + "spotCosCutoff");
    l.lightSpotCut[i] = at(ls + "spotCutoff");
    l.productAmbient[i] = at(lp + "ambient");
    l.productDiffuse[i] = at(lp + "diffuse");
    l.productSpecular[i] = at(lp + "specular");
    l.texture[i] = at("texture" + std::to_string(i));
    l.custom[i] = at("custom" + std::to_string(i));
  }
  l.matrix = at("matrix");
  l.eyepos = at("eyepos");
  l.timer = at("timer");
  l.reflection = at("reflection");
  l.worldup = at("worldup");
  l.colorize = at("colorize");
  l.vieworient = at("vieworient");
  l.alphaRef = at("regs_AlphaRef");
  l.shadowTransform = at("shadowTransform");
  l.shadowWarp = at("shadowWarp");
  l.shadowParams = at("shadowParams");
  l.bones = at("bones");
  l.hookOn = at("regs_HookOn");
  if (l.hookOn >= 0 || at("regs_PostInvViewProj") >= 0) {  // a hooked program or the post program
    l.surfaceClass = at("regs_SurfaceClass");
    l.hookInvViewProj = at("regs_HookInvViewProj");
    l.hookEye = at("regs_HookEye");
    l.hookTime = at("regs_HookTime");
    l.hookSurfaceRect = at("regs_SurfaceMapRect");
    l.hookItem = at("regs_HookItem");
    l.hookTrafficRect = at("regs_TrafficRect");
    l.hookLaneCells = at("regs_LaneCells");
    l.hookFootRect = at("regs_FootRect");
    l.hookNight = at("regs_HookNight");
  }
  l.sunPos = at("regs_SunPos");
  l.eyePos = at("regs_EyePos");
  l.viewInverse = at("regs_ViewInverse");
  l.projInverse = at("regs_ProjInverse");
  l.night = at("regs_Night");
  l.shop = at("regs_Shop");
  l.cabin = at("regs_Cabin");
  l.windowMask = at("regs_WindowMask");
  l.windowOn = at("regs_WindowOn");
  l.lampMap = at("regs_LampMap");
  l.lampMapXf = at("regs_LampMapXf");
  l.lampHeight = at("regs_LampHeight");
  l.carMap = at("regs_CarMap");
  l.carMapXf = at("regs_CarMapXf");
  l.carLight = at("regs_CarLight");
  l.passengerAtlas = at("regs_PassengerAtlas");
  l.passengerInfo = at("regs_PassengerInfo");
  l.passengerAnim = at("regs_PassengerAnim");
  l.cabinUpper = at("regs_CabinUpper");
  return l;
}

// per-pass state of a combiner program (gsIShader::apply with programChanged, interfaces-3d.md §3.3)
void Renderer3D::setupProgram(const SceneView& v, uint32_t prog) {
  const ProgramLocs& l = locs(prog);
  dev().uniformMatrix4fv(l.proj, 1, v.proj.m);
  dev().uniform4fv(l.modelAmbient, 1, v.lightModelAmbient);
  for (int i = 0; i < 8; ++i) dev().uniform1i(l.texture[i], i);
  int n = int(std::min<size_t>(v.lights.size(), 8));
  for (int i = 0; i < n; ++i) {
    const Light& li = v.lights[size_t(i)];
    Vec3 p = transformPoint(v.view, li.position);  // glLightfv(GL_POSITION) under the view matrix: eye space, w = 1
    dev().uniform4f(l.lightPos[i], p.x, p.y, p.z, 1);
    dev().uniform4fv(l.lightAmbient[i], 1, li.ambient);
    dev().uniform4fv(l.lightDiffuse[i], 1, li.diffuse);
    dev().uniform4fv(l.lightSpecular[i], 1, li.specular);
    dev().uniform1f(l.lightConst[i], 1);
    dev().uniform1f(l.lightSpotCut[i], 180);
    dev().uniform1f(l.lightSpotCos[i], -1);
  }
  // no scene fog: push it out of reach (the default shader always applies fog)
  dev().uniform4fv(l.fogColor, 1, v.fogColor);
  dev().uniform1f(l.fogStart, v.fog ? v.fogStart : 1e30f);
  dev().uniform1f(l.fogEnd, v.fog ? v.fogEnd : 1e30f);
  dev().uniform1f(l.fogScale, v.fog && v.fogEnd > v.fogStart ? 1.0f / (v.fogEnd - v.fogStart) : 0.0f);
  dev().uniform1f(l.fogDensity, 1);
  dev().uniform3f(l.eyepos, v.eye.x, v.eye.y, v.eye.z);
  dev().uniform1f(l.timer, v.timer);
  float vo[9];  // vieworient: GLSL r * vieworient = eye-space direction to world space (scene-basics.md §6.5)
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r) vo[c * 3 + r] = v.view.at(r, c);
  dev().uniformMatrix3fv(l.vieworient, 1, vo);
  Vec3 up{v.view.at(0, 1), v.view.at(1, 1), v.view.at(2, 1)};  // world +Y in eye space
  dev().uniform3f(l.worldup, up.x, up.y, up.z);
  dev().uniformMatrix4fv(l.shadowTransform, 1, v.shadowTransform.m);
  dev().uniformMatrix4fv(l.shadowWarp, 1, v.shadowWarp.m);
  dev().uniform4fv(l.shadowParams, 1, v.shadowParams);
  // port night lights (docs/renderer/night-lights.md): the world position from clip space (eye z = −w) and the
  // window and lamp terms of the patched default shader
  Mat4 vi = inverseRigid(v.view);
  dev().uniformMatrix4fv(l.viewInverse, 1, vi.m);
  dev().uniform4f(l.projInverse, 1.0f / v.proj.at(0, 0), 1.0f / v.proj.at(1, 1), v.proj.at(0, 2), v.proj.at(1, 2));
  dev().uniform4fv(l.night, 1, v.night);
  dev().uniform4f(l.sunPos, v.sunWorld.x, v.sunWorld.y, v.sunWorld.z, v.rayShadows ? 1.0f : 0.0f);
  if (l.eyePos >= 0) dev().uniform4f(l.eyePos, v.eye.x, v.eye.y, v.eye.z, 1.0f);
  dev().uniform1f(l.shop, v.shopShare);
  dev().uniform1i(l.windowMask, kNightMapUnit);
  dev().uniform1i(l.lampMap, kNightMapUnit);
  dev().uniform4fv(l.lampMapXf, 1, v.lampMapXf);
  dev().uniform4fv(l.lampHeight, 1, v.lampHeight);
  dev().uniform1i(l.carMap, kNightMapUnit);
  dev().uniform1i(l.passengerAtlas, kNightMapUnit);
  dev().uniform4f(l.passengerAnim, v.timer, 0, 0, 0);
  dev().uniform4fv(l.carMapXf, 1, v.carMapXf);
  dev().uniform4f(l.carLight, v.carLight[0], v.carLight[1], v.carMap ? v.carLight[2] : 0.0f, 0);
  if (l.hookOn >= 0) {  // the mods' shader hooks: colour passes of hooked views; shadow-map passes for some (2)
    bool colour = !(v.env & (kEnvShadowMapRender | kEnvShadowRender));
    static const bool shadowHooks = shadowHooksRegistered();
    bool on = v.hooks && (colour || ((v.env & kEnvShadowMapRender) && shadowHooks));
    dev().uniform1f(l.hookOn, on ? (colour ? 1.0f : 2.0f) : 0.0f);
    if (on) {
      hookState(v, prog);
    }
  }
}

// the shader hooks' uniforms and maps (units 8-14) for a program in use: the view's inverse, eye, timer, the mods'
// uniforms (regsShaderUniform) and the engine's maps
void Renderer3D::hookState(const SceneView& v, uint32_t prog) {
  ProgramLocs& l = locs(prog);
  Mat4 inv = inverse(v.proj * v.view);
  dev().uniformMatrix4fv(l.hookInvViewProj, 1, inv.m);
  dev().uniform3f(l.hookEye, v.eye.x, v.eye.y, v.eye.z);
  dev().uniform1f(l.hookTime, v.timer);
  dev().uniform1f(l.hookNight, v.night[3]);
  ProgramLocs& lw = l;
  for (const auto& [name, value] : shaderHookUniforms()) {
    auto it = lw.hookUniforms.find(name);
    if (it == lw.hookUniforms.end()) it = lw.hookUniforms.emplace(name, dev().uniformLocation(prog, name.c_str())).first;
    if (it->second >= 0) dev().uniform4fv(it->second, 1, value.data());
  }
  dev().uniform4fv(l.hookSurfaceRect, 1, v.hookSurfaceRect);
  dev().bindTexture(kHookEnvUnit, v.hookEnvCube, true);
  dev().bindTexture(kHookSurfaceUnit, v.hookSurfaceMap);
  dev().uniform4fv(l.hookTrafficRect, 1, v.hookTrafficRect);
  dev().uniform4fv(l.hookLaneCells, 1, v.hookLaneCells);
  dev().bindTexture(kHookLaneTrafficUnit, v.hookLaneTraffic);
  dev().bindTexture(kHookTopUnit, v.hookTopMap);
  dev().uniform4fv(l.hookFootRect, 1, v.hookFootRect);
  dev().bindTexture(kHookFootAtlasUnit, v.hookFootAtlas);
  for (const ShaderHookTexture& t : shaderHookTextures()) {  // the mods' own textures (regsShaderTexture)
    auto it = lw.hookUniforms.find(t.sampler);
    if (it == lw.hookUniforms.end()) it = lw.hookUniforms.emplace(t.sampler, dev().uniformLocation(prog, t.sampler.c_str())).first;
    if (it->second < 0) continue;
    dev().uniform1i(it->second, t.unit);
    dev().bindTexture(t.unit, t.texture);
  }
}

void Renderer3D::drawCombined(const SceneView& v, const DrawItem& it, const GpuMesh::Part& p, const GpuMaterial& m,
                              const ProgramLocs& l, float alphaRef) {
  dev().uniform1f(l.alphaRef, alphaRef);  // glAlphaFunc(GREATER, ref) of the queue
  const float es = v.emissionScale;
  dev().uniform4f(l.matEmission, m.emission[0] * es, m.emission[1] * es, m.emission[2] * es, m.emission[3]);
  // ambient alpha = object fade; the scene's surface factors (port atmosphere) on the colours
  const float sd = v.surfaceDiffuse, ss = v.surfaceSpecular;
  float amb[4] = {m.ambient[0] * sd, m.ambient[1] * sd, m.ambient[2] * sd, m.ambient[3] * it.alpha};
  float dif[4] = {m.diffuse[0] * sd, m.diffuse[1] * sd, m.diffuse[2] * sd, m.diffuse[3]};
  float spe[4] = {m.specular[0] * ss, m.specular[1] * ss, m.specular[2] * ss, m.specular[3]};
  dev().uniform4fv(l.matAmbient, 1, amb);
  dev().uniform4fv(l.matDiffuse, 1, dif);
  dev().uniform4fv(l.matSpecular, 1, spe);
  dev().uniform1f(l.matShininess, m.shininess);
  if (l.surfaceClass >= 0) dev().uniform1i(l.surfaceClass, it.surfaceClass >= 0 ? it.surfaceClass : m.surfaceClass);
  if (l.hookItem >= 0) dev().uniform1f(l.hookItem, it.hookValue);
  int n = int(std::min<size_t>(v.lights.size(), 8));
  for (int i = 0; i < n; ++i) {  // gl_FrontLightProduct = light × material, component-wise
    const Light& li = v.lights[size_t(i)];
    float pa[4], pd[4], ps[4];
    for (int k = 0; k < 4; ++k) {
      pa[k] = li.ambient[k] * amb[k];
      pd[k] = li.diffuse[k] * dif[k];
      ps[k] = li.specular[k] * spe[k];
    }
    dev().uniform4fv(l.productAmbient[i], 1, pa);
    dev().uniform4fv(l.productDiffuse[i], 1, pd);
    dev().uniform4fv(l.productSpecular[i], 1, ps);
  }
  float tint[4];
  for (int k = 0; k < 4; ++k) tint[k] = it.colorize[k] * float(p.colorize >> (k == 3 ? 24 : 16 - 8 * k) & 0xFF) / 255.0f;
  dev().uniform4fv(l.colorize, 1, tint);
  dev().uniform1f(l.reflection, m.reflection);
  if (l.windowOn >= 0) {  // port night lights: the material's window groups, on the first unit after its textures
    const int maskUnit = int(m.textures.size());
    float code = !m.windowMask || maskUnit >= kMaterialUnits ? 0.0f : m.windowKind == 1 ? it.lampCode
                 : m.windowKind >= 4 ? (v.night[3] > 0.05f ? float(m.windowKind) : 0.0f)
                 : m.windowKind >= 2 ? (v.night[2] > 0 ? float(m.windowKind) : 0.0f) : v.night[0] > 0 ? 1.0f : 0.0f;
    dev().uniform1f(l.windowOn, code);
    if (m.windowKind == 1) {
      dev().uniform4f(l.cabin, m.cabinY[0], m.cabinY[1], it.passengers, it.cabinLight);
      dev().uniform4f(l.cabinUpper, m.cabinY[2], m.cabinY[3], 0, 0);
    }
    // the sampler points at the light map's unit when unused: never at a unit where the material has a cube
    dev().uniform1i(l.windowMask, code > 0 ? maskUnit : kNightMapUnit);
    if (code > 0) {
      dev().bindTexture(maskUnit, m.windowMask);
    }
    // passenger silhouettes: the atlas on its own unit, else the plain figures
    if (l.passengerInfo >= 0 && m.windowKind == 1) {
      bool atlas = code > 0 && v.passengerAtlas;
      float info[4] = {atlas ? v.passengerInfo[0] : 0.0f, v.passengerInfo[1], v.passengerInfo[2], v.passengerInfo[3]};
      dev().uniform4fv(l.passengerInfo, 1, info);
      dev().uniform1i(l.passengerAtlas, atlas ? kPassengerUnit : kNightMapUnit);
      if (atlas) dev().bindTexture(kPassengerUnit, v.passengerAtlas);
    }
  }
  for (int i = 0; i < 8; ++i) dev().uniform4fv(l.custom[i], 1, it.layerCustom ? it.layerCustom[i] : m.custom[i]);
  if (it.hasCustom4) dev().uniform4f(l.custom[4], it.custom4[0], it.custom4[1], m.custom[4][2], m.custom[4][3]);
  dev().uniformMatrix4fv(l.matrix, 1, it.world.m);
  for (size_t i = 0; i < m.textures.size() && i < size_t(kMaterialUnits); ++i) {
    dev().bindTexture(int(i), m.textures[i], i < m.cube.size() && m.cube[i]);
  }
  if (m.flags & kMatTwoSided) dev().cull(false);
  else dev().cull(true);
  if (p.groups.empty() || l.bones < 0) {
    dev().drawElements(gpu::Prim::Triangles, int(p.indexCount), p.firstIndex);
    return;
  }
  // per bone group: the palette's skin matrices (invBind · world; identity = bind pose), then its index range
  // (gsHWSkinSubMesh_OGL draw 0x10054140)
  static const Mat4 identity;
  std::vector<float>& palette = palette_;
  for (const GpuMesh::Part::SkinGroup& g : p.groups) {
    palette.clear();
    for (uint16_t b : g.palette) {
      const Mat4& s = it.bones && b < it.bones->size() ? (*it.bones)[b] : identity;
      palette.insert(palette.end(), s.m, s.m + 16);
    }
    dev().uniformMatrix4fv(l.bones, int(g.palette.size()), palette.data());
    dev().drawElements(gpu::Prim::Triangles, int(g.indexCount), g.firstIndex);
  }
}

void Renderer3D::setupFallback(const SceneView& v) {
  dev().uniformMatrix4fv(u_.proj, 1, v.proj.m);
  // lights in eye space, as glLightfv transforms positions by the current modelview (the view matrix)
  int n = int(std::min<size_t>(v.lights.size(), 8));
  float pos[24], amb[32], dif[32], spe[32];
  for (int i = 0; i < n; ++i) {
    Vec3 p = transformPoint(v.view, v.lights[size_t(i)].position);
    pos[i * 3] = p.x;
    pos[i * 3 + 1] = p.y;
    pos[i * 3 + 2] = p.z;
    std::memcpy(amb + i * 4, v.lights[size_t(i)].ambient, 16);
    std::memcpy(dif + i * 4, v.lights[size_t(i)].diffuse, 16);
    std::memcpy(spe + i * 4, v.lights[size_t(i)].specular, 16);
  }
  dev().uniform1i(u_.lightCount, n);
  if (n) {
    dev().uniform3fv(u_.lightPos, n, pos);
    dev().uniform4fv(u_.lightAmbient, n, amb);
    dev().uniform4fv(u_.lightDiffuse, n, dif);
    dev().uniform4fv(u_.lightSpecular, n, spe);
  }
  dev().uniform4fv(u_.modelAmbient, 1, v.lightModelAmbient);
  dev().uniform1i(u_.fog, v.fog ? 1 : 0);
  dev().uniform4f(u_.fogParams, v.fogStart, v.fogEnd > v.fogStart ? 1.0f / (v.fogEnd - v.fogStart) : 0.0f, 0, 0);
  float v2w[9];  // eye-space directions back to world space: transpose of the view rotation
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) v2w[c * 3 + r] = v.view.at(c, r);
  dev().uniformMatrix3fv(dev().uniformLocation(program_, "uViewToWorld"), 1, v2w);
  dev().uniform4fv(u_.fogColor, 1, v.fogColor);
  dev().uniform1f(u_.alphaRef, 0.2f);
  static const int debugShade = regs::envVar("REGS_DEBUG_SHADE") ? std::atoi(regs::envVar("REGS_DEBUG_SHADE")) : 0;
  dev().uniform1i(dev().uniformLocation(program_, "uDebug"), debugShade);
}

void Renderer3D::draw(const SceneView& v, const std::vector<DrawItem>& items) {
  // Vulkan: the pipelines this list lacks are made side by side first (gpu::Device::wantsPipelineCollect): the list
  // runs once with the device only collecting their keys (no particles: their programs and state are their own and
  // their vertices would be streamed twice), then for real
  if (dev().wantsPipelineCollect()) {
    SceneView collect = v;
    collect.particles = nullptr;
    dev().beginPipelineCollect();
    drawList(collect, items);
    dev().endPipelineCollect();
  }
  drawList(v, items);
}

void Renderer3D::drawList(const SceneView& v, const std::vector<DrawItem>& items) {
  const uint64_t view = ++viewSerial_;  // each program's per-view uniforms once per call (ProgramLocs::setupView)
  static const GpuMaterial fallbackMaterial;
  auto materialOf = [&](const DrawItem& it, const GpuMesh::Part& p) -> const GpuMaterial& {
    size_t m = it.layerMaterial >= 0 ? size_t(it.layerMaterial) : size_t(p.material);
    return it.materials && m < it.materials->size() ? (*it.materials)[m] : fallbackMaterial;
  };
  // shadow passes (interfaces-3d.md §1.3-1.4): no decals or MATERIAL_NO_SHADOW, only valid variants, alpha cut-outs
  // in the alpha-test queue without blending
  const bool shadowPass = (v.env & (kEnvShadowMapRender | kEnvShadowRender)) != 0;
  // queues (interfaces-3d.md §1.2-1.3): opaque; alpha-tested far to near; blended and decals far to near. The key is
  // per submesh, d·|d| with d = distance to the submesh centre − its radius (decals 0.9999·, just after coplanar
  // geometry); the sort is stable so parts with equal keys keep their order from frame to frame
  struct Q {
    float key;
    const DrawItem* it;
    const GpuMesh::Part* p;
    uint32_t program;
  };
  std::vector<Q> opaque, tested, blended;
  // shadow-map pass: the light's clip sides as world-space planes (SceneView::casterClip), n·x + d ≥ 0 inside, n unit
  struct Plane {
    Vec3 n;
    float d;
  };
  Plane casterPlanes[2][4];
  const int casterVolumes = shadowPass ? std::clamp(v.casterClipCount, 0, 2) : 0;
  for (int k = 0; k < casterVolumes; ++k) {
    const Mat4& c = v.casterClip[k];
    for (int s = 0; s < 4; ++s) {  // w + x, w − x, w + y, w − y
      const int row = s / 2;
      const float sign = s % 2 ? -1.0f : 1.0f;
      Vec3 n{c.at(3, 0) + sign * c.at(row, 0), c.at(3, 1) + sign * c.at(row, 1), c.at(3, 2) + sign * c.at(row, 2)};
      float d = c.at(3, 3) + sign * c.at(row, 3), len = std::sqrt(dot(n, n));
      casterPlanes[k][s] = len > 0 ? Plane{n * (1.0f / len), d / len} : Plane{{0, 0, 0}, 1.0f};  // degenerate: never out
    }
  }
  // the largest column of a matrix's 3×3: how much it can grow a radius
  auto scaleOf = [](const Mat4& m) {
    float s = 0;
    for (int c = 0; c < 3; ++c) s = std::max(s, m.at(0, c) * m.at(0, c) + m.at(1, c) * m.at(1, c) + m.at(2, c) * m.at(2, c));
    return std::sqrt(s);
  };
  // true when the part cannot reach the shadow map. Its bound: the part's sphere through the item's world matrix plus a
  // margin for vertex animation (tree sway moves a 30 m tree's top by about 5 m); a skinned part's posed vertices are
  // blends of the vertex through its bones' skin matrices, so they lie in the hull of the sphere through each bone
  // of its palettes, and a side cuts the part away only when it cuts away every one of those spheres
  constexpr float kCasterMargin = 10.0f;
  std::vector<std::pair<Vec3, float>> casterSpheres;
  auto casterOut = [&](const DrawItem& it, const GpuMesh::Part& p) {
    const float ws = scaleOf(it.world);
    casterSpheres.clear();
    if (!p.groups.empty() && it.bones) {
      for (const GpuMesh::Part::SkinGroup& g : p.groups)
        for (uint16_t b : g.palette) {
          if (b < it.bones->size()) {
            const Mat4& s = (*it.bones)[b];
            casterSpheres.push_back({transformPoint(it.world, transformPoint(s, p.center)), p.radius * scaleOf(s) * ws});
          } else {
            casterSpheres.push_back({transformPoint(it.world, p.center), p.radius * ws});  // identity: the bind pose
          }
        }
    }
    if (casterSpheres.empty()) casterSpheres.push_back({transformPoint(it.world, p.center), p.radius * ws});
    for (int k = 0; k < casterVolumes; ++k) {
      bool out = false;
      for (int s = 0; s < 4 && !out; ++s) {
        const Plane& pl = casterPlanes[k][s];
        out = true;
        for (const auto& [c, r] : casterSpheres)
          if (dot(pl.n, c) + pl.d >= -(r + kCasterMargin)) {
            out = false;
            break;
          }
      }
      if (!out) return false;  // inside (or across) this volume
    }
    return casterVolumes > 0;
  };
  for (const DrawItem& it : items) {
    if (!it.mesh || (it.shadowOnly && !shadowPass)) continue;
    for (const auto& p : it.mesh->parts) {
      if (it.layerMaterial >= 0) {  // a layer: only over the parts with a layerable material of their own
        if (!it.materials || size_t(p.material) >= it.materials->size()) continue;
        if (!((*it.materials)[size_t(p.material)].flags & 0x400000u)) continue;
      }
      const GpuMaterial& m = materialOf(it, p);
      uint32_t prog = m.programFor(v.env);
      if (shadowPass && (!prog || (m.flags & (kMatNoShadow | kMatDecal)))) continue;
      // with raytraced shadows the map keeps what the rays leave out (foliage, blended, skinned)
      if (shadowPass && v.shadowMapLeftovers && !(m.flags & kMatAlphaBlend) && m.surfaceClass != kSurfaceFoliage &&
          p.groups.empty())
        continue;
      if (casterVolumes && casterOut(it, p)) continue;
      Vec3 dv = transformPoint(it.world, p.center) - v.eye;
      // MATERIAL_ZERO_RADIUS (water): radius −100, so it sorts before other blended parts nearby (terrain.md §6.4)
      float radius = (m.flags & 0x02000000u) ? -100.0f : p.radius;
      float d = std::sqrt(dot(dv, dv)) - radius, key = d * std::fabs(d);
      if (shadowPass) (m.flags & (kMatAlphaTest | kMatAlphaBlend) ? tested : opaque).push_back({key, &it, &p, prog});
      else if (it.alpha < 1 || (m.flags & kMatAlphaBlend)) blended.push_back({key, &it, &p, prog});
      else if (m.flags & kMatDecal) blended.push_back({key * 0.9999f, &it, &p, prog});
      else (m.flags & kMatAlphaTest ? tested : opaque).push_back({key, &it, &p, prog});
    }
  }
  auto farToNear = [](const Q& a, const Q& b) { return a.key > b.key; };
  std::stable_sort(tested.begin(), tested.end(), farToNear);
  std::stable_sort(blended.begin(), blended.end(), farToNear);
  if (v.flipCull) dev().cullFace(gpu::Face::Front);
  if (v.env & kEnvShadowMap) {
    dev().bindTexture(7, v.shadowMap);
  }
  if (v.lampMap || v.carMap) {  // port night lights: the light map (lamps and headlights in one) for the whole pass
    dev().bindTexture(kNightMapUnit, v.lampMap ? v.lampMap : v.carMap);
  }
  uint32_t current = 0;
  ProgramLocs* locsCurrent = nullptr;  // locs(current) for a combiner program
  const DrawItem* bound = nullptr;
  gpu::Blend blendSrc = gpu::Blend::SrcAlpha, blendDst = gpu::Blend::OneMinusSrcAlpha;
  // opaque: no alpha test; alpha-tested: GREATER 0.2 with depth writes; blended and decals: GREATER 0.001 without
  // the hooks' data (attachment 1 of a scene target made with it): opaque and cut-out draws write theirs; of the
  // blended draws only walkable ground does (a blended road or pavement layer), the rest (decals on it, glass,
  // route strips) leave the ground's data under them
  bool dataMasked = false, blendedQueue = false;
  auto maskData = [&](bool masked) {
    if (masked != dataMasked) dev().colorMask1(!masked), dataMasked = masked;
  };
  auto run = [&](const std::vector<Q>& q, float ref, bool depthWrite) {
    dev().depthMask(depthWrite);
    for (const Q& e : q) {
      const GpuMaterial& m = materialOf(*e.it, *e.p);
      if (blendedQueue && !shadowPass)
        maskData(m.surfaceClass != kSurfaceRoad && m.surfaceClass != kSurfacePavement && m.surfaceClass != kSurfacePlatform);
      if (!shadowPass) {
        // polygon offset (interfaces-3d.md material state): blended and decal (−2,−2), a depth offset d gives (2d, 2d)
        float off = (m.flags & (kMatAlphaBlend | kMatDecal)) ? -2.0f : 0.0f;
        if (m.depthOffset != 0) off = 2.0f * m.depthOffset;
        dev().polygonOffset(off != 0, off, off);
        // blend function (interfaces-3d.md §2.10): MODULATE (DST_COLOR, ZERO), MODULATE_2 (DST_COLOR, SRC_COLOR),
        // ADDITIVE (ONE, ONE), ADDITIVE_A (SRC_ALPHA, ONE: the street-light glows of the night maps), else alpha
        using gpu::Blend;
        Blend src = Blend::SrcAlpha, dst = Blend::OneMinusSrcAlpha;
        if (m.flags & 0x40000u) src = Blend::DstColor, dst = Blend::Zero;
        else if (m.flags & 0x80000u) src = Blend::DstColor, dst = Blend::SrcColor;
        else if (m.flags & 0x80u) src = Blend::One, dst = Blend::One;
        else if (m.flags & 0x100u) dst = Blend::One;
        if (src != blendSrc || dst != blendDst) dev().blendFunc(blendSrc = src, blendDst = dst);
      }
      uint32_t prog = e.program ? e.program : program_;
      if (prog != current) {
        current = prog;
        bound = nullptr;
        dev().useProgram(prog);
        if (e.program) {
          // the per-view uniforms once per program and view: they stay in the program while others draw, and the
          // units hookState binds hold this view's maps for the whole call (the draws bind units 0-5 only)
          locsCurrent = &locs(prog);
          // REGS_DEBUG_PROGRAM_SETUP: on every program switch, as before (comparisons)
          static const bool setupEverySwitch = regs::envVar("REGS_DEBUG_PROGRAM_SETUP") != nullptr;
          if (locsCurrent->setupView != view || setupEverySwitch) {
            setupProgram(v, prog);
            locsCurrent->setupView = view;
          }
        } else {
          setupFallback(v);
        }
      }
      if (e.it != bound) {
        bound = e.it;
        Mat4 mv = v.view * e.it->world;
        float nm[9];
        normalMatrix(mv, nm);
        if (e.program) {
          const ProgramLocs& l = *locsCurrent;
          Mat4 mvp = v.proj * mv;
          dev().uniformMatrix4fv(l.mv, 1, mv.m);
          dev().uniformMatrix4fv(l.mvp, 1, mvp.m);
          dev().uniformMatrix3fv(l.nm, 1, nm);
        } else {
          dev().uniformMatrix4fv(u_.modelView, 1, mv.m);
          dev().uniformMatrix3fv(u_.normal, 1, nm);
          dev().uniform4fv(u_.colorize, 1, e.it->colorize);
        }
        dev().bindVertexInput(e.it->mesh->vao);
      }
      if (e.program) drawCombined(v, *e.it, *e.p, m, *locsCurrent, ref);
      else drawPart(v, *e.it, *e.p);
    }
  };
  run(opaque, -1.0f, true);
  if (!shadowPass) {
    dev().blend(true);  // the alpha-test queue blends too: cut-out edges are smoothed (§1.2)
    dev().blendFunc(blendSrc = gpu::Blend::SrcAlpha, blendDst = gpu::Blend::OneMinusSrcAlpha);
  }
  run(tested, 0.2f, true);
  blendedQueue = true;
  run(blended, 0.001f, shadowPass);
  if (!shadowPass) maskData(true);  // particles (rain, leaves, glows) keep the data under them
  if (v.particles && !v.particles->sprites.empty()) drawParticles(v, *v.particles);
  maskData(false);
  dev().blend(false);
  if (blendSrc != gpu::Blend::SrcAlpha || blendDst != gpu::Blend::OneMinusSrcAlpha) dev().blendFunc(gpu::Blend::SrcAlpha, gpu::Blend::OneMinusSrcAlpha);
  dev().depthMask(1);
  dev().cullFace(gpu::Face::Back);
  dev().bindVertexInput(0);
}

bool Renderer3D::buildRayScene(const std::vector<DrawItem>& items) {
  gpu::Device& d = dev();
  if (!d.rayTracing()) return false;
  static const GpuMaterial fallbackMaterial;
  std::vector<gpu::Device::Instance> inst;
  inst.reserve(items.size());
  for (const DrawItem& it : items) {
    if (!it.mesh || !it.mesh->vao || it.layerMaterial >= 0 || it.surfaceClass == kSurfaceOverlay) continue;
    auto key = std::make_pair(it.mesh->vao, static_cast<const void*>(it.materials));
    auto found = blas_.find(key);
    if (found == blas_.end()) {
      // the parts that cast a ray-traced shadow: what the shadow-map pass would draw except foliage (its cut-outs need
      // the texture), blended and skinned parts; other alpha-tested materials (most buildings) count as opaque
      std::vector<gpu::Device::IndexRange> ranges;
      for (const GpuMesh::Part& p : it.mesh->parts) {
        const GpuMaterial& m =
            it.materials && size_t(p.material) < it.materials->size() ? (*it.materials)[size_t(p.material)] : fallbackMaterial;
        if (m.flags & (kMatNoShadow | kMatDecal | kMatAlphaBlend)) continue;
        if (m.surfaceClass == kSurfaceOverlay || m.surfaceClass == kSurfaceWater || m.surfaceClass == kSurfaceFoliage ||
            !p.groups.empty())
          continue;
        // hit shading (raytraced reflections): the base texture and the diffuse colour
        gpu::Device::IndexRange r{p.firstIndex, int(p.indexCount)};
        if ((m.flags & kMatDiffuseMap) && !m.textures.empty()) r.texture = m.textures[0];
        auto byte = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        r.color = byte(m.diffuse[0]) | byte(m.diffuse[1]) << 8 | byte(m.diffuse[2]) << 16 | 0xFF000000u;
        ranges.push_back(r);
      }
      uint32_t b = ranges.empty() ? 0 : d.createBlas(it.mesh->vbo, int(sizeof(Vtx)), int(it.mesh->vertexCount), it.mesh->ibo, ranges.data(), int(ranges.size()));
      found = blas_.emplace(key, b).first;
    }
    if (!found->second) continue;
    gpu::Device::Instance k;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) k.transform[r * 4 + c] = it.world.at(r, c);
    k.blas = found->second;
    inst.push_back(k);
  }
  d.buildScene(inst.data(), int(inst.size()));
  return true;
}

// gsParticleSystem_OGL::startRendering / renderParticle / endRendering (L:0x08218b60, 0x08218ee0, 0x08219720;
// docs/renderer/particles.md §4): CPU billboards facing this pass's camera (its right and up axes rotated by the
// particle angle, half extent = size), normal +Y, the sprite colour per vertex. The original draws each sorted particle
// from the blend queue in immediate mode; the port fills one stream buffer and draws runs of consecutive sprites that
// share a batch. Main pass: blended, no depth writes, alpha GREATER 0.001; shadow-map pass: alpha-blended materials
// only, depth written, alpha test 0.2 (additive ones are skipped there, W:0x10051560.c:26-34).
void Renderer3D::drawParticles(const SceneView& v, const ParticleDrawList& list) {
  const bool shadowPass = (v.env & (kEnvShadowMapRender | kEnvShadowRender)) != 0;
  if (v.env & kEnvShadowRender) return;
  struct PVtx {
    float p[3], n[3], uv[2];
    uint32_t c;
  };
  const size_t n = list.sprites.size();
  if (!particleMesh_.vao) {
    particleMesh_.vbo = dev().createBuffer();
    particleMesh_.ibo = dev().createBuffer();
    using gpu::AttribType;
    const gpu::VertexAttrib attrs[4] = {
        {0, 3, AttribType::Float, particleMesh_.vbo, int(sizeof(PVtx)), offsetof(PVtx, p)},
        {1, 3, AttribType::Float, particleMesh_.vbo, int(sizeof(PVtx)), offsetof(PVtx, n)},
        {2, 2, AttribType::Float, particleMesh_.vbo, int(sizeof(PVtx)), offsetof(PVtx, uv)},
        {5, 4, AttribType::UByteNorm, particleMesh_.vbo, int(sizeof(PVtx)), offsetof(PVtx, c)}};
    particleMesh_.vao = dev().createVertexInput(attrs, 4, particleMesh_.ibo);
  }
  dev().bindVertexInput(particleMesh_.vao);
  if (n > particleQuads_) {  // the quad index buffer: (0,1,2) (0,2,3) per quad
    particleQuads_ = std::max(n, particleQuads_ * 2);
    std::vector<uint32_t> idx(particleQuads_ * 6);
    for (size_t q = 0; q < particleQuads_; ++q) {
      uint32_t b = uint32_t(q * 4);
      uint32_t* o = &idx[q * 6];
      o[0] = b, o[1] = b + 1, o[2] = b + 2, o[3] = b, o[4] = b + 2, o[5] = b + 3;
    }
    dev().bufferData(particleMesh_.ibo, idx.size() * 4, idx.data(), false);
  }
  // the pass camera's right and up axes in world space (rows of the view rotation)
  Vec3 R{v.view.at(0, 0), v.view.at(0, 1), v.view.at(0, 2)}, U{v.view.at(1, 0), v.view.at(1, 1), v.view.at(1, 2)};
  particleVerts_.resize(n * 4 * sizeof(PVtx));
  PVtx* out = reinterpret_cast<PVtx*>(particleVerts_.data());
  size_t quads = 0;
  for (const ParticleSprite& s : list.sprites) {
    if (shadowPass && list.batches[s.batch].blend) continue;
    float c = std::cos(s.angle) * s.size, sn = std::sin(s.angle) * s.size;
    Vec3 A = R * c + U * sn, B = U * c - R * sn;
    if (s.axis.x != 0 || s.axis.y != 0 || s.axis.z != 0) {
      // port streak: the texture's v axis along the motion (half length |axis| + size), u across it, size wide,
      // turned to face the camera about the motion; seen end-on it stays a billboard
      Vec3 W = cross(s.axis, v.eye - s.pos);
      float wl = std::sqrt(dot(W, W)), al = std::sqrt(dot(s.axis, s.axis));
      if (wl > 1e-4f * al * std::sqrt(dot(v.eye - s.pos, v.eye - s.pos))) {
        A = W * (s.size / wl);
        B = s.axis * ((al + s.size) / al);
      }
    }
    // ARGB → R,G,B,A bytes (the TEXCOORD order of upload())
    uint32_t col = (s.argb >> 16 & 0xFF) | (s.argb & 0xFF00) | (s.argb & 0xFF) << 16 | (s.argb & 0xFF000000u);
    const Vec3 corner[4] = {s.pos + A + B, s.pos - A + B, s.pos - A - B, s.pos + A - B};
    static const float uv[4][2] = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};
    for (int k = 0; k < 4; ++k) {
      PVtx& o = out[quads * 4 + size_t(k)];
      o.p[0] = corner[k].x, o.p[1] = corner[k].y, o.p[2] = corner[k].z;
      o.n[0] = 0, o.n[1] = 1, o.n[2] = 0;
      o.uv[0] = uv[k][0], o.uv[1] = uv[k][1];
      o.c = col;
    }
    ++quads;
  }
  if (!quads) {
    dev().bindVertexInput(0);
    return;
  }
  dev().bufferData(particleMesh_.vbo, quads * 4 * sizeof(PVtx), nullptr, true);  // orphan
  dev().bufferSubData(particleMesh_.vbo, 0, quads * 4 * sizeof(PVtx), particleVerts_.data());
  dev().polygonOffset(false, 0, 0);
  if (shadowPass) {
    dev().blend(false);
    dev().depthMask(1);
  } else {
    dev().blend(true);
    dev().depthMask(0);
  }
  float nm[9];
  normalMatrix(v.view, nm);
  Mat4 mvp = v.proj * v.view;
  dev().cull(false);  // the system's flag 0x40
  // the batches of one program differ only in their texture: the pass and material uniforms are set once per program
  // (sorting interleaves the batches, so runs are short and switch programs often)
  std::vector<uint32_t> ready;
  uint32_t current = 0, blend = ~0u, texture = ~0u;
  size_t q = 0;
  for (size_t i = 0; i < n;) {  // runs of consecutive sprites of one batch (skipped ones were not written)
    uint32_t b = list.sprites[i].batch;
    size_t j = i, count = 0;
    for (; j < n && list.sprites[j].batch == b; ++j)
      if (!(shadowPass && list.batches[b].blend)) ++count;
    const ParticleBatch& pb = list.batches[b];
    i = j;
    if (!count || !pb.material) {
      q += count;
      continue;
    }
    uint32_t prog = pb.material->programFor(ParticleDrawList::envFor(pb, v.env));
    if (!prog) {
      q += count;
      continue;
    }
    if (prog != current) {
      current = prog;
      dev().useProgram(prog);
      if (std::find(ready.begin(), ready.end(), prog) == ready.end()) {
        ready.push_back(prog);
        setupProgram(v, prog);
        const ProgramLocs& l = locs(prog);
        dev().uniformMatrix4fv(l.mv, 1, v.view.m);
        dev().uniformMatrix4fv(l.mvp, 1, mvp.m);
        dev().uniformMatrix3fv(l.nm, 1, nm);
        const GpuMaterial& m = *pb.material;
        dev().uniform1f(l.alphaRef, shadowPass ? 0.2f : 0.001f);
        dev().uniform4fv(l.matEmission, 1, m.emission);
        dev().uniform4fv(l.matAmbient, 1, m.ambient);
        dev().uniform4fv(l.matDiffuse, 1, m.diffuse);
        dev().uniform4fv(l.matSpecular, 1, m.specular);
        int nl = int(std::min<size_t>(v.lights.size(), 8));
        for (int i = 0; i < nl; ++i) {  // gl_FrontLightProduct
          const Light& li = v.lights[size_t(i)];
          float pa[4], pd[4];
          for (int k = 0; k < 4; ++k) pa[k] = li.ambient[k] * m.ambient[k], pd[k] = li.diffuse[k] * m.diffuse[k];
          dev().uniform4fv(l.productAmbient[i], 1, pa);
          dev().uniform4fv(l.productDiffuse[i], 1, pd);
        }
        static const Mat4 identity;
        dev().uniformMatrix4fv(l.matrix, 1, identity.m);
      }
    }
    if (!shadowPass && pb.blend != blend) {
      blend = pb.blend;
      if (blend & 0x80) dev().blendFunc(gpu::Blend::One, gpu::Blend::One);
      else if (blend & 0x100) dev().blendFunc(gpu::Blend::SrcAlpha, gpu::Blend::One);
      else dev().blendFunc(gpu::Blend::SrcAlpha, gpu::Blend::OneMinusSrcAlpha);
    }
    uint32_t tex = pb.material->textures.empty() ? 0 : pb.material->textures[0];
    if (tex != texture) {
      texture = tex;
      dev().bindTexture(0, tex);
    }
    dev().drawElements(gpu::Prim::Triangles, int(count * 6), q * 6);
    q += count;
  }
  dev().blendFunc(gpu::Blend::SrcAlpha, gpu::Blend::OneMinusSrcAlpha);
  dev().depthMask(1);
  dev().bindVertexInput(0);
}

}  // namespace regs::render
