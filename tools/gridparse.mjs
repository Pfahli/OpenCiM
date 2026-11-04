#!/usr/bin/env node
// gridparse - decode Cities in Motion map/save grids (*.grid, chunk family 0x07) and timelines (*.timeline).
//
// Layouts come from the decompiled writers/loaders in GSystem.dll (see docs/save-format/). Every decoder checks that
// it consumes its chunk exactly; mismatches are reported as issues instead of being silently skipped.
//
// Usage:
//   node gridparse.mjs summary <file.grid|file.timeline> [...]     counts + validation per file
//   node gridparse.mjs dump <file> <section> [limit]                JSON records of one section
//        sections: header terrain types items names vehicles characters citizens lines timeline

import fs from 'node:fs';
import path from 'node:path';

export class Reader {
  constructor(buf) { this.buf = buf; }
  chunks(start, end) {
    const out = [];
    for (let p = start; p < end;) {
      if (p + 8 > end) throw new Error(`truncated chunk header at 0x${p.toString(16)}`);
      const tag = this.buf.readUInt32LE(p), size = this.buf.readUInt32LE(p + 4);
      if (p + 8 + size > end) throw new Error(`chunk 0x${tag.toString(16)} at 0x${p.toString(16)} overruns its parent`);
      out.push({ tag, size, body: p + 8, end: p + 8 + size });
      p += 8 + size;
    }
    return out;
  }
  // C string (latin1/UTF-8 bytes) starting at p; returns [string, nextOffset]
  cstr(p, end, utf8 = false) {
    let e = p;
    while (e < end && this.buf[e] !== 0) e++;
    return [this.buf.toString(utf8 ? 'utf8' : 'latin1', p, e), e + 1];
  }
}

const hex = (n) => `0x${(n >>> 0).toString(16).padStart(8, '0')}`;
const T = {
  GRID: 0x07000000, TERRAIN: 0x07010000, NETWORK: 0x07020000, TYPES: 0x07020100, ITEMS_V0: 0x07020200,
  ITEMS: 0x07020210, ITEMS_V2: 0x07020211, NAMES_V0: 0x07020300, NAMES: 0x07020310,
  VEHICLES: 0x07040000, CHARACTERS: 0x07050000, CHARACTERS_ALT: 0x07050001, LINES: 0x07060000, CITIZENS: 0x07070000,
  TIMELINE: 0x07080000,
};

