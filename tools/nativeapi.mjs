#!/usr/bin/env node
// nativeapi - extract GSystem.dll's native scripting API from Ghidra pseudocode + RTTI.
//
// Globals:  FUN_100022b0(var,"name",len); FUN_1000f5f0(var, obj)  — obj is a native function
//           (FUN_1002e050 gsTransparentFunction / FUN_1002e170 gsOpaqueFunction: handler, minArgs, maxArgs)
//           or a constant value (int/float/string/bool variant constructors).
// Members:  per-class virtual registerMembers() at vtable slot 30 (offset 0x78, called once by FUN_1001c2a0):
//           FUN_1001c020/050/080/0b0/0e0/120/160 -> fields of type int/float/str/wstr/custom/var/bit,
//           FUN_1001bf50(tbl,this,id,name,type,...) -> field with explicit type (2 = bool),
//           FUN_1001c1a0(cls,this,name,id,handler,min,max) -> method, FUN_1001c200(cls,x,name,id,flag) -> event.
//
// Usage: node nativeapi.mjs <functionsDir> <rtti-classes.json> <GSystem.dll> [script-api-surface.json] [out.json]

import fs from 'node:fs';
import path from 'node:path';

const [dir, rttiFile, dll, surfaceFile, outFile] = process.argv.slice(2);
const bin = fs.readFileSync(dll);
const BASE = 0x10000000;
const rtti = JSON.parse(fs.readFileSync(rttiFile, 'utf8'));

const FIELD_REGISTRARS = { FUN_1001bff0: 'bool', FUN_1001c020: 'int', FUN_1001c050: 'float', FUN_1001c080: 'str', FUN_1001c0b0: 'wstr', FUN_1001c0e0: 'custom', FUN_1001c120: 'var', FUN_1001c160: 'bit' };
const HELPERS = new Set(['0x1001bf50', '0x1001c1a0', '0x1001c200', ...Object.keys(FIELD_REGISTRARS).map((r) => `0x${r.slice(4)}`)]);
// Constant factories: name -> [value type, index of the value argument (-1 = all arguments)]
const CONSTANT_CTORS = {
  FUN_1000a560: ['int', 0], FUN_10011860: ['int', 0], FUN_1002cde0: ['int', 1], FUN_1000a5e0: ['float', 0],
  FUN_1002ce70: ['float', 1], FUN_1000aed0: ['string', 0], FUN_1002cff0: ['string', 1], FUN_1000a540: ['bool', 0],
  FUN_100934d0: ['vector', -1], FUN_100933e0: ['quaternion', -1], FUN_1000a640: ['version', -1],
  SINGLETON_b0: ['int', 'fixed:0'], SINGLETON_b4: ['bool', 'fixed:false'], SINGLETON_b8: ['bool', 'fixed:true'], SINGLETON_ac: ['null', 'fixed:null'],
};
const f32 = (s) => {
  const m = String(s).match(/^0x([0-9a-f]{8})$/i);
  if (!m) return s;
  const b = Buffer.alloc(4);
  b.writeUInt32LE(parseInt(m[1], 16));
  return `${s} (${+b.readFloatLE(0).toPrecision(7)})`;
};
const TYPE_CODES = { 2: 'bool', 3: 'int', 4: 'float', 5: 'str', 6: 'wstr', 7: 'custom', 8: 'var', 9: 'bit' };

