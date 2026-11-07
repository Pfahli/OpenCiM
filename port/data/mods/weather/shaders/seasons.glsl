// Weather mod, seasons: the foliage of deciduous trees and bushes through the year. A port shader hook
// (mod-manager.md Â§4.3) on surface class 9 (foliage: the materials the trees' wind shader draws), also run in the
// shadow-map passes (only its alpha counts there), so bare trees cast the shadow of their branches.
//
// All the game's trees and bushes draw from one texture atlas (metro/objects/foliage/foliage-final.dds, 512Â²): two
// conifer branches (top left), five deciduous leaf clusters (grey, tinted green per tree by the game's palette), one
// bare bush (brown twigs, centre), bark, a tree pit, a ground shadow and grass. The hook tells them apart by the
// texture coordinate (RegsSurface.uv) and reads the atlas texel (regs_texel0): conifers, bark, twigs and the rest keep
// their look; leaf texels change colour, fall and come back. A leaf card whose leaves have fallen shows the bare bush's
// twigs instead (mapped from its cluster onto the bare bush), so a winter crown is a net of branches, not empty air.
//
// Each tree has its own number (RegsSurface.uv1.y, from the engine), so trees turn and drop their leaves a few days
// apart and in their own autumn colour; each leaf (an 8-texel cell of the atlas) has its own moment too: the leaves
// that turn first fall first. The day of the year comes from weather.script (seasons_time.x). The manifest lists this
// hook before wet.glsl, so rain and snow settle on the season's foliage (snow on the bare twigs).
uniform vec4 seasons_time;  // x day of the year (0 = 1 January, with the hour), y seasons shown 0..1, z debug view

float seasons_hash(vec2 p) {
  p = fract(p * vec2(443.897, 441.423));
  p += dot(p, p.yx + 19.19);
  return fract((p.x + p.y) * p.x);
}

bool seasons_in(vec2 uv, vec4 r) { return uv.x >= r.x && uv.x < r.z && uv.y >= r.y && uv.y < r.w; }

// the deciduous leaf cluster of the atlas under a texture coordinate (its rect: u0, v0, u1, v1; v from the top), or
// vec4(0) where there is none (conifers, bark, twigs, tree pit, shadow, grass)
vec4 seasons_cluster(vec2 uv) {
  const vec4 kTopRight = vec4(0.58, 0.0, 1.0, 0.31);
  const vec4 kMiddleLeft = vec4(0.0, 0.31, 0.29, 0.6);
  const vec4 kBush = vec4(0.21, 0.555, 0.51, 0.72);
  const vec4 kBottomLeft = vec4(0.0, 0.72, 0.36, 1.0);
  const vec4 kBottomMiddle = vec4(0.36, 0.72, 0.81, 1.0);
  if (seasons_in(uv, kTopRight)) return kTopRight;
  if (seasons_in(uv, kMiddleLeft)) return kMiddleLeft;
  if (seasons_in(uv, kBush)) return kBush;
  if (seasons_in(uv, kBottomLeft)) return kBottomLeft;
  if (seasons_in(uv, kBottomMiddle)) return kBottomMiddle;
  return vec4(0.0);
}

const vec4 kSeasonsBare = vec4(0.295, 0.30, 0.565, 0.53);  // the bare bush: brown twigs

// the year of a deciduous tree, by its day d (its own few days earlier or later):
//   leaves  0 bare .. 1 full: budding from 5 April, full by 30 April; falling from 15 October, bare by 21 November
//   turned  0 green .. 1 autumn colour: from 19 September, full by 25 October (through yellowish green)
//   fresh   the bright spring green: from the budding, gone by mid June
float seasons_leaves(float d) {
  return smoothstep(95.0, 120.0, d) * (1.0 - smoothstep(288.0, 325.0, d));
}
float seasons_turned(float d) { return smoothstep(262.0, 298.0, d) * step(200.0, d); }
float seasons_fresh(float d) { return smoothstep(92.0, 105.0, d) * (1.0 - smoothstep(125.0, 165.0, d)); }

float seasons_lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }

void seasons_foliage(inout vec4 c, RegsSurface s) {
  if (s.surfaceClass != 9) return;
  float on = seasons_time.y;
  if (on <= 0.001 && seasons_time.z < 0.5) return;
  vec2 uv = fract(s.uv);  // the atlas, v from the top (the game stores v negated)
  vec4 rect = seasons_cluster(uv);
  vec4 tex = regs_texel0(s.uv);
  // the bare bush's twig at the same place of this card (read before any branch: texture() needs its neighbours)
  // (2 x 2 small bushes per card with a little air between them: fine twigs a crown can be seen through)
  vec2 q = (uv - rect.xy) / max(rect.zw - rect.xy, vec2(1e-3));
  vec2 qt = (fract(q * 2.0) - 0.5) * 1.3 + 0.5;
  float inside = step(0.0, min(qt.x, qt.y)) * step(max(qt.x, qt.y), 1.0);
  vec4 b = regs_texel0(floor(s.uv) + kSeasonsBare.xy + clamp(qt, 0.0, 1.0) * (kSeasonsBare.zw - kSeasonsBare.xy));
  b.a *= inside;
  // a leaf: inside a leaf cluster and grey (the clusters' twigs are brown)
  bool leafCard = rect.z > 0.0;
  float twig = smoothstep(0.035, 0.07, tex.r - tex.b);
  if (seasons_time.z > 0.5 && s.shadow < 0.5) {  // debug view: leaves red, twigs yellow, the rest blue
    c.rgb = leafCard ? (twig > 0.5 ? vec3(0.9, 0.8, 0.1) : vec3(0.9, 0.15, 0.1)) : vec3(0.1, 0.3, 0.9);
    return;
  }
  if (!leafCard) return;
  float seed = s.uv1.y;
  // this tree's day: up to a week earlier or later than the calendar
  float d = seasons_time.x + (seed - 0.5) * 14.0;
  if (d > 365.0) d -= 365.0;
  if (d < 0.0) d += 365.0;
  vec2 cell = floor(uv * 64.0);
  float h = seasons_hash(cell + seed * 37.0);   // this leaf's moment: low ones turn and fall first
  float leaves = mix(1.0, seasons_leaves(d), on);
  bool kept = twig > 0.5 || h < leaves;          // twigs stay; a leaf stays until the share left is under its moment
  if (!kept) {
    // fallen (or not yet out): the bare bush's twigs on this card
    c.a = b.a;
    if (s.shadow > 0.5) return;
    // lit like the leaf it replaces: the leaf's light over its texel, without the green of the palette (about 0.55)
    float light = clamp(seasons_lum(c.rgb) / max(seasons_lum(tex.rgb) * 0.55, 0.04), 0.35, 1.6);
    // bark grey-brown (the bare bush's texture is dark and greyish: its own shading kept, the colour lifted)
    float shade = clamp(seasons_lum(b.rgb) / 0.14, 0.6, 1.4);
    c.rgb = vec3(0.34, 0.29, 0.24) * shade * light;
    return;
  }
  if (s.shadow > 0.5 || twig > 0.5) return;
  float keep = 1.0 - s.fog;
  float lum = seasons_lum(c.rgb);
  // autumn: each tree its colour (yellow like birches and limes, orange, a few red like maples), each leaf turning on
  // its own; brown before it falls
  float turned = seasons_turned(d) * on;
  if (turned > 0.0) {
    // most trees orange, some golden yellow, a few red like maples
    float kind = fract(seed * 7.31);
    vec3 tint = mix(vec3(1.0, 0.66, 0.16), vec3(1.0, 0.47, 0.1), smoothstep(0.15, 0.45, kind));
    tint = mix(tint, vec3(0.9, 0.25, 0.1), smoothstep(0.84, 0.96, kind));
    tint *= 0.92 + 0.16 * seasons_hash(cell * 1.7 + 3.1);               // leaf to leaf
    float falling = 1.0 - seasons_leaves(d);                             // late: browner
    tint = mix(tint, vec3(0.62, 0.38, 0.18), 0.35 * smoothstep(0.3, 0.95, falling) * step(200.0, d));
    // each leaf turns on its own: green, then yellowish green, then its autumn colour, by hue (a plain mix of green
    // and orange would pass through a dull olive); a little brighter than the green
    float t = smoothstep(h * 0.55, h * 0.55 + 0.45, turned);
    vec3 green = c.rgb / max(lum, 1e-3), mid = vec3(0.86, 0.88, 0.24) / seasons_lum(vec3(0.86, 0.88, 0.24));
    vec3 ripe = tint / seasons_lum(tint);   // all three at luminance 1: the mix moves the hue, not the brightness
    vec3 way = t < 0.4 ? mix(green, mid, t / 0.4) : mix(mid, ripe, (t - 0.4) / 0.6);
    float lift = mix(1.0, mix(1.3, 1.12, smoothstep(0.15, 0.95, kind)), smoothstep(0.0, 0.6, t));
    vec3 autumn = clamp(lum * lift * way / max(seasons_lum(way), 1e-3), 0.0, 1.0);
    c.rgb = mix(c.rgb, autumn, smoothstep(0.0, 0.25, t) * keep);
  }
  // spring: the young leaves bright and yellowish green
  float fresh = seasons_fresh(d) * on;
  if (fresh > 0.0) {
    vec3 tint = vec3(0.62, 1.0, 0.3);
    vec3 young = clamp(lum * 1.12 * tint / seasons_lum(tint), 0.0, 1.0);
    c.rgb = mix(c.rgb, young, 0.75 * fresh * keep);
  }
}
