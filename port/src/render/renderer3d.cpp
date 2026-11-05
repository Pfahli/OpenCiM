#include "render/renderer3d.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "render/gl.h"
#include "render/glsl.h"

namespace regs::render {

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

uint32_t compile(gl::GLenum type, const char* src, std::string* error) {
  uint32_t s = gl::CreateShader(type);
  gl::ShaderSource(s, 1, &src, nullptr);
  gl::CompileShader(s);
  int ok = 0;
  gl::GetShaderiv(s, gl::COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    gl::GetShaderInfoLog(s, sizeof log, nullptr, log);
    if (error) *error = log;
    return 0;
  }
  return s;
}

}  // namespace

bool Renderer3D::init(std::string* error) {
  uint32_t vs = compile(gl::VERTEX_SHADER, kVertex, error), fs = compile(gl::FRAGMENT_SHADER, kFragment, error);
  if (!vs || !fs) return false;
  program_ = gl::CreateProgram();
  gl::AttachShader(program_, vs);
  gl::AttachShader(program_, fs);
  gl::LinkProgram(program_);
  gl::DeleteShader(vs);
  gl::DeleteShader(fs);
  int ok = 0;
  gl::GetProgramiv(program_, gl::LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    gl::GetProgramInfoLog(program_, sizeof log, nullptr, log);
    if (error) *error = log;
    return false;
  }
  auto loc = [&](const char* n) { return gl::GetUniformLocation(program_, n); };
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
  gl::GenVertexArrays(1, &g.vao);
  gl::BindVertexArray(g.vao);
  gl::GenBuffers(1, &g.vbo);
  gl::BindBuffer(gl::ARRAY_BUFFER, g.vbo);
  gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(v.size() * sizeof(Vtx)), v.data(), gl::STATIC_DRAW);
  gl::GenBuffers(1, &g.ibo);
  gl::BindBuffer(gl::ELEMENT_ARRAY_BUFFER, g.ibo);
  gl::BufferData(gl::ELEMENT_ARRAY_BUFFER, gl::GLsizeiptr(m.indices.size() * 4), m.indices.data(), gl::STATIC_DRAW);
  auto attr = [](int i, int n, gl::GLenum type, bool norm, size_t off) {
    gl::EnableVertexAttribArray(gl::GLuint(i));
    gl::VertexAttribPointer(gl::GLuint(i), n, type, norm, sizeof(Vtx), reinterpret_cast<void*>(off));
  };
  attr(0, 3, gl::FLOAT, false, offsetof(Vtx, p));
  attr(1, 3, gl::FLOAT, false, offsetof(Vtx, n));
  attr(2, 2, gl::FLOAT, false, offsetof(Vtx, uv0));
  attr(3, 2, gl::FLOAT, false, offsetof(Vtx, uv1));
  attr(4, 4, gl::FLOAT, false, offsetof(Vtx, t));
  attr(5, 4, gl::UNSIGNED_BYTE, true, offsetof(Vtx, c));
  if (skinned) {  // a second stream at TEXCOORD3/4 (attribute slots 6 and 7)
    gl::GenBuffers(1, &g.skinVbo);
    gl::BindBuffer(gl::ARRAY_BUFFER, g.skinVbo);
    gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(skinAttr.size() * 4), skinAttr.data(), gl::STATIC_DRAW);
    for (int i = 0; i < 2; ++i) {
      gl::EnableVertexAttribArray(gl::GLuint(6 + i));
      gl::VertexAttribPointer(gl::GLuint(6 + i), 4, gl::FLOAT, false, 32, reinterpret_cast<void*>(size_t(16 * i)));
    }
  }
  gl::BindVertexArray(0);
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
  if (m.ibo) gl::DeleteBuffers(1, &m.ibo);
  if (m.skinVbo) gl::DeleteBuffers(1, &m.skinVbo);
  if (m.vbo) gl::DeleteBuffers(1, &m.vbo);
  if (m.vao) gl::DeleteVertexArrays(1, &m.vao);
  m = GpuMesh();
}

bool Renderer3D::makeTarget(RenderTarget& t, int w, int h, TargetFormat format, bool withDepth, bool clampToEdge) {
  t.width = w;
  t.height = h;
  gl::GenTextures(1, &t.color);
  gl::BindTexture(gl::TEXTURE_2D, t.color);
  if (format == TargetFormat::Rgba16f) gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA16F, w, h, 0, gl::RGBA, gl::HALF_FLOAT, nullptr);
  else gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, w, h, 0, gl::BGRA, gl::UNSIGNED_BYTE, nullptr);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, clampToEdge ? gl::CLAMP_TO_EDGE : gl::REPEAT);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, clampToEdge ? gl::CLAMP_TO_EDGE : gl::REPEAT);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAX_LEVEL, 0);
  if (withDepth) {
    gl::GenRenderbuffers(1, &t.depth);
    gl::BindRenderbuffer(gl::RENDERBUFFER, t.depth);
    gl::RenderbufferStorage(gl::RENDERBUFFER, gl::DEPTH_COMPONENT24, w, h);
  }
  gl::GenFramebuffers(1, &t.fbo);
  gl::BindFramebuffer(gl::FRAMEBUFFER, t.fbo);
  gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, t.color, 0);
  if (withDepth) gl::FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::RENDERBUFFER, t.depth);
  bool ok = gl::CheckFramebufferStatus(gl::FRAMEBUFFER) == gl::FRAMEBUFFER_COMPLETE;
  gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
  return ok;
}