function splitArgs(s) {
  const out = [];
  let depth = 0, cur = '', q = false;
  for (let i = 0; i < s.length; i++) {
    const ch = s[i];
    if (ch === '"' && s[i - 1] !== '\\') q = !q;
    if (!q && ch === '(') depth++;
    if (!q && ch === ')') depth--;
    if (!q && ch === ',' && depth === 0) { out.push(cur.trim()); cur = ''; continue; }
    cur += ch;
  }
  if (cur.trim()) out.push(cur.trim());
  return out;
}
function cString(va) {
  const o = va - BASE;
  if (o < 0 || o >= bin.length) return null;
  let e = o;
  while (e < bin.length && bin[e] !== 0 && e - o < 256) e++;
  const s = bin.toString('latin1', o, e);
  return /^[\x20-\x7e]+$/.test(s) ? s : null;
}
function resolveName(arg) {
  const lit = arg.match(/"((?:[^"\\]|\\.)*)"/);
  if (lit) return lit[1];
  const dat = arg.match(/(?:DAT|s_\w*?)_([0-9a-f]{8})/);
  if (dat) return cString(parseInt(dat[1], 16));
  return null;
}
const num = (s) => {
  if (s == null) return null;
  const m = String(s).replace(/\(\w+\)/g, '').trim().match(/^(-?0x[0-9a-f]+|-?\d+)$/i);
  return m ? Number(m[1]) : null;
};
const addrOf = (s) => {
  const m = String(s).match(/(?:FUN|LAB|thunk_FUN)_([0-9a-f]{8})|\b(0x10[0-9a-f]{6})\b/);
  return m ? `0x${(m[1] ?? m[2].slice(2)).toLowerCase()}` : null;
};
// field callbacks: the setter replaces the plain store (PTR_FUN_1018efcc -> FUN_1001bfc0 throws, i.e. read-only);
// the hook is called by the default store path before the value is assigned (FUN_1001cc40)
// (addrOf reads PTR_FUN_1018efcc as 0x1018efcc, the data slot holding 0x1001bfc0)
const READ_ONLY = new Set(['0x1018efcc', '0x1001bfc0']);
const callbacks = (setter, hook) => {
  const out = {};
  if (READ_ONLY.has(addrOf(setter))) out.readOnly = true;
  else if (addrOf(setter)) out.setter = addrOf(setter);
  if (addrOf(hook)) out.hook = addrOf(hook);
  return out;
};
const fieldOffset = (s) => {
  const m = String(s).match(/\+\s*(0x[0-9a-f]+|\d+)\s*$/i) ?? String(s).match(/\+\s*(0x[0-9a-f]+|\d+)\)?$/i);
  return m ? `0x${Number(m[1]).toString(16)}` : null;
};
function* calls(src, name) {
  const re = new RegExp(`\\b${name}\\(`, 'g');
  for (let m; (m = re.exec(src));) {
    let depth = 1, i = re.lastIndex;
    for (; i < src.length && depth; i++) {
      if (src[i] === '(') depth++;
      else if (src[i] === ')') depth--;
    }
    yield { args: splitArgs(src.slice(re.lastIndex, i - 1)), index: m.index };
  }
}

// ---- registrar -> class from RTTI vtable slot 30 ----
const regOwner = new Map();
const byName = new Map(rtti.map((c) => [c.name, c]));
// The script-object base (gsObjectVariant...) is not always the primary subobject (e.g. gsPostEffect_OGL derives
// from gsIVideoOutput_OGL first), so use the first sub-vtable that is long enough to contain slot 30.
const regOf = (c) => {
  if (!c) return null;
  const v = [...c.vtables].sort((a, b) => a.subobjectOffset - b.subobjectOffset).find((x) => x.methods.length > 30);
  return v ? v.methods[30] : null;
};
for (const c of rtti) {
  const r = regOf(c);
  if (!r) continue;
  const base = byName.get(c.bases[0]);
  if (regOf(base) !== r) {
    const list = regOwner.get(r) ?? [];
    list.push(c.name);
    regOwner.set(r, list);
  }
}

