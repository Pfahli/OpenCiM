#!/usr/bin/env node
// match-builds - carry the Linux build's symbol names (Cities In Motion.bin, unstripped GCC ELF) over to the
// functions of GSystem.dll (MSVC, no symbols). Both builds are game version 1.0.22 from the same source.
//
// Anchors (each records how it was found):
//   export   - PE export name == Linux function name (C API: gsInitialize, runThreads, ...)
//   native   - native-api.json handler of Class.member / global == Linux Class::method_<member> / gsFunction_<name>,
//              registrar == Class::initializeClassData
//   string   - a string referenced by exactly one function in each build
//   const    - the same with large constants
//   vtable   - same class, same virtual slot, after bridging the ABI differences (docs/linux-names.md)
// Propagation, repeated until nothing changes:
//   calls    - between consecutive matched callees in the ordered call lists of a matched pair, a gap holding
//              exactly one unmatched callee on each side pairs them
//   callers  - a matched pair's only unmatched caller on each side
//   calleeset- unmatched functions whose matched callees map onto the same set
//   class    - unmatched functions inside a class's address range, compared with that class's Linux methods
// A Windows function that collects two different Linux candidates is dropped (and vice versa).
//
// Usage: node match-builds.mjs <win features.jsonl> <linux features.jsonl> <GSystem.dll> <linux .bin>
//        <native-api.json> <rtti-classes.json> <out.csv>

import fs from 'node:fs';

const [wFeat, lFeat, dllPath, elfPath, apiPath, rttiPath, outPath] = process.argv.slice(2);
const load = (f) => fs.readFileSync(f, 'utf8').trim().split('\n').map((l) => JSON.parse(l));
const W = new Map(load(wFeat).map((f) => [f.entry, f]));
const L = new Map(load(lFeat).map((f) => [f.entry, f]));
const lByName = new Map();
for (const f of L.values()) {
  if (!lByName.has(f.name)) lByName.set(f.name, []);
  lByName.get(f.name).push(f.entry);
}
const hex = (n) => `0x${(n >>> 0).toString(16).padStart(8, '0')}`;

// ---- matching state ----
const w2l = new Map(); // win -> {l, method}
const l2w = new Map();
const bad = new Set(); // conflicting addresses (either side)
let order = 0;
function propose(w, l, method) {
  if (!W.has(w) || !L.has(l) || bad.has('w' + w) || bad.has('l' + l)) return false;
  const a = w2l.get(w), b = l2w.get(l);
  if (a && a.l === l) return false;
  if (a || b) {  // conflict: drop both sides' claims
    if (a) { l2w.delete(a.l); bad.add('l' + a.l); }
    if (b) { w2l.delete(b.w); bad.add('w' + b.w); }
    w2l.delete(w);
    bad.add('w' + w);
    bad.add('l' + l);
    return false;
  }
  w2l.set(w, { l, method, order: order++ });
  l2w.set(l, { w, method });
  return true;
}
const counts = {};
const tally = (m, ok) => { if (ok) counts[m] = (counts[m] ?? 0) + 1; };

// ---- 1. exports ----
{
  const b = fs.readFileSync(dllPath);
  const pe = b.readUInt32LE(0x3c);
  const nsec = b.readUInt16LE(pe + 6), optSize = b.readUInt16LE(pe + 20);
  const secs = [];
  for (let i = 0, s = pe + 24 + optSize; i < nsec; i++, s += 40) secs.push({ va: b.readUInt32LE(s + 12), vsz: b.readUInt32LE(s + 8), raw: b.readUInt32LE(s + 20) });
  const off = (rva) => { const s = secs.find((x) => rva >= x.va && rva < x.va + Math.max(x.vsz, 1)); return s ? rva - s.va + s.raw : -1; };
  const expRva = b.readUInt32LE(pe + 24 + 96);
  const e = off(expRva);
  const nNames = b.readUInt32LE(e + 24), funcs = off(b.readUInt32LE(e + 28)), names = off(b.readUInt32LE(e + 32)), ords = off(b.readUInt32LE(e + 36));
  for (let i = 0; i < nNames; i++) {
    let p = off(b.readUInt32LE(names + i * 4)), name = '';
    while (b[p]) name += String.fromCharCode(b[p++]);
    const rva = b.readUInt32LE(funcs + b.readUInt16LE(ords + i * 2) * 4);
    const w = hex(0x10000000 + rva);
    const cand = (lByName.get(name) ?? []).concat(lByName.get(name.replace(/^_|@\d+$/g, '')) ?? []);
    if (cand.length === 1) tally('export', propose(w, cand[0], 'export'));
  }
}

