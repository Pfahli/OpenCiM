#!/usr/bin/env node
// fontparse - decode Cities in Motion (GSystem) fonts and reproduce the engine's text metrics.
//
// Spec: docs/ui/fonts.md. Evidence: Linux build gsFonts_OGL::readFontChunk / readBMFdata / getFont,
// gsFont_OGL::renderText, gsGuiText_OGL::textWidth / textHeight / charWidth (decompiled/Linux), cross-checked
// against GSystem.dll (FUN_1003f260, FUN_1003f610, FUN_1003d410, 0x10044230, 0x10043b60, 0x10043d80).
//
// Two formats are accepted (decided by the first 4 bytes, like the engine):
//   native  chunk file, root tag 0x03000000, child 0x03010000 (256 x f32 u,v,w,h) or 0x03020000 (256 x i32 / 1024);
//           page texture = same path with the extension changed to .dds
//   BMFont  AngelCode binary v3 ("BMF\x03"); pages = <font dir>/<page name>
//
// Usage:
//   node fontparse.mjs info    <font> [--glyphs "<chars>"] [--all] [--json] [--root <dir> | --game <dir>]
//   node fontparse.mjs measure <font> "<text>" <sizeX> [spacingX] [--sizeY n] [--spacingY n] [--box w]
//                              [--indent x] [--extra <name>] [--layout] [--root <dir> | --game <dir>]
//   node fontparse.mjs check   <dir | gameDir>          parse every .font/.fnt (game dir: inside the .gs archives)
//
// <font> is a .font/.fnt file, or a font directory / engine font name ("metro/fonts/tahoma-8"). A font name is
// resolved like gsFonts_OGL::loadFont: <name>/normal.fnt, else <name>/normal.font; --extra <x> then merges
// <name>/<x>.fnt|.font (the 7th setFont argument; the UI passes MAIN.$language, e.g. "ja" or "languages").
// With --game, paths are looked up in the game's top-level .gs archives (then loose files); with --root, in that
// directory; otherwise relative to the current directory.
//
// "<text>" accepts JS-style escapes: \n \t \r \\ \xHH \uHHHH, and \c<AARRGGBB> as shorthand for the inline colour
// code U+0004 + 8 hex digits; \e is U+0005 (colour reset). Characters are UTF-16 code units, as on Windows.
//
// Arithmetic follows the Windows build (x87, 53-bit precision): every value the engine keeps in a float variable
// is rounded with Math.fround at the point where GSystem.dll stores it (checked against the disassembly of
// textWidth 0x10044230 and textHeight 0x10043b60).

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { readArchive, readEntry } from './gstool.mjs';

const f32 = Math.fround;
const MAGIC_BMF = 0x03464d42; // "BMF\x03"
const TAG_ROOT = 0x03000000, TAG_GLYPHS = 0x03010000, TAG_GLYPHS_FIXED = 0x03020000;
// readFontChunk / readBMFdata: after storing glyph i, the same glyph is also stored under this Unicode code point.
export const ALIASES = { 0x85: 0x2026, 0x8c: 0x0152, 0x92: 0x2019, 0x93: 0x201c, 0x94: 0x201d, 0x96: 0x2013, 0x9c: 0x0153 };
const STYLE_NAMES = ['normal']; // gsFontStyle -> file stem (DAT_1018fa2c holds a single wstring, L"normal")

// ---------------------------------------------------------------------------------------------- file source
export class FileSource {
  constructor({ root = null, game = null } = {}) {
    this.root = root;
    this.map = null;
    if (game) {
      this.map = new Map();
      for (const f of fs.readdirSync(game).filter((n) => n.toLowerCase().endsWith('.gs')).sort()) {
        let a;
        try { a = readArchive(path.join(game, f)); } catch { continue; }
        for (const e of a.entries) {
          const key = a.mount + e.name;
          if (!this.map.has(key)) this.map.set(key, () => readEntry(a, e)); // first mount wins
        }
      }
      this.root = game; // loose files as a fallback
    }
  }
  read(vpath) {
    const norm = vpath.replace(/\\/g, '/');
    if (this.map?.has(norm)) return this.map.get(norm)();
    const p = this.root && !path.isAbsolute(norm) ? path.join(this.root, norm) : norm;
    try { return fs.readFileSync(p); } catch { return null; }
  }
  exists(vpath) {
    const norm = vpath.replace(/\\/g, '/');
    if (this.map?.has(norm)) return true;
    const p = this.root && !path.isAbsolute(norm) ? path.join(this.root, norm) : norm;
    return fs.existsSync(p) && fs.statSync(p).isFile();
  }
}