// ---- scan pseudocode ----
const globals = [];
const registrars = new Map(); // function address -> { members, parentCalls }
const files = fs.readdirSync(dir).filter((f) => f.endsWith('.c'));
for (const file of files) {
  const src = fs.readFileSync(path.join(dir, file), 'utf8');
  const fn = file.replace(/\.c$/, '');

  // globals
  if (src.includes('FUN_1000f5f0(')) {
    const lines = src.split('\n');
    const names = new Map();
    const objs = new Map();
    for (const line of lines) {
      const nm = line.match(/FUN_100022b0\((\w+),\(undefined4 \*\)("(?:[^"\\]|\\.)*"|&DAT_[0-9a-f]{8}),/)
        ?? line.match(/FUN_100025e0\((\w+),(?:\([^)]*\))?("(?:[^"\\]|\\.)*"|&DAT_[0-9a-f]{8})\)/);
      if (nm) names.set(nm[1], resolveName(nm[2]));
      const as = line.match(/^\s*(\w+) = (?:\([^)]*\))?(FUN_[0-9a-f]{8})\((.*)\);\s*$/);
      if (as) objs.set(as[1], { ctor: as[2], args: splitArgs(as[3]) });
      // shared singletons assigned directly (first value of an enum block is the cached 0)
      const sg = line.match(/^\s*(\w+) = (?:\([^)]*\))?DAT_10193a(b0|b4|b8|ac);\s*$/);
      if (sg) objs.set(sg[1], { ctor: `SINGLETON_${sg[2]}`, args: [] });
      const ins = line.match(/FUN_1000f5f0\((\w+),(?:\(\w+\))?(\w+)\)/);
      if (ins) {
        const name = names.get(ins[1]);
        const direct = ins[2].match(/^DAT_10193a(b0|b4|b8|ac)$/);
        const o = objs.get(ins[2]) ?? (direct ? { ctor: `SINGLETON_${direct[1]}`, args: [] } : undefined);
        const g = { name, registeredIn: fn };
        if (o?.ctor === 'FUN_1002e170' || o?.ctor === 'FUN_1002e050') {
          g.kind = 'function';
          g.wrapper = o.ctor === 'FUN_1002e170' ? 'gsOpaqueFunction' : 'gsTransparentFunction';
          g.handler = addrOf(o.args[1]);
          g.minArgs = num(o.args[2]);
          const max = num(o.args[3]);
          g.maxArgs = max === -1 ? g.minArgs : max;
        } else if (o) {
          const k = CONSTANT_CTORS[o.ctor];
          g.kind = k ? 'constant' : 'object';
          if (k) {
            g.valueType = k[0];
            g.value = typeof k[1] === 'string' ? k[1].slice(6) : k[1] < 0 ? o.args.join(', ') : k[0] === 'float' ? f32(o.args[k[1]]) : o.args[k[1]];
          } else {
            g.ctor = o.ctor;
          }
        } else {
          g.kind = 'unresolved';
          g.objectVar = ins[2];
        }
        globals.push(g);
      }
    }
  }

  // members
  const members = [];
  if (!HELPERS.has(fn)) {
    for (const [reg, type] of Object.entries(FIELD_REGISTRARS)) {
      for (const c of calls(src, reg)) {
        // bool/int/float/str/wstr: (cls, obj, name, id, addr, setter, hook); custom/bit/var take an extra arg 5 first
        const k = ['custom', 'bit', 'var'].includes(type) ? 6 : 5;
        members.push({ kind: 'field', type, name: resolveName(c.args[2]), id: num(c.args[3]), offset: fieldOffset(c.args[4]), ...callbacks(c.args[k], c.args[k + 1]), at: c.index });
      }
    }
    for (const c of calls(src, 'FUN_1001bf50')) {
      // FUN_1001bf50(table, owner, id, name, type, extra, addr, hook, setter, castType)
      members.push({ kind: 'field', type: TYPE_CODES[num(c.args[4])] ?? `type${c.args[4]}`, name: resolveName(c.args[3]), id: num(c.args[2]), offset: fieldOffset(c.args[6]), ...callbacks(c.args[8], c.args[7]), at: c.index });
    }
    for (const c of calls(src, 'FUN_1001c1a0')) {
      const min = num(c.args[5]);
      const max = num(c.args[6]);
      members.push({ kind: 'method', name: resolveName(c.args[2]), id: num(c.args[3]), handler: addrOf(c.args[4]), minArgs: min, maxArgs: max === -1 ? min : max, at: c.index });
    }
    for (const c of calls(src, 'FUN_1001c200')) {
      members.push({ kind: 'event', name: resolveName(c.args[2]), id: num(c.args[3]), flag: num(c.args[4]), at: c.index });
    }
  }
  if (members.length) {
    members.sort((a, b) => a.at - b.at);
    const firstReg = members[0].at;
    const parentCall = [...src.slice(0, firstReg).matchAll(/FUN_([0-9a-f]{8})\(/g)].map((m) => `0x${m[1]}`).pop() ?? null;
    registrars.set(fn, { members: members.map(({ at, ...m }) => m), parentCall });
  }
}