// ---- section decoders: each returns { summary, records?, issues[] } ----
export const decoders = {
  header(r, body) {
    const b = r.buf;
    return {
      width: b.readUInt32LE(body), length: b.readUInt32LE(body + 4), cellSize: +b.readFloatLE(body + 8).toFixed(4),
      bucketShift: b.readUInt32LE(body + 12), version: b.readUInt16LE(body + 16), reserved: [b.readUInt16LE(body + 18), b.readUInt32LE(body + 20)],
    };
  },

  terrain(r, c, hdr) {
    const issues = [];
    const n = hdr.width * hdr.length;
    if (c.size !== n * 4) issues.push(`terrain size ${c.size} != width*length*4 (${n * 4})`);
    let min = Infinity, max = -Infinity;
    const flagHist = new Map();
    for (let i = 0; i < Math.min(n, c.size / 4); i++) {
      const h = r.buf.readInt16LE(c.body + i * 4);
      const f = r.buf.readUInt16LE(c.body + i * 4 + 2);
      if (h < min) min = h;
      if (h > max) max = h;
      flagHist.set(f, (flagHist.get(f) ?? 0) + 1);
    }
    const topFlags = [...flagHist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 8).map(([f, k]) => `0x${f.toString(16)}:${k}`);
    // bit names from docs/save-format/terrain-characters-citizens.md (0xf00 / 0x4000 / 0x8000 are transient)
    const BITS = { pavement: 0x8, blocked: 0x10, field: 0x20, pavementPreview: 0x40, coverQuadrants: 0xf00, groundLocked: 0x1000, water: 0x2000, highlight: 0xc000 };
    const bitCounts = Object.fromEntries(Object.entries(BITS).map(([k, m]) => [k, [...flagHist.entries()].reduce((s, [f, c]) => s + ((f & m) ? c : 0), 0)]));
    return { summary: { cells: n, heightMin: min, heightMax: max, heightUnit: '1/256 cell', cellsWithBit: bitCounts, topCellFlags: topFlags }, issues };
  },

  network(r, c) {
    const b = r.buf, issues = [];
    const typeCount = b.readUInt32LE(c.body), itemCount = b.readUInt32LE(c.body + 4);
    const kids = r.chunks(c.body + 8, c.end);
    const out = { types: [], items: [], names: [], issues };
    for (const k of kids) {
      if (k.tag === T.TYPES) {
        let p = k.body;
        for (let i = 0; i < typeCount; i++) {
          const [s, nxt] = r.cstr(p, k.end);
          out.types.push(s);
          p = nxt;
        }
        if (k.end - p > 4) issues.push(`type table: ${k.end - p} unread bytes`);
      } else if (k.tag === T.ITEMS || k.tag === T.ITEMS_V2) {
        if (k.size !== itemCount * 16) issues.push(`items: size ${k.size} != itemCount*16 (${itemCount * 16})`);
        for (let i = 0, p = k.body; i < itemCount && p + 16 <= k.end; i++, p += 16) {
          const tw = b.readUInt16LE(p), fl = b.readUInt32LE(p + 12);
          out.items.push({
            type: tw & 0x3fff, orient: tw >>> 14, x: b.readInt16LE(p + 2), y: b.readInt16LE(p + 4), z: b.readInt16LE(p + 6),
            parent: b.readUInt32LE(p + 8), variation: fl & 0xff, flags: fl & ~0xff,
          });
        }
      } else if (k.tag === T.ITEMS_V0) {
        issues.push('old 8-byte item format (0x07020200) present: decoded counts only');
      } else if (k.tag === T.NAMES || k.tag === T.NAMES_V0) {
        // 0x07020310: UTF-8, each entry padded to 4 bytes; 0x07020300 (older maps): narrow Latin-1, no padding
        let p = k.body;
        for (let i = 0; i < itemCount; i++) {
          const start = p;
          const [s, nxt] = r.cstr(p, k.end, k.tag === T.NAMES);
          out.names.push(s);
          p = k.tag === T.NAMES ? start + Math.ceil((nxt - start) / 4) * 4 : nxt;
        }
        // the old table is padded once at its end (0-3 bytes); the new one pads every entry
        const slack = k.end - p;
        if (k.tag === T.NAMES ? slack !== 0 : slack < 0 || slack > 3) issues.push(`name table: ended at +${p - k.body}, chunk is ${k.size}`);
      } else {
        issues.push(`unknown network child ${hex(k.tag)}`);
      }
    }
    for (const it of out.items) {
      if (it.type >= out.types.length) { issues.push('item type index out of range'); break; }
    }
    for (const it of out.items) {
      if (it.parent > out.items.length) { issues.push('item parent index out of range'); break; }
    }
    out.summary = { typeCount, itemCount, typesRead: out.types.length, itemsRead: out.items.length, namedItems: out.names.filter(Boolean).length };
    return out;
  },
};

// Section decoders contributed by docs/save-format/*.md layouts are registered here.
export const childDecoders = new Map(); // tag -> (reader, chunk, ctx) => { summary, records, issues }

function nameTable(r, k, count) {
  const names = [];
  let p = k.body;
  for (let i = 0; i < count; i++) {
    const [s, nxt] = r.cstr(p, k.end);
    names.push(s);
    p = nxt;
  }
  return { names, rest: k.end - p };
}

// ---- movement path 0x07030100 (docs/save-format/vehicles-movement.md); shared by vehicles and characters ----
function movement(r, k, issues) {
  const b = r.buf, p = k.body;
  const stepCount = b.readInt32LE(p), nibbleCount = b.readInt32LE(p + 4);
  const words = nibbleCount <= 0 ? 0 : (nibbleCount + 7) >> 3;
  if (k.size !== 20 + 4 * words) issues.push(`movement size ${k.size} != 20+4*${words}`);
  // codes 0-7: take link code+1; codes 8-15: repeat link 0 (code-7) times
  let steps = 0;
  for (let i = 0; i < nibbleCount; i++) {
    const code = (b.readUInt32LE(p + 20 + 4 * (i >> 3)) >>> (4 * (i & 7))) & 0xf;
    steps += code < 8 ? 1 : code - 7;
  }
  if (nibbleCount > 0 && steps !== stepCount) issues.push(`movement decodes ${steps} steps, header says ${stepCount}`);
  return { stepCount, nibbleCount, travelCost: b.readFloatLE(p + 8), cursor: b.readUInt32LE(p + 12) };
}

