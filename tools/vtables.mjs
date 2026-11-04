#!/usr/bin/env node
// vtables - for GSystem.dll variant classes: find the vtable a constructor installs (last "&PTR_FUN_x" store in its
// Ghidra pseudocode) and decode trivial methods such as `mov eax, imm32; ret` (type-id getters).
//
// Usage: node vtables.mjs <GSystem.dll> <functionsDir> <ctorAddr ...>   (addresses as 0x1000a560)
//        node vtables.mjs <GSystem.dll> <functionsDir> --vt <vtableAddr ...>

import fs from 'node:fs';
import path from 'node:path';

const [dll, dir, ...rest] = process.argv.slice(2);
const bin = fs.readFileSync(dll);
const BASE = 0x10000000; // .text/.rdata file offset == RVA for GSystem.dll
const u32 = (va) => bin.readUInt32LE(va - BASE);

// Decode tiny method bodies: xor eax,eax; ret | mov eax,imm32; ret | mov al,imm8; ret
function trivial(va) {
  const o = va - BASE;
  if (bin[o] === 0x33 && bin[o + 1] === 0xc0 && bin[o + 2] === 0xc3) return 'return 0';
  if (bin[o] === 0xb8 && bin[o + 5] === 0xc3) return `return ${bin.readUInt32LE(o + 1)}`;
  if (bin[o] === 0xb0 && bin[o + 2] === 0xc3) return `return (char)${bin[o + 1]}`;
  if (bin[o] === 0xc3) return 'ret';
  return null;
}

function describeVtable(vt, slots = 30) {
  const rows = [];
  for (let i = 0; i < slots; i++) {
    const fn = u32(vt + i * 4);
    if (fn < 0x10001000 || fn >= 0x10156000) break;
    const file = path.join(dir, `0x${fn.toString(16)}.c`);
    rows.push({ slot: `0x${(i * 4).toString(16)}`, fn: `0x${fn.toString(16)}`, decompiled: fs.existsSync(file), trivial: trivial(fn) });
  }
  return rows;
}

const vtOnly = rest[0] === '--vt';
for (const a of vtOnly ? rest.slice(1) : rest) {
  let vt = Number(a);
  if (!vtOnly) {
    const src = fs.readFileSync(path.join(dir, `${a}.c`), 'utf8');
    const stores = [...src.matchAll(/=\s*&PTR_(?:FUN|LAB)_([0-9a-f]{8})/g)].map((m) => parseInt(m[1], 16));
    if (!stores.length) { console.log(`${a}: no vtable store found`); continue; }
    vt = stores[stores.length - 1];
  }
  const rows = describeVtable(vt);
  const typeId = rows[6]?.trivial ?? '?';
  const missing = rows.filter((r) => !r.decompiled).length;
  console.log(`${a}: vtable 0x${vt.toString(16)}  typeId(vt+0x18)=${typeId}  slots=${rows.length}  undecompiled=${missing}`);
}
