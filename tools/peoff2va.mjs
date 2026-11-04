#!/usr/bin/env node
// peoff2va - convert PE file offsets to virtual addresses (and print the section table).
// Usage: node peoff2va.mjs <file> [0xoffset ...]

import fs from 'node:fs';

const [file, ...offsets] = process.argv.slice(2);
const b = fs.readFileSync(file);
const pe = b.readUInt32LE(0x3c);
const sections = b.readUInt16LE(pe + 6);
const optSize = b.readUInt16LE(pe + 20);
const imageBase = b.readUInt32LE(pe + 24 + 28);
const table = [];
for (let i = 0, s = pe + 24 + optSize; i < sections; i++, s += 40) {
  table.push({
    name: b.toString('latin1', s, s + 8).replace(/\0+$/, ''),
    va: b.readUInt32LE(s + 12),
    vsize: b.readUInt32LE(s + 8),
    raw: b.readUInt32LE(s + 20),
    rawSize: b.readUInt32LE(s + 16),
  });
}
console.log(`imageBase=0x${imageBase.toString(16)}`);
for (const t of table) console.log(`${t.name.padEnd(8)} va=0x${(imageBase + t.va).toString(16)} raw=0x${t.raw.toString(16)} rawSize=0x${t.rawSize.toString(16)}`);
for (const o of offsets) {
  const off = Number(o);
  const t = table.find((s) => off >= s.raw && off < s.raw + s.rawSize);
  console.log(`${o} -> ${t ? `0x${(imageBase + t.va + off - t.raw).toString(16)} (${t.name})` : 'not in a section'}`);
}
