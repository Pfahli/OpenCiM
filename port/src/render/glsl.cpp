#include "render/glsl.h"

#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <set>

namespace regs::render {

namespace {

// Fixed-function state of GLSL 1.10, re-declared as ordinary uniforms with the same layout and member names.
const char* kReflectionCode = R"(
// raytraced reflections (§9.7): the hit's triangle from the device's hit table (one record per mesh part: vertex and
// index addresses, base texture, colour), its texture from the bindless array, lit by the sun with a shadow ray
layout(set = 1, binding = 0) uniform texture2D regs_Images[];
layout(set = 1, binding = 1) uniform sampler regs_HitSampler;
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RegsHitRecord { uvec4 a; uvec4 b; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RegsU32 { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RegsF32 { float v[]; };
uniform uvec2 regs_HitTable;
uniform vec4 regs_EyePos;  // the camera, world space
uvec2 regs_addr(uvec2 a, uint off) { uint lo = a.x + off; return uvec2(lo, a.y + (lo < a.x ? 1u : 0u)); }
vec3 regs_vec3(RegsF32 b, uint at) { return vec3(b.v[at], b.v[at + 1u], b.v[at + 2u]); }
vec4 regs_reflectCube(samplerCube s, vec3 dir) {
  // the ray leaves along a calmer direction than the shader's: the reflection about the flat surface, bent 25 % of
  // the way toward the rippled one (the water's normal maps would scatter single rays into speckles of sky and bank;
  // the cube lookup they replace is smooth). Glass is flat, so there both are the same.
  vec3 view = normalize(regs_WorldPos - regs_EyePos.xyz);
  vec3 geo = cross(dFdx(regs_WorldPos), dFdy(regs_WorldPos));
  geo = dot(geo, geo) > 1e-12 ? normalize(geo) : vec3(0.0, 1.0, 0.0);
  if (dot(geo, view) > 0.0) geo = -geo;
  vec3 d = normalize(mix(reflect(view, geo), normalize(dir), 0.25));
  if (dot(d, geo) < 0.02) d = normalize(d + geo * (0.02 - dot(d, geo)));
#if REGS_REFL_DEBUG == 1
  return vec4(1.0, 0.0, 0.0, 1.0);
#endif
  rayQueryEXT q;
  rayQueryInitializeEXT(q, regs_Scene, gl_RayFlagsOpaqueEXT, 0xFF, regs_WorldPos + d * 0.05, 0.05, d, 3000.0);
  while (rayQueryProceedEXT(q)) {}
  if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) return texture(s, dir);
#if REGS_REFL_DEBUG == 2
  return vec4(0.0, 1.0, 0.0, 1.0);
#endif
  int rec = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true) + rayQueryGetIntersectionGeometryIndexEXT(q, true);
  RegsHitRecord r = RegsHitRecord(regs_addr(regs_HitTable, uint(rec) * 32u));
  uvec4 ra = r.a, rb = r.b;
#if REGS_REFL_DEBUG == 3
  return unpackUnorm4x8(rb.y);
#endif
  RegsU32 ib = RegsU32(ra.zw);
  RegsF32 vb = RegsF32(ra.xy);
  uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
  uint i0 = ib.v[prim * 3u] * 15u, i1 = ib.v[prim * 3u + 1u] * 15u, i2 = ib.v[prim * 3u + 2u] * 15u;
  vec2 bc = rayQueryGetIntersectionBarycentricsEXT(q, true);
  vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
  // the vertex: position 0-2, normal 3-5, uv 6-7 (15 floats, renderer3d.cpp Vtx)
  vec3 on = regs_vec3(vb, i0 + 3u) * w.x + regs_vec3(vb, i1 + 3u) * w.y + regs_vec3(vb, i2 + 3u) * w.z;
  vec2 uv = vec2(vb.v[i0 + 6u], vb.v[i0 + 7u]) * w.x + vec2(vb.v[i1 + 6u], vb.v[i1 + 7u]) * w.y +
            vec2(vb.v[i2 + 6u], vb.v[i2 + 7u]) * w.z;
  mat4x3 toWorld = rayQueryGetIntersectionObjectToWorldEXT(q, true);
  vec3 n = mat3(toWorld) * on;
  n = dot(n, n) > 1e-12 ? normalize(n) : -d;
  if (dot(n, d) > 0.0) n = -n;
  float t = rayQueryGetIntersectionTEXT(q, true);
  vec4 albedo = unpackUnorm4x8(rb.y);
  if (rb.x != 0u)  // farther hits read smaller mips (no screen derivatives along a reflected ray)
    albedo *= textureLod(sampler2D(regs_Images[nonuniformEXT(rb.x)], regs_HitSampler), uv, clamp(log2(1.0 + t * 0.05) * 2.0, 0.0, 10.0));
  vec3 p = regs_WorldPos + d * t;
  vec3 l = normalize(regs_SunPos.xyz - p);
  float lit = max(dot(n, l), 0.0);
  if (lit > 0.0) {
    rayQueryEXT sq;
    rayQueryInitializeEXT(sq, regs_Scene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, p + n * 0.05, 0.0, l, 4000.0);
    while (rayQueryProceedEXT(sq)) {}
    if (rayQueryGetIntersectionTypeEXT(sq, true) != gl_RayQueryCommittedIntersectionNoneEXT) lit = 0.0;
  }
  vec3 c = albedo.rgb * (regs_LightModel.ambient.rgb + regs_LightSource[0].ambient.rgb + regs_LightSource[0].diffuse.rgb * lit);
  // the reflected point is t farther away: its own share of the fog
  c = mix(c, regs_Fog.color.rgb, clamp((t - regs_Fog.start) * regs_Fog.scale, 0.0, 1.0));
  return vec4(c, 1.0);
}
)";

const char* kBuiltins = R"(struct regs_MaterialParameters { vec4 emission; vec4 ambient; vec4 diffuse; vec4 specular; float shininess; };
struct regs_LightSourceParameters { vec4 ambient; vec4 diffuse; vec4 specular; vec4 position; vec4 halfVector;
  vec3 spotDirection; float spotExponent; float spotCutoff; float spotCosCutoff;
  float constantAttenuation; float linearAttenuation; float quadraticAttenuation; };
struct regs_LightProducts { vec4 ambient; vec4 diffuse; vec4 specular; };
struct regs_LightModelParameters { vec4 ambient; };
struct regs_FogParameters { vec4 color; float density; float start; float end; float scale; };
uniform regs_MaterialParameters regs_FrontMaterial;
uniform regs_LightSourceParameters regs_LightSource[8];
uniform regs_LightProducts regs_FrontLightProduct[8];
uniform regs_LightModelParameters regs_LightModel;
uniform regs_FogParameters regs_Fog;
uniform mat4 regs_ModelViewMatrix;
uniform mat4 regs_ProjectionMatrix;
uniform mat4 regs_ModelViewProjectionMatrix;
uniform mat3 regs_NormalMatrix;
float regs_smoothstep(float a, float b, float x) { float t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
vec2 regs_smoothstep(vec2 a, vec2 b, vec2 x) { vec2 t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
vec3 regs_smoothstep(vec3 a, vec3 b, vec3 x) { vec3 t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
vec4 regs_smoothstep(vec4 a, vec4 b, vec4 x) { vec4 t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
vec2 regs_smoothstep(float a, float b, vec2 x) { return regs_smoothstep(vec2(a), vec2(b), x); }
vec3 regs_smoothstep(float a, float b, vec3 x) { return regs_smoothstep(vec3(a), vec3(b), x); }
vec4 regs_smoothstep(float a, float b, vec4 x) { return regs_smoothstep(vec4(a), vec4(b), x); }
)";

std::vector<ShaderHook>& hooks() {
  static std::vector<ShaderHook> h;
  return h;
}

// the hooks' library: their uniforms, the engine's maps and helpers, then the hook files in registration order (the
// surface hooks' programs get the surface hooks' files, the post program those and the post hooks' files)
std::string hookLibraryCode(bool post) {
  std::string s = R"(
uniform float regs_HookOn = 0.0;    // 1 colour pass, 2 shadow-map pass (only the hooks that asked for it)
uniform int regs_SurfaceClass = 8;  // draws that do not set it (particles, effect quads) are overlays
uniform float regs_HookItem = 0.0;
uniform mat4 regs_HookInvViewProj;
uniform vec3 regs_HookEye;
uniform float regs_HookTime;
uniform float regs_HookNight = 0.0;  // the day/night cycle: 0 by day, 1 at night (0 without the cycle)
uniform samplerCube regs_EnvMap;
uniform sampler2D regs_SurfaceMap;
uniform vec4 regs_SurfaceMapRect;
uniform sampler2D regs_HookTexture0;  // texture unit 0: the material's first texture (its diffuse map)
struct RegsSurface { vec3 pos; vec3 normal; vec3 view; float dist; float fog; int surfaceClass; float time; float item;
                     vec2 uv; vec2 uv1; float shadow; };
// the material's first texture (unit 0, the diffuse map of the game's materials) at a texture coordinate, as stored
// (before the shader's colorize and lighting)
vec4 regs_texel0(vec2 uv) { return texture(regs_HookTexture0, uv); }
// the surface class seen from above at a world point (engine/surface_map.h; 0 where nothing walkable was painted,
// -1 outside the map or while there is no map)
int regs_surfaceAt(vec2 xz) {
  vec2 uv = (xz - regs_SurfaceMapRect.xy) * regs_SurfaceMapRect.zw;
  if (regs_SurfaceMapRect.z <= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return -1;
  ivec2 size = textureSize(regs_SurfaceMap, 0);
  return int(texelFetch(regs_SurfaceMap, min(ivec2(uv * vec2(size)), size - 1), 0).r * 255.0 + 0.5) & 127;
}
// the signed distance (m) from a world point to the edge of the carriageway, + on the road, − off it, within about
// ±1.2 m (beyond: ±1.2): from the share of road in a disk of 1.2 m around the point (centre and two rings of 8 taps
// of the map's linearly filtered road mask), mapped back to the distance from a straight edge that gives that share.
// Smooth along the kerb and rounded at corners, without the map's 0.5 m steps.
float regs_roadEdge(vec2 xz) {
  if (regs_SurfaceMapRect.z <= 0.0) return -1.2;
  vec2 uvScale = regs_SurfaceMapRect.zw;
  vec2 uv = (xz - regs_SurfaceMapRect.xy) * uvScale;
  float sum = 2.0 * texture(regs_SurfaceMap, uv).g;
  for (int r = 1; r <= 2; ++r) {
    float rad = 0.6 * float(r);
    for (int k = 0; k < 8; ++k) {
      float a = float(k) * 0.7853982 + float(r) * 0.39;
      sum += texture(regs_SurfaceMap, uv + rad * vec2(cos(a), sin(a)) * uvScale).g;
    }
  }
  float share = sum / 18.0;
  float f = abs(share - 0.5) + 0.5;  // the same curve on both sides
  // share of a straight edge's road side at distance d: 0.5 (0), 0.587 (0.15), 0.665 (0.3), 0.852 (0.6),
  // 0.898 (0.9), 1 (1.2)
  float d = f < 0.587 ? (f - 0.5) / 0.087 * 0.15
          : f < 0.665 ? 0.15 + (f - 0.587) / 0.078 * 0.15
          : f < 0.852 ? 0.3 + (f - 0.665) / 0.187 * 0.3
          : f < 0.898 ? 0.6 + (f - 0.852) / 0.046 * 0.3
          : 0.9 + (f - 0.898) / 0.102 * 0.3;
  return share >= 0.5 ? d : -d;
}
// the top map (engine/surface_map.h): is something overhead? regs_shelterDepth: 0 in the open, else the distance
// (m, up to 1.5) to the nearest open spot at this height (8 rays of 0.35-1.4 m), so a hook can let snow or rain reach
// a little way in under an edge
uniform sampler2D regs_TopMap;
bool regs_coveredAt(vec2 xz, float h) {
  vec2 uv = (xz - regs_SurfaceMapRect.xy) * regs_SurfaceMapRect.zw;
  if (regs_SurfaceMapRect.z <= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return false;
  float v = texture(regs_TopMap, uv).r * 65535.0;
  return v > 0.5 && v / 100.0 - 100.0 > h + 0.8;
}
// how much is overhead at a point, filtered between the top map's 1 m cells (0..1): the cells' yes/no blended
// bilinearly, so the edge of a roof is a smooth line instead of 1 m steps
float regs_coveredSoft(vec2 xz, float h) {
  ivec2 size = textureSize(regs_TopMap, 0);
  vec2 q = (xz - regs_SurfaceMapRect.xy) * regs_SurfaceMapRect.zw * vec2(size) - 0.5;
  ivec2 b = ivec2(floor(q));
  vec2 f = q - vec2(b);
  float v[4];
  for (int k = 0; k < 4; ++k) {
    ivec2 t = b + ivec2(k & 1, k >> 1);
    float top = (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, size))) ? 0.0
              : texelFetch(regs_TopMap, t, 0).r * 65535.0;
    v[k] = (top > 0.5 && top / 100.0 - 100.0 > h + 0.8) ? 1.0 : 0.0;
  }
  return mix(mix(v[0], v[1], f.x), mix(v[2], v[3], f.x), f.y);
}
// the depth under shelter (m, 0 in the open, up to 1.5): the covered share of a disk of 1 m around the point (centre
// and two rings of 8), mapped to the distance from a straight edge that gives that share; smooth and rounded, no
// cell steps. The map is filtered linearly, so four taps tell at once whether anything is overhead nearby.
float regs_shelterDepth(vec3 p) {
  if (regs_SurfaceMapRect.z <= 0.0) return 0.0;
  vec2 uvScale = regs_SurfaceMapRect.zw;
  vec2 uv = (p.xz - regs_SurfaceMapRect.xy) * uvScale;
  float near = texture(regs_TopMap, uv + vec2(1.0, 1.0) * uvScale).r + texture(regs_TopMap, uv + vec2(-1.0, 1.0) * uvScale).r
             + texture(regs_TopMap, uv + vec2(1.0, -1.0) * uvScale).r + texture(regs_TopMap, uv + vec2(-1.0, -1.0) * uvScale).r;
  if (near <= 0.0) return 0.0;
  float sum = 2.0 * regs_coveredSoft(p.xz, p.y), weight = 2.0;
  for (int r = 1; r <= 2; ++r) {
    float rad = 0.5 * float(r);
    for (int k = 0; k < 8; ++k) {
      float a = float(k) * 0.7853982 + float(r) * 0.39;
      sum += regs_coveredSoft(p.xz + rad * vec2(cos(a), sin(a)), p.y);
      weight += 1.0;
    }
  }
  float share = sum / weight;
  return clamp(1.5 * (share - 0.5) + 2.5 * max(share - 0.85, 0.0), 0.0, 1.5);
}
// the traffic maps (engine/traffic_maps.h)
// regs_LaneTraffic, 1024 texels wide: the lanes (values, curves), after them the lists, then the lane lists' cells
// (2 m, two per texel: the list's first float, near + 64 · all)
uniform sampler2D regs_LaneTraffic;
uniform vec4 regs_LaneCells;  // cells across x, z; the cells' first texel
uniform vec4 regs_TrafficRect;
// a lane's data: k = 0 its values (traffic, plowed, heap), 1 and 2 its curve's Bézier control points (p0.xz, b.xz),
// (c.xz, p1.xz)
vec4 regs_laneData(int i, int k) { return texelFetch(regs_LaneTraffic, ivec2(i % 256 + 256 * k, i / 256), 0); }
// the lane list of the 2 m cell around a world point: x = first entry, y = the near ones (that can touch a point in
// the cell), z = all (with the kerb heaps' reach); entries nearest first
ivec3 regs_laneCell(vec2 xz) {
  if (regs_TrafficRect.z <= 0.0) return ivec3(0);
  ivec2 c = ivec2(floor((xz - regs_TrafficRect.xy) / 2.0));
  if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, ivec2(regs_LaneCells.xy)))) return ivec3(0);
  int k = c.y * int(regs_LaneCells.x) + c.x, tx = int(regs_LaneCells.z) + k / 2;
  vec4 t = texelFetch(regs_LaneTraffic, ivec2(tx % 1024, tx / 1024), 0);
  vec2 v = (k & 1) == 0 ? t.xy : t.zw;
  int counts = int(v.y + 0.5);
  return ivec3(int(v.x + 0.5), counts & 63, counts >> 6);
}
// entry e of the lists: a lane number
int regs_laneListEntry(int e) {
  int tx = e / 4, c = e - tx * 4;
  vec4 v = texelFetch(regs_LaneTraffic, ivec2(tx % 1024, tx / 1024), 0);
  return int((c == 0 ? v.x : c == 1 ? v.y : c == 2 ? v.z : v.w) + 0.5);
}
// vector lanes: a point's signed lateral offset (m) from lane i's own curve (the cubic Bézier of the lane piece, not a
// grid), the curve's direction there and how far (m) the point lies beyond the piece's ends (0
// alongside it); the next piece of the lane takes over there with the same line, so tracks run on without steps
float regs_laneCurve(int i, vec2 q, out vec2 dir, out float beyond) {
  vec4 a = regs_laneData(i, 1), b = regs_laneData(i, 2);
  vec2 p0 = a.xy, c1 = a.zw, c2 = b.xy, p1 = b.zw;
  vec2 ch = p1 - p0;
  float len2 = max(dot(ch, ch), 1e-4);
  float t0 = dot(q - p0, ch) / len2;
  float t = clamp(t0, 0.0, 1.0);
  vec2 B = p0, D = ch;
  for (int k = 0; k < 2; ++k) {  // the nearest point on the curve: chord projection, then Newton steps
    float u = 1.0 - t;
    B = p0 * (u * u * u) + c1 * (3.0 * u * u * t) + c2 * (3.0 * u * t * t) + p1 * (t * t * t);
    D = (c1 - p0) * (3.0 * u * u) + (c2 - c1) * (6.0 * u * t) + (p1 - c2) * (3.0 * t * t);
    float dd = dot(D, D);
    if (dd > 1e-6) t = clamp(t + dot(q - B, D) / dd, 0.0, 1.0);
  }
  float u = 1.0 - t;
  B = p0 * (u * u * u) + c1 * (3.0 * u * u * t) + c2 * (3.0 * u * t * t) + p1 * (t * t * t);
  D = (c1 - p0) * (3.0 * u * u) + (c2 - c1) * (6.0 * u * t) + (p1 - c2) * (3.0 * t * t);
  dir = dot(D, D) > 1e-8 ? normalize(D) : normalize(ch);
  beyond = abs(dot(q - B, dir));  // along the end tangent past an end; about 0 alongside the piece
  return dot(q - B, vec2(-dir.y, dir.x));
}
// the road lanes around a world point, by their own curves: the lanes of the point's cell list (every lane that can
// touch it), each with the point's exact lateral offset from its curve, weighted by how far the point lies inside its
// band (1.35 m half width, a soft edge 'edge' m wide) and alongside the piece. Returns their weighted recent traffic;
// onLane: how much the point is on a lane at all (0..1); lateral / dir: the nearest lane's offset (m) and direction
// tracks: the strongest wheel line of all these lanes, two per lane at ±gauge m, exp(−d² / width2) across, scaled by
// smoothstep(0.02, 0.3) of the lane's traffic (where lanes cross, their lines overlap instead of meeting in a corner)
float regs_laneSoftTracks(vec2 xz, float edge, float gauge, float width2, out float onLane, out float tracks,
                          out float lateral, out vec2 dir) {
  tracks = 0.0;
  onLane = 0.0; lateral = 9.0; dir = vec2(1.0, 0.0);
  ivec3 cell = regs_laneCell(xz);
  float sumW = 0.0, sumT = 0.0, best = 99.0;
  for (int k = 0; k < 24; ++k) {
    if (k >= cell.y) break;
    int lane = regs_laneListEntry(cell.x + k);
    vec2 d;
    float beyond;
    float lat = regs_laneCurve(lane, xz, d, beyond);
    float along = 1.0 - smoothstep(0.0, 1.5, beyond);
    float w = (1.0 - smoothstep(1.35 - 0.5 * edge, 1.35 + 0.5 * edge, abs(lat))) * along;
    float score = abs(lat) + 4.0 * beyond;  // the nearest lane alongside the point
    float wl = abs(abs(lat) - gauge);
    float tr = regs_laneData(lane, 0).r;
    tracks = max(tracks, exp(-wl * wl / width2) * smoothstep(0.02, 0.3, tr) * along * step(abs(lat), 1.6));
    if (score < best) { best = score; lateral = lat; dir = d; }
    if (w > 0.0) {
      sumT += w * regs_laneData(lane, 0).r;
      sumW += w;
      onLane = max(onLane, w);
    }
  }
  return sumW > 0.0 ? sumT / sumW : 0.0;
}
float regs_laneSoft(vec2 xz, float edge, out float onLane, out float lateral, out vec2 dir) {  float tracks;  return regs_laneSoftTracks(xz, edge, 0.8, 0.04, onLane, tracks, lateral, dir);}
// the road lane under a world point: false when none; else its recent traffic (0..1), the point's lateral offset
// from the lane's centre line (m) and the lane's direction (unit, x/z)
bool regs_laneAt(vec2 xz, out float traffic, out float lateral, out vec2 dir) {
  float onLane;
  traffic = regs_laneSoft(xz, 0.0, onLane, lateral, dir);
  return onLane > 0.5;
}
// the recent traffic around a world point, soft across lane borders and the road's edge (0 off the lanes)
float regs_laneTrafficSmooth(vec2 xz) {
  float onLane, lateral;
  vec2 dir;
  float traffic = regs_laneSoft(xz, 1.0, onLane, lateral, dir);
  return traffic * onLane;
}
// the snowplows (engine/snowplows.h) on the lanes around a point: returns how freshly the lane under it was cleared
// (0..1, soft at the lane's edge; the most plowed of the lanes there) and gives the heap the plows pushed out of the
// nearest lanes (0..1), reaching `reach` m (at most 2.5) beyond a lane's 1.35 m half width (onto the gutter and the kerb)
float regs_lanePlowed(vec2 xz, float reach, out float heap) {
  heap = 0.0;
  ivec3 cell = regs_laneCell(xz);
  float plowed = 0.0;
  for (int k = 0; k < 40; ++k) {
    if (k >= cell.z) break;
    int lane = regs_laneListEntry(cell.x + k);
    vec4 v = regs_laneData(lane, 0);
    if (v.g <= 0.0 && v.b <= 0.0) continue;
    vec2 d;
    float beyond;
    float lat = abs(regs_laneCurve(lane, xz, d, beyond));
    float along = 1.0 - smoothstep(0.0, 1.5, beyond);
    plowed = max(plowed, (1.0 - smoothstep(1.05, 1.65, lat)) * along * v.g);
    heap = max(heap, v.b * (1.0 - smoothstep(1.35 + 0.6 * reach, 1.35 + reach, lat)) * along);
  }
  return plowed;
}
// the footprint map (engine/traffic_maps.h): single steps of the walkers near the camera, 0..1 (several steps on the
// same spot add up), 5 cm texels in a tile atlas; 0 where nobody stepped or while the map is off (gsGrid.regsFootprints)
// one texture: the tile atlas (32-texel tiles, 64 per row: 2048² at the top), below it the tile index (two texels per
// 1.6 m tile: slot + 1, low and high byte; 0 where nobody stepped)
uniform sampler2D regs_FootAtlas;
uniform vec4 regs_FootRect;  // origin x, z, texel size (m), atlas tiles per row
float regs_footTexel(ivec2 g) {
  ivec2 size = textureSize(regs_FootAtlas, 0);
  ivec2 tile = g / 32;
  if (any(lessThan(g, ivec2(0))) || tile.x >= size.x / 2 || tile.y >= size.y - 2048) return 0.0;
  ivec2 at = ivec2(tile.x * 2, 2048 + tile.y);
  int slot = int(texelFetch(regs_FootAtlas, at, 0).r * 255.0 + 0.5) +
             256 * int(texelFetch(regs_FootAtlas, at + ivec2(1, 0), 0).r * 255.0 + 0.5);
  if (slot == 0) return 0.0;
  slot -= 1;
  int row = int(regs_FootRect.w);
  return texelFetch(regs_FootAtlas, ivec2(slot % row, slot / row) * 32 + (g - tile * 32), 0).r;
}
float regs_footprintAt(vec2 xz) {
  if (regs_FootRect.z <= 0.0) return 0.0;
  vec2 p = (xz - regs_FootRect.xy) / regs_FootRect.z - 0.5;
  ivec2 b = ivec2(floor(p));
  vec2 f = p - vec2(b);
  return mix(mix(regs_footTexel(b), regs_footTexel(b + ivec2(1, 0)), f.x),
             mix(regs_footTexel(b + ivec2(0, 1)), regs_footTexel(b + ivec2(1, 1)), f.x), f.y);
}
// how trodden the ground is at a world point (0..1, recent pedestrians)
float regs_troddenAt(vec2 xz) {
  if (regs_SurfaceMapRect.z <= 0.0) return 0.0;
  return texture(regs_TopMap, (xz - regs_SurfaceMapRect.xy) * regs_SurfaceMapRect.zw).g;  // the top map's second channel
}
)";
  std::set<std::string> files;  // a file several hooks name is included once
  for (int pass = 0; pass < (post ? 2 : 1); ++pass)
    for (const ShaderHook& h : hooks())
      if (h.post == (pass == 1) && files.insert(h.origin).second) s += "// hook file " + h.origin + "\n" + h.code + "\n";
  return s;
}

