// Minimap reference check, step 2 (docs/ui/minimap.md). Re-implementation of gsGrid::renderMap (L:0x08143f60) + the incrementalUpdate counters (L:0x0813c2d0) in mode
// GRID_VIEW_DEFAULT, compared against a shipped minimap thumbnail (<map>.dds, written by main.script:276).
// usage: node minimap-check.mjs <templates.json> <map.grid> <map.dds> [diff.rgba]   env DRYOUT=1 applies the terrain
// dry-out first. The port does the same check in-engine: REGS_MINIMAP_CHECK=<map>,... (port/README).
import fs from 'node:fs';
import { Reader, parseGrid } from './gridparse.mjs';

const [tplFile, gridFile, ddsFile, pngOut] = process.argv.slice(2);
const tpl = JSON.parse(fs.readFileSync(tplFile));
const buf = fs.readFileSync(gridFile);
const r = new Reader(buf);
const [root] = r.chunks(0, buf.length);
const W = buf.readUInt32LE(root.body), L = buf.readUInt32LE(root.body + 4);
let terr = null;
for (const c of r.chunks(root.body + 24, root.end)) if (c.tag === 0x07010000) terr = c;
const flags = new Uint16Array(W * L), hgt = new Int16Array(W * L);
for (let i = 0; i < W * L; i++) { hgt[i] = buf.readInt16LE(terr.body + i * 4); flags[i] = buf.readUInt16LE(terr.body + i * 4 + 2); }
const net = parseGrid(buf).sections['0x07020000'];

if (process.env.DRYOUT === '1') {
  const H = (x, z) => hgt[Math.min(L - 1, z) * W + Math.min(W - 1, x)];
  for (let z = 0; z < L; z++) for (let x = 0; x < W; x++) {
    const i = z * W + x;
    if ((flags[i] & 0x2000) && Math.min(H(x, z), H(x + 1, z), H(x, z + 1), H(x + 1, z + 1)) >= -0x140) flags[i] &= ~0x2000;
  }
}

const N = 256 * 256;
const c7 = new Uint8Array(N), c8 = new Uint8Array(N), c9 = new Uint8Array(N), c10 = new Uint8Array(N),
  c11 = new Uint8Array(N), c12 = new Uint8Array(N), unsure = new Uint8Array(N);
const sx = Math.trunc(0x1000000 / W), sz = Math.trunc(0x1000000 / L);
const px = (x, z) => (((z * sz) >> 8) & 0xff00) + ((x * sx) >> 16);
for (let z = 0; z < L; z++) for (let x = 0; x < W; x++) {
  const f = flags[z * W + x], p = px(x, z);
  if (f & 0x2000) c9[p]++;
  else if (f & 0x20) c7[p]++;
  else if (f & 0x40) c10[p]++;
  else if (f & 0x08) c11[p]++;
}
let nItems = 0, nDiag = 0;
for (const it of net.items) {
  const t = tpl[net.types[it.type]];
  if (!t) continue;
  if (it.flags & 0x400000) continue;
  const w = it.orient & 1 ? t.length : t.width, l = it.orient & 1 ? t.width : t.length;
  const cx = it.x + (w >> 1), cz = it.z + (l >> 1);
  if (cx < 0 || cz < 0 || cx >= W || cz >= L) continue;
  nItems++;
  if (t.type === 0x13) {
    for (let z = Math.max(0, it.z - 2); z <= Math.min(L - 1, it.z + 2); z++)
      for (let x = Math.max(0, it.x - 2); x <= Math.min(W - 1, it.x + 2); x++) c8[px(x, z)]++;
    continue;
  }
  if (!(t.type < 0x13 || t.type === 0x14)) continue;
  const diag = t.diagonals !== 0 || t.generated;
  if (diag) nDiag++;
  for (let z = Math.max(0, it.z); z <= Math.min(L - 1, it.z + l - 1); z++)
    for (let x = Math.max(0, it.x); x <= Math.min(W - 1, it.x + w - 1); x++) {
      const p = px(x, z);
      if (diag) { unsure[p] = 1; continue; }
      if (t.type === 0x14) c11[p]++; else c12[p]++;
    }
}

