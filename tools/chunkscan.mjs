#!/usr/bin/env node
// chunkscan - survey binary asset types inside GS10 archives and dump their chunk trees.
//
// GSystem serializes most binary data as nested chunks: u32 tag + u32 size + body. The tag's top byte
// selects a format family (0x01 archive, 0x05 state machine, ...). This tool walks a chunk tree
// heuristically: a body is treated as [fixed header][sub-chunk list] when, for the smallest 4-aligned
// header length up to 512 bytes, the remainder parses exactly into chunks sharing the parent's family byte.
//
// Usage:
//   node chunkscan.mjs survey <gameDir>                       first tag per extension across all archives
//   node chunkscan.mjs tree <gameDir> <archive.gs> <entry> [maxDepth]
//   node chunkscan.mjs treefile <file> [maxDepth]

import fs from 'node:fs';
import path from 'node:path';
import { readArchive, readEntry } from './gstool.mjs';

const hex = (n) => `0x${n.toString(16).padStart(8, '0')}`;

function parseChunks(buf, start, end, family) {
  const out = [];
  let pos = start;
  while (pos < end) {
    if (pos + 8 > end) return null;
    const tag = buf.readUInt32LE(pos);
    const size = buf.readUInt32LE(pos + 4);
    if ((tag >>> 24) !== family || pos + 8 + size > end) return null;
    out.push({ tag, size, body: pos + 8 });
    pos += 8 + size;
  }
  return out;
}

function printable(buf, start, len) {
  return [...buf.subarray(start, start + len)].map((c) => (c >= 32 && c < 127 ? String.fromCharCode(c) : '.')).join('');
}

export function tree(buf, maxDepth = 6) {
  const lines = [];
  const family = buf.readUInt32LE(0) >>> 24;
  const walk = (start, end, depth) => {
    let chunks = null;
    let header = 0;
    for (; header <= 512 && start + header < end; header += 4) {
      chunks = parseChunks(buf, start + header, end, family);
      if (chunks?.length) break;
    }
    if (!chunks?.length) return false;
    if (header) lines.push(`${'  '.repeat(depth)}[header ${header} bytes] ${[...Array(Math.min(header / 4, 8)).keys()].map((i) => buf.readUInt32LE(start + i * 4)).join(' ')}`);
    const counts = new Map();
    for (const c of chunks) counts.set(c.tag, (counts.get(c.tag) ?? 0) + 1);
    const shown = new Map();
    for (const c of chunks) {
      const n = (shown.get(c.tag) ?? 0) + 1;
      shown.set(c.tag, n);
      if (n > 3) {
        if (n === 4) lines.push(`${'  '.repeat(depth)}... ${counts.get(c.tag) - 3} more ${hex(c.tag)}`);
        continue;
      }
      lines.push(`${'  '.repeat(depth)}${hex(c.tag)} size=${c.size}  |${printable(buf, c.body, Math.min(c.size, 40))}|`);
      if (depth < maxDepth && c.size >= 8) walk(c.body, c.body + c.size, depth + 1);
    }
    return true;
  };
  if (!walk(0, buf.length, 0)) lines.push('(not a clean chunk tree at top level)');
  return lines.join('\n');
}

function survey(gameDir) {
  const byExt = new Map();
  for (const name of fs.readdirSync(gameDir).filter((f) => f.endsWith('.gs')).sort()) {
    const archive = readArchive(path.join(gameDir, name));
    for (const e of archive.entries) {
      const ext = path.extname(e.name).toLowerCase() || '(none)';
      const s = byExt.get(ext) ?? { count: 0, heads: new Map(), example: `${name}:${e.name}` };
      s.count++;
      const head = e.magic.readUInt32LE(0);
      const key = (head >>> 24) <= 0x10 && (head & 0xffff) === 0 ? hex(head) : `"${printable(e.magic, 0, 4)}"`;
      s.heads.set(key, (s.heads.get(key) ?? 0) + 1);
      byExt.set(ext, s);
    }
  }
  for (const [ext, s] of [...byExt.entries()].sort((a, b) => b[1].count - a[1].count)) {
    const heads = [...s.heads.entries()].sort((a, b) => b[1] - a[1]).slice(0, 4).map(([k, v]) => `${k} x${v}`).join(', ');
    console.log(`${ext.padEnd(13)} ${String(s.count).padStart(5)}  ${heads}   e.g. ${s.example}`);
  }
}

const [cmd, ...args] = process.argv.slice(2);
if (cmd === 'survey') survey(args[0]);
else if (cmd === 'tree') {
  const archive = readArchive(path.join(args[0], args[1]));
  const entry = archive.entries.find((e) => archive.mount + e.name === args[2] || e.name === args[2]);
  if (!entry) throw new Error(`entry not found: ${args[2]}`);
  console.log(tree(readEntry(archive, entry), Number(args[3] ?? 6)));
} else if (cmd === 'treefile') console.log(tree(fs.readFileSync(args[0]), Number(args[1] ?? 6)));
else console.error('usage: chunkscan.mjs survey <gameDir> | tree <gameDir> <archive.gs> <entry> [depth] | treefile <file> [depth]');