// the fragment side of the surface hooks: the world position from the interpolated clip position (any vertex
// transform: terrain, merged items, skinning), the texture coordinates, the surface record, then each hook in
// registration order; in shadow-map passes only the hooks that asked for them ("shadowPass"), for their alpha
std::string hookFragmentCode() {
  std::string s = "\nin vec4 regs_HookClip;\nin vec4 regs_HookUV;\n" + hookLibraryCode(false);
  s += R"(void regs_hooks(inout vec4 c) {
  if (regs_SurfaceClass == 8) return;  // kSurfaceOverlay: route strips, highlights, previews, markers, sky, people
  vec4 w = regs_HookInvViewProj * regs_HookClip;
  RegsSurface s;
  s.pos = w.xyz / w.w;
  vec3 e = s.pos - regs_HookEye;
  s.dist = length(e);
  s.view = e / max(s.dist, 1e-4);
  vec3 n = normalize(cross(dFdx(s.pos), dFdy(s.pos)));
  s.normal = dot(n, s.view) > 0.0 ? -n : n;
  s.fog = clamp((s.dist - regs_Fog.start) * regs_Fog.scale, 0.0, 1.0);
  s.surfaceClass = regs_SurfaceClass;
  s.time = regs_HookTime;
  s.item = regs_HookItem;
  s.uv = regs_HookUV.xy;
  s.uv1 = regs_HookUV.zw;
  s.shadow = regs_HookOn > 1.5 ? 1.0 : 0.0;
  if (s.shadow > 0.5) {
)";
  for (const ShaderHook& h : hooks())
    if (h.shadow && !h.post) s += "    " + h.function + "(c, s);\n";
  s += "    return;\n  }\n";
  for (const ShaderHook& h : hooks())
    if (!h.post) s += "  " + h.function + "(c, s);\n";
  s += "}\n";
  return s;
}

}  // namespace

