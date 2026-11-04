#!/usr/bin/env node
// assetparse - decode Cities in Motion 3D assets (chunk family 0x02) and export them.
//
// Layouts: docs/asset-format/mesh-material.md (meshes, materials) and skeleton-animation-scene.md.
// Right-handed, +Y up, metres, CCW front faces; texture V is stored negated (glTF v = v + 1).
//
// Usage:
//   node assetparse.mjs check <gameDir>                          parse every shipped .object/.material, report issues
//   node assetparse.mjs export <file.object> <out.glb> [out.png]  convert one mesh (render LOD 0) to glTF (+ preview)
//   node assetparse.mjs info <file.object|file.material>          print decoded structure as JSON

import fs from 'node:fs';
import path from 'node:path';
import { Reader } from './gridparse.mjs';
import { readArchive, readEntry } from './gstool.mjs';
import { renderPreview, writeGLB } from './meshpreview.mjs';

const hex = (n) => `0x${(n >>> 0).toString(16).padStart(8, '0')}`;
const MESH_TAGS = { 0x02010000: 'render', 0x02020000: 'collision', 0x02030000: 'hit', 0x02040000: 'lod1', 0x02050000: 'lod2', 0x02060000: 'lod3', 0x02070000: 'unflagged' };
const SLOT_TAGS = { 0x02013110: 'diffuse', 0x02013120: 'normal', 0x02013130: 'specular', 0x02013140: 'illumination', 0x02013150: 'ambientOcclusion' };
const cstr = (b, p, n) => { const e = b.indexOf(0, p); return b.toString('latin1', p, e < 0 || e > p + n ? p + n : e); };

function parseMaterial(r, c, issues) {
  const b = r.buf, p = c.body;
  const m = {
    name: cstr(b, p, 32), twoSided: !!b[p + 34], opacity: b[p + 35], specularLevel: b[p + 36], glossiness: b[p + 37],
    diffuse: [...b.subarray(p + 44, p + 48)], firstVertex: b.readUInt32LE(p + 56), vertexCount: b.readUInt32LE(p + 60),
    firstIndex: b.readUInt32LE(p + 64), indexCount: b.readUInt32LE(p + 68), slots: {},
  };
  if (c.size < 72) { issues.push(`material ${m.name}: ${c.size} < 72 bytes`); return m; }
  for (const s of r.chunks(p + 72, c.end)) {
    const slot = SLOT_TAGS[s.tag];
    if (!slot) { issues.push(`material ${m.name}: unknown child ${hex(s.tag)}`); continue; }
    if (s.size < 40) { issues.push(`slot ${slot}: header < 40 bytes`); continue; }
    m.slots[slot] = { disabled: !!b[s.body], alphaUsage: b[s.body + 2], alphaBits: b[s.body + 3], texture: cstr(b, s.body + 8, 32) };
    for (const src of r.chunks(s.body + 40, s.end)) if (src.tag !== 0x02013111 && src.tag !== 0x02013112) issues.push(`slot ${slot}: unknown child ${hex(src.tag)}`);
  }
  return m;
}

