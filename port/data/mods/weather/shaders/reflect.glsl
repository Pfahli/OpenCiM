// Weather mod, rain: wet ground mirrors the street. A port post hook (mod-manager.md §4.3) on the finished map view,
// after the surface hook (wet.glsl) has told, per pixel, how much its wet film mirrors, how much of it is puddle and
// how the rain's ripples tilt the puddle (regs_HookOut, read here with regs_postData; zero on everything that is not
// wet walkable ground: cars, people, trees, walls, grass). The mirrored ray is followed across the image (screen-space
// reflections): where it meets something drawn (a facade, a tram, a lamp, a tree) that shows in the ground, blurred
// and stretched along its upright like on a real wet road, sharp in the puddles; where it leaves the image or meets
// nothing but sky, the ground keeps the surface hook's own grey sky reflection. A film mirrors faintly, a puddle
// strongly; both fade out far away and at the image's edges, so nothing pops in or out.

// the mirrored ray from a point of wet ground: the image coordinate where it meets the scene and the distance it
// travelled (m); false when it leaves the image or passes behind everything
bool weather_trace(vec3 o, vec3 r, out vec2 hitUv, out float travel) {
  float t = 0.3, prev = 0.0;
  for (int i = 0; i < 22; ++i) {
    vec3 s = regs_postProject(o + r * t);
    if (s.z < 0.0 || s.z > 1.0 || any(lessThan(s.xy, vec2(0.0))) || any(greaterThan(s.xy, vec2(1.0)))) return false;
    float sd = regs_postDepth(s.xy);
    if (sd < 0.99999 && s.z > sd) {
      // behind what is drawn there: a hit when it is not too far behind it (else the ray passed behind a pole)
      vec3 q = o + r * t, w = regs_postWorld(s.xy);
      float behind = length(q - regs_HookEye) - length(w - regs_HookEye);
      if (behind > max(0.8, 1.5 * (t - prev))) return false;
      float lo = prev, hi = t;  // the crossing, more exactly
      for (int k = 0; k < 5; ++k) {
        float mid = 0.5 * (lo + hi);
        vec3 m = regs_postProject(o + r * mid);
        if (m.z > regs_postDepth(m.xy)) hi = mid; else lo = mid;
      }
      hitUv = regs_postProject(o + r * hi).xy;
      travel = hi;
      return true;
    }
    prev = t;
    t = t * 1.25 + 0.15;  // 0.3 m first, about 60 m after 20 steps
  }
  return false;
}

void weather_reflect(inout vec3 c, RegsPost p) {
  if (weather_wet.x + weather_wet.y <= 0.001 || p.sky > 0.5 || weather_misc.y > 0.5) return;
  vec4 d = regs_postData(p.uv);
  float film = d.x, pud = d.y;
  float near = 1.0 - smoothstep(350.0, 700.0, p.dist);  // far away the reflections would only flicker
  if (film + pud < 0.01 || near <= 0.0) {
    if (weather_misc.z > 0.5) c = mix(c, vec3(0.0, 0.0, 0.3), 0.6);
    return;
  }
  // the mirror: the ground's upright, tilted in the puddles by the ripples
  vec2 slope = (d.zw - 0.5) * step(0.05, pud);
  vec3 n = normalize(vec3(slope.x, 1.0, slope.y));
  vec3 r = reflect(p.view, n);
  if (r.y < 0.01) return;
  vec2 uv;
  float travel;
  bool hit = weather_trace(p.pos + n * 0.05, r, uv, travel);
  if (weather_misc.z > 0.5) {  // debug: film red, puddle green, a hit blue
    c = mix(c, vec3(film, pud, hit ? 1.0 : 0.0), 0.7);
    return;
  }
  // what the film mirrors where the ray finds nothing: the grey sky of the rain (as the puddles do in wet.glsl), so
  // open ground shines a little; a lightning flash shows in it
  vec3 sky = mix(texture(regs_EnvMap, r).rgb, regs_Fog.color.rgb, 0.7) * 0.75 * (1.0 + 2.5 * weather_misc.x);
  float fres = pow(1.0 - clamp(dot(-p.view, n), 0.0, 1.0), 5.0);
  float filmAmount = film * (1.0 - pud) * (0.3 + 0.6 * fres) * near;
  if (!hit) {
    c = mix(c, sky * 0.85, clamp(0.35 * filmAmount, 0.0, 0.9));  // the flat sky: only a touch (no glare)
    return;
  }
  // the blur: a cone about 3.5° wide on the film, 0.7° in a puddle, measured on the image at the hit along the
  // world's upright there (the streak of a wet road) and a third of that across
  float rough = mix(0.035, 0.01, smoothstep(0.1, 0.6, pud));
  vec3 hp = p.pos + r * travel;
  vec2 up = regs_postProject(hp + vec3(0.0, rough * travel, 0.0)).xy - uv;
  float len = length(up);
  vec2 along = len > 1e-6 ? up : vec2(0.0, p.texel.y);
  vec2 across = vec2(-along.y * p.texel.x / p.texel.y, along.x * p.texel.y / p.texel.x) * 0.33;  // square in pixels
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  for (int k = -3; k <= 3; ++k) {
    float f = float(k) / 3.0, w = exp(-2.0 * f * f);
    sum += w * regs_postColor(clamp(uv + along * f, vec2(0.0), vec2(1.0)));
    wsum += w;
  }
  sum += 0.6 * (regs_postColor(clamp(uv + across, vec2(0.0), vec2(1.0))) + regs_postColor(clamp(uv - across, vec2(0.0), vec2(1.0))));
  wsum += 1.2;
  vec3 seen = sum / wsum;
  // what was hit fades into the sky near the image's edges (the ray's end would jump in and out there) and over
  // long rays, so nothing pops in or out
  float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
  float sure = smoothstep(0.0, 0.08, edge) * (1.0 - smoothstep(30.0, 60.0, travel));
  // the film faintly (more at a glancing view), what was hit or the sky; water gives back a little less than it gets
  // a dark reflection (a wall in shade) only darkens a little, else the small puddles read as stains
  vec3 before = c;
  c = mix(c, mix(sky, seen, sure) * 0.85, clamp(filmAmount * mix(0.35, 1.0, sure), 0.0, 0.9));
  // a puddle strongly, what was hit (the sky in it is wet.glsl's)
  c = mix(c, seen * 0.85, clamp(pud * (0.45 + 0.4 * fres) * sure * near, 0.0, 0.9));
  c = max(c, before * 0.88);
}