const dirOf = (p) => { const i = Math.max(p.lastIndexOf('/'), p.lastIndexOf('\\')); return i < 0 ? '' : p.slice(0, i + 1); };
const changeExt = (p, ext) => { const s = Math.max(p.lastIndexOf('/'), p.lastIndexOf('\\')); const d = p.lastIndexOf('.'); return (d > s ? p.slice(0, d) : p) + ext; };

// ---------------------------------------------------------------------------------------------- font model
// In-memory glyph (gsCharData_OGL, 36 bytes): u, v, w, h (texture rect, normalised), xoff, yoff (normalised offsets),
// adv (normalised advance), lh (font line height, normalised V units), page (index into font.pages).
export function newFont() {
  return { lineHeight: -1, pages: [], glyphs: new Map(), extras: new Set(), files: [], notes: [] };
}

function setGlyph(font, code, g) {
  const key = code & 0xffff; // GSystem.dll keys the std::map with a 16-bit wchar_t
  font.glyphs.set(key, { ...g });
}

export function loadFontData(font, src, vpath) {
  const data = src.read(vpath);
  if (!data) { font.notes.push(`missing ${vpath}`); return null; }
  font.files.push(vpath);
  if (data.length >= 4 && data.readUInt32LE(0) === MAGIC_BMF) return readBMF(font, data, dirOf(vpath), vpath);
  return readNative(font, data, vpath);
}

function readNative(font, b, vpath) {
  const info = { file: vpath, format: 'native', chunks: [] };
  if (b.length < 8 || b.readUInt32LE(0) !== TAG_ROOT) {
    font.notes.push(`${vpath}: root tag 0x${b.length >= 4 ? b.readUInt32LE(0).toString(16) : '?'} is not 0x03000000; ignored`);
    info.ignored = true;
    return info;
  }
  const rootSize = b.readUInt32LE(4), end = Math.min(b.length, 8 + rootSize);
  info.rootSize = rootSize;
  font.pages.push(changeExt(vpath, '.dds'));
  const page = font.pages.length - 1;
  info.pages = [font.pages[page]];
  for (let p = 8; p + 8 <= end;) {
    const tag = b.readUInt32LE(p), size = b.readUInt32LE(p + 4), body = p + 8;
    info.chunks.push({ tag: `0x${tag.toString(16).padStart(8, '0')}`, size });
    if ((tag === TAG_GLYPHS || tag === TAG_GLYPHS_FIXED) && body + 4096 <= b.length) {
      const fixed = tag === TAG_GLYPHS_FIXED;
      const rd = (o) => (fixed ? f32(b.readInt32LE(o) * 0.0009765625) : b.readFloatLE(o));
      if (font.lineHeight < 0) font.lineHeight = rd(body + 12); // glyph[0].h
      info.encoding = fixed ? 'i32/1024' : 'f32';
      for (let i = 0; i < 256; i++) {
        const o = body + i * 16;
        const g = { u: rd(o), v: rd(o + 4), w: rd(o + 8), h: rd(o + 12), xoff: 0, yoff: 0, adv: rd(o + 8), lh: font.lineHeight, page };
        setGlyph(font, i, g);
        if (ALIASES[i]) setGlyph(font, ALIASES[i], g);
      }
    }
    p = body + size;
  }
  return info;
}