void addShaderHook(const ShaderHook& h) { hooks().push_back(h); }
const std::vector<ShaderHook>& shaderHooks() { return hooks(); }
std::vector<ShaderHookTexture>& shaderHookTextures() {
  static std::vector<ShaderHookTexture> t;
  return t;
}
std::map<std::string, std::array<float, 4>>& shaderHookUniforms() {
  static std::map<std::string, std::array<float, 4>> u;
  return u;
}

namespace {

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// GLSL 1.10 → 3.30 renames in one pass over the identifiers (whole words, as \b...\b did; comments too). A "call"
// entry only renames when the next non-blank character is '('; "main" only after "void". Returns the first game
// built-in (gl_...) left that GLSL 3.30 core lacks in *unsupported, or "" when all were translated. std::regex was
// slow here: 13 passes over ~160 sources made the first game frame wait over a second.
std::string renameIdentifiers(const std::string& src, bool fragment, const char* mainName, std::string* unsupported) {
  struct Rule {
    const char* from;
    const char* to;
    bool call;
  };
  static const std::vector<Rule> common = {
      {"texture2D", "texture", true}, {"textureCube", "texture", true}, {"shadow2D", "regs_shadow2D", true},
      // smoothstep(0.8, 0.0, x) (terrain maps) has edge0 > edge1, undefined in GLSL; the old drivers used the formula
      {"smoothstep", "regs_smoothstep", true},
      {"gl_ModelViewProjectionMatrix", "regs_ModelViewProjectionMatrix", false},
      {"gl_ModelViewMatrix", "regs_ModelViewMatrix", false}, {"gl_ProjectionMatrix", "regs_ProjectionMatrix", false},
      {"gl_NormalMatrix", "regs_NormalMatrix", false}, {"gl_FrontMaterial", "regs_FrontMaterial", false},
      {"gl_LightSource", "regs_LightSource", false}, {"gl_FrontLightProduct", "regs_FrontLightProduct", false},
      {"gl_LightModel", "regs_LightModel", false}, {"gl_Fog", "regs_Fog", false}};
  static const std::vector<Rule> vertex = {
      {"attribute", "in", false}, {"varying", "out", false}, {"gl_Vertex", "regs_Vertex", false},
      {"gl_Normal", "regs_Normal", false}, {"gl_Color", "regs_Color", false}, {"gl_FrontColor", "regs_FrontColor", false},
      {"gl_MultiTexCoord0", "regs_MultiTexCoord0", false}, {"gl_MultiTexCoord1", "regs_MultiTexCoord1", false},
      {"gl_MultiTexCoord2", "regs_MultiTexCoord2", false}, {"gl_MultiTexCoord3", "regs_MultiTexCoord3", false},
      {"gl_MultiTexCoord4", "regs_MultiTexCoord4", false}, {"gl_MultiTexCoord5", "regs_MultiTexCoord5", false},
      {"gl_MultiTexCoord6", "regs_MultiTexCoord6", false}, {"gl_MultiTexCoord7", "regs_MultiTexCoord7", false}};
  static const std::vector<Rule> frag = {
      {"varying", "in", false}, {"gl_Color", "regs_FrontColor", false}, {"gl_FragColor", "regs_FragColor", false}};
  static const char* const allowed[] = {"gl_Position", "gl_FragCoord", "gl_FragDepth", "gl_PointSize", "gl_FrontFacing"};
  std::string out;
  out.reserve(src.size() + 256);
  size_t i = 0, n = src.size();
  // the #version line (the first one) goes: the translated source gets its own
  {
    size_t k = 0;
    while (k < n && std::isspace(static_cast<unsigned char>(src[k]))) ++k;
    if (src.compare(k, 8, "#version") == 0) {
      size_t e = src.find('\n', k);
      i = e == std::string::npos ? n : e + 1;
    }
  }
  std::string lastWord;
  while (i < n) {
    char c = src[i];
    if (!identChar(c) || (std::isdigit(static_cast<unsigned char>(c)) && i > 0 && identChar(src[i - 1]))) {
      out += c;
      ++i;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {  // a number (1.0e5, 2u): copied as it is
      while (i < n && (identChar(src[i]) || src[i] == '.')) out += src[i++];
      continue;
    }
    size_t b = i;
    while (i < n && identChar(src[i])) ++i;
    std::string w = src.substr(b, i - b);
    size_t next = i;
    while (next < n && (src[next] == ' ' || src[next] == '\t' || src[next] == '\r' || src[next] == '\n')) ++next;
    const bool paren = next < n && src[next] == '(';
    const char* to = nullptr;
    for (const std::vector<Rule>* table : {&common, fragment ? &frag : &vertex})
      for (const Rule& r : *table)
        if (!to && w == r.from && (!r.call || paren)) to = r.to;
    if (!to && w == "main" && paren && lastWord == "void") to = mainName;
    std::string result = to ? std::string(to) : w;
    if (result.compare(0, 3, "gl_") == 0) {
      bool ok = false;
      for (const char* a : allowed) ok = ok || result == a;
      if (!ok) {
        if (unsupported) *unsupported = result;
        return "";
      }
    }
    out += result;
    lastWord = w;
  }
  return out;
}

}  // namespace

// whole-word replacement (the reflection hook: textureCube → regs_reflectCube before the GLSL 1.10 renames)
static std::string replaceWord(const std::string& s, const std::string& from, const std::string& to) {
  std::string out;
  out.reserve(s.size() + 64);
  size_t i = 0;
  while (i < s.size()) {
    size_t at = s.find(from, i);
    if (at == std::string::npos) break;
    bool before = at > 0 && identChar(s[at - 1]), after = at + from.size() < s.size() && identChar(s[at + from.size()]);
    out.append(s, i, at - i);
    out += before || after ? from : to;
    i = at + from.size();
  }
  out.append(s, i, std::string::npos);
  return out;
}

static std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
  return s;
}

