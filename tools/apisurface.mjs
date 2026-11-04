#!/usr/bin/env node
// apisurface - estimate the native scripting API of GSystem.dll by cross-referencing identifiers that the
// shipped scripts use against strings present in the engine binary.
//
//   global functions : `name(` not preceded by '.', not defined by any script `function name`, present in engine
//   methods/props    : `.name` present in engine (native members) vs. absent (script-defined members)
//   constants        : UPPER_CASE identifiers present in engine
//
// Usage: node apisurface.mjs <extractedDir> <GSystem.dll> [out.json]

import fs from 'node:fs';
import path from 'node:path';

const [root, dll, outFile] = process.argv.slice(2);
const engine = fs.readFileSync(dll);
const engineStrings = new Set();
{
  // ASCII and UTF-16LE identifier-like strings
  const re = /[A-Za-z_][A-Za-z0-9_]{1,63}/g;
  for (const m of engine.toString('latin1').matchAll(re)) engineStrings.add(m[0]);
  for (const m of engine.toString('utf16le').matchAll(re)) engineStrings.add(m[0]);
  for (const m of engine.subarray(1).toString('utf16le').matchAll(re)) engineStrings.add(m[0]);
}

const KEYWORDS = new Set('array assert break continue current defined delete do echo else event exit export false for foreach foreach_reverse function global if keys load local loop macro map null pause resume return skip_frame this thread true values wait wait_until wait_while while with'.split(' '));

const files = [];
(function walk(dir) {
  for (const d of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, d.name);
    if (d.isDirectory()) walk(p);
    else if (/\.(script|scenario|campaign|shader|strings)$/i.test(d.name)) files.push(p);
  }
})(root);

const calls = new Map();
const members = new Map();
const consts = new Map();
const scriptDefined = new Set();
const bump = (m, k) => m.set(k, (m.get(k) ?? 0) + 1);

for (const f of files) {
  const src = fs.readFileSync(f, 'latin1').replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '').replace(/"(?:[^"\\]|\\.)*"/g, '""');
  for (const m of src.matchAll(/\bfunction\s+([A-Za-z_]\w*)/g)) scriptDefined.add(m[1]);
  for (const m of src.matchAll(/(?<![.$\w])([a-z_][A-Za-z0-9_]*)\s*\(/g)) if (!KEYWORDS.has(m[1])) bump(calls, m[1]);
  for (const m of src.matchAll(/\.([A-Za-z_][A-Za-z0-9_]*)/g)) bump(members, m[1]);
  for (const m of src.matchAll(/\b([A-Z][A-Z0-9]*_[A-Z0-9_]+)\b/g)) bump(consts, m[1]);
}

const sortDesc = (m) => [...m.entries()].sort((a, b) => b[1] - a[1]);
const nativeGlobals = sortDesc(calls).filter(([k]) => engineStrings.has(k) && !scriptDefined.has(k));
const nativeMembers = sortDesc(members).filter(([k]) => engineStrings.has(k));
const scriptMembers = sortDesc(members).filter(([k]) => !engineStrings.has(k));
const nativeConsts = sortDesc(consts).filter(([k]) => engineStrings.has(k));

const report = {
  scriptsScanned: files.length,
  nativeGlobalFunctions: nativeGlobals.map(([name, uses]) => ({ name, uses })),
  nativeMembers: nativeMembers.map(([name, uses]) => ({ name, uses })),
  scriptOnlyMembersTop: scriptMembers.slice(0, 80).map(([name, uses]) => ({ name, uses })),
  nativeConstants: nativeConsts.map(([name, uses]) => ({ name, uses })),
  scriptDefinedFunctions: scriptDefined.size,
};
if (outFile) fs.writeFileSync(outFile, JSON.stringify(report, null, 1));
console.log(`scripts=${files.length} nativeGlobals=${nativeGlobals.length} nativeMembers=${nativeMembers.length} nativeConstants=${nativeConsts.length} scriptFunctions=${scriptDefined.size}`);
console.log('top native globals:', nativeGlobals.slice(0, 60).map(([k, v]) => `${k}(${v})`).join(' '));
console.log('top native members:', nativeMembers.slice(0, 60).map(([k, v]) => `${k}(${v})`).join(' '));
console.log('constant prefixes:', [...new Set(nativeConsts.map(([k]) => k.split('_').slice(0, 2).join('_')))].join(' '));