function parseMesh(r, c, issues) {
  const b = r.buf;
  const mesh = { kind: MESH_TAGS[c.tag], vertexCount: 0, indexCount: 0, materials: [] };
  for (const k of r.chunks(c.body, c.end)) {
    if (k.tag === 0x02011000) {
      const n = b.readUInt32LE(k.body);
      mesh.vertexCount = n;
      const end = k.body + 4 + 36 * n;
      if (end > k.end) { issues.push(`vertex array overruns chunk (${n} vertices)`); continue; }
      mesh.vertexOffset = k.body + 4;
      for (const s of r.chunks(end, k.end)) {
        if (s.tag === 0x02011100) {
          const inf = b.readUInt16LE(s.body);
          if (s.size !== 4 + 8 * n * inf) issues.push(`skin size ${s.size} != 4+8*${n}*${inf}`);
          mesh.skin = { influences: inf, offset: s.body + 4 };
        } else if (s.tag === 0x02011200) {
          if (b.readUInt16LE(s.body) !== 2) issues.push('uv2 type != 2');
          if (s.size !== 4 + 8 * n) issues.push(`uv2 size ${s.size} != 4+8*${n}`);
          mesh.uv2Offset = s.body + 4;
        } else issues.push(`unknown vertex child ${hex(s.tag)}`);
      }
    } else if (k.tag === 0x02012000) {
      const n = b.readUInt32LE(k.body);
      if (k.size !== 4 + 4 * n) issues.push(`index chunk size ${k.size} != 4+4*${n}`);
      if (n % 3) issues.push(`index count ${n} not a multiple of 3`);
      mesh.indexCount = n;
      mesh.indexOffset = k.body + 4;
    } else if (k.tag === 0x02013000) {
      const n = b.readUInt32LE(k.body);
      const mats = r.chunks(k.body + 4, k.end);
      if (mats.length !== n) issues.push(`material list declares ${n}, holds ${mats.length}`);
      for (const m of mats) mesh.materials.push(parseMaterial(r, m, issues));
    } else issues.push(`unknown mesh child ${hex(k.tag)}`);
  }
  // index range and submesh coverage
  for (let i = 0; i < mesh.indexCount; i++) {
    if (b.readUInt32LE(mesh.indexOffset + 4 * i) >= mesh.vertexCount) { issues.push('index out of range'); break; }
  }
  let next = 0;
  for (const m of mesh.materials) {
    if (m.firstIndex !== next) issues.push(`submesh ${m.name}: firstIndex ${m.firstIndex}, expected ${next}`);
    next = m.firstIndex + m.indexCount;
  }
  if (mesh.materials.length && next !== mesh.indexCount) issues.push(`submesh ranges cover ${next} of ${mesh.indexCount} indices`);
  return mesh;
}

export function parseObject(buf) {
  const r = new Reader(buf);
  const issues = [];
  const [root, ...extra] = r.chunks(0, buf.length);
  if (root.tag !== 0x02000000) throw new Error(`not an object (root ${hex(root.tag)})`);
  if (extra.length) issues.push('trailing data after root chunk');
  const b = buf, h = root.body;
  const obj = {
    name: cstr(b, h, 32), position: [0, 1, 2].map((i) => b.readFloatLE(h + 32 + 4 * i)),
    rotation: [0, 1, 2, 3].map((i) => b.readFloatLE(h + 44 + 4 * i)), meshes: [], locators: [], cameras: 0, issues,
  };
  for (const c of r.chunks(h + 60, root.end)) {
    if (MESH_TAGS[c.tag]) obj.meshes.push(parseMesh(r, c, issues));
    else if (c.tag === 0x02600000) obj.locators.push({ name: cstr(b, c.body + 4, 32) });
    else if (c.tag === 0x02700000) obj.cameras++;
    else issues.push(`unknown object child ${hex(c.tag)}`);
  }
  obj.reader = r;
  return obj;
}

export function parseMaterialFile(buf) {
  const r = new Reader(buf);
  const issues = [];
  const [root, ...extra] = r.chunks(0, buf.length);
  if (root.tag !== 0x02013100) throw new Error(`not a material (root ${hex(root.tag)})`);
  if (extra.length) issues.push('trailing data after root chunk');
  return { ...parseMaterial(r, root, issues), issues };
}