const lerp = (a, b, t) => {   // gsInterpolate(gsColor, gsColor, uint) L:0x080ee4d0, 32-bit SWAR
  const dl = ((b & 0xff00ff) - (a & 0xff00ff)) >>> 0;
  const lo = ((Math.imul(dl, t) >>> 8) + (a & 0xff00ff)) >>> 0;
  const dh = ((((b & 0xff00ff00) >>> 8) - ((a & 0xff00ff00) >>> 8))) >>> 0;
  const hi = (Math.imul(dh, t) + ((a & 0xff00ff00) >>> 0)) >>> 0;
  return ((lo & 0xff00ff) | (hi & 0xff00ff00)) >>> 0;
};
const K = Math.trunc(0x10000 / (Math.trunc(W / 256) * Math.trunc(L / 256)));
const out = new Uint32Array(N);
let seed = 0;
for (let y = 0; y < 256; y++) for (let x = 0; x < 256; x++) {
  const p = y * 256 + x;
  seed = (Math.imul(seed, 0x343fd) + 0x269ec3) | 0;
  const n = (seed & 0xff0000) >> 16;
  const wt = (c, sh) => Math.min(255, (c * K) >> sh);
  let c = lerp(0xd0495226, 0xd0414c20, n);
  if (c7[p]) c = lerp(c, lerp(0xd070601a, 0xd0605010, n), wt(c7[p], 8));
  if (c8[p]) c = lerp(c, 0xd0202c0a, wt(c8[p], 9));
  if (c10[p]) c = lerp(c, 0xff92491c, wt(c10[p], 8));
  if (c9[p]) c = lerp(c, 0xd0295383, wt(c9[p], 8));
  if (c11[p]) c = lerp(c, 0xff42391c, wt(c11[p], 8));
  if (c12[p]) c = lerp(c, 0xff605d58, wt(c12[p], 8));
  out[p] = c;
}

const dds = fs.readFileSync(ddsFile);
const ref = new Uint32Array(N);
for (let i = 0; i < N; i++) ref[i] = dds.readUInt32LE(128 + i * 4);
const hex = (v) => '0x' + v.toString(16).padStart(8, '0');
let eq = 0, sure = 0, eqSure = 0, near = 0;
const bad = [];
for (let i = 0; i < N; i++) {
  if (out[i] === ref[i]) eq++;
  if (unsure[i]) continue;
  sure++;
  if (out[i] === ref[i]) eqSure++;
  else {
    const d = [0, 8, 16, 24].map((s) => Math.abs(((out[i] >>> s) & 255) - ((ref[i] >>> s) & 255)));
    if (Math.max(...d) <= 2) near++;
    if (bad.length < 12) bad.push([i % 256, i >> 8, hex(out[i]), hex(ref[i]), `c7=${c7[i]} c8=${c8[i]} c9=${c9[i]} c10=${c10[i]} c11=${c11[i]} c12=${c12[i]}`].join(' '));
  }
}
console.log(`${gridFile}: ${W}x${L}, items counted ${nItems} (diag/generated skipped ${nDiag})`);
console.log(`exact ${eq}/${N}; outside diagonal-item pixels: exact ${eqSure}/${sure}, off by <=2 per channel ${near}`);
console.log(bad.join('\n'));
if (pngOut) {
  // side-by-side diff dump as raw RGBA for inspection: ours | ref | mismatch mask
  const img = Buffer.alloc(768 * 256 * 4);
  for (let y = 0; y < 256; y++) for (let x = 0; x < 256; x++) {
    const i = y * 256 + x;
    for (const [k, v] of [[0, out[i]], [1, ref[i]], [2, out[i] === ref[i] ? 0xff000000 : unsure[i] ? 0xff808080 : 0xffff0000]]) {
      const o = (y * 768 + k * 256 + x) * 4;
      img[o] = (v >>> 16) & 255; img[o + 1] = (v >>> 8) & 255; img[o + 2] = v & 255; img[o + 3] = 255;
    }
  }
  fs.writeFileSync(pngOut, img);
}
