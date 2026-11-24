// The shader combiner (gsShaders_OGL / gsShaderCombiner_OGL / gsTemplate): .shader scripts choose components in
// onProgram, and each component is a template whose '#' lines are GSystem script (docs/renderer/scene-basics.md §6.1,
// interfaces-3d.md §2.12). Templates run through the port's own VM, so every shipped shader variant is generated the
// way the engine generates it.
#include "engine/shaders.h"

#include <cctype>
#include <memory>
#include <set>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

namespace {

std::unordered_map<std::u16string, std::unique_ptr<ShaderCombiner>>& cache() {
  static std::unordered_map<std::u16string, std::unique_ptr<ShaderCombiner>> c;
  return c;
}

std::unordered_map<const Object*, ShaderCombiner*>& byScript() {
  static std::unordered_map<const Object*, ShaderCombiner*> m;
  return m;
}

// output target of the template being run
struct Expansion {
  std::u16string text;
  std::set<std::u16string>* once = nullptr;  // one set per program stage
};
Expansion* g_expansion = nullptr;

int32_t intField(VM& vm, Object* o, const char16_t* name) {
  try {
    return vm.toInt(vm.resolve(vm.getMember(Value::heap(VT::Object, o), name)));
  } catch (const ScriptError&) {
    return 0;
  }
}

Value field(VM& vm, Object* o, const char16_t* name) {
  try {
    return vm.resolve(vm.getMember(Value::heap(VT::Object, o), name));
  } catch (const ScriptError&) {
    return Value();
  }
}

Value copyComponents(const Value& v) {
  // a derived combiner starts from copies of its base's component maps
  if (MapObj* m = v.map()) {
    auto c = make<MapObj>();
    c->entries = m->entries;
    return Value::heap(VT::Map, c.get());
  }
  return v;
}

// gsShaderCombiner_OGL variant setup (FUN_10036b80): the base's onProgram first, then inherit its results
void runProgram(VM& vm, ShaderCombiner* c, uint32_t mat, uint32_t mesh, uint32_t env) {
  Object* s = c->script.get();
  if (c->base) {
    runProgram(vm, c->base, mat, mesh, env);
    Object* b = c->base->script.get();
    for (const char16_t* f : {u"attributes", u"textures", u"parameters"}) s->fieldValues[f] = Value::integer(intField(vm, b, f));
    for (const char16_t* f : {u"vertexComponent", u"fragmentComponent"}) s->fieldValues[f] = copyComponents(field(vm, b, f));
  }
  vm.fireEvent(s, u"onProgram", {Value::integer(int32_t(mat)), Value::integer(int32_t(mesh)), Value::integer(int32_t(env))});
}

// Port adjustments to shipped shader components, applied in memory (the game files stay untouched). The trees' wind
// (metro/shaders/trees/alter.vertex, grid-items.md §3.6) scales the sway with the square of the vertex height above
// the model's origin. The engine draws trees as instances, so pos is model space there; the port merges them into
// division meshes (grid_items.cpp), where pos is division space and the terrain height would join the square. The
// merged tree vertices carry their base height in uv1.x, as the engine's own merged trees do (§3.2.3), and the
// subtraction the shipped line has commented out is switched on.
//
// Night lights (docs/renderer/night-lights.md, engine/night_lights.cpp): the default shader's maps component adds the
// lit windows of building materials (their window-group mask on unit 16, the building's seed in regs_Seed) and its
// colour component adds the street-lamp light (the lamp map on unit 17, sampled at the world position) times the
// surface colour, plus the window glow, before fog. Both are zero by day and without the day/night cycle, and
// shadow passes and additive materials leave them out.
namespace {

const char16_t* kNightMapsDecl =
    u"#$regsNight = $diffusemap && !bool($envFlags & (ENVIROMENT_SHADOWMAP_RENDER | ENVIROMENT_SHADOW_RENDER)) && "
    u"!bool($matFlags & (MATERIAL_ADDITIVE | MATERIAL_ADDITIVE_A));\n"
    u"#once# vec3 regs_windowGlow = vec3(0.0);\n"
    u"#if ($regsNight) {\n"
    u"#once# uniform vec4 regs_Night;\n"
    u"uniform sampler2D regs_WindowMask;\n"
    u"uniform float regs_WindowOn;\n"
    u"uniform float regs_Shop;\n"
    u"vec4 regs_hash4(float s, float k) {\n"
    u"  return fract(sin(vec4(s * 12.9898 + k * 4.1, s * 78.233 + k * 1.3, s * 37.719 + k * 2.9, s * 93.989 + k * 0.7)) * 43758.5453);\n"
    u"}\n"
    u"uniform vec4 regs_Cabin;\n"
    // the passenger atlas (engine/passenger_atlas.h): the game's citizen models from 8 angles; info = rows (models),
    // columns (angles), cell height and width in model units; rows 0 = no atlas (the plain figures below)
    u"uniform sampler2D regs_PassengerAtlas;\n"
    u"uniform vec4 regs_PassengerInfo;\n"
    u"uniform vec4 regs_PassengerAnim;\n"  // x: the effect timer (s)
    // one figure: u along the car from its centre, top = the head's top; k picks mirror, model, angle and timing.
    // It is alive: now and then (every 9-25 s, `calm` slower) it turns 45° one way and later back, crossfading the
    // two views over about a second, and it sways along the car and bobs a little (standing more than seated)
    u"float regs_figure(float u, float top, float py, vec4 k, float lod, float calm) {\n"
    u"  vec4 I = regs_PassengerInfo;\n"
    u"  float T = regs_PassengerAnim.x, w = fract(k.w * 9.7);\n"
    u"  u -= (0.045 / calm) * sin(T * (0.45 + 0.4 * w) + k.x * 6.3) + (0.02 / calm) * sin(T * (1.3 + 0.6 * k.y) + k.z * 4.1);\n"
    u"  top += (0.012 / calm) * sin(T * (0.8 + 0.5 * w) + k.y * 5.7);\n"
    u"  float cu = u / I.w + 0.5, cv = (top - py) / I.z;\n"
    u"  float in_ = step(0.0, cu) * step(cu, 1.0) * step(0.0, cv) * step(cv, 1.0);\n"
    u"  cu = clamp(k.x > 0.5 ? 1.0 - cu : cu, 0.0, 1.0);\n"
    u"  cv = clamp(cv, 0.0, 1.0);\n"
    u"  float ph = T / ((9.0 + 16.0 * w) * calm) + k.w * 17.0;\n"
    u"  float n = floor(ph), f = smoothstep(1.0 - 1.0 / ((9.0 + 16.0 * w) * calm), 1.0, fract(ph));\n"
    u"  float dir = fract(k.w * 31.3) > 0.5 ? 1.0 : -1.0;\n"
    u"  float row = min(floor(k.y * I.x), I.x - 1.0), base = min(floor(k.z * I.y), I.y - 1.0);\n"
    u"  float c0 = mod(base + mod(n, 2.0) * dir + I.y, I.y), c1 = mod(base + mod(n + 1.0, 2.0) * dir + I.y, I.y);\n"
    u"  float a = textureLod(regs_PassengerAtlas, vec2((c0 + cu) / I.y, (row + cv) / I.x), lod).r;\n"
    u"  float b = textureLod(regs_PassengerAtlas, vec2((c1 + cu) / I.y, (row + cv) / I.x), lod).r;\n"
    u"  return in_ * mix(a, b, f);\n"
    u"}\n"
    // the load in steps the eye can count: shown = 0, 0.25, 0.5, 0.75, 1 (the simulation eases between them). Seats
    // fill first: half of them at a quarter, all at half; the standing places fill from half to full. Nobody in an
    // empty car
    // a double-decker: regs_CabinUpper is its upper deck's band (0 0 for one deck); each deck has its own figures
    u"uniform vec4 regs_CabinUpper;\n"  // xy: a double-decker's upper deck band; z: the vehicle's seed 0..1
    // figures seen from the front (the driver, a double-decker's upper front): the atlas rows of citizens whose head is
    // round from the front (no tall or wide hat, which reads as a block: row 11 was the first drivers' box head); with
    // the base game's 27 rows, else any row
    u"float regs_bareHead(float s) {\n"
    u"  const float rows[8] = float[8](1.0, 7.0, 15.0, 16.0, 17.0, 19.0, 21.0, 26.0);\n"
    u"  if (regs_PassengerInfo.x < 26.5) return s;\n"
    u"  return (rows[int(min(floor(s * 8.0), 7.0))] + 0.5) / regs_PassengerInfo.x;\n"
    u"}\n"
    u"float regs_passengers(float lod) {\n"
    u"  vec3 p = regs_ModelPos;\n"
    u"  vec2 band = regs_Cabin.xy;\n"
    u"  if (regs_CabinUpper.y > regs_CabinUpper.x + 0.3 && p.y > 0.5 * (regs_Cabin.y + regs_CabinUpper.x)) band = regs_CabinUpper.xy;\n"
    u"  float H = band.y - band.x;\n"
    u"  if (H < 0.3) return 0.0;\n"
    u"  float side = p.x < 0.0 ? 1.0 : 2.0, occ = regs_Cabin.z;\n"
    u"  float seatFill = clamp(occ * 2.0, 0.0, 1.0), standFill = clamp(occ * 2.0 - 1.0, 0.0, 1.0);\n"
    // figures at the game's character scale (its people are drawn ~1.6x life size): a seat every 1.3 m per side
    u"  float slot = floor(p.z / 1.3);\n"
    u"  vec4 h = regs_hash4(slot * 3.0 + side, 7.0);\n"
    u"  float ux = p.z - (slot + 0.5) * 1.3 - (h.y - 0.5) * 0.25;\n"
    u"  float deck = band.x > regs_Cabin.x + 0.1 ? 17.0 : 0.0;\n"  // the upper deck's own people
    u"  h = regs_hash4(slot * 3.0 + side + deck, 7.0);\n"
    u"  float headY = band.x + H * (0.48 + 0.1 * h.z), r = 0.15 + 0.03 * h.w;\n"
    u"  float slotS = floor(p.z / 1.3 + 0.5);\n"
    u"  vec4 g = regs_hash4(slotS * 5.0 + side + deck, 9.0);\n"
    u"  float us = p.z - slotS * 1.3 - (g.y - 0.5) * 0.25;\n"
    u"  float headS = band.x + H * (0.84 + 0.1 * g.z), rs = 0.16 + 0.03 * g.w;\n"
    u"  float seated, standing;\n"
    u"  if (regs_PassengerInfo.x > 0.5) {\n"
    u"    seated = regs_figure(ux, headY + r, p.y, regs_hash4(slot * 3.0 + side + deck, 11.0), lod, 2.0);\n"
    u"    standing = regs_figure(us, headS + rs, p.y, regs_hash4(slotS * 5.0 + side + deck, 13.0), lod, 1.0);\n"
    u"  } else {\n"
    u"    float head = 1.0 - smoothstep(r - 0.03, r + 0.01, length(vec2(ux, (p.y - headY) * 0.9)));\n"
    u"    float body = (1.0 - smoothstep(0.26, 0.31, abs(ux))) * (1.0 - smoothstep(headY - r - 0.12, headY - r - 0.03, p.y));\n"
    u"    seated = max(head, body);\n"
    u"    float headSt = 1.0 - smoothstep(rs - 0.03, rs + 0.01, length(vec2(us, (p.y - headS) * 0.9)));\n"
    u"    float bodySt = (1.0 - smoothstep(0.24, 0.29, abs(us))) * (1.0 - smoothstep(headS - rs - 0.12, headS - rs - 0.03, p.y));\n"
    u"    standing = max(headSt, bodySt);\n"
    u"  }\n"
    u"  seated *= smoothstep(h.x * 0.9, h.x * 0.9 + 0.1, seatFill);\n"
    u"  standing *= smoothstep(g.x * 0.9, g.x * 0.9 + 0.1, standFill);\n"
    u"  return max(seated, standing);\n"
    u"}\n"
    u"vec3 regs_windows(vec2 uv, vec3 albedo) {\n"
    u"  if (regs_WindowOn < 0.5) return vec3(0.0);\n"
    u"  vec4 m = texture2D(regs_WindowMask, uv);\n"
    u"  if (regs_WindowOn > 1.5 && regs_WindowOn < 2.5) {\n"
    u"    float ls = floor(regs_Seed * 255.0 + 0.5);\n"
    u"    float onAt = 0.36 + 0.14 * fract(sin(ls * 12.9898 + 4.1) * 43758.5453);\n"
    u"    float lit = smoothstep(onAt, onAt + 0.04, regs_Night.w);\n"
    u"    return m.r * vec3(2.6, 2.15, 1.5) * lit * (0.65 + 0.7 * dot(albedo, vec3(0.3, 0.59, 0.11)));\n"
    u"  }\n"
    u"  if (regs_WindowOn > 4.5 && regs_WindowOn < 5.5) {\n"
    u"    return m.r * albedo * vec3(1.3, 1.2, 0.95) * smoothstep(0.3, 0.5, regs_Night.w);\n"
    u"  }\n"
    u"  if (regs_WindowOn > 2.5 && regs_WindowOn < 3.5) {\n"
    u"    float ss = floor(regs_Seed * 255.0 + 0.5);\n"
    u"    float pick = fract(sin(ss * 78.233 + 1.7) * 43758.5453), onAt = 0.3 + 0.2 * fract(sin(ss * 37.719) * 43758.5453);\n"
    u"    float lit = pick < 0.6 ? smoothstep(onAt, onAt + 0.04, regs_Night.w) : 0.0;\n"
    u"    return m.r * albedo * 1.7 * lit;\n"
    u"  }\n"
    u"  if (regs_WindowOn > 9.5) {\n"
    u"    float code = floor(regs_WindowOn), on = fract(regs_WindowOn) / 0.9;\n"
    u"    float inside = code > 99.5 ? 1.0 : 0.0;\n"
    u"    code -= 100.0 * inside;\n"
    u"    float fm = floor((code - 10.0) / 4.0), rm = code - 10.0 - fm * 4.0;\n"
    u"    vec3 head = vec3(2.3, 2.15, 1.85), tail = vec3(1.5, 0.1, 0.05);\n"
    u"    vec3 fc = fm < 0.5 ? vec3(0.0) : fm < 1.5 ? head : fm < 2.5 ? tail : tail * 1.8;\n"
    u"    vec3 rc = rm < 0.5 ? vec3(0.0) : rm < 1.5 ? head : rm < 2.5 ? tail : tail * 1.8;\n"
    u"    vec3 cabin = m.b * inside * vec3(0.78, 0.7, 0.56) * (0.55 + 1.2 * dot(albedo, vec3(0.3, 0.59, 0.11)));\n"
    // the atlas's mip level from the model-space footprint of a pixel (here, outside the branch below: derivatives)
    u"    vec2 pdx = dFdx(regs_ModelPos.zy), pdy = dFdy(regs_ModelPos.zy);\n"
    u"    float plod = log2(max(max(length(pdx), length(pdy)) / max(regs_PassengerInfo.z / 92.0, 1e-4), 1.0));\n"
    // (glass facing out of an end, marked in alpha, gets its own figures below, not the side windows' rows)
    u"    if (inside > 0.5 && m.b > 0.0 && m.a < 0.1) cabin *= 1.0 - 0.85 * regs_passengers(plod);\n"
    // a double-decker's upper front (alpha 64): two passengers seen from the front, the second from half full
    u"    if (inside > 0.5 && m.a > 0.15 && m.a < 0.35 && regs_PassengerInfo.x > 0.5 && regs_CabinUpper.y > regs_CabinUpper.x + 0.3) {\n"
    u"      float HU = regs_CabinUpper.y - regs_CabinUpper.x, fillU = clamp(regs_Cabin.z * 2.0, 0.0, 1.0);\n"
    u"      float topU = regs_CabinUpper.x + HU * 0.62 + 0.17;\n"
    u"      float pa = regs_figure(regs_ModelPos.x + 0.5, topU, regs_ModelPos.y, vec4(0.0, regs_bareHead(fract(regs_CabinUpper.z + 0.37)), 0.0, 0.61), plod, 2.0) * step(0.25, fillU);\n"
    u"      float pb = regs_figure(regs_ModelPos.x - 0.5, topU, regs_ModelPos.y, vec4(1.0, regs_bareHead(fract(regs_CabinUpper.z + 0.71)), 0.0, 0.13), plod, 2.0) * step(0.75, fillU);\n"
    u"      cabin *= 1.0 - 0.85 * max(pa, pb);\n"
    u"    }\n"
    // the driver (2026-11-24): on the windscreen of the leading end (head lamps there, or the model's front by day),
    // a citizen seen from the front, seated on the left side (left of the direction of travel: -x for the model's
    // front, +x for its rear)
    u"    if (inside > 0.5 && m.a > 0.3 && regs_PassengerInfo.x > 0.5) {\n"
    u"      bool frontGlass = m.a > 0.75;\n"
    u"      bool lead = frontGlass ? (fm > 0.5 && fm < 1.5) || (fm < 0.5 && rm < 0.5) : (rm > 0.5 && rm < 1.5);\n"
    u"      float HB = regs_Cabin.y - regs_Cabin.x;\n"
    u"      if (lead && HB > 0.3) {\n"
    u"        float seat = frontGlass ? -0.45 : 0.45;\n"
    u"        float top = regs_Cabin.x + HB * 0.62 + 0.17;\n"
    u"        float drv = regs_figure(regs_ModelPos.x - seat, top, regs_ModelPos.y, vec4(0.0, regs_bareHead(regs_CabinUpper.z), 0.0, 0.37), plod, 3.0);\n"
    u"        cabin *= 1.0 - 0.85 * drv;\n"
    u"      }\n"
    u"    }\n"
    // a lamp smaller than a pixel keeps its light through the mask's mip levels, which hold the brightest texel
    // (night_lights.cpp buildMips); until 2026-11-24 a boost of up to 16x from the pixel's footprint made up for an
    // averaged mip and flickered as the car moved
    u"    float boost = 1.0;\n"
    u"    return (m.r * fc + m.g * rc) * boost * (0.75 + 0.5 * dot(albedo, vec3(0.3, 0.59, 0.11))) * on + cabin * regs_Cabin.w;\n"
    u"  }\n"
    u"  float s = floor(regs_Seed * 255.0 + 0.5);\n"
    u"  vec4 r = regs_hash4(s, 1.0), t = regs_hash4(s, 2.0), b = regs_hash4(s, 3.0);\n"
    u"  vec4 on = smoothstep(r - 0.03, r + 0.03, vec4(regs_Night.y));\n"
    u"  vec3 sum = vec3(0.0);\n"
    u"  for (int k = 0; k < 3; ++k) {\n"
    u"    vec3 tint = t[k] < 0.06 ? vec3(0.55, 0.64, 0.82) : mix(vec3(1.0, 0.6, 0.3), vec3(1.0, 0.84, 0.62), (t[k] - 0.06) / 0.94);\n"
    u"    sum += m[k] * on[k] * (0.7 + 0.6 * b[k]) * tint;\n"
    u"  }\n"
    u"  float shopOn = smoothstep(r.w - 0.03, r.w + 0.03, regs_Shop);\n"
    u"  sum += m.a * shopOn * 1.6 * mix(vec3(1.0, 0.82, 0.6), vec3(1.0, 0.95, 0.88), t.w);\n"
    u"  return sum * (0.6 + 1.5 * dot(albedo, vec3(0.3, 0.59, 0.11))) * regs_Night.x;\n"
    u"}\n"
    u"#}\n";
const char16_t* kNightMapsCall =
    u"  #if ($regsNight) {\n"
    u"  vec3 regsSignGlow = vec3(0.0);\n"
    u"  diffusemap.rgb = regs_signs(v_texCoord.xy, diffusemap.rgb, regsSignGlow);\n"
    u"  regs_windowGlow = regs_windows(v_texCoord.xy, diffusemap.rgb) + regsSignGlow;\n"
    u"  #}\n";
// destination displays (2026-11-24): a line vehicle's number field (engine/night_lights.h VehicleLampInfo::Sign)
// redrawn as an amber LED dot matrix of the real line number, a 5x7 font, dark between the dots; it glows, more at
// night. regs_SignMeta: x signs, y line number, z night01
const char16_t* kSignDecl =
    u"uniform vec4 regs_SignA0, regs_SignB0, regs_SignA1, regs_SignB1, regs_SignA2, regs_SignB2;\n"
    u"uniform vec4 regs_SignC0, regs_SignC1, regs_SignC2;\n"
    u"uniform vec4 regs_SignD0, regs_SignD1, regs_SignD2;\n"  // the board: u0, u1, aspect, number last
    u"uniform vec4 regs_SignT0, regs_SignT1, regs_SignT2;\n"  // texts: number first, last, alone (strip, LED w, print w)
    u"uniform vec4 regs_SignMeta;\n"
    u"uniform sampler2D regs_SignAtlas;\n"
    u"const int regs_font5x7[70] = int[70](14,17,19,21,25,17,14, 4,12,4,4,4,4,14, 14,17,1,2,4,8,31, 31,2,4,2,1,17,14,\n"
    u"  2,6,10,18,31,2,2, 31,16,30,1,1,17,14, 6,8,16,30,17,17,14, 31,1,2,4,8,8,8, 14,17,17,14,17,17,14, 14,17,17,15,1,2,12);\n"
    // the sign atlas (engine/sign_atlas.h): strips of 48 rows, LED text in rows 0-13, printed text in rows 16-47
    u"float regs_signLed(vec4 T, float X, float Y) {\n"
    u"  if (X < 0.0 || Y < 0.0 || Y >= 14.0 || X >= T.y) return 0.0;\n"
    u"  return step(0.5, texelFetch(regs_SignAtlas, ivec2(int(X), int(T.x) * 48 + int(Y)), 0).r);\n"
    u"}\n"
    // B: x flip along the text, y flip across, z width over height, w style (+10: the text runs along v); C: colour;
    // D: the whole board (number and name), u0 = u1 for the number alone
    u"vec3 regs_signAt(vec4 A, vec4 B, vec4 C, vec4 D, vec2 uv, vec3 col, inout vec3 glow) {\n"
    u"  vec2 f = fract(uv);\n"
    u"  bool board = D.y > D.x;\n"
    u"  float ux0 = board ? D.x : A.x, ux1 = board ? D.y : A.z;\n"
    u"  if (f.x < ux0 || f.x > ux1 || f.y < A.y || f.y > A.w) return col;\n"
    u"  float s = (f.x - ux0) / (ux1 - ux0), t = (f.y - A.y) / (A.w - A.y);\n"
    u"  bool rot = B.w > 9.5;\n"
    u"  float style = B.w - (rot ? 10.0 : 0.0);\n"
    u"  bool solid = style > 0.5, plate = style > 1.5;\n"  // 1 light glyphs on dark, 2 dark glyphs on a light plate
    u"  if (rot) { float tmp = s; s = t; t = tmp; }\n"
    u"  if (B.x > 0.5) s = 1.0 - s;\n"
    u"  if (B.y > 0.5) t = 1.0 - t;\n"
    u"  float aspect = board ? D.z : B.z;\n"
    u"  vec4 T = board ? (D.w > 0.5 ? regs_SignT1 : regs_SignT0) : regs_SignT2;\n"
    u"  vec3 bg = vec3(0.025, 0.022, 0.02), amber = C.rgb;\n"
    u"  float lit = 0.0, dotMask = 1.0;\n"
    u"  if (T.x >= 0.0 && !solid) {\n"
    // LED: the board 14 dots high; a text longer than the board scrolls, pausing at its ends
    u"    float rows = 14.0, cols = floor(aspect * rows);\n"
    u"    float X = s * aspect * rows, Y = t * rows;\n"
    u"    float off;\n"
    u"    if (T.y <= cols) off = board ? (D.w > 0.5 ? T.y - cols : -1.0) : 0.5 * (T.y - cols);\n"
    u"    else off = clamp(mod(regs_PassengerAnim.x * 7.0, T.y - cols + 24.0) - 12.0, 0.0, T.y - cols);\n"
    u"    lit = regs_signLed(T, floor(X + floor(off)), floor(Y));\n"
    u"    dotMask = 1.0 - smoothstep(0.32, 0.45, length(fract(vec2(X, Y)) - 0.5));\n"
    u"  } else if (T.x >= 0.0) {\n"
    // printed (a roller blind, a number box): the Blair text, squeezed when it is wider than the board
    u"    float avail = aspect * 32.0, xs = max(1.0, T.z / avail);\n"
    u"    float x = (s * avail - 0.5 * (avail - T.z / xs)) * xs, y = 16.0 + t * 32.0;\n"
    u"    if (x >= 0.0 && x < T.z) lit = textureLod(regs_SignAtlas, vec2(x / 512.0, (T.x * 48.0 + y) / 9216.0), 0.0).r;\n"
    u"  } else {\n"
    // no atlas: the number in a 5x7 font
    u"    int n = int(regs_SignMeta.y + 0.5);\n"
    u"    int nd = n >= 100 ? 3 : n >= 10 ? 2 : 1;\n"
    u"    float dots = float(nd * 6 - 1);\n"
    u"    float d = min(1.0 / 9.0, aspect / (dots + 1.0));\n"
    u"    float X = (s * aspect - 0.5 * (aspect - dots * d)) / d, Y = (t - 0.5 * (1.0 - 7.0 * d)) / d;\n"
    u"    if (!solid) dotMask = 1.0 - smoothstep(0.32, 0.45, length(fract(vec2(X, Y)) - 0.5));\n"
    u"    if (X >= 0.0 && Y >= 0.0 && X < dots && Y < 7.0) {\n"
    u"      int ci = int(X) / 6, cx = int(X) - ci * 6, row = int(Y);\n"
    u"      int p = nd - 1 - ci, v = n;\n"
    u"      for (int k = 0; k < 3; ++k) if (k < p) v /= 10;\n"
    u"      int digit = v - (v / 10) * 10;\n"
    u"      if (cx < 5) lit = float((regs_font5x7[digit * 7 + row] >> (4 - cx)) & 1);\n"
    u"    }\n"
    u"  }\n"
    // a roller blind is printed, not lit: its glyphs only reflect light; LEDs glow, more at night
    u"  if (plate) return mix(amber, vec3(0.06, 0.06, 0.07), lit);\n"
    u"  if (solid) return mix(bg, amber * 0.85, lit);\n"
    u"  glow += amber * lit * dotMask * (0.5 + 1.5 * regs_SignMeta.z);\n"
    u"  return mix(bg, mix(amber * 0.07, amber, lit), dotMask);\n"
    u"}\n"
    u"vec3 regs_signs(vec2 uv, vec3 col, inout vec3 glow) {\n"
    u"  if (regs_SignMeta.x < 0.5) return col;\n"
    u"  col = regs_signAt(regs_SignA0, regs_SignB0, regs_SignC0, regs_SignD0, uv, col, glow);\n"
    u"  if (regs_SignMeta.x > 1.5) col = regs_signAt(regs_SignA1, regs_SignB1, regs_SignC1, regs_SignD1, uv, col, glow);\n"
    u"  if (regs_SignMeta.x > 2.5) col = regs_signAt(regs_SignA2, regs_SignB2, regs_SignC2, regs_SignD2, uv, col, glow);\n"
    u"  return col;\n"
    u"}\n";
const char16_t* kNightColorDecl =
    u"#$regsLamp = !bool($envFlags & (ENVIROMENT_SHADOWMAP_RENDER | ENVIROMENT_SHADOW_RENDER)) && "
    u"!bool($matFlags & (MATERIAL_ADDITIVE | MATERIAL_ADDITIVE_A));\n"
    u"#once# vec3 regs_windowGlow = vec3(0.0);\n"
    u"#if ($regsLamp) {\n"
    u"#once# uniform vec4 regs_Night;\n"
    u"uniform sampler2D regs_LampMap;\n"
    u"uniform vec4 regs_LampMapXf;\n"
    u"uniform vec4 regs_LampHeight;\n"
    u"uniform sampler2D regs_CarMap;\n"
    u"uniform vec4 regs_CarMapXf;\n"
    u"uniform vec4 regs_CarLight;\n"
    u"vec3 regs_carLight() {\n"
    u"  if (regs_CarLight.z <= 0.0) return vec3(0.0);\n"
    u"  vec2 uv = (regs_WorldPos.xz - regs_CarMapXf.xy) * regs_CarMapXf.zw;\n"
    u"  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return vec3(0.0);\n"
    u"  vec4 l = texture2D(regs_CarMap, vec2(uv.x, min(uv.y * 0.5, 0.5 - 0.5 / 4096.0)));\n"  // the light map's lower half
    u"  if (l.r <= 0.0005) return vec3(0.0);\n"
    u"  float g = regs_CarLight.x + l.b / l.r;\n"
    u"  vec3 tint = mix(vec3(1.0, 0.93, 0.8), vec3(1.0, 0.74, 0.45), clamp(l.g / l.r, 0.0, 1.0));\n"
    u"  float y = regs_WorldPos.y;\n"
    u"  return tint * (l.r / (1.0 + 0.4 * l.r)) * (smoothstep(g + 3.2, g + 1.0, y) * smoothstep(g - 1.5, g - 0.3, y) * regs_CarLight.z);\n"
    u"}\n"
    u"vec3 regs_lampLight() {\n"
    u"  if (regs_Night.z <= 0.0) return regs_carLight();\n"
    u"  vec2 uv = (regs_WorldPos.xz - regs_LampMapXf.xy) * regs_LampMapXf.zw;\n"
    u"  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return regs_carLight();\n"
    u"  vec4 l = texture2D(regs_LampMap, vec2(uv.x * regs_LampHeight.z, 0.5 + max(uv.y * regs_LampHeight.w, 0.5 / 4096.0)));\n"  // upper half
    u"  float head = regs_LampHeight.x + l.a * regs_LampHeight.y;\n"
    u"  float y = regs_WorldPos.y;\n"
    u"  return l.rgb * (smoothstep(head + 0.5, head - 1.0, y) * smoothstep(head - 18.0, head - 11.0, y) * regs_Night.z) + regs_carLight();\n"
    u"}\n"
    u"#}\n";
const char16_t* kNightColorStart =
    u"  #if ($regsLamp) {\n"
    u"  vec3 regs_albedo = color.rgb * diffusemap.rgb;\n"
    u"  #}\n";
const char16_t* kNightColorEnd =
    u"  #if ($regsLamp) {\n"
    u"  color.rgb += regs_albedo * regs_lampLight() + regs_windowGlow;\n"
    u"  #}\n";

bool insertBefore(std::u16string& text, const std::u16string& anchor, const std::u16string& what) {
  size_t at = text.find(anchor);
  if (at == std::u16string::npos) return false;
  text.insert(at, what);
  return true;
}
bool insertAfterLine(std::u16string& text, const std::u16string& anchor, const std::u16string& what) {
  size_t at = text.find(anchor);
  if (at == std::u16string::npos) return false;
  size_t eol = text.find(u'\n', at);
  text.insert(eol == std::u16string::npos ? text.size() : eol + 1, what);
  return true;
}

}  // namespace

std::u16string portShaderPatch(const std::u16string& path, std::u16string text) {
  static const std::u16string kFrom = u"float y = pos.y;// - gl_MultiTexCoord1.x;";
  static const std::u16string kTo = u"float y = pos.y - gl_MultiTexCoord1.x;";
  if (path.ends_with(u"shaders/trees/alter.vertex")) {
    if (size_t at = text.find(kFrom); at != std::u16string::npos) text.replace(at, kFrom.size(), kTo);
    else LOG_WARN("shader component %s: the tree wind line was not found, trees sway in division space",
                  utf16ToUtf8(path).c_str());
  }
  if (path.ends_with(u"shaders/terrain/maps.fragment")) {  // port: the OSM import's sand (terrain.cpp, weight over 1)
    std::u16string t = text;
    if (insertAfterLine(t, u"    diffusemap = mix(diffusemap, seafloor, smoothstep(0.3, 0.7, seafloora));",
                        u"    float regsSand = clamp(v_texCoord2.y - 1.0, 0.0, 1.0);\n"
                        u"    diffusemap.rgb = mix(diffusemap.rgb, min(seafloor.rgb * vec3(1.7, 1.6, 1.3) + vec3(0.10, 0.09, 0.05), vec3(1.0)), regsSand);\n"))
      text = t;
    else
      LOG_WARN("shader component %s: no anchor for the sand", utf16ToUtf8(path).c_str());
  }
  // port (owner 2026-11-19: "the water at night looks very bright"): the water mixes in a reflection of a fixed
  // daylight cube map (textures/envmap.dds) and a fixed light grey where it breaks at the shore; neither follows the
  // light, so with the day/night cycle the river glowed in the dark. Both now scale with the sky's brightness, read
  // from the fog colour (the location's daylight fog ~0.78 luminance = 1; darker at dusk, under rain, at night)
  if (path.ends_with(u"shaders/water/water.fragment")) {
    std::u16string t = text;
    static const std::u16string kGrey = u"color = mix(vec4(0.55, 0.55, 0.60, ";
    size_t at = t.find(kGrey);
    bool ok = at != std::u16string::npos &&
              insertAfterLine(t, u"vec4 ref = textureCube(texture2, reflect(view, normal) * vieworient) * r;",
                              u"  float regsSkyLight = clamp(dot(gl_Fog.color.rgb, vec3(0.3, 0.59, 0.11)) / 0.78, 0.04, 1.0);\n"
                              u"  ref.rgb *= regsSkyLight;\n");
    if (ok) {
      at = t.find(kGrey);
      t.replace(at, kGrey.size(), u"color = mix(vec4(vec3(0.55, 0.55, 0.60) * regsSkyLight, ");
      text = t;
    } else {
      LOG_WARN("shader component %s: no anchor for the night reflection", utf16ToUtf8(path).c_str());
    }
  }
  if (path.ends_with(u"shaders/default/maps.fragment")) {
    std::u16string t = text;
    if (insertBefore(t, u"void maps_input() {", kNightMapsDecl) && insertBefore(t, u"void maps_input() {", u"#if ($regsNight) {\n") &&
        insertBefore(t, u"void maps_input() {", kSignDecl) && insertBefore(t, u"void maps_input() {", u"#}\n") &&
        insertBefore(t, u"  geometry_input();", kNightMapsCall))
      text = t;
    else
      LOG_WARN("shader component %s: no anchor for the lit windows", utf16ToUtf8(path).c_str());
  }
  if (path.ends_with(u"shaders/default/color.fragment")) {
    std::u16string t = text;
    size_t last = t.rfind(u'}');
    bool ok = last != std::u16string::npos;
    if (ok) t.insert(last, kNightColorEnd);
    ok = ok && insertAfterLine(t, u"void color_output() {", kNightColorStart) &&
         insertBefore(t, u"void color_input() {", kNightColorDecl);
    if (ok) text = t;
    else LOG_WARN("shader component %s: no anchor for the night lights", utf16ToUtf8(path).c_str());
  }
  return text;
}

// A component template read, patched (portShaderPatch), converted (templateToScript) and compiled once; every
// variant runs the same code with its flags set as script variables. The engine builds the script again for each
// variant with the flags prepended as assignments; the six declarations below stand in for them (same script
// variables in the same order, same line numbers), so the output is the same.
struct TemplateCode {
  bool found = false;
  Ref<Code> code;  // null when it does not compile
};
const char16_t* const kTemplatePath = u"memory/regs/shader-template.script";  // the name its errors carry

const TemplateCode& templateCode(VM& vm, const std::u16string& path) {
  static std::unordered_map<std::u16string, TemplateCode> cache;
  if (auto it = cache.find(path); it != cache.end()) return it->second;
  TemplateCode& tc = cache[path];
  std::vector<uint8_t> data;
  if (!vm.vfs().read(utf16ToUtf8(path), data)) return tc;
  tc.found = true;
  std::u16string script = u"global $matFlags;\nglobal $meshFlags;\nglobal $envFlags;\nglobal $directionalCount;\n"
                          u"global $omniCount;\nglobal $spotCount;\n" +
                          templateToScript(portShaderPatch(path, decodeTextFile(data)));
  // the engine's way through a memory file: written as UTF-8, read back as an 8-bit file (one code unit per byte),
  // which turns any non-ASCII character of a template into its UTF-8 bytes; kept so the output stays the same
  std::string u8 = utf16ToUtf8(script);
  script = decodeTextFile(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(u8.data()), u8.size()));
  tc.code = vm.compileSource(script, kTemplatePath);
  return tc;
}