bool Renderer3D::makeDepthTarget(RenderTarget& t, int w, int h) {
  t.width = w;
  t.height = h;
  t.depthTexture = true;
  gl::GenTextures(1, &t.depth);
  gl::BindTexture(gl::TEXTURE_2D, t.depth);
  gl::TexImage2D(gl::TEXTURE_2D, 0, gl::DEPTH_COMPONENT24, w, h, 0, gl::DEPTH_COMPONENT, gl::UNSIGNED_INT, nullptr);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
  // outside the map counts as lit: clamp to a border of depth 1 (shadows.md §4.1)
  static const float border[4] = {1, 1, 1, 1};
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_BORDER);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_BORDER);
  gl::TexParameterfv(gl::TEXTURE_2D, gl::TEXTURE_BORDER_COLOR, border);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAX_LEVEL, 0);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_COMPARE_MODE, gl::COMPARE_REF_TO_TEXTURE);
  gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_COMPARE_FUNC, gl::LEQUAL);
  gl::GenFramebuffers(1, &t.fbo);
  gl::BindFramebuffer(gl::FRAMEBUFFER, t.fbo);
  gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::TEXTURE_2D, t.depth, 0);
  gl::DrawBuffer(gl::NONE);
  gl::ReadBuffer(gl::NONE);
  bool ok = gl::CheckFramebufferStatus(gl::FRAMEBUFFER) == gl::FRAMEBUFFER_COMPLETE;
  gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
  return ok;
}

bool Renderer3D::makeSceneTarget(RenderTarget& t, int w, int h, int samples, bool depthTexture, TargetFormat format) {
  if (!makeTarget(t, w, h, format, samples <= 0 && !depthTexture, true)) return false;
  if (depthTexture) {  // a sampleable depth on the (resolve) framebuffer
    gl::GenTextures(1, &t.depthTex);
    gl::BindTexture(gl::TEXTURE_2D, t.depthTex);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::DEPTH_COMPONENT24, w, h, 0, gl::DEPTH_COMPONENT, gl::UNSIGNED_INT, nullptr);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAX_LEVEL, 0);
    gl::BindFramebuffer(gl::FRAMEBUFFER, t.fbo);
    gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::TEXTURE_2D, t.depthTex, 0);
    bool ok = gl::CheckFramebufferStatus(gl::FRAMEBUFFER) == gl::FRAMEBUFFER_COMPLETE;
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    if (!ok) return false;
  }
  if (samples <= 0) return true;
  t.samples = samples;
  gl::GenRenderbuffers(1, &t.msColor);
  gl::BindRenderbuffer(gl::RENDERBUFFER, t.msColor);
  gl::RenderbufferStorageMultisample(gl::RENDERBUFFER, samples, format == TargetFormat::Rgba16f ? gl::RGBA16F : gl::RGBA8, w, h);
  gl::GenRenderbuffers(1, &t.msDepth);
  gl::BindRenderbuffer(gl::RENDERBUFFER, t.msDepth);
  gl::RenderbufferStorageMultisample(gl::RENDERBUFFER, samples, gl::DEPTH_COMPONENT24, w, h);
  gl::GenFramebuffers(1, &t.msFbo);
  gl::BindFramebuffer(gl::FRAMEBUFFER, t.msFbo);
  gl::FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::RENDERBUFFER, t.msColor);
  gl::FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::RENDERBUFFER, t.msDepth);
  bool ok = gl::CheckFramebufferStatus(gl::FRAMEBUFFER) == gl::FRAMEBUFFER_COMPLETE;
  gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
  return ok;
}

void Renderer3D::resolve(const RenderTarget& t) {
  if (!t.msFbo) return;
  gl::BindFramebuffer(gl::READ_FRAMEBUFFER, t.msFbo);
  gl::BindFramebuffer(gl::DRAW_FRAMEBUFFER, t.fbo);
  gl::Disable(gl::SCISSOR_TEST);
  gl::BlitFramebuffer(0, 0, t.width, t.height, 0, 0, t.width, t.height,
                      gl::COLOR_BUFFER_BIT | (t.depthTex ? gl::DEPTH_BUFFER_BIT : 0), gl::NEAREST);
  gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
}