function readBMF(font, b, dir, vpath) {
  const info = { file: vpath, format: 'bmfont3', blocks: [] };
  const pagesBefore = font.pages.length;
  let p = 4, sx = 1, sy = 1, yAdj = 0, advPad = 0, pageCount = 0, charsSize = -1;
  while (p + 5 <= b.length) {
    const type = b[p], size = b.readInt32LE(p + 1);
    p += 5;
    info.blocks.push({ type, size });
    if (type === 1) {
      const bits = b[p + 2];
      const nameEnd = b.indexOf(0, p + 14);
      info.info = {
        fontSize: b.readInt16LE(p), bold: !!(bits & 0x10), italic: !!(bits & 0x20), unicode: !!(bits & 0x40), smooth: !!(bits & 0x80),
        charSet: b[p + 3], stretchH: b.readUInt16LE(p + 4), aa: b[p + 6], padding: [...b.subarray(p + 7, p + 11)],
        spacing: [b[p + 11], b[p + 12]], outline: b[p + 13], face: b.toString('latin1', p + 14, nameEnd < 0 ? p + size : nameEnd),
      };
      p += size; // the engine skips this block
    } else if (type === 2) {
      const lh = b.readUInt16LE(p), base = b.readUInt16LE(p + 2), scaleW = b.readUInt16LE(p + 4), scaleH = b.readUInt16LE(p + 6);
      pageCount = b.readUInt16LE(p + 8);
      sx = f32(1 / scaleW); sy = f32(1 / scaleH);
      if (font.lineHeight >= 0) yAdj = f32(Math.trunc((font.lineHeight / sy - lh) * 0.5) * sy); // _ftol2 truncates
      else font.lineHeight = f32(lh * sy);
      advPad = f32((Math.floor(lh / 20) + 1) * sx);
      info.common = { lineHeight: lh, base, scaleW, scaleH, pages: pageCount, packed: b[p + 10], yAdjustPx: yAdj / sy, advancePadPx: advPad / sx };
      p += size; // engine: 10 bytes read + skip(size - 10)
    } else if (type === 3) {
      info.pages = [];
      // The engine reads exactly pageCount NUL-terminated names and does not skip the rest of the block.
      for (let k = 0; k < pageCount; k++) {
        const e = b.indexOf(0, p);
        const name = b.toString('latin1', p, e);
        p = e + 1;
        font.pages.push(dir + name);
        info.pages.push(dir + name);
      }
    } else if (type === 4) {
      charsSize = size;
      break;
    } else {
      p += size;
    }
  }
  if (charsSize < 0) { font.notes.push(`${vpath}: no chars block`); return info; }
  const n = Math.trunc(charsSize / 20);
  info.charCount = n;
  let maxId = 0, negIds = 0;
  for (let i = 0; i < n; i++, p += 20) {
    const id = b.readInt32LE(p);
    const [x, y, w, h, xo, yo, xa] = [0, 1, 2, 3, 4, 5, 6].map((k) => b.readInt16LE(p + 4 + 2 * k));
    const g = {
      u: f32(x * sx), v: f32(y * sy), w: f32(w * sx), h: f32(h * sy), xoff: f32(xo * sx), yoff: f32(yo * sy + yAdj),
      adv: f32(xa * sx + advPad), lh: font.lineHeight, page: b[p + 18] + pagesBefore,
    };
    if (id < 0) negIds++; else maxId = Math.max(maxId, id);
    setGlyph(font, id, g);
    if (ALIASES[id]) setGlyph(font, ALIASES[id], g);
  }
  info.maxId = maxId;
  if (negIds) info.negativeIds = negIds;
  return info;
}

// gsFonts_OGL::getFont(name, extra, style 0)
export function getFont(src, name, extra = '', cache = new Map()) {
  let font = cache.get(name);
  const infos = [];
  if (!font) {
    font = newFont();
    const stem = `${name}/${STYLE_NAMES[0]}`;
    infos.push(loadFontData(font, src, src.exists(`${stem}.fnt`) ? `${stem}.fnt` : `${stem}.font`));
    cache.set(name, font);
  }
  if (extra && !font.extras.has(extra)) {
    const stem = `${name}/${extra}`;
    const file = src.exists(`${stem}.fnt`) ? `${stem}.fnt` : src.exists(`${stem}.font`) ? `${stem}.font` : null;
    if (file) { infos.push(loadFontData(font, src, file)); font.extras.add(extra); }
    else font.notes.push(`extra "${extra}": neither ${stem}.fnt nor ${stem}.font exists (nothing merged, not remembered)`);
  }
  return { font, infos };
}

