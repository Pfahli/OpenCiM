#include "render/glsl.h"

#include <regex>

namespace regs::render {

namespace {

// Fixed-function state of GLSL 1.10, re-declared as ordinary uniforms with the same layout and member names.
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

// the fragment side of the hooks: the world position from the interpolated clip position (any vertex transform:
// terrain, merged items, skinning), the surface record, then each hook in registration order
std::string hookFragmentCode() {
  std::string s = R"(
in vec4 regs_HookClip;
uniform float regs_HookOn = 0.0;
uniform int regs_SurfaceClass = 8;  // draws that do not set it (particles, effect quads) are overlays
uniform float regs_HookItem = 0.0;
uniform mat4 regs_HookInvViewProj;
uniform vec3 regs_HookEye;
uniform float regs_HookTime;
uniform samplerCube regs_EnvMap;
uniform sampler2D regs_SurfaceMap;
uniform vec4 regs_SurfaceMapRect;
struct RegsSurface { vec3 pos; vec3 normal; vec3 view; float dist; float fog; int surfaceClass; float time; float item; };
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
uniform sampler2D regs_LaneMap;
uniform sampler2D regs_LaneTraffic;
uniform sampler2D regs_Trodden;
uniform vec4 regs_TrafficRect;
// a lane map texel: the lane number + 1 (18 bits: R, G and the top 2 bits of B; 0 = none) and the texel centre's
// stored lateral offset from the lane's centre line (m, the low 6 bits of B in 0.08 m steps)
int regs_laneIndex(vec4 l) {
  return int(l.r * 255.0 + 0.5) + 256 * int(l.g * 255.0 + 0.5) + 65536 * (int(l.b * 255.0 + 0.5) >> 6);
}
float regs_laneStoredLat(vec4 l) { return float((int(l.b * 255.0 + 0.5) & 63) - 32) * 0.08; }
// a lane's data in regs_LaneTraffic (RGBA32F, 768 × rows; engine/traffic_maps.h): k = 0 its values (traffic, plowed,
// heap), 1 and 2 its curve's Bézier control points (p0.xz, b.xz), (c.xz, p1.xz)
vec4 regs_laneData(int i, int k) { return texelFetch(regs_LaneTraffic, ivec2(i % 256 + 256 * k, i / 256), 0); }
// vector lanes: a point's signed lateral offset (m) from lane i's own curve (the cubic Bézier of the lane piece, not
// the lane map's 1 m texels), the curve's direction there and how far (m) the point lies beyond the piece's ends (0
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
// the distinct lanes of the (2r+1)² lane map texels around a point (the lane map only says which lanes are near; the
// lines come from the lanes' own curves): fills lanes[], returns how many (at most 25)
int regs_lanesAround(vec2 xz, int r, out int lanes[25]) {
  int n = 0;
  for (int k = 0; k < 25; ++k) lanes[k] = -1;
  ivec2 size = textureSize(regs_LaneMap, 0);
  ivec2 c = ivec2(floor((xz - regs_TrafficRect.xy) * regs_TrafficRect.zw * vec2(size)));
  int w = 2 * r + 1;
  for (int k = 0; k < w * w; ++k) {
    ivec2 t = c + ivec2(k % w - r, k / w - r);
    if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, size))) continue;
    int idx = regs_laneIndex(texelFetch(regs_LaneMap, t, 0)) - 1;
    if (idx < 0) continue;
    bool dup = false;
    for (int j = 0; j < 25; ++j)
      if (j < n && lanes[j] == idx) dup = true;
    if (dup || n >= 25) continue;
    lanes[n] = idx;
    ++n;
  }
  return n;
}
// the road lanes around a world point, by their own curves: the distinct lanes of the 3×3 lane map texels around it,
// each with the point's exact lateral offset from its curve, weighted by how far the point lies inside its band
// (1.35 m half width, a soft edge 'edge' m wide) and alongside the piece. Returns their weighted recent traffic;
// onLane: how much the point is on a lane at all (0..1); lateral / dir: the nearest lane's offset (m) and direction
float regs_laneSoft(vec2 xz, float edge, out float onLane, out float lateral, out vec2 dir) {
  onLane = 0.0; lateral = 9.0; dir = vec2(1.0, 0.0);
  if (regs_TrafficRect.z <= 0.0) return 0.0;
  int lanes[25];
  int n = regs_lanesAround(xz, 1, lanes);
  float sumW = 0.0, sumT = 0.0, best = 99.0;
  for (int k = 0; k < 9; ++k) {
    if (k >= n) break;
    vec2 d;
    float beyond;
    float lat = regs_laneCurve(lanes[k], xz, d, beyond);
    float along = 1.0 - smoothstep(0.0, 0.75, beyond);
    float w = (1.0 - smoothstep(1.35 - 0.5 * edge, 1.35 + 0.5 * edge, abs(lat))) * along;
    float score = abs(lat) + 4.0 * beyond;  // the nearest lane alongside the point
    if (score < best) { best = score; lateral = lat; dir = d; }
    if (w > 0.0) {
      sumT += w * regs_laneData(lanes[k], 0).r;
      sumW += w;
      onLane = max(onLane, w);
    }
  }
  return sumW > 0.0 ? sumT / sumW : 0.0;
}
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
// nearest lanes (0..1), reaching `reach` m beyond a lane's 1.35 m half width (onto the gutter and the kerb next to it)
float regs_lanePlowed(vec2 xz, float reach, out float heap) {
  heap = 0.0;
  if (regs_TrafficRect.z <= 0.0) return 0.0;
  int lanes[25];
  int n = regs_lanesAround(xz, 2, lanes);
  float plowed = 0.0;
  for (int k = 0; k < 25; ++k) {
    if (k >= n) break;
    vec2 d;
    float beyond;
    float lat = abs(regs_laneCurve(lanes[k], xz, d, beyond));
    float along = 1.0 - smoothstep(0.0, 0.75, beyond);
    vec4 v = regs_laneData(lanes[k], 0);
    plowed = max(plowed, (1.0 - smoothstep(1.05, 1.65, lat)) * along * v.g);
    heap = max(heap, v.b * (1.0 - smoothstep(1.35 + 0.6 * reach, 1.35 + reach, lat)) * along);
  }
  return plowed;
}
// the footprint map (engine/traffic_maps.h): single steps of the walkers near the camera, 0..1 (several steps on the
// same spot add up), 5 cm texels in a tile atlas; 0 where nobody stepped or while the map is off (gsGrid.regsFootprints)
uniform sampler2D regs_FootIndex;
uniform sampler2D regs_FootAtlas;
uniform vec4 regs_FootRect;  // origin x, z, texel size (m), atlas tiles per row
float regs_footTexel(ivec2 g) {
  ivec2 tiles = textureSize(regs_FootIndex, 0);
  ivec2 tile = g / 32;
  if (any(lessThan(g, ivec2(0))) || any(greaterThanEqual(tile, tiles))) return 0.0;
  int slot = int(texelFetch(regs_FootIndex, tile, 0).r * 65535.0 + 0.5);
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
  if (regs_TrafficRect.z <= 0.0) return 0.0;
  return texture(regs_Trodden, (xz - regs_TrafficRect.xy) * regs_TrafficRect.zw).r;
}
)";
  for (const ShaderHook& h : hooks()) s += "// hook " + h.function + " (" + h.origin + ")\n" + h.code + "\n";
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
)";
  for (const ShaderHook& h : hooks()) s += "  " + h.function + "(c, s);\n";
  s += "}\n";
  return s;
}

}  // namespace