void Renderer3D::blitToBack(const RenderTarget& t, int x, int y, int w, int h, int backH) {
  if (!t.fbo) return;
  resolve(t);  // a multisample source must be resolved at its own size before a blit to another position
  gl::BindFramebuffer(gl::READ_FRAMEBUFFER, t.fbo);
  gl::BindFramebuffer(gl::DRAW_FRAMEBUFFER, 0);
  gl::Disable(gl::SCISSOR_TEST);
  gl::BlitFramebuffer(0, 0, w, h, x, backH - y - h, x + w, backH - y, gl::COLOR_BUFFER_BIT, gl::NEAREST);
  gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
}

void Renderer3D::blitDepth(const RenderTarget& src, const RenderTarget& dst, int w, int h) {
  gl::BindFramebuffer(gl::READ_FRAMEBUFFER, src.msFbo ? src.msFbo : src.fbo);
  gl::BindFramebuffer(gl::DRAW_FRAMEBUFFER, dst.fbo);
  gl::Disable(gl::SCISSOR_TEST);
  gl::BlitFramebuffer(0, 0, w, h, 0, 0, w, h, gl::DEPTH_BUFFER_BIT, gl::NEAREST);
  gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
}

void Renderer3D::readRed(uint32_t texture, int w, int h, std::vector<float>& out) {
  out.assign(size_t(w) * size_t(h), 0.0f);
  gl::BindTexture(gl::TEXTURE_2D, texture);
  gl::PixelStorei(gl::PACK_ALIGNMENT, 4);
  gl::GetTexImage(gl::TEXTURE_2D, 0, gl::RED, gl::FLOAT, out.data());
  gl::BindTexture(gl::TEXTURE_2D, 0);
}

void Renderer3D::destroyTarget(RenderTarget& t) {
  if (t.msFbo) gl::DeleteFramebuffers(1, &t.msFbo);
  if (t.msColor) gl::DeleteRenderbuffers(1, &t.msColor);
  if (t.msDepth) gl::DeleteRenderbuffers(1, &t.msDepth);
  if (t.fbo) gl::DeleteFramebuffers(1, &t.fbo);
  if (t.depthTex) gl::DeleteTextures(1, &t.depthTex);
  if (t.depth && t.depthTexture) gl::DeleteTextures(1, &t.depth);
  else if (t.depth) gl::DeleteRenderbuffers(1, &t.depth);
  if (t.color) gl::DeleteTextures(1, &t.color);
  t = RenderTarget{};
}

void Renderer3D::drawQuad(uint32_t program, const uint32_t* textures, int count, const float custom0[4], const float uv[4][4],
                          const float* rect, const float* custom1) {
  if (!program) return;
  if (!quadVao_) {
    gl::GenVertexArrays(1, &quadVao_);
    gl::BindVertexArray(quadVao_);
    gl::GenBuffers(1, &quadVbo_);
    gl::BindBuffer(gl::ARRAY_BUFFER, quadVbo_);
    gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(4 * 7 * sizeof(float)), nullptr, gl::STREAM_DRAW);
    gl::EnableVertexAttribArray(0);  // regs_Vertex
    gl::VertexAttribPointer(0, 3, gl::FLOAT, false, 7 * sizeof(float), nullptr);
    gl::EnableVertexAttribArray(2);  // regs_MultiTexCoord0
    gl::VertexAttribPointer(2, 4, gl::FLOAT, false, 7 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
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
  gl::BindVertexArray(quadVao_);
  gl::BindBuffer(gl::ARRAY_BUFFER, quadVbo_);
  gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(sizeof(v)), v, gl::STREAM_DRAW);
  gl::Disable(gl::DEPTH_TEST);
  gl::Disable(gl::CULL_FACE);
  gl::Disable(gl::BLEND);
  gl::Disable(gl::POLYGON_OFFSET_FILL);
  gl::DepthMask(0);
  gl::UseProgram(program);
  const ProgramLocs& l = locs(program);
  static const Mat4 identity;
  gl::UniformMatrix4fv(l.mvp, 1, 0, identity.m);
  gl::UniformMatrix4fv(l.mv, 1, 0, identity.m);
  gl::UniformMatrix4fv(l.proj, 1, 0, identity.m);
  gl::Uniform1f(l.alphaRef, -1.0f);
  if (custom0) gl::Uniform4fv(l.custom[0], 1, custom0);
  if (custom1) gl::Uniform4fv(l.custom[1], 1, custom1);
  for (int i = 0; i < count && i < 8; ++i) {
    gl::Uniform1i(l.texture[i], i);
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(i));
    gl::BindTexture(gl::TEXTURE_2D, textures[i]);
  }
  gl::ActiveTexture(gl::TEXTURE0);
  gl::DrawArrays(gl::TRIANGLES, 0, 6);
  gl::DepthMask(1);
  gl::BindVertexArray(0);
}