// ---- 2. native API ----
{
  const api = JSON.parse(fs.readFileSync(apiPath, 'utf8'));
  const wCount = new Map();  // identical-code-folded handlers serve several names on Windows: only unique ones
  const note = (a) => wCount.set(a, (wCount.get(a) ?? 0) + 1);
  for (const g of api.globals) if (g.kind === 'function' && g.handler) note(g.handler);
  for (const c of api.classes) {
    note(c.registrar);
    for (const m of c.members) if (m.kind === 'method' && m.handler) note(m.handler);
  }
  const one = (w, lname) => {
    if (wCount.get(w) !== 1) return;
    const cand = lByName.get(lname) ?? [];
    if (cand.length === 1) tally('native', propose(w, cand[0], 'native'));
  };
  for (const g of api.globals) if (g.kind === 'function' && g.handler) one(g.handler, `gsFunction_${g.name}`);
  for (const c of api.classes) {
    if (c.classes.length !== 1) continue;
    const cls = c.classes[0];
    one(c.registrar, `${cls}::initializeClassData`);
    for (const m of c.members) if (m.kind === 'method' && m.handler) one(m.handler, `${cls}::method_${m.name}`);
  }
}

// ---- 3. unique strings ----
{
  const index = (M) => {
    const m = new Map();
    for (const f of M.values()) for (const s of f.strings) {
      if (!m.has(s)) m.set(s, new Set());
      m.get(s).add(f.entry);
    }
    return m;
  };
  const wi = index(W), li = index(L);
  // vote per pair; a function whose unique strings point at different partners is dropped
  const votes = new Map();
  for (const [s, ws] of wi) {
    const ls = li.get(s);
    if (!ls || ws.size !== 1 || ls.size !== 1) continue;
    const key = `${[...ws][0]}|${[...ls][0]}`;
    votes.set(key, (votes.get(key) ?? 0) + 1);
  }
  // strings can move between functions through different inlining: a clear majority (>= 2 votes, at least twice
  // the runner-up, on both sides) is accepted, a single unopposed vote too
  const best = (side) => {
    const m = new Map();
    for (const [key, n] of votes) {
      const [w, l] = key.split('|');
      const me = side === 'w' ? w : l, other = side === 'w' ? l : w;
      if (!m.has(me)) m.set(me, []);
      m.get(me).push([other, n]);
    }
    const out = new Map();
    for (const [me, list] of m) {
      list.sort((a, b) => b[1] - a[1]);
      if (list.length === 1 || (list[0][1] >= 2 && list[0][1] >= 2 * list[1][1])) out.set(me, list[0][0]);
    }
    return out;
  };
  const bw = best('w'), bl = best('l');
  for (const [w, l] of bw) if (bl.get(l) === w) tally('string', propose(w, l, 'string'));
}

// ---- 3b. unique large constants (same voting) ----
{
  const index = (M) => {
    const m = new Map();
    for (const f of M.values()) for (const c of f.consts) {
      if (!m.has(c)) m.set(c, new Set());
      m.get(c).add(f.entry);
    }
    return m;
  };
  const wi = index(W), li = index(L);
  const votes = new Map();
  for (const [c, ws] of wi) {
    const ls = li.get(c);
    if (!ls || ws.size !== 1 || ls.size !== 1) continue;
    const key = `${[...ws][0]}|${[...ls][0]}`;
    votes.set(key, (votes.get(key) ?? 0) + 1);
  }
  const partnersW = new Map(), partnersL = new Map();
  for (const key of votes.keys()) {
    const [w, l] = key.split('|');
    partnersW.set(w, (partnersW.get(w) ?? new Set()).add(l));
    partnersL.set(l, (partnersL.get(l) ?? new Set()).add(w));
  }
  for (const [key, n] of votes) {
    const [w, l] = key.split('|');
    if (n >= 2 && partnersW.get(w).size === 1 && partnersL.get(l).size === 1) tally('const', propose(w, l, 'const'));
  }
}