// ---- AI vehicles 0x07040000 (docs/save-format/vehicles-movement.md) ----
childDecoders.set(T.VEHICLES, (r, c, ctx) => {
  const b = r.buf, issues = [];
  const modelCount = b.readUInt32LE(c.body), count = b.readUInt32LE(c.body + 4);
  const kids = r.chunks(c.body + 8, c.end);
  const models = kids[0]?.tag === 0x07040100 ? nameTable(r, kids[0], modelCount).names : [];
  const itemCount = ctx.result.sections[hex(T.NETWORK)]?.items.length ?? Infinity;
  const vehicles = [];
  const lists = {};
  let inlineNodes = 0;
  const checkRef = (ref) => {
    if (!ref || (ref >>> 24) === 0x7f || (ref & 0x80000000)) return;
    if ((ref >>> 8) > itemCount) issues.push(`vehicle node ref -> item ${ref >>> 8} out of range`);
  };
  for (const k of kids.slice(1)) {
    const p = k.body;
    if (k.tag === 0x07040200) {
      const ringCount = b.readUInt16LE(p + 8);
      const v = {
        model: b.readUInt16LE(p), ringCount, posOnNode: b.readInt32LE(p + 12) / 65536, nextCar: b.readUInt32LE(p + 16), prevCar: b.readUInt32LE(p + 20),
        speed: b.readFloatLE(p + 24), waitTime: b.readFloatLE(p + 32), pathStepsLeft: b.readInt32LE(p + 60), flags: b.readUInt32LE(p + 64),
        curNode: b.readUInt32LE(p + 68), prevNode: b.readUInt32LE(p + 72),
      };
      if (v.model >= modelCount) issues.push(`vehicle model ${v.model} >= ${modelCount}`);
      if (v.nextCar > count || v.prevCar > count) issues.push('carriage ref out of range');
      checkRef(v.curNode);
      checkRef(v.prevNode);
      for (let i = 0; i < ringCount; i++) checkRef(b.readUInt32LE(p + 88 + 4 * i));
      const sub = r.chunks(p + 88 + 4 * ringCount, k.end);
      for (const s of sub) {
        if (s.tag === 0x07030200) { inlineNodes++; if (s.size !== 84) issues.push(`inline node size ${s.size}`); }
        else if (s.tag === 0x07030100) v.movement = movement(r, s, issues);
        else issues.push(`unknown vehicle child ${hex(s.tag)}`);
      }
      if (sub.at(-1)?.tag !== 0x07030100) issues.push('vehicle record does not end with a movement chunk');
      vehicles.push(v);
    } else if (k.tag === 0x07040300 || k.tag === 0x07040400 || k.tag === 0x07040500) {
      const n = b.readUInt32LE(p);
      if (k.size !== 4 + 4 * n) issues.push(`list ${hex(k.tag)} size mismatch`);
      const refs = [];
      for (let i = 0; i < n; i++) refs.push(b.readUInt32LE(p + 4 + 4 * i));
      if (refs.some((x) => x < 1 || x > count)) issues.push(`list ${hex(k.tag)} ref out of range`);
      lists[{ 0x07040300: 'trains', 0x07040400: 'aircraft', 0x07040500: 'boats' }[k.tag]] = refs.length;
    } else issues.push(`unknown vehicles child ${hex(k.tag)}`);
  }
  if (vehicles.length !== count) issues.push(`vehicleCount ${count} but ${vehicles.length} records`);
  return { name: 'vehicles', records: vehicles, issues, summary: { modelCount, vehicleCount: count, ...lists, inlinePathNodes: inlineNodes, models } };
});