void Renderer3D::begin(const RenderTarget* t, int x, int y, int w, int h, uint32_t clearArgb, bool clearColor, int backW,
                       int backH) {
  gl::BindFramebuffer(gl::FRAMEBUFFER, t ? (t->msFbo ? t->msFbo : t->fbo) : 0);
  // the back buffer takes a top-left rect; a target holds the view in its bottom-left corner (GL rows from the bottom),
  // which is the part the GUI image crops when the target is larger than the view (scene-basics.md §7.1)
  int vy = t ? y : backH - y - h;
  gl::Viewport(x, vy, w, h);
  gl::Enable(gl::SCISSOR_TEST);
  gl::Scissor(x, vy, w, h);
  gl::ClearColor(float(clearArgb >> 16 & 0xFF) / 255, float(clearArgb >> 8 & 0xFF) / 255, float(clearArgb & 0xFF) / 255, 0);
  gl::ClearDepth(1.0);
  gl::DepthMask(1);
  gl::Clear(gl::DEPTH_BUFFER_BIT | (clearColor ? gl::COLOR_BUFFER_BIT : 0));
  gl::Enable(gl::DEPTH_TEST);
  gl::DepthFunc(gl::LEQUAL);
  gl::Enable(gl::CULL_FACE);
  gl::CullFace(gl::BACK);
  gl::Disable(gl::BLEND);
  (void)backW;
}

void Renderer3D::end(int backW, int backH) {
  gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
  gl::Disable(gl::DEPTH_TEST);
  gl::Disable(gl::CULL_FACE);
  gl::DepthMask(1);
  gl::Viewport(0, 0, backW, backH);
  gl::BindVertexArray(0);
}

void Renderer3D::drawPart(const SceneView& v, const DrawItem& it, const GpuMesh::Part& p) {
  static const GpuMaterial fallback;
  const GpuMaterial& m = it.materials && size_t(p.material) < it.materials->size() ? (*it.materials)[size_t(p.material)] : fallback;
  // texture units in material order: diffuse, specular, normal, illumination, ambient occlusion
  int unit = 0;
  auto bindSlot = [&](uint32_t flag, const char* name) {
    if (!(m.flags & flag)) return;
    if (size_t(unit) < m.textures.size()) {
      gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
      gl::BindTexture(gl::TEXTURE_2D, m.textures[size_t(unit)]);
    }
    gl::Uniform1i(gl::GetUniformLocation(program_, name), unit);
    ++unit;
  };
  bindSlot(kMatDiffuseMap, "uDiffuseMap");
  bindSlot(kMatSpecularMap, "uSpecularMap");
  bindSlot(kMatNormalMap, "uNormalMap");
  bindSlot(0x20000, "uIlluminationMap");
  bindSlot(0x100000, "uAoMap");
  if ((m.flags & kMatReflection) && m.envCube) {  // the environment cube follows the 2D maps
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
    gl::BindTexture(gl::TEXTURE_CUBE_MAP, m.envCube);
    gl::Uniform1i(gl::GetUniformLocation(program_, "uEnvMap"), unit);
  }
  gl::Uniform1f(gl::GetUniformLocation(program_, "uReflection"), m.reflection);
  gl::ActiveTexture(gl::TEXTURE0);
  gl::Uniform1i(u_.flags, int((m.flags & kMatReflection) && !m.envCube ? m.flags & ~kMatReflection : m.flags));
  gl::Uniform4fv(u_.matAmbient, 1, m.ambient);
  float d[4] = {m.diffuse[0], m.diffuse[1], m.diffuse[2], m.diffuse[3] * it.alpha};
  gl::Uniform4fv(u_.matDiffuse, 1, d);
  gl::Uniform4fv(u_.matSpecular, 1, m.specular);
  float em[4] = {m.emission[0] * v.emissionScale, m.emission[1] * v.emissionScale, m.emission[2] * v.emissionScale, m.emission[3]};
  gl::Uniform4fv(u_.matEmission, 1, em);
  gl::Uniform1f(u_.shininess, m.shininess);
  if (m.flags & kMatTwoSided) gl::Disable(gl::CULL_FACE);
  else gl::Enable(gl::CULL_FACE);
  gl::DrawElements(gl::TRIANGLES, gl::GLsizei(p.indexCount), gl::UNSIGNED_INT,
                   reinterpret_cast<void*>(size_t(p.firstIndex) * 4));
}

uint32_t Renderer3D::buildProgram(const std::string& vs110, const std::string& fs110, std::string* error) {
  std::string why;
  std::string vs = translateGlsl110(vs110, false, &why), fs = vs.empty() ? "" : translateGlsl110(fs110, true, &why);
  if (vs.empty() || fs.empty()) {
    if (error) *error = "unsupported built-in " + why;
    return 0;
  }
  uint32_t v = compile(gl::VERTEX_SHADER, vs.c_str(), error), f = v ? compile(gl::FRAGMENT_SHADER, fs.c_str(), error) : 0;
  if (!v || !f) return 0;
  uint32_t prog = gl::CreateProgram();
  gl::AttachShader(prog, v);
  gl::AttachShader(prog, f);
  // the vertex layout of upload(): fixed-function arrays become generic attributes at fixed slots
  const char* names[] = {"regs_Vertex", "regs_Normal", "regs_MultiTexCoord0", "regs_MultiTexCoord1",
                         "regs_MultiTexCoord2", "regs_Color", "regs_MultiTexCoord3", "regs_MultiTexCoord4"};
  for (int i = 0; i < 8; ++i) gl::BindAttribLocation(prog, gl::GLuint(i), names[i]);
  gl::LinkProgram(prog);
  gl::DeleteShader(v);
  gl::DeleteShader(f);
  int ok = 0;
  gl::GetProgramiv(prog, gl::LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    gl::GetProgramInfoLog(prog, sizeof log, nullptr, log);
    if (error) *error = log;
    return 0;
  }
  return prog;
}