// ---- 4. vtables ----
{
  const e = fs.readFileSync(elfPath);
  const shoff = e.readUInt32LE(0x20), shentsize = e.readUInt16LE(0x2e), shnum = e.readUInt16LE(0x30);
  const sec = (i) => { const o = shoff + i * shentsize; return { type: e.readUInt32LE(o + 4), addr: e.readUInt32LE(o + 12), off: e.readUInt32LE(o + 16), size: e.readUInt32LE(o + 20), link: e.readUInt32LE(o + 24) }; };
  const secs = Array.from({ length: shnum }, (_, i) => sec(i));
  const vaOff = (va) => { const s = secs.find((x) => x.addr && va >= x.addr && va < x.addr + x.size && x.type !== 8); return s ? va - s.addr + s.off : -1; };
  const sym = secs.find((s) => s.type === 2), str = secs[sym.link];
  // GCC this-adjusting thunk: sub dword [esp+4], imm8/imm32 ; jmp rel8/rel32 (usually to the function right after it).
  // `this` sits at [esp+8] when a hidden struct-return pointer comes first.
  const thunkTarget = (a) => {
    const va = parseInt(a, 16), o = vaOff(va);
    if (o < 0 || e[o + 1] !== 0x6c || e[o + 2] !== 0x24 || (e[o + 3] !== 0x04 && e[o + 3] !== 0x08)) return null;
    const j = e[o] === 0x83 ? 5 : e[o] === 0x81 ? 8 : -1;
    if (j < 0) return null;
    const t = e[o + j] === 0xe9 ? hex(va + j + 5 + e.readInt32LE(o + j + 1)) : e[o + j] === 0xeb ? hex(va + j + 2 + e.readInt8(o + j + 1)) : null;
    return t && L.has(t) ? t : null;
  };
  const vtables = new Map();
  for (let i = 0; i < sym.size / 16; i++) {
    const o = sym.off + i * 16;
    let p = str.off + e.readUInt32LE(o), name = '';
    while (e[p]) name += String.fromCharCode(e[p++]);
    const m = name.match(/^_ZTV(\d+)(gs\w+)$/);
    if (!m || m[2].length !== Number(m[1])) continue;
    const va = e.readUInt32LE(o + 4), size = e.readUInt32LE(o + 8), fo = vaOff(va);
    if (fo < 0) continue;
    const words = [];
    for (let k = 0; k < size / 4; k++) words.push(e.readUInt32LE(fo + k * 4));
    vtables.set(m[2], words);
  }
  const rtti = JSON.parse(fs.readFileSync(rttiPath, 'utf8'));
  const purecall = '0x10120b90';
  for (const c of rtti) {
    const words = vtables.get(c.name);
    const primary = c.vtables.find((v) => v.subobjectOffset === 0);
    if (!words || !primary) continue;
    // primary vtable: [offset-to-top, typeinfo, functions...] until the next group's offset-to-top
    const fns = [];
    for (let k = 2; k < words.length; k++) {
      const a = hex(words[k]);
      if (!L.has(a)) break;
      fns.push(a);
    }
    const folded = [];
    for (let k = 0; k < fns.length; k++) {
      const n = L.get(fns[k]).name;
      folded.push(fns[k]);
      if (/::~/.test(n) && k + 1 < fns.length && /::~/.test(L.get(fns[k + 1]).name)) k++;  // D1 + D0 -> one slot
    }
    const mapSlots = (wList, lListIn, secondary = false) => {
      // MSVC orders a run of overloads in reverse declaration order, GCC in declaration order: per run of equal
      // names, take whichever direction fits the function sizes better
      const lList = [...lListIn];
      const cost = (ws, ls) => ws.reduce((s, w, i) => s + Math.abs(Math.log(((W.get(w)?.insns ?? 1) || 1) / (L.get(ls[i]).insns || 1))), 0);
      for (let k = 0; k < lList.length;) {
        let n = 1;
        while (k + n < lList.length && L.get(lList[k + n]).name === L.get(lList[k]).name) n++;
        if (n > 1) {
          const ws = wList.slice(k, k + n), ls = lList.slice(k, k + n), rev = [...ls].reverse();
          if (cost(ws, rev) < cost(ws, ls)) lList.splice(k, n, ...rev);
        }
        k += n;
      }
      for (let k = 0; k < lList.length; k++) {
        const w = wList[k], l = lList[k];
        if (!w || w === purecall || /pure_virtual/.test(L.get(l).name)) continue;
        // MSVC gives each secondary vtable a destructor thunk of its own; the primary slot names the real one
        if (secondary && /::~/.test(L.get(l).name)) continue;
        // inherited slots point at the base class's function: only the class that defines it should name it
        if (!L.get(l).name.startsWith(`${c.name}::`)) continue;
        tally('vtable', propose(w, l, 'vtable'));
      }
    };
    // secondary vtables (multiple inheritance): [offset-to-top < 0, typeinfo, thunks...], in subobject order. A GCC
    // thunk ("non-virtual thunk to X::f") stands for X::f; MSVC's entry is the function itself.
    const secondaries = c.vtables.filter((v) => v.subobjectOffset !== 0).sort((a, b) => a.subobjectOffset - b.subobjectOffset);
    const inSecondary = new Set();
    // The two compilers lay objects out differently (GCC's offsets are smaller), so groups pair up by order.
    let k = 2 + fns.length, g = 0;
    while (k + 2 <= words.length && g < secondaries.length) {
      if ((words[k] | 0) >= 0) break;
      const group = [];
      for (k += 2; k < words.length && L.has(hex(words[k])); k++) group.push(hex(words[k]));
      const real = group.map((a) => thunkTarget(a) ?? a);
      for (const a of real) inSecondary.add(a);
      const sfold = [];
      for (let i = 0; i < real.length; i++) {
        sfold.push(real[i]);
        if (/::~/.test(L.get(real[i]).name) && i + 1 < real.length && /::~/.test(L.get(real[i + 1]).name)) i++;
      }
      if (sfold.length === secondaries[g].methods.length) mapSlots(secondaries[g].methods, sfold, true);
      g++;
    }
    // GCC also gives an overrider of a secondary base's function a slot of its own in the primary vtable; MSVC
    // reaches it through the secondary vtable only. Without those the slot lists line up again. The destructor
    // keeps its slot when the primary base already had a virtual one.
    const own = folded.filter((a) => !inSecondary.has(a));
    const ownDtor = folded.filter((a) => !inSecondary.has(a) || /::~/.test(L.get(a).name));
    if (folded.length === primary.methods.length) mapSlots(primary.methods, folded);
    else if (own.length === primary.methods.length) { mapSlots(primary.methods, own); counts['vtable realigned classes'] = (counts['vtable realigned classes'] ?? 0) + 1; }
    else if (ownDtor.length === primary.methods.length) { mapSlots(primary.methods, ownDtor); counts['vtable realigned classes'] = (counts['vtable realigned classes'] ?? 0) + 1; }
    else counts['vtable skipped classes'] = (counts['vtable skipped classes'] ?? 0) + 1;
  }
}