// gsFont_OGL::getCharData: exact key, else the glyph with the smallest key (std::map begin()).
export function lookup(font, c) {
  const g = font.glyphs.get(c & 0xffff);
  if (g) return g;
  if (font._firstKey === undefined || font._firstSize !== font.glyphs.size) {
    let k = Infinity;
    for (const key of font.glyphs.keys()) if (key < k) k = key;
    font._firstKey = k;
    font._firstSize = font.glyphs.size;
  }
  return font.glyphs.get(font._firstKey) ?? null; // null only for an empty font (engine reads garbage there)
}

const ZERO = { u: 0, v: 0, w: 0, h: 0, xoff: 0, yoff: 0, adv: 0, lh: 1, page: -1 };
const glyphOf = (font, c) => lookup(font, c) ?? ZERO;

// ---------------------------------------------------------------------------------------------- metrics
// gsGuiText_OGL::charWidth (vtable slot 16): codes 4, 5, 10, 13 -> 0.
export function charWidth(font, c, sizeX, spacingX) {
  if (c === 4 || c === 5 || c === 10 || c === 13) return 0;
  const g = glyphOf(font, c);
  return f32((g.adv / g.lh) * f32(sizeX) + f32(spacingX));
}

const codes = (text) => { const a = []; for (let i = 0; i < text.length; i++) a.push(text.charCodeAt(i)); a.push(0); return a; };

// gsGuiText_OGL::textWidth (slot 14, script getTextWidth): widest '\n'-separated line, no wrapping, tab = glyph 9.
export function measureWidth(font, text, sizeX, spacingX = 0) {
  if (text == null) return 0;
  const s = codes(text), sx = f32(sizeX), spx = f32(spacingX);
  let max = 0, cur = 0;
  for (let i = 0; i < s.length && s[i] !== 0; i++) {
    const c = s[i];
    if (c === 4) { i += 8; if (i >= s.length - 1) break; continue; }
    if (c === 5 || c === 13) continue;
    if (c === 10) { max = max > cur ? max : cur; cur = 0; continue; }
    const g = glyphOf(font, c);
    cur = f32((g.adv / g.lh) * sx + spx + cur);
  }
  return max > cur ? max : cur;
}

// gsGuiText_OGL::textHeight (slot 15, script getTextHeight): line count after word wrap at box.x.
export function measureHeight(font, text, { sizeX, sizeY = sizeX, spacingX = 0, spacingY = 0, boxX = 10000 }) {
  if (text == null) return 0;
  const s = codes(text), sx = f32(sizeX), sy = f32(sizeY), spx = f32(spacingX), spy = f32(spacingY), bx = f32(boxX);
  let h = sy, x = 0, tab = 0;
  const newline = () => { h = f32(spy + sy + h); x = 0; tab = 0; };
  for (let i = 0; i < s.length && s[i] !== 0; i++) {
    const c = s[i];
    if (c === 4) { i += 8; if (i >= s.length - 1) break; continue; }
    if (c === 5 || c === 13) continue;
    if (c === 9) { tab = f32((spx + sx) * 3 + tab); x = tab; continue; }
    if (c === 10) { newline(); continue; }
    if (c === 32) {
      const g = glyphOf(font, 32);
      const w = f32((g.adv / g.lh) * sx + spx);
      const xs = f32(w + x);
      let look = xs;
      for (let j = i + 1; j < s.length && s[j] !== 0; j++) {
        const c2 = s[j];
        if (c2 === 4) { j += 8; if (j >= s.length - 1) break; continue; }
        if (c2 === 5) continue;
        const g2 = glyphOf(font, c2);
        if (c2 <= 0x20 || g2.lh === 0 || look > bx) break;
        look = f32((g2.adv / g2.lh) * sx + spx + look);
      }
      if (look <= bx) x = xs; else newline();
      continue;
    }
    const g = glyphOf(font, c);
    if (!(g.lh > 0)) continue;
    const w = f32((g.adv / g.lh) * sx + spx);
    if (w + x > bx) newline();
    x = f32(w + x);
  }
  return h;
}

