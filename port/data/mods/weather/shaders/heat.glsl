// Weather mod, summer heat: the shimmer of hot air over the ground. A port post hook (mod-manager.md §4.3): it runs
// per screen pixel on the finished map view while weather.script asks for it (gsScene.regsPostHooks, only on hot
// days), reads the scene a fraction of a pixel away along a slowly boiling turbulence field anchored on the ground, so
// edges (lane markings, kerbs, cars) wobble gently. Strongest over asphalt (roads, then pavements and platforms),
// weaker over grass, barely on walls; fading with distance. No change of colour or brightness: only displacement.
uniform vec4 weather_hot;   // x shimmer 0..1, y debug view (the strength as red)

void weather_heat(inout vec3 c, RegsPost p) {
  float amount = weather_hot.x;
  if (amount <= 0.001 || p.sky > 0.5) return;
  // how much hot air lies over this point: the surface below (the engine's surface map), up-facing ground most
  int cls = regs_surfaceAt(p.pos.xz);
  float hot = cls == 4 ? 1.0 : (cls == 3 || cls == 5) ? 0.75 : (cls == 1 || cls == 2) ? 0.35 : 0.45;
  vec3 n = cross(dFdx(p.pos), dFdy(p.pos));
  float up = length(n) > 1e-6 ? abs(normalize(n).y) : 1.0;
  hot *= 0.35 + 0.65 * smoothstep(0.5, 0.9, up);
  // a fraction of a pixel far away (it would only blur), up to about a pixel near the camera
  float near = 1.0 - smoothstep(250.0, 900.0, p.dist);
  float strength = amount * hot * near;
  if (weather_hot.y > 0.5) { c = mix(c, vec3(1.0, 0.0, 0.0), 0.6 * strength); return; }
  if (strength <= 0.002) return;
  // turbulence: cells of about a metre on the ground, two octaves, rising and churning (2-3 changes per second)
  vec2 g = p.pos.xz;
  float t = p.time;
  vec2 w = vec2(weather_noise(g * 0.9 + vec2(0.0, t * 1.9)), weather_noise(g * 1.1 + vec2(t * 1.6, 7.3))) - 0.5;
  w += 0.6 * (vec2(weather_noise(g * 2.3 + vec2(t * 3.1, 2.9)), weather_noise(g * 2.0 + vec2(5.1, -t * 2.7))) - 0.5);
  // hot air rises: the image wobbles more up and down than sideways
  vec2 off = w * vec2(1.2, 2.2) * strength * p.texel;
  c = regs_postColor(p.uv + off);
}