bool expandStage(VM& vm, ShaderCombiner* c, const Value& components, uint32_t mat, uint32_t mesh, uint32_t env,
                 std::string& out) {
  std::vector<std::u16string> files;
  if (components.type() == VT::String) files.push_back(components.str());
  else if (MapObj* m = components.map())
    for (auto& [k, v] : m->entries) files.push_back(vm.toStr(v));  // key order: "!base" first, then alphabetical
  if (files.empty()) return false;
  std::set<std::u16string> once;
  Expansion e;
  e.once = &once;
  for (const auto& f : files) {
    std::u16string path = vm.resolvePath(f, c->script.get());
    const TemplateCode& tc = templateCode(vm, path);
    if (!tc.found) {
      LOG_WARN("shader %s: component %s not found", utf16ToUtf8(c->path).c_str(), utf16ToUtf8(path).c_str());
      return false;
    }
    Expansion* outer = g_expansion;
    g_expansion = &e;
    LoadOptions opt;
    opt.cls = vm.findClass("gsTemplate");
    opt.keepInScriptList = false;
    Value r;
    if (tc.code)
      r = vm.loadCompiled(tc.code, kTemplatePath, opt, [&](Object* o) {
        // the variant's flags (the engine prepends them to the template's script as assignments)
        o->var(u"matFlags") = Value::integer(int32_t(mat));
        o->var(u"meshFlags") = Value::integer(int32_t(mesh));
        o->var(u"envFlags") = Value::integer(int32_t(env));
        o->var(u"directionalCount") = Value::integer(int32_t(env >> 8 & 15));
        o->var(u"omniCount") = Value::integer(int32_t(env >> 12 & 15));
        o->var(u"spotCount") = Value::integer(int32_t(env >> 16 & 15));
      });
    g_expansion = outer;
    if (!r.object() || !r.object()->code) {
      LOG_WARN("shader %s: template %s does not compile", utf16ToUtf8(c->path).c_str(), utf16ToUtf8(path).c_str());
      return false;
    }
  }
  out = utf16ToUtf8(e.text);
  return true;
}

}  // namespace