// gsFont_OGL::renderText as called by gsGuiText_OGL::render: returns the pen walk in screen units.
// Glyph space: 1 unit = one line height; x is scaled by size.x and y by size.y (glScalef), and every
// parameter is divided by size.x first (including spacing.y and box.x).
export function layout(font, text, { sizeX, sizeY = sizeX, spacingX = 0, spacingY = 0, boxX = 10000, indent = 0 }) {
  const s = codes(text), sx = f32(sizeX), sy = f32(sizeY);
  const inv = f32(1 / sx);
  const wrapW = f32(inv * f32(boxX)), spX = f32(inv * f32(spacingX)), spY = f32(inv * f32(spacingY));
  let pen = f32(inv * f32(indent)), tabAcc = 0, lineY = 0, drawn = 0;
  const lines = [{ y: 0, items: [] }];
  const newline = () => { pen = 0; tabAcc = 0; lineY += spY + 1; lines.push({ y: lineY, items: [] }); };
  const cur = () => lines[lines.length - 1];
  for (let i = 0; i < s.length && s[i] !== 0; i++) {
    const c = s[i];
    switch (c) {
      case 4: cur().items.push({ code: 4, color: String.fromCharCode(...s.slice(i + 1, i + 9)) }); i += 8; break;
      case 5: case 13: break;
      case 9: tabAcc = f32((spX + 1) * 3 + tabAcc); pen = tabAcc; break;
      case 10: newline(); break;
      case 32: {
        const g = glyphOf(font, 32);
        const adv = f32(g.adv / g.lh + spX);
        let look = f32(adv + pen);
        const save = look;
        for (let j = i + 1; j < s.length && s[j] !== 0; j++) {
          const c2 = s[j];
          if (c2 === 4) { j += 8; continue; }
          if (c2 === 5) continue;
          const g2 = glyphOf(font, c2);
          if (c2 < 0x21 || g2.lh === 0 || wrapW < look) break;
          look = f32(g2.adv / g2.lh + look + spX);
        }
        if (look <= wrapW) pen = save; else newline();
        break;
      }
      default: {
        const g = glyphOf(font, c);
        if (!(g.lh > 0)) break;
        const adv = f32(g.adv / g.lh + spX);
        if (wrapW < adv + pen) newline();
        cur().items.push({ code: c, x: pen, adv });
        pen = f32(adv + pen);
        drawn++;
      }
    }
  }
  const out = lines.map((l) => {
    const glyphs = l.items.filter((it) => it.code !== 4);
    return {
      y: l.y * sy, text: glyphs.map((it) => String.fromCharCode(it.code)).join(''),
      penEnd: glyphs.reduce((m, it) => Math.max(m, (it.x + it.adv) * sx), 0), firstX: glyphs[0] ? glyphs[0].x * sx : 0,
      glyphs: glyphs.map((it) => ({ ch: String.fromCharCode(it.code), x: it.x * sx, adv: it.adv * sx })),
    };
  });
  return { lines: out, glyphsDrawn: drawn, lineAdvance: (spY + 1) * sy };
}

// ---------------------------------------------------------------------------------------------- DDS info
export function ddsInfo(buf) {
  if (!buf || buf.length < 128 || buf.toString('latin1', 0, 4) !== 'DDS ') return null;
  const height = buf.readUInt32LE(12), width = buf.readUInt32LE(16), mips = buf.readUInt32LE(28);
  const pfFlags = buf.readUInt32LE(80), fourCC = buf.toString('latin1', 84, 88), bits = buf.readUInt32LE(88);
  let format;
  if (pfFlags & 4) format = fourCC;
  else if (pfFlags & 0x40) format = `RGB${pfFlags & 1 ? 'A' : ''}${bits}`;
  else if (pfFlags & 0x20000) format = `L${bits}`;
  else if (pfFlags & 2) format = `A${bits}`;
  else format = `flags 0x${pfFlags.toString(16)} ${bits}bpp`;
  return { width, height, mips, format };
}

// ---------------------------------------------------------------------------------------------- CLI
function unescape(t) {
  return t.replace(/\\(c[0-9A-Fa-f]{8}|x[0-9A-Fa-f]{2}|u[0-9A-Fa-f]{4}|[ntre\\])/g, (m, e) => {
    if (e[0] === 'c') return '\u0004' + e.slice(1).toUpperCase();
    if (e[0] === 'x' || e[0] === 'u') return String.fromCharCode(parseInt(e.slice(1), 16));
    return { n: '\n', t: '\t', r: '\r', e: '\u0005', '\\': '\\' }[e];
  });
}

