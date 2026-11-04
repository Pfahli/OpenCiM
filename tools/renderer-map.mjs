#!/usr/bin/env node
// renderer-map - mechanical map of GSystem's renderer interfaces (gsI*) and their OpenGL implementations (*_OGL).
//
// For every interface: its vtable slots, the implementing function in each *_OGL class (via RTTI base offsets),
// abstract slots (_purecall), and the OpenGL / WGL / extension calls each implementation makes (directly or in its
// direct callees). Extension calls go through function pointers filled by wglGetProcAddress("gl...").
//
// Usage: node renderer-map.mjs <rtti-classes.json> <functionsDir> [out.json]

import fs from 'node:fs';
import path from 'node:path';

const [rttiFile, dir, outFile] = process.argv.slice(2);
const rtti = JSON.parse(fs.readFileSync(rttiFile, 'utf8'));
const byName = new Map(rtti.map((c) => [c.name, c]));
const src = (a) => {
  const p = path.join(dir, `${a}.c`);
  return fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : null;
};

// 1. extension function pointers: DAT_x = wglGetProcAddress("glFoo")
const ext = new Map();
for (const f of fs.readdirSync(dir)) {
  const s = fs.readFileSync(path.join(dir, f), 'utf8');
  if (!s.includes('wglGetProcAddress')) continue;
  // the name argument may sit on the next line of the pseudocode
  for (const m of s.matchAll(/DAT_([0-9a-f]{8}) = (?:\([^)]*\))?wglGetProcAddress\(\s*(?:\([^)]*\))?"([A-Za-z0-9_]+)"\s*\)/g)) ext.set(m[1], m[2]);
}

// 2. _purecall = the most common target among slots of interface classes' own vtables
const counts = new Map();
for (const c of rtti.filter((x) => /^gsI[A-Z]/.test(x.name))) for (const a of c.vtables[0].methods) counts.set(a, (counts.get(a) ?? 0) + 1);
const purecall = [...counts.entries()].sort((a, b) => b[1] - a[1])[0]?.[0];

// GL usage of one function (+ direct callees)
const glCache = new Map();
function glCalls(addr, depth = 1) {
  const key = `${addr}/${depth}`;
  if (glCache.has(key)) return glCache.get(key);
  const s = src(addr);
  const out = new Set();
  if (s) {
    for (const m of s.matchAll(/\b((?:gl|wgl)[A-Z][A-Za-z0-9]*)\(/g)) out.add(m[1]);
    for (const m of s.matchAll(/\(\*(?:\([^)]*\))?DAT_([0-9a-f]{8})\)\(/g)) if (ext.has(m[1])) out.add(ext.get(m[1]));
    if (depth > 0) {
      const callees = [...new Set([...s.matchAll(/FUN_([0-9a-f]{8})\(/g)].map((m) => `0x${m[1]}`))].filter((c) => c !== addr);
      for (const c of callees.slice(0, 30)) for (const g of glCalls(c, depth - 1)) out.add(g);
    }
  }
  const res = [...out].sort();
  glCache.set(key, res);
  return res;
}
const size = (a) => {
  const s = src(a);
  if (!s) return null;
  return [...s.matchAll(/^\/\/ range: 0x([0-9a-f]+)-0x([0-9a-f]+)/gm)].reduce((n, m) => n + parseInt(m[2], 16) - parseInt(m[1], 16) + 1, 0);
};

// 3. interfaces = gsI* names that appear as bases of *_OGL classes (plus those with their own vtables)
const ogl = rtti.filter((c) => c.name.endsWith('_OGL'));
const ifaceNames = new Set(rtti.filter((c) => /^gsI[A-Z]/.test(c.name)).map((c) => c.name));
for (const c of ogl) for (const b of c.bases) if (/^gsI[A-Z]/.test(b)) ifaceNames.add(b);

const interfaces = [];
for (const name of [...ifaceNames].sort()) {
  const own = byName.get(name);
  const implementers = ogl.filter((c) => c.baseOffsets && name in c.baseOffsets).map((c) => {
    const off = c.baseOffsets[name];
    const vt = c.vtables.find((v) => v.subobjectOffset === off);
    return { cls: c.name, offset: off, vtable: vt?.address, methods: vt?.methods ?? [] };
  });
  const slotCount = own ? own.vtables[0].methods.length : Math.max(0, ...implementers.map((i) => i.methods.length));
  const slots = [];
  for (let i = 0; i < slotCount; i++) {
    const declared = own?.vtables[0].methods[i];
    const impls = {};
    for (const im of implementers) {
      const a = im.methods[i];
      if (!a) continue;
      impls[im.cls] = { address: a, bytes: size(a), gl: a === purecall ? [] : glCalls(a) };
    }
    slots.push({ slot: i, offset: `0x${(i * 4).toString(16)}`, abstract: declared === purecall, declared: declared ?? null, impls });
  }
  interfaces.push({ name, bases: own?.bases ?? byName.get(implementers[0]?.cls)?.bases.filter((b) => b !== name) ?? [], ownVtable: own?.vtables[0].address ?? null, implementers: implementers.map((i) => i.cls), slots });
}

const allGl = new Map();
for (const f of fs.readdirSync(dir)) {
  const s = fs.readFileSync(path.join(dir, f), 'utf8');
  for (const m of s.matchAll(/\b((?:gl|wgl)[A-Z][A-Za-z0-9]*)\(/g)) allGl.set(m[1], (allGl.get(m[1]) ?? 0) + 1);
  for (const m of s.matchAll(/\(\*(?:\([^)]*\))?DAT_([0-9a-f]{8})\)\(/g)) if (ext.has(m[1])) allGl.set(ext.get(m[1]), (allGl.get(ext.get(m[1])) ?? 0) + 1);
}

const result = { purecall, extensions: Object.fromEntries([...ext.entries()].map(([k, v]) => [v, `DAT_${k}`])), glUsage: Object.fromEntries([...allGl.entries()].sort()), interfaces };
if (outFile) fs.writeFileSync(outFile, JSON.stringify(result, null, 1));
console.log(`purecall=${purecall} extensions=${ext.size} distinct GL/WGL/extension functions used=${allGl.size}`);
for (const i of interfaces) {
  const abstract = i.slots.filter((s) => s.abstract).length;
  console.log(`${i.name.padEnd(22)} slots=${String(i.slots.length).padStart(2)} abstract=${String(abstract).padStart(2)} impl=${i.implementers.join(',') || '-'}`);
}