std::u16string templateToScript(const std::u16string& source) {
  // parseTemplate (L:0x0826a8f0): '#once#' lines and plain lines become output calls with $vars spliced in,
  // other '#' lines are script; leading bytes 0x01-0x20 are stripped and empty lines dropped
  auto text = [](const std::u16string& t) {
    std::u16string r;
    for (size_t i = 0; i < t.size();) {
      if (t[i] == u'$') {
        size_t j = i + 1;
        while (j < t.size() && (std::isalnum(int(t[j] < 128 ? t[j] : 0)) || t[j] == u'_')) ++j;
        r += u"\" + " + t.substr(i, j - i) + u" + \"";
        i = j;
      } else {
        r += t[i++];
      }
    }
    return r;
  };
  std::u16string out;
  size_t start = 0;
  while (start <= source.size()) {
    size_t end = source.find(u'\n', start);
    if (end == std::u16string::npos) end = source.size();
    std::u16string line = source.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && line.back() == u'\r') line.pop_back();
    size_t k = 0;
    while (k < line.size() && line[k] >= 1 && line[k] <= 0x20) ++k;
    line = line.substr(k);
    if (line.empty()) continue;
    if (line.rfind(u"#once#", 0) == 0) out += u"this.output_once(\"" + text(line.substr(6)) + u"\\n\");\n";
    else if (line[0] == u'#') out += line.substr(1) + u"\n";
    else out += u"this.output(\"" + text(line) + u"\\n\");\n";
  }
  return out;
}