// ---- characters (pedestrians, animals) 0x07050000 (docs/save-format/terrain-characters-citizens.md) ----
childDecoders.set(T.CHARACTERS, (r, c) => {
  const b = r.buf, issues = [];
  const typeCount = b.readUInt32LE(c.body), count = b.readUInt32LE(c.body + 4);
  const kids = r.chunks(c.body + 8, c.end);
  const types = kids[0]?.tag === 0x07050100 ? nameTable(r, kids[0], typeCount).names : [];
  const chars = [];
  const lists = {};
  for (const k of kids.slice(1)) {
    const p = k.body;
    if (k.tag === 0x07050200) {
      const ch = { type: b.readUInt16LE(p), x: b.readInt32LE(p + 4) / 4, y: b.readInt32LE(p + 8), z: b.readInt32LE(p + 16) / 4, heading: b.readFloatLE(p + 72), flags: b.readUInt32LE(p + 76) };
      if (ch.type >= typeCount) issues.push(`character type ${ch.type} >= ${typeCount}`);
      const sub = r.chunks(p + 80, k.end);
      if (sub.length !== 1 || sub[0].tag !== 0x07030100) issues.push('character record must hold exactly one 0x07030100');
      else ch.movement = movement(r, sub[0], issues);
      chars.push(ch);
    } else if (k.tag === 0x07050300 || k.tag === 0x07050400) {
      const n = b.readUInt32LE(p);
      if (k.size !== 4 + 4 * n) issues.push(`animal list size mismatch`);
      for (let i = 0; i < n; i++) if (b.readUInt32LE(p + 4 + 4 * i) > count) { issues.push('animal ref out of range'); break; }
      lists[k.tag === 0x07050300 ? 'flyingAnimals' : 'groundAnimals'] = n;
    } else issues.push(`unknown characters child ${hex(k.tag)}`);
  }
  if (chars.length !== count) issues.push(`count ${count} but ${chars.length} records`);
  const byType = {};
  for (const ch of chars) byType[types[ch.type]] = (byType[types[ch.type]] ?? 0) + 1;
  return { name: 'characters', records: chars, issues, summary: { typeCount, count, ...lists, byType } };
});

// ---- citizens 0x07070000 (docs/save-format/terrain-characters-citizens.md) ----
const CLASSES = ['blue_collar', 'white_collar', 'business', 'student', 'tourist', 'pensioner', 'unemployed'];
childDecoders.set(T.CITIZENS, (r, c, ctx) => {
  const b = r.buf, issues = [];
  const count = b.readUInt32LE(c.body);
  const stat = (o) => Array.from({ length: 7 }, (_, i) => b.readUInt32LE(c.body + o + 4 * i));
  const stats = { walk: stat(4), drive: stat(32), travel: stat(60), canTravel: stat(88), priceTooHigh: stat(116) };
  const S = ctx.result.sections;
  const nItems = S[hex(T.NETWORK)]?.items.length ?? Infinity;
  const nChars = S[hex(T.CHARACTERS)]?.records?.length ?? Infinity;
  const nCars = S[hex(T.VEHICLES)]?.records?.length ?? Infinity;
  const lineRecs = S[hex(T.LINES)]?.records ?? [];
  const nStops = lineRecs.reduce((s, l) => s + l.stops.length, 0), nLineVeh = lineRecs.reduce((s, l) => s + l.vehicles.length, 0);
  const citizens = [];
  const byClass = {};
  let routes = 0;
  const bad = new Set();
  for (const k of r.chunks(c.body + 144, c.end)) {
    if (k.tag !== 0x07070100) { issues.push(`unknown citizens child ${hex(k.tag)}`); continue; }
    const p = k.body, fl = b.readUInt32LE(p);
    const z = {
      class: fl & 0xf, state: (fl >>> 4) & 0xf, visitType: (fl >>> 12) & 0xff, mood: (fl >>> 24) & 7,
      character: b.readUInt32LE(p + 24), car: b.readUInt32LE(p + 28), lineNode: b.readUInt32LE(p + 32), transportVehicle: b.readUInt32LE(p + 36),
      home: b.readUInt32LE(p + 40), workplace: b.readUInt32LE(p + 44), start: b.readUInt32LE(p + 48), target: b.readUInt32LE(p + 52),
    };
    if (z.class > 6) bad.add('class > 6');
    if (z.character > nChars) bad.add('character ref out of range');
    if (z.car > nCars) bad.add('car ref out of range');
    if (z.lineNode > nStops) bad.add('lineNode out of range');
    if (z.transportVehicle > nLineVeh) bad.add('transportVehicle out of range');
    for (const f of ['home', 'workplace', 'start', 'target']) if (z[f] > nItems) bad.add(`${f} item out of range`);
    const sub = r.chunks(p + 64, k.end);
    for (const s of sub) {
      if (s.tag !== 0x07061000) { bad.add(`unknown citizen child ${hex(s.tag)}`); continue; }
      const n = b.readUInt32LE(s.body + 4);
      if (s.size !== 12 + 4 * n) bad.add('route size mismatch');
      for (let i = 0; i < n; i++) if (b.readUInt32LE(s.body + 12 + 4 * i) > nStops) { bad.add('route node out of range'); break; }
      routes++;
    }
    byClass[CLASSES[z.class] ?? z.class] = (byClass[CLASSES[z.class] ?? z.class] ?? 0) + 1;
    citizens.push(z);
  }
  issues.push(...bad);
  if (citizens.length !== count) issues.push(`count ${count} but ${citizens.length} records`);
  return { name: 'citizens', records: citizens, issues, summary: { count, byClass, withPlannedRoute: routes, tripStats: stats } };
});