const Renderer3D::ProgramLocs& Renderer3D::locs(uint32_t prog) {
  auto it = locs_.find(prog);
  if (it != locs_.end()) return it->second;
  ProgramLocs& l = locs_[prog];
  auto at = [&](const std::string& n) { return gl::GetUniformLocation(prog, n.c_str()); };
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
  return l;
}

// per-pass state of a combiner program (gsIShader::apply with programChanged, interfaces-3d.md §3.3)
void Renderer3D::setupProgram(const SceneView& v, uint32_t prog) {
  const ProgramLocs& l = locs(prog);
  gl::UniformMatrix4fv(l.proj, 1, 0, v.proj.m);
  gl::Uniform4fv(l.modelAmbient, 1, v.lightModelAmbient);
  for (int i = 0; i < 8; ++i) gl::Uniform1i(l.texture[i], i);
  int n = int(std::min<size_t>(v.lights.size(), 8));
  for (int i = 0; i < n; ++i) {
    const Light& li = v.lights[size_t(i)];
    Vec3 p = transformPoint(v.view, li.position);  // glLightfv(GL_POSITION) under the view matrix: eye space, w = 1
    gl::Uniform4f(l.lightPos[i], p.x, p.y, p.z, 1);
    gl::Uniform4fv(l.lightAmbient[i], 1, li.ambient);
    gl::Uniform4fv(l.lightDiffuse[i], 1, li.diffuse);
    gl::Uniform4fv(l.lightSpecular[i], 1, li.specular);
    gl::Uniform1f(l.lightConst[i], 1);
    gl::Uniform1f(l.lightSpotCut[i], 180);
    gl::Uniform1f(l.lightSpotCos[i], -1);
  }
  // no scene fog: push it out of reach (the default shader always applies fog)
  gl::Uniform4fv(l.fogColor, 1, v.fogColor);
  gl::Uniform1f(l.fogStart, v.fog ? v.fogStart : 1e30f);
  gl::Uniform1f(l.fogEnd, v.fog ? v.fogEnd : 1e30f);
  gl::Uniform1f(l.fogScale, v.fog && v.fogEnd > v.fogStart ? 1.0f / (v.fogEnd - v.fogStart) : 0.0f);
  gl::Uniform1f(l.fogDensity, 1);
  gl::Uniform3f(l.eyepos, v.eye.x, v.eye.y, v.eye.z);
  gl::Uniform1f(l.timer, v.timer);
  float vo[9];  // vieworient: GLSL r * vieworient = eye-space direction to world space (scene-basics.md §6.5)
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r) vo[c * 3 + r] = v.view.at(r, c);
  gl::UniformMatrix3fv(l.vieworient, 1, 0, vo);
  Vec3 up{v.view.at(0, 1), v.view.at(1, 1), v.view.at(2, 1)};  // world +Y in eye space
  gl::Uniform3f(l.worldup, up.x, up.y, up.z);
  gl::UniformMatrix4fv(l.shadowTransform, 1, 0, v.shadowTransform.m);
  gl::UniformMatrix4fv(l.shadowWarp, 1, 0, v.shadowWarp.m);
  gl::Uniform4fv(l.shadowParams, 1, v.shadowParams);
}

