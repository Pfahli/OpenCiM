#!/usr/bin/env node
// gsverify - inflate every entry of every GS10 archive in a directory and check format hypotheses:
//   * every entry inflates to exactly `size` bytes (stored entries: packedSize === size)
//   * the 4-byte directory "magic" equals the first 4 bytes of the unpacked content
// Also prints a per-extension inventory across all archives.
//
// Usage: node gsverify.mjs <gameDir> [--json out.json]

import fs from 'node:fs';
import path from 'node:path';
import { readArchive, readEntry } from './gstool.mjs';

const [gameDir, ...rest] = process.argv.slice(2);
const jsonOut = rest[rest.indexOf('--json') + 1];
const archives = fs.readdirSync(gameDir).filter((f) => f.endsWith('.gs')).sort();

const byExt = new Map();
const failures = [];
let entries = 0;
let magicMatches = 0;
const magicMismatches = [];

for (const name of archives) {
  const archive = readArchive(path.join(gameDir, name));
  for (const e of archive.entries) {
    entries++;
    let data;
    try {
      data = readEntry(archive, e);
    } catch (err) {
      failures.push({ archive: name, entry: e.name, error: err.message });
      continue;
    }
    const head = Buffer.alloc(4);
    data.copy(head, 0, 0, Math.min(4, data.length));
    if (head.equals(e.magic)) magicMatches++;
    else if (magicMismatches.length < 20) magicMismatches.push({ archive: name, entry: e.name, magic: e.magic.toString('hex'), head: head.toString('hex'), size: e.size });

    const ext = path.extname(e.name).toLowerCase() || '(none)';
    const s = byExt.get(ext) ?? { count: 0, bytes: 0, archives: new Set() };
    s.count++;
    s.bytes += e.size;
    s.archives.add(name);
    byExt.set(ext, s);
  }
}

const inventory = [...byExt.entries()]
  .sort((a, b) => b[1].count - a[1].count)
  .map(([ext, s]) => ({ ext, count: s.count, MB: +(s.bytes / 1048576).toFixed(1), archives: s.archives.size }));

const report = { archives: archives.length, entries, inflateFailures: failures, magicMatches, magicMismatchSample: magicMismatches, inventory };
if (jsonOut) fs.writeFileSync(jsonOut, JSON.stringify(report, null, 2));
console.log(`archives=${archives.length} entries=${entries} inflateFailures=${failures.length} magicMatches=${magicMatches}/${entries}`);
if (magicMismatches.length) console.log('magic mismatch sample:', magicMismatches.slice(0, 5));
console.table(inventory.slice(0, 40));