// library and helper functions muddle call-list alignment: standard library, runtime and very widely called code
const indeg = (M) => {
  const d = new Map();
  for (const f of M.values()) for (const c of new Set(f.calls)) d.set(c, (d.get(c) ?? 0) + 1);
  return d;
};
const wIn = indeg(W), lIn = indeg(L);
const lLib = (a) => /^(std::|operator|__|<EXTERNAL>|_Unwind|__gnu|basic_|lodepng|SDL_|FMOD|_init|_fini|~)/.test(L.get(a)?.name ?? '') || (lIn.get(a) ?? 0) > 40;
const wLib = (a) => parseInt(a, 16) >= 0x10120000 || (wIn.get(a) ?? 0) > 40;
const sizeOk = (w, l) => {
  const a = W.get(w).insns, b = L.get(l).insns;
  return a > 0 && b > 0 && Math.max(a, b) / Math.min(a, b) <= 3;
};

// ---- 5. propagation through ordered call lists, callers and callee sets ----
const callers = (M) => {
  const m = new Map();
  for (const f of M.values()) for (const c of new Set(f.calls)) {
    if (!m.has(c)) m.set(c, []);
    m.get(c).push(f.entry);
  }
  return m;
};
const wCallers = callers(W), lCallers = callers(L);
for (let round = 0; round < 30; round++) {
  let made = 0;
  for (const [w, { l }] of [...w2l]) {
    const wc = W.get(w).calls.filter((a) => W.has(a) && (w2l.has(a) || !wLib(a)));
    const lc = L.get(l).calls.filter((a) => L.has(a) && (l2w.has(a) || !lLib(a)));
    // anchors: positions where both sides call a matched pair, in order
    const anchors = [[-1, -1]];
    let j = 0;
    for (let i = 0; i < wc.length; i++) {
      const m = w2l.get(wc[i]);
      if (!m) continue;
      const k = lc.indexOf(m.l, j);
      if (k >= 0) { anchors.push([i, k]); j = k + 1; }
    }
    anchors.push([wc.length, lc.length]);
    for (let a = 0; a + 1 < anchors.length; a++) {
      const wg = [...new Set(wc.slice(anchors[a][0] + 1, anchors[a + 1][0]).filter((x) => !w2l.has(x) && !bad.has('w' + x)))];
      const lg = [...new Set(lc.slice(anchors[a][1] + 1, anchors[a + 1][1]).filter((x) => !l2w.has(x) && !bad.has('l' + x)))];
      if (wg.length === 1 && lg.length === 1 && sizeOk(wg[0], lg[0])) made += propose(wg[0], lg[0], 'calls') ? 1 : 0;
    }
    // callers: a matched pair whose unmatched callers are one function on each side
    const wu = (wCallers.get(w) ?? []).filter((x) => !w2l.has(x) && !bad.has('w' + x));
    const lu = (lCallers.get(l) ?? []).filter((x) => !l2w.has(x) && !bad.has('l' + x));
    if (wu.length === 1 && lu.length === 1 && sizeOk(wu[0], lu[0])) made += propose(wu[0], lu[0], 'callers') ? 1 : 0;
  }
  // callee sets: unmatched functions whose matched callees (at least 3) map onto exactly the same set
  const sig = new Map();
  for (const f of W.values()) {
    if (w2l.has(f.entry) || bad.has('w' + f.entry)) continue;
    const s = [...new Set(f.calls.filter((a) => w2l.has(a)).map((a) => w2l.get(a).l))].sort();
    if (s.length < 3) continue;
    const key = s.join(' ');
    sig.set(key, (sig.get(key) ?? []).concat(f.entry));
  }
  const lsig = new Map();
  for (const f of L.values()) {
    if (l2w.has(f.entry) || bad.has('l' + f.entry)) continue;
    const s = [...new Set(f.calls.filter((a) => l2w.has(a)))].sort();
    if (s.length < 3) continue;
    const key = s.join(' ');
    lsig.set(key, (lsig.get(key) ?? []).concat(f.entry));
  }
  for (const [key, ws] of sig) {
    const ls = lsig.get(key);
    if (ws.length === 1 && ls?.length === 1 && sizeOk(ws[0], ls[0])) made += propose(ws[0], ls[0], 'calleeset') ? 1 : 0;
  }
  made += classNeighbourhood();
  counts[`propagation round ${round}`] = made;
  if (!made) break;
}