void Renderer3D::drawCombined(const SceneView& v, const DrawItem& it, const GpuMesh::Part& p, const GpuMaterial& m,
                              uint32_t prog, float alphaRef) {
  const ProgramLocs& l = locs(prog);
  gl::Uniform1f(l.alphaRef, alphaRef);  // glAlphaFunc(GREATER, ref) of the queue
  const float es = v.emissionScale;
  gl::Uniform4f(l.matEmission, m.emission[0] * es, m.emission[1] * es, m.emission[2] * es, m.emission[3]);
  float amb[4] = {m.ambient[0], m.ambient[1], m.ambient[2], m.ambient[3] * it.alpha};  // ambient alpha = object fade
  gl::Uniform4fv(l.matAmbient, 1, amb);
  gl::Uniform4fv(l.matDiffuse, 1, m.diffuse);
  gl::Uniform4fv(l.matSpecular, 1, m.specular);
  gl::Uniform1f(l.matShininess, m.shininess);
  int n = int(std::min<size_t>(v.lights.size(), 8));
  for (int i = 0; i < n; ++i) {  // gl_FrontLightProduct = light × material, component-wise
    const Light& li = v.lights[size_t(i)];
    float pa[4], pd[4], ps[4];
    for (int k = 0; k < 4; ++k) {
      pa[k] = li.ambient[k] * amb[k];
      pd[k] = li.diffuse[k] * m.diffuse[k];
      ps[k] = li.specular[k] * m.specular[k];
    }
    gl::Uniform4fv(l.productAmbient[i], 1, pa);
    gl::Uniform4fv(l.productDiffuse[i], 1, pd);
    gl::Uniform4fv(l.productSpecular[i], 1, ps);
  }
  float tint[4];
  for (int k = 0; k < 4; ++k) tint[k] = it.colorize[k] * float(p.colorize >> (k == 3 ? 24 : 16 - 8 * k) & 0xFF) / 255.0f;
  gl::Uniform4fv(l.colorize, 1, tint);
  gl::Uniform1f(l.reflection, m.reflection);
  for (int i = 0; i < 8; ++i) gl::Uniform4fv(l.custom[i], 1, m.custom[i]);
  if (it.hasCustom4) gl::Uniform4f(l.custom[4], it.custom4[0], it.custom4[1], m.custom[4][2], m.custom[4][3]);
  gl::UniformMatrix4fv(l.matrix, 1, 0, it.world.m);
  for (size_t i = 0; i < m.textures.size() && i < 7; ++i) {
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(i));
    gl::BindTexture(i < m.cube.size() && m.cube[i] ? gl::TEXTURE_CUBE_MAP : gl::TEXTURE_2D, m.textures[i]);
  }
  gl::ActiveTexture(gl::TEXTURE0);
  if (m.flags & kMatTwoSided) gl::Disable(gl::CULL_FACE);
  else gl::Enable(gl::CULL_FACE);
  if (p.groups.empty() || l.bones < 0) {
    gl::DrawElements(gl::TRIANGLES, gl::GLsizei(p.indexCount), gl::UNSIGNED_INT,
                     reinterpret_cast<void*>(size_t(p.firstIndex) * 4));
    return;
  }
  // per bone group: the palette's skin matrices (invBind · world; identity = bind pose), then its index range
  // (gsHWSkinSubMesh_OGL draw 0x10054140)
  static const Mat4 identity;
  std::vector<float> palette;
  for (const GpuMesh::Part::SkinGroup& g : p.groups) {
    palette.clear();
    for (uint16_t b : g.palette) {
      const Mat4& s = it.bones && b < it.bones->size() ? (*it.bones)[b] : identity;
      palette.insert(palette.end(), s.m, s.m + 16);
    }
    gl::UniformMatrix4fv(l.bones, gl::GLsizei(g.palette.size()), 0, palette.data());
    gl::DrawElements(gl::TRIANGLES, gl::GLsizei(g.indexCount), gl::UNSIGNED_INT,
                     reinterpret_cast<void*>(size_t(g.firstIndex) * 4));
  }
}

void Renderer3D::setupFallback(const SceneView& v) {
  gl::UniformMatrix4fv(u_.proj, 1, 0, v.proj.m);
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
  gl::Uniform1i(u_.lightCount, n);
  if (n) {
    gl::Uniform3fv(u_.lightPos, n, pos);
    gl::Uniform4fv(u_.lightAmbient, n, amb);
    gl::Uniform4fv(u_.lightDiffuse, n, dif);
    gl::Uniform4fv(u_.lightSpecular, n, spe);
  }
  gl::Uniform4fv(u_.modelAmbient, 1, v.lightModelAmbient);
  gl::Uniform1i(u_.fog, v.fog ? 1 : 0);
  gl::Uniform4f(u_.fogParams, v.fogStart, v.fogEnd > v.fogStart ? 1.0f / (v.fogEnd - v.fogStart) : 0.0f, 0, 0);
  float v2w[9];  // eye-space directions back to world space: transpose of the view rotation
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) v2w[c * 3 + r] = v.view.at(c, r);
  gl::UniformMatrix3fv(gl::GetUniformLocation(program_, "uViewToWorld"), 1, 0, v2w);
  gl::Uniform4fv(u_.fogColor, 1, v.fogColor);
  gl::Uniform1f(u_.alphaRef, 0.2f);
  static const int debugShade = std::getenv("REGS_DEBUG_SHADE") ? std::atoi(std::getenv("REGS_DEBUG_SHADE")) : 0;
  gl::Uniform1i(gl::GetUniformLocation(program_, "uDebug"), debugShade);
}