void addShaderHook(const ShaderHook& h) { hooks().push_back(h); }
const std::vector<ShaderHook>& shaderHooks() { return hooks(); }
std::map<std::string, std::array<float, 4>>& shaderHookUniforms() {
  static std::map<std::string, std::array<float, 4>> u;
  return u;
}

std::string translateGlsl110(const std::string& src, bool fragment, std::string* unsupported, bool withHooks) {
  withHooks = withHooks && !hooks().empty();
  std::string s = src;
  auto sub = [&](const char* re, const char* to) { s = std::regex_replace(s, std::regex(re), to); };
  sub(R"(^\s*#version[^\n]*\n)", "");
  sub(R"(\btexture2D\s*\()", "texture(");
  sub(R"(\btextureCube\s*\()", "texture(");
  sub(R"(\bshadow2D\s*\()", "regs_shadow2D(");
  // smoothstep(0.8, 0.0, x) (terrain maps) has edge0 > edge1, undefined in GLSL; the old drivers used the formula
  sub(R"(\bsmoothstep\s*\()", "regs_smoothstep(");
  sub(R"(\bgl_(ModelViewProjectionMatrix|ModelViewMatrix|ProjectionMatrix|NormalMatrix)\b)", "regs_$1");
  sub(R"(\bgl_(FrontMaterial|LightSource|FrontLightProduct|LightModel|Fog)\b)", "regs_$1");
  std::string head = "#version 330 core\n";
  head += kBuiltins;
  // shadow2D as GLSL 1.10 has it (vec4); the particle shader samples the shadow map in its vertex shader too
  head += "vec4 regs_shadow2D(sampler2DShadow s, vec3 c) { return vec4(texture(s, c)); }\n";
  if (!fragment) {
    sub(R"(\battribute\b)", "in");
    sub(R"(\bvarying\b)", "out");
    sub(R"(\bgl_(Vertex|Normal|Color|MultiTexCoord[0-7])\b)", "regs_$1");
    sub(R"(\bgl_FrontColor\b)", "regs_FrontColor");
    head += "in vec4 regs_Vertex;\nin vec3 regs_Normal;\nin vec4 regs_Color;\n";
    for (int i = 0; i < 8; ++i) head += "in vec4 regs_MultiTexCoord" + std::to_string(i) + ";\n";
    head += "out vec4 regs_FrontColor;\n";
    if (withHooks) {  // the clip position for the hooks' world position
      sub(R"(\bvoid\s+main\s*\()", "void regs_vmain(");
      head += "out vec4 regs_HookClip;\n";
      s += "\nvoid main() {\n  regs_vmain();\n  regs_HookClip = gl_Position;\n}\n";
    }
  } else {
    sub(R"(\bvarying\b)", "in");
    sub(R"(\bgl_Color\b)", "regs_FrontColor");
    sub(R"(\bgl_FragColor\b)", "regs_FragColor");
    head += "in vec4 regs_FrontColor;\nout vec4 regs_FragColor;\n";
    // the fixed-function alpha test (glAlphaFunc GREATER ref) runs after the shader's own main
    sub(R"(\bvoid\s+main\s*\()", "void regs_main(");
    if (withHooks)
      s += hookFragmentCode() +
           "\nuniform float regs_AlphaRef = -1.0;\nvoid main() {\n  regs_main();\n  if (regs_HookOn > 0.5) regs_hooks(regs_FragColor);\n"
           "  if (regs_FragColor.a <= regs_AlphaRef) discard;\n}\n";
    else
      s += "\nuniform float regs_AlphaRef = -1.0;\nvoid main() {\n  regs_main();\n  if (regs_FragColor.a <= regs_AlphaRef) discard;\n}\n";
  }
  std::regex builtin(R"(\bgl_[A-Za-z0-9_]+)");
  for (auto it = std::sregex_iterator(s.begin(), s.end(), builtin); it != std::sregex_iterator(); ++it) {
    const std::string n = it->str();
    if (n == "gl_Position" || n == "gl_FragCoord" || n == "gl_FragDepth" || n == "gl_PointSize" || n == "gl_FrontFacing")
      continue;
    if (unsupported) *unsupported = n;
    return "";
  }
  return head + s;
}

}  // namespace regs::render