std::string translateGlsl110(const std::string& src, bool fragment, std::string* unsupported, bool withHooks,
                             unsigned rayBits) {
  withHooks = withHooks && !hooks().empty();
  const bool rayShadows = (rayBits & kRayShadows) != 0;
  const bool hookEnv = withHooks && hooksReadEnvMap();
  const bool reflections = fragment && (rayBits & kRayReflections) && (src.find("textureCube") != std::string::npos || hookEnv);
  std::string why;
  std::string s = renameIdentifiers(reflections ? replaceWord(src, "textureCube", "regs_reflectCube") : src, fragment,
                                    fragment ? "regs_main" : "regs_vmain", &why);
  if (s.empty() && !why.empty()) {
    if (unsupported) *unsupported = why;
    return "";
  }
  std::string head = "#version 330 core\n";
  const bool rays = rayShadows && fragment;
  if (rays || reflections) head += "#extension GL_EXT_ray_query : require\n";
  if (reflections)
    head += "#extension GL_EXT_buffer_reference : require\n#extension GL_EXT_buffer_reference_uvec2 : require\n"
            "#extension GL_EXT_nonuniform_qualifier : require\n";
  // REGS_SHADER_SALT=<text>: a comment that makes every source new to the driver's shader cache (cold-start tests)
  static const char* salt = std::getenv("REGS_SHADER_SALT");
  if (salt) head += std::string("// ") + salt + "\n";
  head += kBuiltins;
  // shadow2D as GLSL 1.10 has it (vec4); the particle shader samples the shadow map in its vertex shader too
  if (!rays) head += "vec4 regs_shadow2D(sampler2DShadow s, vec3 c) { return vec4(texture(s, c)); }\n";
  if (!fragment) {
    head += "in vec4 regs_Vertex;\nin vec3 regs_Normal;\nin vec4 regs_Color;\n";
    for (int i = 0; i < 8; ++i) head += "in vec4 regs_MultiTexCoord" + std::to_string(i) + ";\n";
    head += "out vec4 regs_FrontColor;\n";
    // port night lights (docs/renderer/night-lights.md): every vertex stage passes the world position (from clip
    // space, so skinned and altered vertices are right too) and the vertex colour alpha (merged buildings: a seed);
    // with the shader hooks also the clip position for the hooks' world position and the mesh's two texture coordinates
    head += "uniform mat4 regs_ViewInverse;\nuniform vec4 regs_ProjInverse;\nout vec3 regs_WorldPos;\nout float regs_Seed;\nout vec3 regs_ModelPos;\n";
    if (withHooks) head += "out vec4 regs_HookClip;\nout vec4 regs_HookUV;\n";
    s += "\nvoid main() {\n  regs_vmain();\n";
    if (withHooks)
      s += "  regs_HookClip = gl_Position;\n  regs_HookUV = vec4(regs_MultiTexCoord0.xy, regs_MultiTexCoord1.xy);\n";
    s += "  float ze = -gl_Position.w;\n"
         "  vec3 e = vec3((gl_Position.x - regs_ProjInverse.z * ze) * regs_ProjInverse.x,\n"
         "                (gl_Position.y - regs_ProjInverse.w * ze) * regs_ProjInverse.y, ze);\n"
         "  regs_WorldPos = (regs_ViewInverse * vec4(e, 1.0)).xyz;\n  regs_Seed = regs_Color.a;\n  regs_ModelPos = regs_Vertex.xyz;\n}\n";
  } else {
    head += "in vec4 regs_FrontColor;\nout vec4 regs_FragColor;\nin vec3 regs_WorldPos;\nin float regs_Seed;\nin vec3 regs_ModelPos;\n";
    if (rays || reflections) head += "uniform accelerationStructureEXT regs_Scene;\nuniform vec4 regs_SunPos;\n";
    if (reflections) {
      // REGS_RT_REFL_DEBUG=1|2|3 (a check): a constant, green hits, the hit record's colour
      static const char* dbg = std::getenv("REGS_RT_REFL_DEBUG");
      head += std::string("#define REGS_REFL_DEBUG ") + (dbg ? dbg : "0") + "\n";
      head += kReflectionCode;
    }
    if (rays)  // a ray from the surface toward the sun: 1 lit, 0 in shadow; the shadow map (foliage) multiplies it
      head +=
          "float regs_sunVisible() {\n"
          "  if (regs_SunPos.w < 0.5) return 1.0;\n"
          "  vec3 p = regs_WorldPos;\n"
          "  vec3 l = normalize(regs_SunPos.xyz - p);\n"
          "  vec3 n = cross(dFdx(p), dFdy(p));\n"
          "  n = dot(n, n) > 1e-12 ? normalize(n) : l;\n"
          "  if (dot(n, l) < 0.0) n = -n;\n"
          "  vec3 o = p + n * 0.05 + l * 0.02;\n"
          "  rayQueryEXT q;\n"
          "  rayQueryInitializeEXT(q, regs_Scene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, o, 0.0, l, 4000.0);\n"
          "  while (rayQueryProceedEXT(q)) {}\n"
          "  return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;\n"
          "}\n"
          "vec4 regs_shadow2D(sampler2DShadow s, vec3 c) { return vec4(texture(s, c) * regs_sunVisible()); }\n";
    // the fixed-function alpha test (glAlphaFunc GREATER ref) runs after the shader's own main
    if (withHooks)
      s += (reflections && hookEnv ? replaceAll(hookFragmentCode(), "texture(regs_EnvMap,", "regs_reflectCube(regs_EnvMap,")
                                   : hookFragmentCode()) +
           "\nuniform float regs_AlphaRef = -1.0;\nvoid main() {\n  regs_main();\n  if (regs_HookOn > 0.5) regs_hooks(regs_FragColor);\n"
           "  if (regs_FragColor.a <= regs_AlphaRef) discard;\n}\n";
    else
      s += "\nuniform float regs_AlphaRef = -1.0;\nvoid main() {\n  regs_main();\n  if (regs_FragColor.a <= regs_AlphaRef) discard;\n}\n";
  }
  return head + s;
}