// ---- transport lines 0x07060000 (docs/save-format/lines-timeline.md) ----
const LINE_TYPES = ['bus', 'tram', 'metro', 'boat', 'helicopter'];
childDecoders.set(T.LINES, (r, c, ctx) => {
  const b = r.buf, issues = [];
  const [modelCount, lineCount, vehicleCount, stopCount] = [0, 4, 8, 12].map((o) => b.readUInt32LE(c.body + o));
  const kids = r.chunks(c.body + 16, c.end);
  const models = kids[0]?.tag === 0x07060100 ? nameTable(r, kids[0], modelCount).names : [];
  if (kids[0]?.tag !== 0x07060100) issues.push('missing model name table 0x07060100');
  const lines = [];
  for (const k of kids.filter((x) => x.tag === 0x07060200)) {
    const h = k.body;
    const line = {
      type: LINE_TYPES[b.readUInt32LE(h + 4)] ?? b.readUInt32LE(h + 4), headway: b.readFloatLE(h), effAttractiveness: b.readFloatLE(h + 8),
      vehicleCount: b.readUInt32LE(h + 12), stopCount: b.readUInt32LE(h + 16), avgWait: b.readFloatLE(h + 20),
      ticketPrice: b.readFloatLE(h + 24), passengersTransported: b.readInt32LE(h + 28), isClosed: b[h + 32], stops: [], vehicles: [],
    };
    for (const s of r.chunks(h + 36, k.end)) {
      const p = s.body;
      if (s.tag === 0x07060220) {
        if (s.size !== 20) issues.push(`stop record size ${s.size}`);
        line.stops.push({ networkItem: b.readUInt32LE(p), flags: b.readUInt32LE(p + 4), sinceDeparture: b.readFloatLE(p + 8), passengersTransported: b.readInt32LE(p + 12), waitingTime: b.readInt32LE(p + 16) });
      } else if (s.tag === 0x07060210) {
        const unitCount = b.readUInt32LE(p + 48);
        if (s.size !== 60 + 4 * unitCount) issues.push(`line vehicle size ${s.size} != 60+4*${unitCount}`);
        const units = [];
        for (let u = 0; u < unitCount; u++) units.push({ model: b.readUInt16LE(p + 60 + u * 4), reversed: b[p + 62 + u * 4] });
        line.vehicles.push({
          gridVehicle: b.readUInt32LE(p), stopA: b.readUInt32LE(p + 4), stopB: b.readUInt32LE(p + 8), dockedAt: b.readUInt32LE(p + 12),
          stopTime: b.readFloatLE(p + 16), respawnRepathTimer: b.readFloatLE(p + 20), nextLegSteps: b.readInt32LE(p + 24),
          passengerCount: b.readInt32LE(p + 32), boardingReserved: b.readInt32LE(p + 36), attractiveness: b.readFloatLE(p + 40),
          travelDistance: b.readFloatLE(p + 44), bellRung: b[p + 52], announced: b[p + 53], atStopB: b[p + 54], broken: b[p + 55], passengersTransported: b.readInt32LE(p + 56), units,
        });
      } else issues.push(`unknown line child ${hex(s.tag)}`);
    }
    if (line.stops.length !== line.stopCount || line.vehicles.length !== line.vehicleCount) issues.push('line stop/vehicle count mismatch');
    lines.push(line);
  }
  if (lines.length !== lineCount) issues.push(`lineCount ${lineCount} but ${lines.length} line chunks`);
  const allStops = lines.flatMap((l) => l.stops), allVeh = lines.flatMap((l) => l.vehicles);
  if (allStops.length !== stopCount) issues.push(`stopCount ${stopCount} != ${allStops.length}`);
  if (allVeh.length !== vehicleCount) issues.push(`vehicleCount ${vehicleCount} != ${allVeh.length}`);
  // cross-check stops against the network items
  const net = ctx.result.sections[hex(T.NETWORK)];
  let stopItemsOk = 0;
  for (const s of allStops) {
    const it = net?.items[s.networkItem - 1];
    if (!it) { issues.push(`stop -> missing network item ${s.networkItem}`); continue; }
    if (/stop|station|dock|heli|pier|port|terminal|platform/i.test(net.types[it.type] ?? '')) stopItemsOk++;
  }
  for (const v of allVeh) for (const u of v.units) if (u.model >= models.length) { issues.push('unit model index out of range'); break; }
  const nGridVeh = ctx.result.sections[hex(T.VEHICLES)]?.records?.length ?? Infinity;
  if (allVeh.some((v) => v.gridVehicle > nGridVeh)) issues.push('line vehicle -> grid vehicle out of range');
  return {
    name: 'lines', records: lines, issues,
    summary: { modelCount, lineCount, vehicleCount, stopCount, models, stopsOnStopItems: `${stopItemsOk}/${allStops.length}`, lineTypes: lines.map((l) => l.type) },
  };
});

