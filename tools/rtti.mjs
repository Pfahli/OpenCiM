#!/usr/bin/env node
// rtti - recover MSVC x86 RTTI from GSystem.dll: class names, vtables, virtual-method addresses and base classes.
//
//   TypeDescriptor           +0 pVFTable(type_info)  +4 spare  +8 char name[]  (".?AVgsVariant@@")
//   CompleteObjectLocator    +0 signature=0  +4 offset  +8 cdOffset  +12 pTypeDescriptor  +16 pClassHierarchyDescriptor
//   ClassHierarchyDescriptor +0 signature  +4 attributes  +8 numBaseClasses  +12 pBaseClassArray
//   BaseClassDescriptor      +0 pTypeDescriptor  +4 numContainedBases  +8 mdisp  +12 pdisp  +16 vdisp  +20 attributes
//   vtable[-1] points at the COL; vtable entries are consecutive pointers into .text.
//
// Usage: node rtti.mjs <GSystem.dll> [out.json]

import fs from 'node:fs';

const [dll, out] = process.argv.slice(2);
const bin = fs.readFileSync(dll);
const BASE = 0x10000000;
// .text/.rdata/.data have file offset == RVA in this image
const SECT = { text: [0x1000, 0x156000], rdata: [0x156000, 0x18c000], data: [0x18c000, 0x194000] };
const inSect = (va, s) => va - BASE >= SECT[s][0] && va - BASE < SECT[s][1];
const u32 = (va) => bin.readUInt32LE(va - BASE);

function demangle(raw) {
  // ".?AVgsVariant@@" -> "gsVariant"; templates/namespaces are kept readable but not fully undecorated
  let s = raw.replace(/^\.\?A[VU]/, '').replace(/@@$/, '');
  if (!s.includes('?$')) return s.split('@').reverse().join('::');
  return s;
}

// 1. type descriptors
const tds = new Map();
const text = bin.toString('latin1');
for (const m of text.matchAll(/\.\?A[VU][A-Za-z0-9_@?$]+@@/g)) {
  const va = BASE + m.index - 8;
  tds.set(va, { raw: m[0], name: demangle(m[0]) });
}

// 2. complete object locators (scan .rdata, 4-aligned)
const cols = new Map();
for (let o = SECT.rdata[0]; o + 20 <= SECT.rdata[1]; o += 4) {
  if (bin.readUInt32LE(o) !== 0) continue;
  const td = bin.readUInt32LE(o + 12);
  const chd = bin.readUInt32LE(o + 16);
  if (tds.has(td) && inSect(chd, 'rdata')) {
    cols.set(BASE + o, { td, offset: bin.readUInt32LE(o + 4), chd });
  }
}

// 3. vtables: a pointer to a COL immediately followed by code pointers
const classes = new Map();
for (let o = SECT.rdata[0]; o + 8 <= SECT.rdata[1]; o += 4) {
  const col = cols.get(bin.readUInt32LE(o));
  if (!col) continue;
  const vt = BASE + o + 4;
  const methods = [];
  for (let p = vt; inSect(p, 'rdata') && inSect(u32(p), 'text'); p += 4) {
    if (p !== vt && cols.has(u32(p - 4 + 4)) ) break;
    methods.push(`0x${u32(p).toString(16)}`);
    if (cols.has(u32(p + 4))) break; // next vtable's COL pointer
  }
  if (!methods.length) continue;
  const td = tds.get(col.td);
  const c = classes.get(td.name) ?? { name: td.name, raw: td.raw, vtables: [], bases: [] };
  c.vtables.push({ address: `0x${vt.toString(16)}`, subobjectOffset: col.offset, methods });
  // 4. base classes from the class hierarchy descriptor (with their offset inside the object, mdisp)
  if (!c.bases.length) {
    const n = u32(col.chd + 8);
    const arr = u32(col.chd + 12);
    c.baseOffsets = {};
    for (let i = 1; i < n && i < 64; i++) {
      const bcd = u32(arr + i * 4);
      const btd = tds.get(u32(bcd));
      if (btd) {
        c.bases.push(btd.name);
        if (!(btd.name in c.baseOffsets)) c.baseOffsets[btd.name] = bin.readInt32LE(bcd - BASE + 8);
      }
    }
  }
  classes.set(td.name, c);
}

const list = [...classes.values()].sort((a, b) => a.name.localeCompare(b.name));
if (out) fs.writeFileSync(out, JSON.stringify(list, null, 1));
console.log(`type descriptors: ${tds.size}, object locators: ${cols.size}, classes with vtables: ${list.length}`);
const gs = list.filter((c) => c.name.startsWith('gs'));
console.log(`gs* classes: ${gs.length}`);
for (const c of gs.slice(0, 400)) {
  console.log(`${c.name.padEnd(34)} vt=${c.vtables[0].address} methods=${String(c.vtables[0].methods.length).padStart(3)}  bases: ${c.bases.join(' > ') || '-'}`);
}
