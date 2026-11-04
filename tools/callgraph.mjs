#!/usr/bin/env node
// callgraph - direct-call graph over the Ghidra pseudocode in decompiled/GSystem-v2/functions.
//
// Edges are FUN_xxxxxxxx( / thunk_FUN_xxxxxxxx( calls; virtual calls (**(code **)(...))() are not resolved.
//
// Usage:
//   node callgraph.mjs <functionsDir> reach <root> [maxDepth] [lo-hi]   functions reachable from root (BFS depth, bytes)
//   node callgraph.mjs <functionsDir> callers <addr>                     direct callers of addr
//   node callgraph.mjs <functionsDir> own <root,root,...> [lo-hi]        partition: functions reachable from exactly one root

import fs from 'node:fs';
import path from 'node:path';

const [dir, cmd, arg, a4, a5] = process.argv.slice(2);
const files = fs.readdirSync(dir).filter((f) => f.endsWith('.c'));
const calls = new Map();
const bytes = new Map();
for (const f of files) {
  const a = f.slice(0, -2);
  const s = fs.readFileSync(path.join(dir, f), 'utf8');
  calls.set(a, [...new Set([...s.matchAll(/(?:thunk_)?FUN_([0-9a-f]{8})\(/g)].map((m) => `0x${m[1]}`))].filter((c) => c !== a));
  bytes.set(a, [...s.matchAll(/^\/\/ range: 0x([0-9a-f]+)-0x([0-9a-f]+)/gm)].reduce((n, m) => n + parseInt(m[2], 16) - parseInt(m[1], 16) + 1, 0));
}
const range = (spec) => {
  if (!spec) return () => true;
  const [lo, hi] = spec.split('-').map((x) => parseInt(x, 16));
  return (a) => parseInt(a, 16) >= lo && parseInt(a, 16) < hi;
};
function reach(root, maxDepth = Infinity, inRange = () => true) {
  const depth = new Map([[root, 0]]);
  const q = [root];
  while (q.length) {
    const a = q.shift();
    if (depth.get(a) >= maxDepth) continue;
    for (const c of calls.get(a) ?? []) if (!depth.has(c) && inRange(c)) { depth.set(c, depth.get(a) + 1); q.push(c); }
  }
  return depth;
}

if (cmd === 'reach') {
  const d = reach(arg, a4 ? Number(a4) : Infinity, range(a5));
  const list = [...d.entries()].sort((x, y) => x[1] - y[1] || x[0].localeCompare(y[0]));
  console.log(`${list.length} functions, ${list.reduce((n, [a]) => n + (bytes.get(a) ?? 0), 0)} bytes`);
  for (const [a, k] of list) console.log(`${'  '.repeat(Math.min(k, 8))}${a} ${bytes.get(a) ?? '?'}`);
} else if (cmd === 'callers') {
  for (const [a, cs] of calls) if (cs.includes(arg)) console.log(a, bytes.get(a));
} else if (cmd === 'own') {
  const roots = arg.split(',');
  const inRange = range(a4);
  const sets = roots.map((r) => reach(r, Infinity, inRange));
  const owner = new Map();
  sets.forEach((s, i) => { for (const a of s.keys()) owner.set(a, owner.has(a) ? -1 : i); });
  roots.forEach((r, i) => {
    const own = [...owner.entries()].filter(([, o]) => o === i).map(([a]) => a).sort();
    console.log(`== ${r}: ${own.length} own functions, ${own.reduce((n, a) => n + (bytes.get(a) ?? 0), 0)} bytes`);
    console.log(own.join(' '));
  });
  const shared = [...owner.entries()].filter(([, o]) => o === -1).map(([a]) => a).sort();
  console.log(`== shared: ${shared.length}`);
  console.log(shared.join(' '));
} else {
  console.error('usage: callgraph.mjs <functionsDir> reach|callers|own ...');
  process.exit(1);
}