// ---- timeline 0x07080000 (docs/save-format/lines-timeline.md); day = days since 1920-01-01 ----
const ACTION = { 0x07081010: 'group', 0x07081020: 'create', 0x07081030: 'createWithParent', 0x07081040: 'delete', 0x07081050: 'brush', 0x07081060: 'water', 0x07081070: 'name_v0', 0x07081071: 'name', 0x07081080: 'variation' };
const FIXED = { 0x07081020: 44, 0x07081030: 88, 0x07081040: 44, 0x07081060: 8, 0x07081080: 48 };
function objRef(r, p) {
  const b = r.buf;
  return { name: r.cstr(p, p + 32)[0], x: b.readInt16LE(p + 32), y: b.readInt16LE(p + 34), z: b.readInt16LE(p + 36), orient: b.readUInt16LE(p + 38), flags: b.readUInt32LE(p + 40) };
}
function decodeActions(r, start, end, counts, issues, depth) {
  const out = [];
  for (const a of r.chunks(start, end)) {
    const kind = ACTION[a.tag];
    counts[kind ?? hex(a.tag)] = (counts[kind ?? hex(a.tag)] ?? 0) + 1;
    if (!kind) { issues.push(`unknown action ${hex(a.tag)}`); continue; }
    if (FIXED[a.tag] && a.size !== FIXED[a.tag]) issues.push(`${kind} size ${a.size} != ${FIXED[a.tag]}`);
    const p = a.body, b = r.buf;
    let rec;
    if (kind === 'group') rec = { label: r.cstr(p, p + 32)[0], children: decodeActions(r, p + 32, a.end, counts, issues, depth + 1) };
    else if (kind === 'create' || kind === 'delete') rec = { object: objRef(r, p) };
    else if (kind === 'createWithParent') rec = { object: objRef(r, p), parent: objRef(r, p + 44) };
    else if (kind === 'brush') {
      const count = b.readUInt16LE(p + 2);
      if (a.size !== 8 + 4 * count) issues.push(`brush size ${a.size} != 8+4*${count}`);
      rec = { brushType: b[p], radius: b.readInt8(p + 1), strength: b.readInt16LE(p + 4), points: count };
    } else if (kind === 'water') rec = { x: b.readInt16LE(p), z: b.readInt16LE(p + 4), on: b.readUInt16LE(p + 6) };
    else if (kind === 'variation') rec = { object: objRef(r, p), variation: b.readUInt32LE(p + 44) };
    else rec = { object: objRef(r, p), text: r.cstr(p + 44, a.end, kind === 'name')[0] };
    out.push({ kind, ...rec });
  }
  return out;
}
childDecoders.set(T.TIMELINE, (r, c) => {
  const b = r.buf, issues = [];
  const dayCount = b.readUInt32LE(c.body), currentDay = b.readUInt32LE(c.body + 4);
  const counts = {};
  const days = [];
  let prev = -1;
  for (const d of r.chunks(c.body + 8, c.end)) {
    if (d.tag !== 0x07081000) { issues.push(`unknown timeline child ${hex(d.tag)}`); continue; }
    const day = b.readUInt32LE(d.body);
    if (day <= prev) issues.push(`day ${day} not ascending after ${prev}`);
    prev = day;
    days.push({ day, actions: decodeActions(r, d.body + 8, d.end, counts, issues, 0) });
  }
  if (days.length && dayCount !== prev + 1) issues.push(`dayCount ${dayCount} != last day + 1 (${prev + 1})`);
  const toDate = (n) => new Date(Date.UTC(1920, 0, 1) + n * 86400000).toISOString().slice(0, 10);
  return {
    name: 'timeline', records: days, issues,
    summary: { dayCount, currentDay, currentDate: toDate(currentDay), dayGroups: days.length, firstDate: days.length ? toDate(days[0].day) : null, lastDate: days.length ? toDate(prev) : null, actions: counts },
  };
});