void Renderer3D::draw(const SceneView& v, const std::vector<DrawItem>& items) {
  static const GpuMaterial fallbackMaterial;
  auto materialOf = [&](const DrawItem& it, const GpuMesh::Part& p) -> const GpuMaterial& {
    return it.materials && size_t(p.material) < it.materials->size() ? (*it.materials)[size_t(p.material)] : fallbackMaterial;
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
  for (const DrawItem& it : items) {
    if (!it.mesh || (it.shadowOnly && !shadowPass)) continue;
    for (const auto& p : it.mesh->parts) {
      const GpuMaterial& m = materialOf(it, p);
      uint32_t prog = m.programFor(v.env);
      if (shadowPass && (!prog || (m.flags & (kMatNoShadow | kMatDecal)))) continue;
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
  if (v.flipCull) gl::CullFace(gl::FRONT);
  if (v.env & kEnvShadowMap) {
    gl::ActiveTexture(gl::TEXTURE0 + 7);
    gl::BindTexture(gl::TEXTURE_2D, v.shadowMap);
    gl::ActiveTexture(gl::TEXTURE0);
  }
  uint32_t current = 0;
  const DrawItem* bound = nullptr;
  gl::GLenum blendSrc = gl::SRC_ALPHA, blendDst = gl::ONE_MINUS_SRC_ALPHA;
  // opaque: no alpha test; alpha-tested: GREATER 0.2 with depth writes; blended and decals: GREATER 0.001 without
  auto run = [&](const std::vector<Q>& q, float ref, bool depthWrite) {
    gl::DepthMask(depthWrite);
    for (const Q& e : q) {
      const GpuMaterial& m = materialOf(*e.it, *e.p);
      if (!shadowPass) {
        // polygon offset (interfaces-3d.md material state): blended and decal (−2,−2), a depth offset d gives (2d, 2d)
        float off = (m.flags & (kMatAlphaBlend | kMatDecal)) ? -2.0f : 0.0f;
        if (m.depthOffset != 0) off = 2.0f * m.depthOffset;
        if (off != 0) {
          gl::Enable(gl::POLYGON_OFFSET_FILL);
          gl::PolygonOffset(off, off);
        } else {
          gl::Disable(gl::POLYGON_OFFSET_FILL);
        }
        // blend function (interfaces-3d.md §2.10): MODULATE (DST_COLOR, ZERO), MODULATE_2 (DST_COLOR, SRC_COLOR),
        // ADDITIVE (ONE, ONE), ADDITIVE_A (SRC_ALPHA, ONE: the street-light glows of the night maps), else alpha
        constexpr gl::GLenum kSrcColor = 0x0300, kDstColor = 0x0306;
        gl::GLenum src = gl::SRC_ALPHA, dst = gl::ONE_MINUS_SRC_ALPHA;
        if (m.flags & 0x40000u) src = kDstColor, dst = gl::ZERO;
        else if (m.flags & 0x80000u) src = kDstColor, dst = kSrcColor;
        else if (m.flags & 0x80u) src = gl::ONE, dst = gl::ONE;
        else if (m.flags & 0x100u) dst = gl::ONE;
        if (src != blendSrc || dst != blendDst) gl::BlendFunc(blendSrc = src, blendDst = dst);
      }
      uint32_t prog = e.program ? e.program : program_;
      if (prog != current) {
        current = prog;
        bound = nullptr;
        gl::UseProgram(prog);
        if (e.program) setupProgram(v, prog);
        else setupFallback(v);
      }
      if (e.it != bound) {
        bound = e.it;
        Mat4 mv = v.view * e.it->world;
        float nm[9];
        normalMatrix(mv, nm);
        if (e.program) {
          const ProgramLocs& l = locs(prog);
          Mat4 mvp = v.proj * mv;
          gl::UniformMatrix4fv(l.mv, 1, 0, mv.m);
          gl::UniformMatrix4fv(l.mvp, 1, 0, mvp.m);
          gl::UniformMatrix3fv(l.nm, 1, 0, nm);
        } else {
          gl::UniformMatrix4fv(u_.modelView, 1, 0, mv.m);
          gl::UniformMatrix3fv(u_.normal, 1, 0, nm);
          gl::Uniform4fv(u_.colorize, 1, e.it->colorize);
        }
        gl::BindVertexArray(e.it->mesh->vao);
      }
      if (e.program) drawCombined(v, *e.it, *e.p, m, prog, ref);
      else drawPart(v, *e.it, *e.p);
    }
  };
  run(opaque, -1.0f, true);
  if (!shadowPass) {
    gl::Enable(gl::BLEND);  // the alpha-test queue blends too: cut-out edges are smoothed (§1.2)
    gl::BlendFunc(blendSrc = gl::SRC_ALPHA, blendDst = gl::ONE_MINUS_SRC_ALPHA);
  }
  run(tested, 0.2f, true);
  run(blended, 0.001f, shadowPass);
  if (v.particles && !v.particles->sprites.empty()) drawParticles(v, *v.particles);
  gl::Disable(gl::BLEND);
  if (blendSrc != gl::SRC_ALPHA || blendDst != gl::ONE_MINUS_SRC_ALPHA) gl::BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
  gl::DepthMask(1);
  gl::CullFace(gl::BACK);
  gl::BindVertexArray(0);
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
    gl::GenVertexArrays(1, &particleMesh_.vao);
    gl::BindVertexArray(particleMesh_.vao);
    gl::GenBuffers(1, &particleMesh_.vbo);
    gl::GenBuffers(1, &particleMesh_.ibo);
    gl::BindBuffer(gl::ARRAY_BUFFER, particleMesh_.vbo);
    gl::BindBuffer(gl::ELEMENT_ARRAY_BUFFER, particleMesh_.ibo);
    auto attr = [](int i, int k, gl::GLenum type, bool norm, size_t off) {
      gl::EnableVertexAttribArray(gl::GLuint(i));
      gl::VertexAttribPointer(gl::GLuint(i), k, type, norm, sizeof(PVtx), reinterpret_cast<void*>(off));
    };
    attr(0, 3, gl::FLOAT, false, offsetof(PVtx, p));
    attr(1, 3, gl::FLOAT, false, offsetof(PVtx, n));
    attr(2, 2, gl::FLOAT, false, offsetof(PVtx, uv));
    attr(5, 4, gl::UNSIGNED_BYTE, true, offsetof(PVtx, c));
  }
  gl::BindVertexArray(particleMesh_.vao);
  if (n > particleQuads_) {  // the quad index buffer: (0,1,2) (0,2,3) per quad
    particleQuads_ = std::max(n, particleQuads_ * 2);
    std::vector<uint32_t> idx(particleQuads_ * 6);
    for (size_t q = 0; q < particleQuads_; ++q) {
      uint32_t b = uint32_t(q * 4);
      uint32_t* o = &idx[q * 6];
      o[0] = b, o[1] = b + 1, o[2] = b + 2, o[3] = b, o[4] = b + 2, o[5] = b + 3;
    }
    gl::BufferData(gl::ELEMENT_ARRAY_BUFFER, gl::GLsizeiptr(idx.size() * 4), idx.data(), gl::STATIC_DRAW);
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
    gl::BindVertexArray(0);
    return;
  }
  gl::BindBuffer(gl::ARRAY_BUFFER, particleMesh_.vbo);
  gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(quads * 4 * sizeof(PVtx)), nullptr, gl::STREAM_DRAW);  // orphan
  gl::BufferSubData(gl::ARRAY_BUFFER, 0, gl::GLsizeiptr(quads * 4 * sizeof(PVtx)), particleVerts_.data());
  gl::Disable(gl::POLYGON_OFFSET_FILL);
  if (shadowPass) {
    gl::Disable(gl::BLEND);
    gl::DepthMask(1);
  } else {
    gl::Enable(gl::BLEND);
    gl::DepthMask(0);
  }
  float nm[9];
  normalMatrix(v.view, nm);
  Mat4 mvp = v.proj * v.view;
  gl::Disable(gl::CULL_FACE);  // the system's flag 0x40
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
      gl::UseProgram(prog);
      if (std::find(ready.begin(), ready.end(), prog) == ready.end()) {
        ready.push_back(prog);
        setupProgram(v, prog);
        const ProgramLocs& l = locs(prog);
        gl::UniformMatrix4fv(l.mv, 1, 0, v.view.m);
        gl::UniformMatrix4fv(l.mvp, 1, 0, mvp.m);
        gl::UniformMatrix3fv(l.nm, 1, 0, nm);
        const GpuMaterial& m = *pb.material;
        gl::Uniform1f(l.alphaRef, shadowPass ? 0.2f : 0.001f);
        gl::Uniform4fv(l.matEmission, 1, m.emission);
        gl::Uniform4fv(l.matAmbient, 1, m.ambient);
        gl::Uniform4fv(l.matDiffuse, 1, m.diffuse);
        gl::Uniform4fv(l.matSpecular, 1, m.specular);
        int nl = int(std::min<size_t>(v.lights.size(), 8));
        for (int i = 0; i < nl; ++i) {  // gl_FrontLightProduct
          const Light& li = v.lights[size_t(i)];
          float pa[4], pd[4];
          for (int k = 0; k < 4; ++k) pa[k] = li.ambient[k] * m.ambient[k], pd[k] = li.diffuse[k] * m.diffuse[k];
          gl::Uniform4fv(l.productAmbient[i], 1, pa);
          gl::Uniform4fv(l.productDiffuse[i], 1, pd);
        }
        static const Mat4 identity;
        gl::UniformMatrix4fv(l.matrix, 1, 0, identity.m);
      }
    }
    if (!shadowPass && pb.blend != blend) {
      blend = pb.blend;
      if (blend & 0x80) gl::BlendFunc(gl::ONE, gl::ONE);
      else if (blend & 0x100) gl::BlendFunc(gl::SRC_ALPHA, gl::ONE);
      else gl::BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
    }
    uint32_t tex = pb.material->textures.empty() ? 0 : pb.material->textures[0];
    if (tex != texture) {
      texture = tex;
      gl::ActiveTexture(gl::TEXTURE0);
      gl::BindTexture(gl::TEXTURE_2D, tex);
    }
    gl::DrawElements(gl::TRIANGLES, gl::GLsizei(count * 6), gl::UNSIGNED_INT, reinterpret_cast<void*>(q * 6 * 4));
    q += count;
  }
  gl::BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
  gl::DepthMask(1);
  gl::BindVertexArray(0);
}

}  // namespace regs::render
