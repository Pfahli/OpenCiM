#!/usr/bin/env node
// codeptrs - estimate code that Ghidra's auto-analysis never turned into functions: scan .rdata/.data for aligned
// pointers into .text, and report targets that are neither a decompiled function entry nor inside a known body.
// Many are vtable methods (C++ virtual functions only reachable indirectly). Jump-table and SEH targets that lie
// inside known function bodies are excluded using the body ranges in decompile-index.json.
//
// Usage: node codeptrs.mjs <GSystem.dll> <decompile-index.json | functionsDir> [out.txt]
//        (a functions directory is read from each file's "// address:" and "// range:" header lines)

import fs from 'node:fs';

const [dll, indexFile, out] = process.argv.slice(2);
const bin = fs.readFileSync(dll);
const BASE = 0x10000000;
const TEXT = [0x10001000, 0x10156000];
const DATA = [[0x156000, 0x18c000], [0x18c000, 0x194000]]; // .rdata, .data file ranges

const entries = new Set();
const ranges = [];
if (fs.statSync(indexFile).isDirectory()) {
  for (const name of fs.readdirSync(indexFile)) {
    const head = fs.readFileSync(`${indexFile}/${name}`, 'utf8').slice(0, 4096);
    const a = head.match(/^\/\/ address: (0x[0-9a-f]+)/m);
    if (a) entries.add(parseInt(a[1], 16));
    for (const m of head.matchAll(/^\/\/ range: (0x[0-9a-f]+)-(0x[0-9a-f]+)/gm)) ranges.push([parseInt(m[1], 16), parseInt(m[2], 16)]);
  }
} else {
  const index = JSON.parse(fs.readFileSync(indexFile, 'utf8'));
  for (const f of index.functions ?? []) {
    const a = typeof f.address === 'string' && f.address.startsWith('0x') ? parseInt(f.address, 16) : NaN;
    if (!Number.isNaN(a)) entries.add(a);
    for (const r of f.body?.ranges ?? []) {
      const s = parseInt(r.start, 16), e = parseInt(r.end, 16);
      if (!Number.isNaN(s) && !Number.isNaN(e)) ranges.push([s, e]);
    }
  }
}
ranges.sort((x, y) => x[0] - y[0]);
const inBody = (a) => {
  let lo = 0, hi = ranges.length - 1;
  while (lo <= hi) {
    const m = (lo + hi) >> 1;
    if (a < ranges[m][0]) hi = m - 1;
    else if (a > ranges[m][1]) lo = m + 1;
    else return true;
  }
  return false;
};

const targets = new Map();
for (const [s, e] of DATA) {
  for (let o = s; o + 4 <= e; o += 4) {
    const v = bin.readUInt32LE(o);
    if (v >= TEXT[0] && v < TEXT[1]) targets.set(v, (targets.get(v) ?? 0) + 1);
  }
}
const missing = [...targets.keys()].filter((a) => !entries.has(a) && !inBody(a)).sort((a, b) => a - b);
console.log(`function entries: ${entries.size}, body ranges: ${ranges.length}`);
console.log(`distinct code pointers in .rdata/.data: ${targets.size}`);
console.log(`not a known entry and not inside a known body: ${missing.length}`);
if (out) fs.writeFileSync(out, missing.map((a) => `0x${a.toString(16)}`).join('\n') + '\n');