export function parseGrid(buf) {
  const r = new Reader(buf);
  const [root] = r.chunks(0, buf.length);
  if (root.tag !== T.GRID) throw new Error(`not a grid (root ${hex(root.tag)})`);
  const header = decoders.header(r, root.body);
  const result = { kind: 'grid', header, sections: {}, issues: [] };
  for (const c of r.chunks(root.body + 24, root.end)) {
    let d;
    if (c.tag === T.TERRAIN) d = decoders.terrain(r, c, header);
    else if (c.tag === T.NETWORK) d = decoders.network(r, c);
    else if (childDecoders.has(c.tag)) d = childDecoders.get(c.tag)(r, c, { header, result });
    else d = { summary: { size: c.size, decoded: false }, issues: [`no decoder for ${hex(c.tag)}`] };
    result.sections[hex(c.tag)] = d;
    for (const i of d.issues ?? []) result.issues.push(`${hex(c.tag)}: ${i}`);
  }
  return result;
}

export function parseFile(file) {
  const buf = fs.readFileSync(file);
  const tag = buf.readUInt32LE(0);
  if (tag === T.GRID) return parseGrid(buf);
  if (tag === T.TIMELINE && childDecoders.has(T.TIMELINE)) {
    const r = new Reader(buf);
    const [root] = r.chunks(0, buf.length);
    const d = childDecoders.get(T.TIMELINE)(r, root, {});
    return { kind: 'timeline', sections: { [hex(T.TIMELINE)]: d }, issues: d.issues ?? [] };
  }
  throw new Error(`${file}: unsupported root ${hex(tag)}`);
}

function main(argv) {
  const [cmd, ...args] = argv;
  if (cmd === 'summary') {
    for (const f of args) {
      try {
        const res = parseFile(f);
        const sums = Object.fromEntries(Object.entries(res.sections).map(([k, v]) => [k, v.summary]));
        console.log(JSON.stringify({ file: path.basename(path.dirname(f)) + '/' + path.basename(f), header: res.header, sections: sums, issues: res.issues }, null, 1));
      } catch (e) {
        console.log(JSON.stringify({ file: f, error: e.message }));
      }
    }
  } else if (cmd === 'dump') {
    const [file, section, limit = '20'] = args;
    const res = parseFile(file);
    const net = res.sections[hex(T.NETWORK)];
    const pick = {
      header: () => res.header, types: () => net?.types, items: () => net?.items, names: () => net?.names,
    }[section];
    const data = pick ? pick() : Object.values(res.sections).find((s) => s.name === section)?.records;
    console.log(JSON.stringify(Array.isArray(data) ? data.slice(0, Number(limit)) : data, null, 1));
  } else {
    console.error('usage: gridparse.mjs summary <file...> | dump <file> <section> [limit]');
    process.exit(2);
  }
}

if (process.argv[1]?.endsWith('gridparse.mjs')) main(process.argv.slice(2));
