#!/usr/bin/env node
// pestrings - dump printable ASCII and UTF-16LE strings from a binary with their file offsets.
// Usage: node pestrings.mjs <file> [minLen=5] > out.txt

import fs from 'node:fs';

const [file, minArg] = process.argv.slice(2);
const min = Number(minArg ?? 5);
const buf = fs.readFileSync(file);
const out = [];
const isPrint = (c) => c >= 0x20 && c < 0x7f;

for (let i = 0, start = -1; i <= buf.length; i++) {
  if (i < buf.length && isPrint(buf[i])) {
    if (start < 0) start = i;
  } else if (start >= 0) {
    if (i - start >= min) out.push([start, 'a', buf.toString('latin1', start, i)]);
    start = -1;
  }
}
for (let i = 0, start = -1; i + 1 <= buf.length; i += 2) {
  if (i + 1 < buf.length && isPrint(buf[i]) && buf[i + 1] === 0) {
    if (start < 0) start = i;
  } else if (start >= 0) {
    if ((i - start) / 2 >= min) out.push([start, 'w', buf.toString('utf16le', start, i)]);
    start = -1;
  }
}
out.sort((a, b) => a[0] - b[0]);
for (const [off, kind, s] of out) console.log(`${off.toString(16).padStart(8, '0')} ${kind} ${s}`);
