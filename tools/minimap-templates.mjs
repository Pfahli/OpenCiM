// Minimap reference check, step 1 (docs/ui/minimap.md §3.7). Resolve grid template ids -> {type, width, length, diagonals, flags} from the shipped scripts (approximation of
// loadGridObject + the environment.script helpers loadTree/loadProp/loadBuilding).
// usage: node minimap-templates.mjs <extractedDir> <out.json> [location dir, e.g. metro/locations/amsterdam]
import fs from 'node:fs';
import path from 'node:path';

const [EX, OUT, LOC] = process.argv.slice(2);
const files = [];
if (LOC) { const ls = fs.readFileSync(path.join(EX, LOC, 'location.script'), 'latin1'); for (const m of ls.matchAll(/"([^"]+.script)"/g)) files.push(path.join(EX, m[1])); }
else (function walk(d) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p); else if (e.name.endsWith('.script')) files.push(p);
  }
})(EX);

const TYPES = { NONE: 0, ALLEY: 1, SMALL_ROAD: 2, GROUND_RAMP: 3, ELEVATED_RAMP: 4, LARGE_ROAD: 5, AVENUE_ROAD: 6,
  EXPRESS_ROAD: 7, SINGLE_TRACK: 8, DOUBLE_TRACK: 9, SMALL_TRAM: 10, LARGE_TRAM: 11, AVENUE_TRAM: 12, GROUND_METRO: 13,
  SUBWAY_METRO: 14, ELEVATED_METRO: 15, METRO_STATION: 16, DOCK: 17, HELIPORT: 18, TREE: 19, BUILDING: 20, MARKER: 21,
  SPAWNPOINT: 22, TIMER: 23 };
const exists = (rel) => fs.existsSync(path.join(EX, rel));
const readTpl = (rel) => {
  const t = { type: 0, width: 1, length: 1, diagonals: 0, concave: false, script: rel };
  if (!exists(rel)) { t.missing = true; return t; }
  const s = fs.readFileSync(path.join(EX, rel), 'latin1');
  let m;
  if ((m = /\b(?:this)?\.width\s*=\s*(\d+)/.exec(s))) t.width = +m[1];
  if ((m = /\b(?:this)?\.length\s*=\s*(\d+)/.exec(s))) t.length = +m[1];
  if ((m = /\.type\s*=\s*GRID_ITEM_(\w+)/.exec(s))) t.type = TYPES[m[1]] ?? -1;
  if ((m = /\.diagonals\s*=\s*(0x[0-9a-fA-F]+|\d+)/.exec(s))) t.diagonals = Number(m[1]);
  if ((m = /\.flags\s*=\s*([^;]+);/.exec(s))) t.concave = /GRID_ITEM_CONCAVE/.test(m[1]);
  return t;
};
const stem = (p) => p.replace(/\.object$/, '');
const model = (m) => (m.startsWith('/') ? m.slice(1) : 'metro/objects/' + m);
const out = {};
const reg = (id, objPath, basePath) => {
  // loadGridObject step 3: with a base, own <stem>.script if it exists, else the base's script
  let scr = stem(objPath) + '.script';
  if (basePath && !exists(scr)) scr = stem(basePath) + '.script';
  out[id] = { ...readTpl(scr), generated: !!basePath && !exists(stem(objPath) + '.script') };
};
for (const f of files) {
  const s = fs.readFileSync(f, 'latin1');
  for (const m of s.matchAll(/loadGridObject\(\s*"([^"]+)"\s*,\s*"([^"]+)"(?:\s*,\s*"([^"]+)")?/g)) reg(m[1], m[2], m[3]);
  for (const m of s.matchAll(/load(?:Tree|Prop)\(\s*(?:"[^"]*"\s*,\s*)?"([^"]+)"\s*,\s*"([^"]+)"\s*(?:,\s*(true|false))?\s*\)/g)) {
    const mm = model(m[2]);
    reg(m[1], mm + '.object');
    if (m[3] === 'true') reg(m[1] + 'b', mm + 'b.object', mm + '.object');
    else if (exists(mm + 'b.object') || exists(mm + 'b.script')) reg(m[1] + 'b', mm + 'b.object');
  }
  for (const m of s.matchAll(/loadBuilding\(\s*"[^"]*"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"(?:\s*,\s*(\d+)\s*,\s*(\d+))?/g)) {
    const mm = model(m[2]);
    reg(m[1], mm + '.object');
    if (exists(mm + 'b.script')) reg(m[1] + 'b', mm + 'b.object');
    else if (m[3]) reg(m[1] + 'b', mm + 'b.object', mm + '.object');
  }
}
fs.writeFileSync(OUT, JSON.stringify(out));
const byType = {};
for (const t of Object.values(out)) byType[t.type] = (byType[t.type] ?? 0) + 1;
console.log(Object.keys(out).length, 'templates; by type', JSON.stringify(byType),
  'missing scripts', Object.values(out).filter((t) => t.missing).length, 'generated', Object.values(out).filter((t) => t.generated).length);