ShaderCombiner* getCombiner(VM& vm, const std::u16string& rawPath) {
  std::u16string path = rawPath.empty() ? u"shaders/default/default.shader" : rawPath;
  while (!path.empty() && path[0] == u'/') path.erase(0, 1);
  auto& c = cache();
  if (auto it = c.find(path); it != c.end()) return it->second->script ? it->second.get() : nullptr;
  auto& slot = c[path];
  slot = std::make_unique<ShaderCombiner>();
  ShaderCombiner* sc = slot.get();
  sc->path = path;
  // the script object exists before its code runs, so setBaseShader can find its combiner
  Ref<Object> obj = vm.newObject(vm.findClass("gsShaderCombiner_OGL"), nullptr);
  byScript()[obj.get()] = sc;
  LoadOptions opt;
  opt.into = obj;
  Value r = vm.load(u"/" + path, opt);  // already resolved: absolute, even when setBaseShader runs inside a script
  if (!r.object() || !obj->code) {
    LOG_WARN("shader %s: cannot load", utf16ToUtf8(path).c_str());
    byScript().erase(obj.get());
    return nullptr;
  }
  sc->script = obj;
  sc->matMask = uint32_t(intField(vm, obj.get(), u"matMask"));
  sc->meshMask = uint32_t(intField(vm, obj.get(), u"meshMask"));
  sc->envMask = uint32_t(intField(vm, obj.get(), u"envMask"));
  return sc;
}