function parseArgs(argv) {
  const pos = [], opt = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith('--')) {
      const k = a.slice(2);
      if (['all', 'json', 'layout'].includes(k)) opt[k] = true;
      else opt[k] = argv[++i];
    } else pos.push(a);
  }
  return { pos, opt };
}

// Open <font>: a .font/.fnt file (loaded alone) or a font directory / engine name (normal.fnt|.font + extra).
function openFont(spec, opt) {
  const src = new FileSource({ root: opt.root ?? null, game: opt.game ?? null });
  if (/\.(font|fnt)$/i.test(spec)) {
    const font = newFont();
    const info = loadFontData(font, src, spec);
    if (!info) throw new Error(`cannot read ${spec}`);
    const infos = [info];
    if (opt.extra) {
      const stem = dirOf(spec) + opt.extra;
      const file = src.exists(`${stem}.fnt`) ? `${stem}.fnt` : src.exists(`${stem}.font`) ? `${stem}.font` : null;
      if (file) infos.push(loadFontData(font, src, file)); else font.notes.push(`extra ${stem}.fnt|.font not found`);
    }
    return { font, infos, src };
  }
  const name = spec.replace(/[\\/]+$/, '');
  const { font, infos } = getFont(src, name, opt.extra ?? '');
  if (!font.files.length) throw new Error(`no ${name}/normal.fnt or ${name}/normal.font (${font.notes.join('; ')})`);
  return { font, infos, src };
}

const fmt = (v, d = 6) => (Number.isInteger(v) ? String(v) : v.toFixed(d).replace(/0+$/, '').replace(/\.$/, ''));
const charLabel = (c) => (c >= 0x21 && c < 0x7f ? `'${String.fromCharCode(c)}'` : c === 0x20 ? "' '" : `U+${c.toString(16).toUpperCase().padStart(4, '0')}`);

function fontSummary(font, infos, src) {
  const pages = font.pages.map((p) => ({ path: p, exists: src.exists(p), dds: ddsInfo(src.read(p)) }));
  const page0 = pages[0]?.dds;
  const keys = [...font.glyphs.keys()].sort((a, b) => a - b);
  const hs = new Map();
  for (const g of font.glyphs.values()) hs.set(g.h, (hs.get(g.h) ?? 0) + 1);
  const empty = keys.filter((k) => font.glyphs.get(k).w === 0).length;
  return {
    files: font.files, sources: infos, lineHeight: font.lineHeight,
    lineHeightPx: page0 ? font.lineHeight * page0.height : null,
    glyphs: font.glyphs.size, zeroWidthGlyphs: empty, firstKey: keys[0], lastKey: keys[keys.length - 1],
    distinctHeights: hs.size, pages, notes: font.notes,
  };
}

function glyphRow(font, c, pageDims) {
  const g = font.glyphs.get(c);
  if (!g) return `  ${charLabel(c).padEnd(8)} (missing -> falls back to ${charLabel([...font.glyphs.keys()].sort((a, b) => a - b)[0])})`;
  const d = pageDims[g.page];
  const px = d ? ` px[x ${fmt(g.u * d.width, 2)} y ${fmt(g.v * d.height, 2)} w ${fmt(g.w * d.width, 2)} h ${fmt(g.h * d.height, 2)} xoff ${fmt(g.xoff * d.width, 2)} yoff ${fmt(g.yoff * d.height, 2)} adv ${fmt(g.adv * d.width, 2)}]` : '';
  return `  ${charLabel(c).padEnd(8)} uv(${fmt(g.u)}, ${fmt(g.v)}) wh(${fmt(g.w)}, ${fmt(g.h)}) off(${fmt(g.xoff)}, ${fmt(g.yoff)}) adv ${fmt(g.adv)} page ${g.page}  advance/lineHeight = ${fmt(g.adv / g.lh)}${px}`;
}

