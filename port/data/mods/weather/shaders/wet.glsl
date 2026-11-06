// Weather mod: wet ground, puddles with reflections and rain ripples. A port shader hook (mod-manager.md §4.3):
// GLSL 3.30, run on the final colour of the map's surfaces; RegsSurface, regs_EnvMap, regs_surfaceAt and regs_Fog
// come from the engine, the two uniforms below from weather.script (regsShaderUniform).
//
// Puddles belong to one surface. The road lies a little lower than its pavement (the kerb step), so water collects in
// the gutter: most and the longest-lasting road puddles sit in a strip along the kerb on the carriageway, a few small
// ones in the lanes. Pavements and squares drain towards the road: fewer and smaller puddles that dry sooner, none
// near their edge. Every puddle mask fades to nothing at its surface's edge (found in the engine's surface map), and
// roads and pavements draw from different noise, so no patch runs across a kerb.
uniform vec4 weather_wet;   // x water film 0..1, y puddle amount 0..1, z rain now 0..1 (ripples), w heavy rain 0..1
uniform vec4 weather_misc;  // x lightning flash 0..1, y debug view of the surface map

float weather_hash(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}

float weather_noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  float a = weather_hash(i), b = weather_hash(i + vec2(1.0, 0.0));
  float c = weather_hash(i + vec2(0.0, 1.0)), d = weather_hash(i + vec2(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// small irregular patches: value noise in world space, features of about 1-2 m
float weather_fbm(vec2 p) {
  return 0.6 * weather_noise(p) + 0.28 * weather_noise(p * 2.7 + 17.1) + 0.12 * weather_noise(p * 6.3 + 3.7);
}

// rain ripples: in each 0.9 m cell (two offset layers) a ring grows from a random point and fades; the slope of the
// ring's wave tilts the puddle's mirror normal
vec2 weather_ripples(vec2 p, float t) {
  vec2 sum = vec2(0.0);
  for (int k = 0; k < 2; ++k) {
    vec2 q = p / 0.9 + float(k) * vec2(0.5, 0.37);
    vec2 cell = floor(q), f = fract(q);
    float h = weather_hash(cell + float(k) * 7.3);
    vec2 centre = vec2(0.3) + 0.4 * vec2(h, weather_hash(cell + 3.1));
    float phase = fract(t * (0.8 + 0.5 * h) + h * 7.0);
    vec2 d = f - centre;
    float r = length(d), ring = phase * 0.3;
    float x = (r - ring) * 30.0;
    float wave = abs(x) < 3.1416 ? sin(x) * (1.0 - phase) : 0.0;
    sum += (r > 1e-3 ? d / r : vec2(0.0)) * wave;
  }
  return sum;
}

// distances (m) from p to the edge of its surface: dEdge to any other surface, dLow to a lower one (for a pavement:
// the road), dHigh to a higher one (for a road: pavement, platform or verge); searched on 8 rays out to 2.2 m
void weather_edges(vec2 p, int own, out float dEdge, out float dLow, out float dHigh) {
  dEdge = 9.0; dLow = 9.0; dHigh = 9.0;
  float radii[5] = float[5](0.3, 0.6, 1.0, 1.5, 2.2);
  for (int r = 0; r < 5; ++r) {
    for (int k = 0; k < 8; ++k) {
      float a = float(k) * 0.7853982 + float(r) * 0.39;
      int c = regs_surfaceAt(p + radii[r] * vec2(cos(a), sin(a)));
      if (c < 0 || c == own) continue;
      dEdge = min(dEdge, radii[r]);
      if (own == 4) dHigh = min(dHigh, radii[r]);          // next to a road everything else is higher
      else if (c == 4) dLow = min(dLow, radii[r]);         // a pavement or square next to the road
    }
    if (dEdge < 9.0 && (own == 4 ? dHigh < 9.0 : true) && r >= 2) break;
  }
}

// the noise level above which a share (0..1) of the ground is under water: weather_fbm is roughly normal around 0.5
// (sd 0.13); the quantile by Tukey's lambda approximation
float weather_level(float share) {
  float c = clamp(share, 0.0005, 0.9995);
  return 0.5 + 0.13 * 4.91 * (pow(1.0 - c, 0.14) - pow(c, 0.14));
}

void weather_wet_apply(inout vec4 c, RegsSurface s);

// the hook: rain only ever darkens the ground. A puddle mirrors the grey sky, a wet film shines, but neither may end
// up brighter than the dry surface was (in the shaded streets a bright reflection read as the ground lighting up
// while the rain set in); the lightning flash is the exception
void weather_wet_surface(inout vec4 c, RegsSurface s) {
  vec3 dry = c.rgb;
  weather_wet_apply(c, s);
  if (weather_misc.y < 0.5) c.rgb = min(c.rgb, dry * (1.0 + 2.0 * weather_misc.x));
}

void weather_wet_apply(inout vec4 c, RegsSurface s) {
  int cls = s.surfaceClass;
  if (weather_misc.y > 0.5) {  // debug view (REGS_DEBUG_WEATHER=2): the surface map and the edge distance
    int m = regs_surfaceAt(s.pos.xz);
    vec3 col = m == 4 ? vec3(0.2, 0.2, 0.9) : m == 3 ? vec3(0.9, 0.8, 0.2) : m == 5 ? vec3(0.9, 0.3, 0.9)
             : m == 1 ? vec3(0.2, 0.7, 0.2) : m == 2 ? vec3(0.5, 0.4, 0.1) : vec3(0.3);
    float e0, e1, e2;
    weather_edges(s.pos.xz, cls, e0, e1, e2);
    c.rgb = mix(col, vec3(1.0, 0.0, 0.0), 1.0 - smoothstep(0.2, 1.0, e0));
    return;
  }
  // 0 buildings and props (the scene's regsSurfaceDiffuse darkens them), 6 water
  if (cls == 0 || cls == 6 || weather_wet.x + weather_wet.y <= 0.001) return;
  float keep = 1.0 - s.fog;  // the game's shader fogged the colour already: the effect fades with the fog
  // Wetting and drying are uniform: the film darkens a whole surface by one value, no pattern. Wet ground is the dry
  // colour a little darker and a little more saturated, the same hue (with the scene's regsSurfaceDiffuse 0.88 at
  // full wetness: hard surfaces about 0.8 of dry, grass and earth about 0.9). Pavements and squares drain and dry
  // sooner than the roads (their wetness is the film's to the power 1.5).
  float wet = clamp(weather_wet.x, 0.0, 1.0);
  if (cls == 1 || cls == 2) {  // grass, earth, fields: a little darker, no puddles
    c.rgb *= 1.0 - 0.03 * wet * keep;
    return;
  }
  // roads (4), pavement and squares (3), platforms (5); green texels (verges, sidewalk grass) darken like grass
  float green = smoothstep(0.015, 0.06, c.g - max(c.r, c.b));
  float hard = 1.0 - green;
  float up = smoothstep(0.85, 0.97, s.normal.y);
  bool road = cls == 4;
  float dEdge = 9.0, dLow = 9.0, dHigh = 9.0;
  if (s.dist < 450.0) weather_edges(s.pos.xz, cls, dEdge, dLow, dHigh);
  float gutter = road ? 1.0 - smoothstep(0.5, 1.6, dHigh) : 0.0;  // the road's strip along the kerb (puddles)
  float wetHere = road ? wet : pow(wet, 1.5);
  float k = wetHere * keep;
  float lum = dot(c.rgb, vec3(0.3, 0.59, 0.11));
  c.rgb = mix(vec3(lum), c.rgb, 1.0 + 0.15 * k * hard);           // a little more saturated, same hue
  c.rgb *= 1.0 - (0.03 * green + 0.09 * hard) * k;                // and darker
  // the sheen: subtle, slow (with the standing water, not the film), the surface's own colour, never a tint
  float sheen = smoothstep(0.2, 1.0, weather_wet.y);
  float f0 = 0.04 + 0.96 * pow(1.0 - max(dot(-s.view, s.normal), 0.0), 5.0);
  c.rgb *= 1.0 + 0.8 * f0 * sheen * up * hard * k;
  float light = 1.0 + 2.5 * weather_misc.x;                         // a lightning flash shows in the reflections
  vec3 overcast = regs_Fog.color.rgb;
  // puddles. The amount falls linearly while drying; the lanes and pavements follow its square (they dry first), the
  // gutter its square root (it dries last). Heavy rain lets them grow a little.
  float pud = weather_wet.y;
  if (pud <= 0.001) return;
  // the share of the ground under water: a few % in the lanes, up to half of the gutter strip, a few % of a pavement
  float share = road ? mix(0.05 * pud * pud, 0.45 * sqrt(pud), gutter) : 0.035 * pud * pud;
  share *= (1.0 + 0.3 * weather_wet.w) * up * hard;
  if (share <= 0.001) return;
  // per surface: its own noise, and nothing near its edge (the kerb line, a square's border)
  vec2 seed = road ? vec2(0.0) : vec2(53.7, 91.3) + float(cls) * vec2(11.0, 7.0);
  float n = weather_fbm(s.pos.xz * (road ? 0.6 : 0.8) + seed);
  float level = weather_level(share);
  float p = smoothstep(level, level + 0.035, n) * smoothstep(0.15, 0.45, dEdge) * (road ? 1.0 : smoothstep(0.5, 1.0, dLow));
  if (p <= 0.0) return;
  vec3 nrm = vec3(0.0, 1.0, 0.0);
  float ripple = weather_wet.z * clamp(1.0 - s.dist / 300.0, 0.0, 1.0);  // tiny far away: they would only shimmer
  if (ripple > 0.01) {
    vec2 slope = weather_ripples(s.pos.xz, s.time) * 0.25 * ripple * (1.0 + 0.6 * weather_wet.w);
    nrm = normalize(vec3(slope.x, 1.0, slope.y));
  }
  vec3 rr = reflect(s.view, nrm);
  float fr = 0.08 + 0.92 * pow(1.0 - max(dot(-s.view, nrm), 0.0), 5.0);
  vec3 refl = mix(texture(regs_EnvMap, rr).rgb, overcast, 0.7) * 0.75 * light;  // mostly the grey sky of the rain
  // dark still water with the sky in it: the reflection is what makes it read as water, the clamp in the hook keeps
  // it from lighting up the ground
  vec3 water = mix(c.rgb * 0.45, refl, clamp(0.3 + fr, 0.0, 0.85));
  water = min(water, c.rgb * 1.1);  // at most a touch lighter than the wet asphalt around it (still darker than dry)
  c.rgb = mix(c.rgb, water, p * keep);
}