ShaderVariant* combinerVariant(VM& vm, ShaderCombiner* c, uint32_t mat, uint32_t mesh, uint32_t env) {
  // getVariant (0x1003ca40, interfaces-3d.md §3.5): shadow passes reduce the material and environment keys
  uint32_t mk = mat & c->matMask, ek = env & c->envMask;
  if (env & 4) {  // shadow-map render
    mk &= 0x4000 | 0x1 | 0x2;  // DIFFUSEMAP | ALPHABLEND | ALPHATEST
    ek &= 4;
  } else if (env & 8) {  // screen-space shadow render
    mk &= 0x4000;
    ek &= 2 | 8;
  }
  auto key = std::make_tuple(mk, mesh & c->meshMask, ek);
  auto it = c->variants.find(key);
  if (it != c->variants.end()) return &it->second;
  ShaderVariant& v = c->variants[key];
  auto [m, me, e] = key;
  runProgram(vm, c, m, me, e);
  Object* s = c->script.get();
  v.attributes = uint32_t(intField(vm, s, u"attributes"));
  v.textures = uint32_t(intField(vm, s, u"textures"));
  v.parameters = uint32_t(intField(vm, s, u"parameters"));
  bool ok = expandStage(vm, c, field(vm, s, u"vertexComponent"), m, me, e, v.vertex) &&
            expandStage(vm, c, field(vm, s, u"fragmentComponent"), m, me, e, v.fragment);
  v.failed = !ok;
  return &v;
}

