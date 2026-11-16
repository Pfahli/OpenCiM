#!/usr/bin/env node
// Makes the photo textures of the procedural palaces and castles (engine/proc_buildings.cpp) from CC0 textures of Poly
// Haven (polyhaven.com, CC0 1.0: public domain; credited in data/regs/textures/proc/CREDITS.txt). Owner 2026-11-16:
// "search the internet for cc0 pictures for textures".
//
//   1. download the 1k diffuse JPGs of the ids below into one folder:
//      https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/<id>/<id>_diff_1k.jpg
//   2. node tools/make-proc-textures.mjs <folder> [--ffmpeg <ffmpeg.exe>] [--out data/regs/textures/proc]
//
// ffmpeg decodes and scales each to 512 x 512; then optionally a blur, a tint (the photo's luminance relief times a
// colour: the plaster tints) or per-channel factors; written as an uncompressed 32-bit DDS with mipmaps, bottom row
// first (the procedural textures' convention: v = 0 at the bottom).
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';

const args = process.argv.slice(2);
const src = args.find((a) => !a.startsWith('--') && args[args.indexOf(a) - 1] !== '--ffmpeg' && args[args.indexOf(a) - 1] !== '--out');
const opt = (k, d) => (args.includes(k) ? args[args.indexOf(k) + 1] : d);
const ffmpeg = opt('--ffmpeg', 'ffmpeg');
const out = opt('--out', path.join(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..', 'data', 'regs', 'textures', 'proc'));
if (!src) {
  console.error('usage: node tools/make-proc-textures.mjs <folder with the JPGs> [--ffmpeg path] [--out dir]');
  process.exit(2);
}

// name, Poly Haven id, mode (photo | tint | blur-tint), colour (tint) or channel factors (photo), size (512 when left out;
// the smooth plasters and the blurred sandstone 256)
const jobs = [
  ['palace-plaster-1', 'beige_wall_001', 'tint', [0.95, 0.87, 0.64], 256],  // pale yellow
  ['palace-plaster-2', 'beige_wall_001', 'tint', [0.93, 0.73, 0.67], 256],  // salmon pink
  ['palace-plaster-3', 'beige_wall_001', 'tint', [0.93, 0.92, 0.88], 256],  // white
  ['palace-rustica', 'red_sandstone_wall', 'photo', [1.0, 0.97, 0.95]],
  ['palace-sandstone', 'red_sandstone_wall', 'blur-tint', [0.78, 0.50, 0.42], 256],  // the red Buntsandstein of the surrounds
  ['slate', 'grey_roof_tiles_02', 'photo', [0.92, 0.95, 1.0]],
  ['castle-stone', 'castle_wall_slates', 'photo', [1.0, 1.0, 1.0]],
  ['roof-tiles-red', 'clay_roof_tiles_02', 'photo', [1.0, 1.0, 1.0]],
];
fs.mkdirSync(out, { recursive: true });
for (const [name, id, mode, col, size0] of jobs) {
  const N = size0 || 512;
  const file = path.join(src, `${id}_diff_1k.jpg`);
  if (!fs.existsSync(file)) {
    console.warn(`missing ${file}`);
    continue;
  }
  const raw = execFileSync(ffmpeg, ['-loglevel', 'error', '-i', file, '-vf', `scale=${N}:${N}:flags=lanczos`, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']);
  let f = new Float32Array(N * N * 3);
  for (let i = 0; i < N * N * 3; ++i) f[i] = raw[i] / 255;
  if (mode === 'photo') {
    for (let i = 0; i < N * N; ++i) for (let c = 0; c < 3; ++c) f[i * 3 + c] *= col[c];
  } else {
    let l = new Float32Array(N * N);
    for (let i = 0; i < N * N; ++i) l[i] = 0.3 * f[i * 3] + 0.59 * f[i * 3 + 1] + 0.11 * f[i * 3 + 2];
    if (mode === 'blur-tint')  // a soft box blur (radius 3, twice, wrapping): the stone's grain without its joints
      for (let pass = 0; pass < 2; ++pass) {
        const t = new Float32Array(N * N);
        for (let y = 0; y < N; ++y)
          for (let x = 0; x < N; ++x) {
            let s = 0;
            for (let k = -3; k <= 3; ++k) s += l[y * N + ((x + k + N) % N)];
            t[y * N + x] = s / 7;
          }
        for (let y = 0; y < N; ++y)
          for (let x = 0; x < N; ++x) {
            let s = 0;
            for (let k = -3; k <= 3; ++k) s += t[((y + k + N) % N) * N + x];
            l[y * N + x] = s / 7;
          }
      }
    let mean = 0;
    for (let i = 0; i < N * N; ++i) mean += l[i];
    mean /= N * N;
    for (let i = 0; i < N * N; ++i) {  // the relief around the mean, times the tint
      const k = 1 + (l[i] - mean) * 1.6;
      for (let c = 0; c < 3; ++c) f[i * 3 + c] = col[c] * k;
    }
  }
  // the levels, box filtered; each bottom row first, BGRA
  const levels = [];
  let size = N;
  for (;;) {
    const lv = Buffer.alloc(size * size * 4);
    for (let y = 0; y < size; ++y)
      for (let x = 0; x < size; ++x) {
        const si = (y * size + x) * 3, di = ((size - 1 - y) * size + x) * 4;
        const b8 = (v) => Math.max(0, Math.min(255, Math.round(v * 255)));
        lv[di] = b8(f[si + 2]), lv[di + 1] = b8(f[si + 1]), lv[di + 2] = b8(f[si]), lv[di + 3] = 255;
      }
    levels.push(lv);
    if (size === 1) break;
    const ns = size / 2, next = new Float32Array(ns * ns * 3);
    for (let y = 0; y < ns; ++y)
      for (let x = 0; x < ns; ++x)
        for (let c = 0; c < 3; ++c)
          next[(y * ns + x) * 3 + c] = (f[(2 * y * size + 2 * x) * 3 + c] + f[(2 * y * size + 2 * x + 1) * 3 + c] +
                                        f[((2 * y + 1) * size + 2 * x) * 3 + c] + f[((2 * y + 1) * size + 2 * x + 1) * 3 + c]) / 4;
    f = next, size = ns;
  }
  const h = Buffer.alloc(128);
  h.write('DDS ', 0, 'latin1');
  h.writeUInt32LE(124, 4), h.writeUInt32LE(0x21007, 8), h.writeUInt32LE(N, 12), h.writeUInt32LE(N, 16);
  h.writeUInt32LE(levels.length, 28);
  h.writeUInt32LE(32, 76), h.writeUInt32LE(0x41, 80), h.writeUInt32LE(32, 88);
  h.writeUInt32LE(0x00ff0000, 92), h.writeUInt32LE(0x0000ff00, 96), h.writeUInt32LE(0x000000ff, 100), h.writeUInt32LE(0xff000000, 104);
  h.writeUInt32LE(0x401008, 108);
  const target = path.join(out, `${name}.dds`);
  fs.writeFileSync(target, Buffer.concat([h, ...levels]));
  console.log(`texture: ${target} (${levels.length} levels)`);
}