function cmdInfo(spec, opt) {
  const { font, infos, src } = openFont(spec, opt);
  const sum = fontSummary(font, infos, src);
  if (opt.json) {
    const glyphs = {};
    for (const [k, g] of [...font.glyphs.entries()].sort((a, b) => a[0] - b[0])) glyphs[k] = g;
    console.log(JSON.stringify({ ...sum, glyphTable: opt.all ? glyphs : undefined }, null, 2));
    return;
  }
  for (const inf of infos) {
    if (!inf) continue;
    console.log(`file      ${inf.file}  (${inf.format})`);
    if (inf.format === 'native') console.log(`  root 0x03000000 size ${inf.rootSize}; chunks ${inf.chunks.map((c) => `${c.tag}[${c.size}]`).join(' ')}; encoding ${inf.encoding}`);
    else {
      if (inf.info) console.log(`  info    face "${inf.info.face}" size ${inf.info.fontSize}${inf.info.bold ? ' bold' : ''}${inf.info.italic ? ' italic' : ''} charset ${inf.info.charSet} unicode ${inf.info.unicode} padding ${inf.info.padding.join(',')} spacing ${inf.info.spacing.join(',')}`);
      if (inf.common) console.log(`  common  lineHeight ${inf.common.lineHeight} base ${inf.common.base} scale ${inf.common.scaleW}x${inf.common.scaleH} pages ${inf.common.pages}; engine adds ${inf.common.advancePadPx} px to every advance${infos.indexOf(inf) > 0 ? `, y adjust ${inf.common.yAdjustPx} px` : ''}`);
      console.log(`  chars   ${inf.charCount} (max id ${inf.maxId}${inf.negativeIds ? `, ${inf.negativeIds} negative id(s) -> key 0xFFFF on Windows` : ''})`);
    }
  }
  console.log(`lineHeight ${fmt(font.lineHeight, 8)} (normalised V)${sum.lineHeightPx != null ? ` = ${fmt(sum.lineHeightPx, 3)} px of page 0` : ''}`);
  console.log(`glyphs    ${sum.glyphs} keys (${charLabel(sum.firstKey)}..${charLabel(sum.lastKey)}), ${sum.zeroWidthGlyphs} with w = 0, ${sum.distinctHeights} distinct h; missing glyph -> ${charLabel(sum.firstKey)}`);
  for (const p of sum.pages) console.log(`page      ${p.path}  ${p.exists ? (p.dds ? `${p.dds.width}x${p.dds.height} ${p.dds.format} mips ${p.dds.mips}` : 'not a DDS') : 'MISSING'}`);
  for (const n of font.notes) console.log(`note      ${n}`);
  const dims = sum.pages.map((p) => p.dds);
  const sample = opt.all ? [...font.glyphs.keys()].sort((a, b) => a - b) : opt.glyphs ? [...unescape(opt.glyphs)].map((ch) => ch.charCodeAt(0)) : [0, 0x20, 0x41, 0x61, 0x69, 0x57, 0x2e];
  console.log('glyphs:');
  for (const c of sample) console.log(glyphRow(font, c, dims));
}

function cmdMeasure(spec, text, sizeXs, spacingXs, opt) {
  const { font, src } = openFont(spec, opt);
  const t = unescape(text ?? '');
  const sizeX = Number(sizeXs), spacingX = Number(spacingXs ?? 0);
  if (!Number.isFinite(sizeX)) throw new Error('sizeX must be a number');
  const sizeY = opt.sizeY != null ? Number(opt.sizeY) : sizeX;
  const spacingY = Number(opt.spacingY ?? 0), boxX = Number(opt.box ?? 10000), indent = Number(opt.indent ?? 0);
  for (const n of font.notes) console.log(`note: ${n}`);
  const w = measureWidth(font, t, sizeX, spacingX);
  const h = measureHeight(font, t, { sizeX, sizeY, spacingX, spacingY, boxX });
  console.log(`font ${font.files.join(' + ')}  lineHeight ${fmt(font.lineHeight, 8)}`);
  console.log(`text ${JSON.stringify(t)} (${t.length} UTF-16 units)  size (${sizeX}, ${sizeY})  spacing (${spacingX}, ${spacingY})  box.x ${boxX}`);
  console.log('per character (charWidth = advance/lineHeight * size.x + spacing.x):');
  for (const ch of t) {
    const c = ch.charCodeAt(0);
    const g = lookup(font, c);
    const fb = font.glyphs.has(c) ? '' : '  [missing glyph, uses fallback]';
    console.log(`  ${charLabel(c).padEnd(8)} ${fmt(charWidth(font, c, sizeX, spacingX), 6)}${g ? `  (adv/lh ${fmt(g.adv / g.lh, 6)})` : ''}${fb}`);
  }
  console.log(`measureWidth  (getTextWidth)  = ${w}`);
  console.log(`measureHeight (getTextHeight) = ${h}`);
  if (opt.layout) {
    const L = layout(font, t, { sizeX, sizeY, spacingX, spacingY, boxX, indent });
    console.log(`render layout (pen positions relative to the text position; line advance ${fmt(L.lineAdvance, 6)}):`);
    L.lines.forEach((l, i) => {
      console.log(`  line ${i}: y ${fmt(l.y, 4)}  x ${fmt(l.firstX, 4)}..${fmt(l.penEnd, 4)}  ${JSON.stringify(l.text)}`);
      if (l.glyphs.length) console.log(`          ${l.glyphs.map((g) => `${g.ch}@${fmt(g.x, 3)}`).join(' ')}`);
    });
    console.log(`  glyph quads drawn: ${L.glyphsDrawn}`);
  }
}

function* walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) yield* walk(p); else yield p;
  }
}

function cmdCheck(dir) {
  const isGame = fs.readdirSync(dir).some((n) => n.toLowerCase().endsWith('.gs'));
  const src = new FileSource(isGame ? { game: dir } : { root: dir });
  const files = isGame ? [...src.map.keys()] : [...walk(dir)].map((p) => path.relative(dir, p).split(path.sep).join('/'));
  const fonts = files.filter((f) => /\.(font|fnt)$/i.test(f)).sort();
  let issues = 0;
  const rows = [];
  for (const f of fonts) {
    const font = newFont();
    const info = loadFontData(font, src, f);
    const pages = font.pages.map((p) => ({ p, d: ddsInfo(src.read(p)) }));
    const prob = [...font.notes];
    for (const { p, d } of pages) if (!d) prob.push(`page ${p} missing or not DDS`);
    for (const [k, g] of font.glyphs) {
      if (g.page < 0 || g.page >= font.pages.length) { prob.push(`glyph ${k}: page ${g.page} out of range`); break; }
      if (g.u < 0 || g.v < 0 || g.u + g.w > 1.0001 || g.v + g.h > 1.0001) { prob.push(`glyph ${k}: rect outside [0,1]`); break; }
    }
    issues += prob.length;
    const d0 = pages[0]?.d;
    rows.push({
      file: f, format: info?.format, glyphs: font.glyphs.size, pages: pages.length,
      page0: d0 ? `${d0.width}x${d0.height} ${d0.format}` : '-',
      lineHeight: font.lineHeight, lineHeightPx: d0 ? +(font.lineHeight * d0.height).toFixed(3) : null,
      extra: info?.format === 'bmfont3' ? `${info.info?.face ?? '?'} ${info.info?.fontSize ?? ''}px lh ${info.common?.lineHeight} base ${info.common?.base} pad +${info.common?.advancePadPx}px` : '',
      problems: prob,
    });
  }
  for (const r of rows) {
    console.log(`${r.file.padEnd(42)} ${String(r.format).padEnd(8)} glyphs ${String(r.glyphs).padStart(5)}  pages ${String(r.pages).padStart(2)} (${r.page0})  lineHeight ${fmt(r.lineHeight, 8)} = ${r.lineHeightPx} px  ${r.extra}`);
    for (const p of r.problems) console.log(`    ! ${p}`);
  }
  console.log(`${rows.length} font files, ${issues} issue(s)`);
}

function main(argv) {
  const { pos, opt } = parseArgs(argv);
  const [cmd, ...rest] = pos;
  if (cmd === 'info' && rest[0]) return cmdInfo(rest[0], opt);
  if (cmd === 'measure' && rest.length >= 3) return cmdMeasure(rest[0], rest[1], rest[2], rest[3], opt);
  if (cmd === 'check' && rest[0]) return cmdCheck(rest[0]);
  const lines = fs.readFileSync(new URL(import.meta.url), 'utf8').split(/\r?\n/).slice(1);
  console.error(lines.slice(0, lines.findIndex((l) => !l.startsWith('//'))).map((l) => l.slice(3)).join('\n'));
  process.exit(1);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { main(process.argv.slice(2)); } catch (e) { console.error(`error: ${e.message}`); process.exit(1); }
}