bool postHooksRegistered() {
  for (const ShaderHook& h : hooks())
    if (h.post) return true;
  return false;
}

bool hooksReadEnvMap() {
  for (const ShaderHook& h : hooks())
    if (!h.post && h.code.find("regs_EnvMap") != std::string::npos) return true;
  return false;
}

bool shadowHooksRegistered() {
  for (const ShaderHook& h : hooks())
    if (h.shadow && !h.post) return true;
  return false;
}

std::string postHookFragment() {
  std::string s = "#version 330 core\n";
  s += kBuiltins;
  s += R"(in vec4 texCoord;
out vec4 regs_FragColor;
uniform sampler2D texture0;  // the scene's colour
uniform sampler2D texture1;  // and depth
uniform vec4 custom0;        // texel size x, y
uniform mat4 regs_PostInvViewProj;
struct RegsPost { vec2 uv; vec3 pos; vec3 view; float dist; float depth; float sky; float time; vec2 texel; };
// the scene's colour, depth (0..1, 1 = nothing drawn) and world position at an image coordinate (0..1, bottom left)
vec3 regs_postColor(vec2 uv) { return texture(texture0, uv).rgb; }
float regs_postDepth(vec2 uv) { return texture(texture1, uv).r; }
vec3 regs_postWorld(vec2 uv) {
  vec4 w = regs_PostInvViewProj * vec4(uv * 2.0 - 1.0, texture(texture1, uv).r * 2.0 - 1.0, 1.0);
  return w.xyz / w.w;
}
)";
  s += hookLibraryCode(true);
  s += R"(void main() {
  RegsPost p;
  p.uv = texCoord.xy;
  p.depth = regs_postDepth(p.uv);
  p.sky = p.depth >= 0.99999 ? 1.0 : 0.0;
  p.pos = regs_postWorld(p.uv);
  vec3 e = p.pos - regs_HookEye;
  p.dist = length(e);
  p.view = e / max(p.dist, 1e-4);
  p.time = regs_HookTime;
  p.texel = custom0.xy;
  vec3 c = regs_postColor(p.uv);
)";
  for (const ShaderHook& h : hooks())
    if (h.post) s += "  " + h.function + "(c, p);\n";
  s += "  regs_FragColor = vec4(c, 1.0);\n}\n";
  return s;
}

}  // namespace regs::render