void registerShaderNatives(VM& vm) {
  // setBaseShader(path): the base combiner, loaded through the same cache; its masks are copied (0x1003bf40)
  implement(vm, "gsShaderCombiner_OGL", u"setBaseShader", [](VM& v, Object* s, int, const Value* a) {
    auto it = byScript().find(s);
    if (it == byScript().end()) return Value();
    ShaderCombiner* base = getCombiner(v, v.resolvePath(v.toStr(a[0]), s));
    if (!base) return Value();
    it->second->base = base;
    s->fieldValues[u"matMask"] = Value::integer(int32_t(base->matMask));
    s->fieldValues[u"meshMask"] = Value::integer(int32_t(base->meshMask));
    s->fieldValues[u"envMask"] = Value::integer(int32_t(base->envMask));
    return Value();
  });
  implement(vm, "gsTemplate", u"output", [](VM& v, Object*, int, const Value* a) {
    if (g_expansion) g_expansion->text += v.toStr(a[0]);
    return Value();
  });
  implement(vm, "gsTemplate", u"output_once", [](VM& v, Object*, int, const Value* a) {
    if (g_expansion) {
      std::u16string s = v.toStr(a[0]);
      if (g_expansion->once->insert(s).second) g_expansion->text += s;
    }
    return Value();
  });
}

}  // namespace regs::engine
