#!/usr/bin/env node
// gen-port-api - generate the ReGS registration of the whole native script API from docs/native-api.json:
// every class (with parents), field (type, read-only), method and event, every global function (as a stub or a
// `new_*` constructor) and every constant with its real value. Constants that point into the DLL's data section are
// read from GSystem.dll; the vector/quaternion constants (runtime-initialized) are filled in by hand.
//
// Usage: node gen-port-api.mjs <native-api.json> <GSystem.dll> <out.cpp>

import fs from 'node:fs';

const [apiFile, dllFile, outFile] = process.argv.slice(2);
const api = JSON.parse(fs.readFileSync(apiFile, 'utf8'));
const dll = fs.readFileSync(dllFile);
const BASE = 0x10000000;
const DATA = [0x18c000, 0x194000];  // .data raw range (file offset == RVA)

const TYPE_CODE = { bool: 2, int: 3, float: 4, str: 5, wstr: 6, custom: 7, var: 8, bit: 9 };
const VECTORS = {
  VEC_ZERO: [0, 0, 0], VEC_POS_X: [1, 0, 0], VEC_NEG_X: [-1, 0, 0], VEC_POS_Y: [0, 1, 0], VEC_NEG_Y: [0, -1, 0],
  VEC_POS_Z: [0, 0, 1], VEC_NEG_Z: [0, 0, -1],
};

const u16 = (s) => `u"${String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
const lines = [];
const warn = [];

// Class hierarchy from RTTI (immediate base = first entry of the base list), which is complete; the API data only
// knows parents of classes with members. Every gs* class below gsObjectVariant is defined, members or not.
const rtti = JSON.parse(fs.readFileSync(new URL('../docs/rtti-classes.json', import.meta.url), 'utf8'));
// An object class has gsObjectVariant among its bases. Its parent is the first base that is itself an object class:
// interfaces such as gsITextureContainer can come first in multiple inheritance (gsTextureVariant_OGL).
const rttiBases = new Map(rtti.filter((r) => /^gs[A-Za-z0-9_]+$/.test(r.name)).map((r) => [r.name, r.bases]));
const objectInRtti = (name) => name === 'gsObjectVariant' || (rttiBases.get(name) ?? []).includes('gsObjectVariant');
const rttiParent = new Map();
for (const [name, bases] of rttiBases) rttiParent.set(name, bases.find((b) => objectInRtti(b)) ?? bases.find((b) => /^gs[A-Za-z0-9_]+$/.test(b)) ?? '');
const byName = new Map();
for (const c of api.classes) for (const n of c.classes) byName.set(n, c);
const parentOf = (name) => {
  if (rttiParent.has(name)) return rttiParent.get(name);
  return byName.get(name)?.parentClass ?? 'gsScript';
};
const isObjectClass = (name) => {
  for (let n = name, guard = 0; n && guard < 32; n = parentOf(n), ++guard) if (n === 'gsObjectVariant') return true;
  return false;
};
const done = new Set(['gsVariant']);
const order = [];
function visit(name) {
  if (done.has(name)) return;
  done.add(name);
  const p = parentOf(name);
  if (p && p !== 'gsVariant') visit(p);
  order.push(name);
}
for (const name of new Set([...rttiParent.keys(), ...byName.keys()])) if (isObjectClass(name)) visit(name);

for (const name of order) {
  const parent = name === 'gsObjectVariant' ? '' : parentOf(name);
  lines.push(`  c = vm.defineClass("${name}", "${parent}");`);
  const c = byName.get(name);
  if (!c) continue;
  for (const m of c.members) {
    if (m.kind === 'field') {
      lines.push(`  addField(c, ${u16(m.name)}, ${TYPE_CODE[m.type] ?? 0}, ${m.readOnly ? 'true' : 'false'});`);
    } else if (m.kind === 'method') {
      lines.push(`  addMethod(c, ${u16(m.name)}, ${m.minArgs}, ${m.maxArgs});`);
    } else if (m.kind === 'event') {
      lines.push(`  addEvent(c, ${u16(m.name)}, ${m.id}, ${m.flag ? 'true' : 'false'});`);
    }
  }
}

function dataU32(ref) {
  const m = String(ref).match(/DAT_([0-9a-f]{8})/);
  if (!m) return null;
  const off = parseInt(m[1], 16) - BASE;
  if (off < DATA[0] || off + 4 > DATA[1]) return null;
  return dll.readUInt32LE(off);
}

const seen = new Set();
for (const g of api.globals) {
  if (!g.name || seen.has(g.name)) continue;
  if (g.kind === 'function') {
    seen.add(g.name);
    if (g.creates) lines.push(`  ctorFunction(vm, ${u16(g.name)}, "${g.creates}", ${g.minArgs}, ${g.maxArgs});`);
    else lines.push(`  stubFunction(vm, ${u16(g.name)}, ${g.minArgs}, ${g.maxArgs});`);
  } else if (g.kind === 'constant') {
    let v = null;
    switch (g.valueType) {
      case 'int': {
        const sv = String(g.value).trim();
        let n = sv.startsWith('-') ? -Number(sv.slice(1)) : Number(sv);  // Number() rejects "-0x..."
        if (Number.isNaN(n)) n = dataU32(g.value);
        if (n === null) warn.push(`${g.name}: ${g.value}`);
        else v = `Value::integer(int32_t(${(n >>> 0) >>> 0}u))`;
        break;
      }
      case 'float': {
        const bits = parseInt(String(g.value), 16);
        const f = new Float32Array(new Uint32Array([bits]).buffer)[0];
        v = `Value::real(${f.toPrecision(9)}f)`;
        break;
      }
      case 'string': v = `Value::string(std::u16string(${u16(String(g.value).replace(/^L"|"$/g, ''))}))`; break;
      case 'vector': {
        const xyz = VECTORS[g.name];
        if (xyz) v = `Value::vector(${xyz.map((x) => `${x}.0f`).join(', ')})`;
        break;
      }
      case 'quaternion': v = 'Value::quat(0.0f, 0.0f, 0.0f, 1.0f)'; break;
      case 'version': v = `Value::string(std::u16string(u"4.0.1"))`; break;
      case 'null': continue;  // MAIN: registered when the root object is attached
      default: break;
    }
    if (!v) { warn.push(`${g.name}: ${g.valueType} ${g.value}`); continue; }
    seen.add(g.name);
    lines.push(`  vm.setGlobal(${u16(g.name)}, ${v});`);
  }
}

const out = `// Generated by tools/gen-port-api.mjs from docs/native-api.json and GSystem.dll. Do not edit.
#include "engine/api.h"

namespace regs::engine {

using namespace regs::script;

void registerGeneratedApi(VM& vm) {
  ClassInfo* c = nullptr;
${lines.join('\n')}
  (void)c;
}

}  // namespace regs::engine
`;
fs.writeFileSync(outFile, out);
console.log(`classes ${order.length}, lines ${lines.length}, unresolved constants ${warn.length}${warn.length ? ': ' + warn.join('; ') : ''}`);