// ---- skeleton 0x02200000 / animation 0x02300000 (docs/asset-format/skeleton-animation-scene.md) ----
// bone: name[32], i32 parent (-1 root, always < own index), f32 t[3], s[3], q[4] (x,y,z,w; glTF rotation = conjugate)
export function parseSkeleton(buf) {
  const r = new Reader(buf), b = buf, issues = [];
  const [root] = r.chunks(0, buf.length);
  if (root.tag !== 0x02200000) throw new Error(`not a skeleton (root ${hex(root.tag)})`);
  const boneCount = b.readUInt32LE(root.body + 32);
  const bones = [];
  for (const c of r.chunks(root.body + 36, root.end)) {
    if (c.tag !== 0x02201000) { issues.push(`unknown skeleton child ${hex(c.tag)}`); continue; }
    const p = c.body;
    const bone = { name: cstr(b, p, 32), parent: b.readInt32LE(p + 32), t: [0, 1, 2].map((i) => b.readFloatLE(p + 36 + 4 * i)),
      s: [0, 1, 2].map((i) => b.readFloatLE(p + 48 + 4 * i)), q: [0, 1, 2, 3].map((i) => b.readFloatLE(p + 60 + 4 * i)) };
    if (bone.parent >= bones.length || bone.parent < -1) issues.push(`bone ${bone.name}: parent ${bone.parent} not before it`);
    for (const k of r.chunks(p + 76, c.end)) if (k.tag !== 0x02201010 || k.size !== 32) issues.push(`bone ${bone.name}: unexpected child ${hex(k.tag)}`);
    bones.push(bone);
  }
  if (bones.length !== boneCount) issues.push(`boneCount ${boneCount} but ${bones.length} bones`);
  return { name: cstr(b, root.body, 32), bones, issues };
}

// take: name[32], u32 trackCount, f32 duration (s); track: bone name[32], u32 keyCount; key (40 B): t[3], s[3], q[4]
// keys carry no time: key i is at i*duration/(n-1)
export function parseAnimation(buf) {
  const r = new Reader(buf), b = buf, issues = [];
  const [root] = r.chunks(0, buf.length);
  if (root.tag !== 0x02300000) throw new Error(`not an animation (root ${hex(root.tag)})`);
  const take = { name: cstr(b, root.body, 32), declaredTracks: b.readUInt32LE(root.body + 32), duration: b.readFloatLE(root.body + 36), tracks: [] };
  let worstQuat = 0;
  for (const c of r.chunks(root.body + 40, root.end)) {
    if (c.tag !== 0x02301000) { issues.push(`unknown take child ${hex(c.tag)}`); continue; }
    const keyCount = b.readUInt32LE(c.body + 32);
    const keys = r.chunks(c.body + 36, c.end);
    if (keys.length !== keyCount) issues.push(`track ${cstr(b, c.body, 32)}: ${keys.length} keys, header says ${keyCount}`);
    for (const k of keys) {
      if (k.tag !== 0x02301010 || k.size !== 40) { issues.push(`bad key chunk ${hex(k.tag)}/${k.size}`); break; }
      const q = [0, 1, 2, 3].map((i) => b.readFloatLE(k.body + 24 + 4 * i));
      worstQuat = Math.max(worstQuat, Math.abs(Math.hypot(...q) - 1));
    }
    take.tracks.push({ bone: cstr(b, c.body, 32), keys: keyCount });
  }
  if (!(take.duration > 0)) issues.push(`non-positive duration ${take.duration}`);
  return { ...take, worstQuatError: worstQuat, issues };
}

// Geometry of one mesh as typed arrays (positions, normals, uvs with V un-negated, indices)
export function meshGeometry(obj, mesh) {
  const b = obj.reader.buf, n = mesh.vertexCount;
  const positions = new Float32Array(3 * n), normals = new Float32Array(3 * n), uvs = new Float32Array(2 * n);
  for (let i = 0; i < n; i++) {
    const p = mesh.vertexOffset + 36 * i;
    for (let k = 0; k < 3; k++) { positions[3 * i + k] = b.readFloatLE(p + 4 * k); normals[3 * i + k] = b.readFloatLE(p + 12 + 4 * k); }
    uvs[2 * i] = b.readFloatLE(p + 28);
    uvs[2 * i + 1] = b.readFloatLE(p + 32) + 1;
  }
  const indices = new Uint32Array(mesh.indexCount);
  for (let i = 0; i < mesh.indexCount; i++) indices[i] = b.readUInt32LE(mesh.indexOffset + 4 * i);
  return { positions, normals, uvs, indices };
}