// gsObjectVariant's registrar (FUN_1001ff50) writes its member table directly instead of calling the registrars;
// these rows were read by hand from that function.
registrars.set('0x1001ff50', {
  parentCall: null,
  members: [
    { kind: 'field', type: 'bool', name: 'destroyed', id: 0, offset: null },
    { kind: 'method', name: 'hasMember', id: 1, handler: '0x1001f600', minArgs: 1, maxArgs: 1 },
    { kind: 'method', name: 'listMembers', id: 2, handler: '0x1001d690', minArgs: 0, maxArgs: 0 },
    { kind: 'method', name: 'className', id: 3, handler: null, minArgs: 0, maxArgs: 0 },
    { kind: 'method', name: 'memberInfo', id: 4, handler: '0x1001f6b0', minArgs: 1, maxArgs: 1 },
    { kind: 'method', name: 'getMember', id: 5, handler: '0x1001f870', minArgs: 1, maxArgs: 1 },
    { kind: 'event', name: 'onDestroy', id: 0, flag: null },
  ],
  handRead: true,
});
if (!regOwner.has('0x1001ff50')) regOwner.set('0x1001ff50', ['gsObjectVariant']);

// ---- assemble classes ----
const classes = [];
for (const [fn, r] of registrars) {
  const owners = regOwner.get(fn) ?? [];
  classes.push({ registrar: fn, classes: owners, parentRegistrar: registrars.has(r.parentCall) ? r.parentCall : null, members: r.members, ...(r.handRead ? { handRead: true } : {}) });
}
const regClass = new Map(classes.map((c) => [c.registrar, c.classes[0] ?? `registrar_${c.registrar}`]));
for (const c of classes) c.parentClass = c.parentRegistrar ? regClass.get(c.parentRegistrar) : null;
classes.sort((a, b) => (a.classes[0] ?? 'zz').localeCompare(b.classes[0] ?? 'zz'));

// ---- which class does each new_* constructor build? (RTTI vtables stored in the handler or its direct callees) ----
const vt2class = new Map(rtti.flatMap((c) => c.vtables.map((v) => [v.address, c.name])));
const classOrder = new Map(rtti.map((c) => [c.name, c.bases.length]));
const vtablesIn = (addr) => {
  const p = path.join(dir, `${addr}.c`);
  if (!fs.existsSync(p)) return { classes: [], callees: [] };
  const src = fs.readFileSync(p, 'utf8');
  return {
    classes: [...src.matchAll(/PTR_(?:FUN|LAB)_([0-9a-f]{8})/g)].map((m) => vt2class.get(`0x${m[1]}`)).filter(Boolean),
    callees: [...new Set([...src.matchAll(/FUN_([0-9a-f]{8})\(/g)].map((m) => `0x${m[1]}`))],
  };
};
for (const g of globals) {
  if (g.kind !== 'function' || !String(g.name).startsWith('new_') || !g.handler) continue;
  const top = vtablesIn(g.handler);
  const found = new Set(top.classes);
  for (const c of top.callees.slice(0, 40)) for (const k of vtablesIn(c).classes) found.add(k);
  // the most-derived class (longest base chain) is the one being constructed
  g.creates = [...found].sort((a, b) => (classOrder.get(b) ?? 0) - (classOrder.get(a) ?? 0))[0] ?? null;
}

// ---- script usage ----
let usage = null;
if (surfaceFile && fs.existsSync(surfaceFile)) {
  const s = JSON.parse(fs.readFileSync(surfaceFile, 'utf8'));
  usage = new Map([...s.nativeGlobalFunctions, ...s.nativeMembers, ...s.nativeConstants].map((x) => [x.name, x.uses]));
  for (const g of globals) g.scriptUses = usage.get(g.name) ?? 0;
  for (const c of classes) for (const m of c.members) m.scriptUses = usage.get(m.name) ?? 0;
}

const result = { globals: globals.sort((a, b) => String(a.name).localeCompare(String(b.name))), classes };
if (outFile) fs.writeFileSync(outFile, JSON.stringify(result, null, 1));

const count = (arr, k) => arr.reduce((m, x) => ((m[x[k]] = (m[x[k]] ?? 0) + 1), m), {});
const allMembers = classes.flatMap((c) => c.members);
console.log(`globals: ${globals.length}`, count(globals, 'kind'));
console.log(`unnamed globals: ${globals.filter((g) => !g.name).length}`);
console.log(`registrar functions: ${classes.length}; mapped to RTTI classes: ${classes.filter((c) => c.classes.length).length}`);
console.log(`members: ${allMembers.length}`, count(allMembers, 'kind'), 'field types:', count(allMembers.filter((m) => m.kind === 'field'), 'type'));
console.log(`unnamed members: ${allMembers.filter((m) => !m.name).length}`);