// Class neighbourhood: MSVC lays out a class's methods together, so an unmatched Windows function between two
// matched methods of class C is a method of C; it is compared only with C's unmatched Linux methods. Pairs are taken
// when they are mutual best matches by size, matched callees, call count and constants, with a clear margin.
function classNeighbourhood() {
  const clsOf = (name) => {
    const m = name.match(/^([A-Za-z_][\w<>, ]*)::[~\w]+$/);
    return m ? m[1] : null;
  };
  const wSorted = [...W.keys()].sort((a, b) => parseInt(a, 16) - parseInt(b, 16));
  const ctx = new Map();  // windows -> class
  let prevCls = null;
  let pending = [];
  for (const w of wSorted) {
    const m = w2l.get(w);
    if (m) {
      const c = clsOf(L.get(m.l).name);
      if (c && c === prevCls) for (const p of pending) ctx.set(p, c);
      prevCls = c;
      pending = [];
    } else if (!bad.has('w' + w) && !wLib(w) && W.get(w).insns >= 4) {
      pending.push(w);
    }
  }
  const lByCls = new Map();
  for (const f of L.values()) {
    if (l2w.has(f.entry) || bad.has('l' + f.entry) || lLib(f.entry) || f.insns < 4) continue;
    const c = clsOf(f.name);
    if (!c) continue;
    if (!lByCls.has(c)) lByCls.set(c, []);
    lByCls.get(c).push(f.entry);
  }
  const score = (w, l) => {
    const a = W.get(w), b = L.get(l);
    const size = Math.min(a.insns, b.insns) / Math.max(a.insns, b.insns);
    const wm = new Set(a.calls.filter((x) => w2l.has(x)).map((x) => w2l.get(x).l));
    const lm = new Set(b.calls.filter((x) => l2w.has(x)));
    const inter = [...wm].filter((x) => lm.has(x)).length;
    const callSim = wm.size + lm.size ? (2 * inter) / (wm.size + lm.size) : 0.5;
    const ws = new Set(a.consts), consts = b.consts.filter((x) => ws.has(x)).length;
    const constSim = ws.size + b.consts.length ? (2 * consts) / (ws.size + b.consts.length) : 0.5;
    return 0.45 * size + 0.4 * callSim + 0.15 * constSim;
  };
  const byCls = new Map();
  for (const [w, c] of ctx) {
    if (!byCls.has(c)) byCls.set(c, []);
    byCls.get(c).push(w);
  }
  let made = 0;
  for (const [c, ws] of byCls) {
    const ls = lByCls.get(c);
    if (!ls) continue;
    const bestOf = (x, list, f) => {
      let best = null, s1 = -1, s2 = -1;
      for (const y of list) {
        const s = f(x, y);
        if (s > s1) { s2 = s1; s1 = s; best = y; } else if (s > s2) s2 = s;
      }
      return { best, s1, s2 };
    };
    for (const w of ws) {
      if (w2l.has(w)) continue;
      const free = ls.filter((l) => !l2w.has(l));
      if (!free.length) break;
      const fw = bestOf(w, free, score);
      if (!fw.best || fw.s1 < 0.6 || (free.length > 1 && fw.s1 - fw.s2 < 0.1)) continue;
      const back = bestOf(fw.best, ws.filter((x) => !w2l.has(x)), (l, x) => score(x, l));
      if (back.best !== w) continue;
      made += propose(w, fw.best, 'class') ? 1 : 0;
    }
  }
  return made;
}

// ---- output ----
const rows = ['windows,linux,name,method'];
for (const [w, { l, method }] of [...w2l].sort()) {
  const n = L.get(l).name;
  rows.push(`${w},${l},${/[,"]/.test(n) ? `"${n.replace(/"/g, '""')}"` : n},${method}`);
}
fs.writeFileSync(outPath, rows.join('\n') + '\n');
const wFuncs = [...W.values()].filter((f) => f.insns > 3).length;
console.log(JSON.stringify(counts));
console.log(`matched ${w2l.size} functions (${(100 * w2l.size / L.size).toFixed(1)}% of Linux, ${(100 * w2l.size / wFuncs).toFixed(1)}% of non-trivial Windows), conflicts dropped ${bad.size}`);