function check(gameDir) {
  const totals = { objects: 0, materials: 0, skeletons: 0, bones: 0, animations: 0, tracks: 0, keys: 0, meshes: 0, skinned: 0, vertices: 0, triangles: 0, filesWithIssues: 0 };
  const byKind = {};
  const failures = [];
  let worstQuat = 0, animsBoundToFolderSkeleton = 0, animsWithFolderSkeleton = 0;
  for (const f of fs.readdirSync(gameDir).filter((x) => x.endsWith('.gs')).sort()) {
    const ar = readArchive(path.join(gameDir, f));
    const skeletonBonesByDir = new Map();
    const anims = [];
    for (const e of ar.entries) {
      const ext = path.extname(e.name).toLowerCase();
      if (!['.object', '.material', '.skeleton', '.animation'].includes(ext)) continue;
      let issues;
      try {
        const buf = readEntry(ar, e);
        if (ext === '.skeleton') {
          const s = parseSkeleton(buf);
          totals.skeletons++;
          totals.bones += s.bones.length;
          for (const bn of s.bones) worstQuat = Math.max(worstQuat, Math.abs(Math.hypot(...bn.q) - 1));
          const dir = path.posix.dirname(e.name);
          skeletonBonesByDir.set(dir, new Set([...(skeletonBonesByDir.get(dir) ?? []), ...s.bones.map((x) => x.name)]));
          issues = s.issues;
        } else if (ext === '.animation') {
          const a = parseAnimation(buf);
          totals.animations++;
          totals.tracks += a.tracks.length;
          totals.keys += a.tracks.reduce((s, t) => s + t.keys, 0);
          worstQuat = Math.max(worstQuat, a.worstQuatError);
          anims.push({ dir: path.posix.dirname(e.name), bones: a.tracks.map((t) => t.bone) });
          issues = a.issues;
        } else if (ext === '.object') {
          const o = parseObject(buf);
          totals.objects++;
          for (const m of o.meshes) {
            totals.meshes++;
            byKind[m.kind] = (byKind[m.kind] ?? 0) + 1;
            if (m.skin) totals.skinned++;
            totals.vertices += m.vertexCount;
            totals.triangles += m.indexCount / 3;
          }
          issues = o.issues;
        } else {
          totals.materials++;
          issues = parseMaterialFile(buf).issues;
        }
      } catch (err) {
        issues = [err.message];
      }
      if (issues.length) {
        totals.filesWithIssues++;
        if (failures.length < 15) failures.push(`${f}:${e.name}: ${issues[0]}`);
      }
    }
    // do the tracks of each animation name bones of a skeleton in the same folder?
    for (const a of anims) {
      const bones = skeletonBonesByDir.get(a.dir);
      if (!bones) continue;
      animsWithFolderSkeleton++;
      if (a.bones.every((b) => bones.has(b))) animsBoundToFolderSkeleton++;
    }
  }
  console.log(JSON.stringify({ ...totals, meshKinds: byKind, worstQuaternionNormError: +worstQuat.toExponential(2),
    animationsFullyBoundToFolderSkeleton: `${animsBoundToFolderSkeleton}/${animsWithFolderSkeleton}`, sampleFailures: failures }, null, 1));
}

function main(argv) {
  const [cmd, ...a] = argv;
  if (cmd === 'check') check(a[0]);
  else if (cmd === 'info') {
    const buf = fs.readFileSync(a[0]);
    const res = buf.readUInt32LE(0) === 0x02013100 ? parseMaterialFile(buf) : parseObject(buf);
    delete res.reader;
    console.log(JSON.stringify(res, (k, v) => (k.endsWith('Offset') ? undefined : v), 1));
  } else if (cmd === 'export') {
    const obj = parseObject(fs.readFileSync(a[0]));
    const mesh = obj.meshes.find((m) => m.kind === 'render') ?? obj.meshes[0];
    const g = meshGeometry(obj, mesh);
    fs.writeFileSync(a[1], writeGLB({ ...g, name: obj.name }));
    if (a[2]) {
      const r = renderPreview(g.positions, g.indices, { size: 512 });
      fs.writeFileSync(a[2], r.png);
      console.log(JSON.stringify({ name: obj.name, ...r.stats, issues: obj.issues }));
    }
  } else {
    console.error('usage: assetparse.mjs check <gameDir> | export <file.object> <out.glb> [out.png] | info <file>');
    process.exit(2);
  }
}

if (process.argv[1]?.endsWith('assetparse.mjs')) main(process.argv.slice(2));
