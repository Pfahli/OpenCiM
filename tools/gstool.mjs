#!/usr/bin/env node
// gstool - list / extract Cities in Motion "GS10" archives (*.gs).
//
// Container layout (all integers little-endian u32; confirmed against the decompiled reader in GSystem.dll,
// FUN_1007ef80 / FUN_1007e200):
//
//   "GS10" size                      file magic; size = file length - 8 (validatePackage deletes files that fail this)
//   0x01000000 size                  root chunk; entry offsets are relative to its data start (file offset 0x10)
//     0x01100000 size                directory chunk
//       count                        number of file entries
//       [0x01130000 128]             parent prefix: virtual path = prefix + <archive stem> + "/" + name
//                                    (sounds.gs + "metro/" -> "metro/sounds/...")
//       [0x01140000 128]             absolute mount: virtual path = prefix + name ("" for data.gs, "addons/usa/")
//                                    with neither chunk: <archive stem> + "/" + name (metro.gs -> "metro/...")
//       count x 0x0111xxxx|0x0112xxxx 144   entry: magic[4] offset size packedSize name[128]
//                                    tag 0x01110000 = stored, 0x01120000 = zlib (all shipped entries are zlib)
//     0x01200000 size                data chunk, 4-byte aligned; magic = first 4 unpacked bytes
//   0x00000001 size                  trailer comment (plain text)
//
// Usage:
//   node gstool.mjs list    <archive.gs> [--json]
//   node gstool.mjs extract <archive.gs> <outDir> [--filter <regex>]
//   node gstool.mjs info    <archive.gs>
//   node gstool.mjs pack    <folder> <out.gs>       a folder as a save package (zlib entries, as writePackage)

import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const TAG = {
  ROOT: 0x01000000,
  DIRECTORY: 0x01100000,
  ENTRY_STORED: 0x01110000,
  ENTRY_ZLIB: 0x01120000,
  MOUNT_PARENT: 0x01130000,
  MOUNT_ABSOLUTE: 0x01140000,
  DATA: 0x01200000,
  COMMENT: 0x00000001,
};

function cstr(buf, start, len) {
  const end = buf.indexOf(0, start);
  return buf.toString('latin1', start, end < 0 || end > start + len ? start + len : end);
}

export function readArchive(file) {
  const buf = fs.readFileSync(file);
  if (buf.toString('latin1', 0, 4) !== 'GS10') throw new Error(`${file}: not a GS10 archive`);
  if (buf.readUInt32LE(4) !== buf.length - 8) throw new Error(`${file}: size field mismatch`);

  const archive = { file, buf, mount: '', mountTag: null, entries: [], comment: null, dataStart: null };
  let pos = 8;
  while (pos + 8 <= buf.length) {
    const tag = buf.readUInt32LE(pos);
    const size = buf.readUInt32LE(pos + 4);
    const body = pos + 8;
    if (tag === TAG.ROOT) {
      archive.base = body;
      parseRoot(archive, body, body + size);
    } else if (tag === TAG.COMMENT) {
      archive.comment = buf.toString('latin1', body, body + size);
    } else {
      throw new Error(`unknown top-level chunk 0x${tag.toString(16)} at 0x${pos.toString(16)}`);
    }
    pos = body + size;
  }
  // The engine strips ".gs" from the archive's path relative to the virtual root; top-level archives use the basename.
  if (archive.mountTag !== TAG.MOUNT_ABSOLUTE) archive.mount = `${archive.mount}${path.basename(file, '.gs')}/`;
  return archive;
}

function parseRoot(archive, start, end) {
  const { buf } = archive;
  let pos = start;
  while (pos < end) {
    const tag = buf.readUInt32LE(pos);
    const size = buf.readUInt32LE(pos + 4);
    const body = pos + 8;
    if (tag === TAG.DIRECTORY) parseDirectory(archive, body, body + size);
    else if (tag === TAG.DATA) archive.dataStart = body;
    else throw new Error(`unknown root chunk 0x${tag.toString(16)} at 0x${pos.toString(16)}`);
    pos = body + size;
  }
}

function parseDirectory(archive, start, end) {
  const { buf } = archive;
  const count = buf.readUInt32LE(start);
  let pos = start + 4;
  while (pos < end) {
    const tag = buf.readUInt32LE(pos);
    const size = buf.readUInt32LE(pos + 4);
    const body = pos + 8;
    if (tag === TAG.MOUNT_PARENT || tag === TAG.MOUNT_ABSOLUTE) {
      archive.mount = cstr(buf, body, size);
      archive.mountTag = tag;
    } else if (tag === TAG.ENTRY_STORED || tag === TAG.ENTRY_ZLIB) {
      archive.entries.push({
        stored: tag === TAG.ENTRY_STORED,
        magic: buf.subarray(body, body + 4),
        offset: buf.readUInt32LE(body + 4),
        size: buf.readUInt32LE(body + 8),
        packedSize: buf.readUInt32LE(body + 12),
        name: cstr(buf, body + 16, size - 16),
      });
    } else {
      throw new Error(`unknown directory chunk 0x${tag.toString(16)} at 0x${pos.toString(16)}`);
    }
    pos = body + size;
  }
  if (archive.entries.length !== count) {
    throw new Error(`directory declares ${count} entries, parsed ${archive.entries.length}`);
  }
}

export function readEntry(archive, entry) {
  const start = archive.base + entry.offset;
  // The entry tag decides compression; packedSize === size does not imply stored (metro.gs route.fragment).
  const raw = archive.buf.subarray(start, start + entry.packedSize);
  const data = entry.stored ? Buffer.from(raw) : zlib.inflateSync(raw);
  if (data.length !== entry.size) throw new Error(`${entry.name}: expected ${entry.size} bytes, got ${data.length}`);
  return data;
}

function printable(b) {
  return [...b].map((c) => (c >= 32 && c < 127 ? String.fromCharCode(c) : '.')).join('');
}

// The files of a folder (not recursive) as a package like the engine's writePackage writes saves: zlib entries, magic =
// the first 4 unpacked bytes, offsets from the root body, no mount chunks, no trailer.
function writePackage(dir, out) {
  const files = fs.readdirSync(dir, { withFileTypes: true }).filter((d) => d.isFile()).map((d) => d.name).sort();
  const packed = files.map((n) => {
    const data = fs.readFileSync(path.join(dir, n));
    return { name: n, data, z: zlib.deflateSync(data) };
  });
  const dirSize = 4 + packed.length * (8 + 144);
  const dataSize = packed.reduce((s, p) => s + p.z.length, 0);
  const rootSize = 8 + dirSize + 8 + dataSize;
  const buf = Buffer.alloc(16 + rootSize);
  let o = 0;
  const u32 = (v) => { buf.writeUInt32LE(v >>> 0, o); o += 4; };
  buf.write('GS10', 0, 'latin1');
  o = 4;
  u32(8 + rootSize); u32(TAG.ROOT); u32(rootSize); u32(0x01100000); u32(dirSize); u32(packed.length);
  let offset = 8 + dirSize + 8;
  for (const p of packed) {
    u32(0x01120000); u32(144);
    u32(p.data.length >= 4 ? p.data.readUInt32LE(0) : 0); u32(offset); u32(p.data.length); u32(p.z.length);
    buf.write(p.name.slice(0, 127), o, 'latin1');
    o += 128;
    offset += p.z.length;
  }
  u32(0x01200000); u32(dataSize);
  for (const p of packed) { p.z.copy(buf, o); o += p.z.length; }
  fs.writeFileSync(out, buf);
  console.log(`${out}: ${packed.length} files, ${buf.length} bytes`);
}

function main(argv) {
  const [cmd, file, ...rest] = argv;
  if (!cmd || !file) {
    console.error('usage: gstool.mjs list|info|extract <archive.gs> [outDir] [--filter regex] [--json]\n' +
                  '       gstool.mjs pack <folder> <out.gs>');
    process.exit(2);
  }
  if (cmd === 'pack') return writePackage(file, rest[0]);
  const archive = readArchive(file);
  const flag = (name) => {
    const i = rest.indexOf(name);
    return i < 0 ? null : rest[i + 1] ?? true;
  };

  if (cmd === 'info') {
    const total = archive.entries.reduce((n, e) => n + e.size, 0);
    console.log(JSON.stringify({
      file: path.basename(file),
      mount: archive.mount,
      mountTag: archive.mountTag && `0x${archive.mountTag.toString(16)}`,
      entries: archive.entries.length,
      unpackedBytes: total,
      comment: archive.comment,
    }, null, 2));
  } else if (cmd === 'list') {
    if (flag('--json')) {
      console.log(JSON.stringify(archive.entries.map(({ magic, ...e }) => ({ ...e, magic: printable(magic) })), null, 1));
    } else {
      for (const e of archive.entries) {
        console.log(`${String(e.size).padStart(10)} ${String(e.packedSize).padStart(10)}  ${printable(e.magic)}  ${archive.mount}${e.name}`);
      }
    }
  } else if (cmd === 'extract') {
    const outDir = rest[0];
    if (!outDir || outDir.startsWith('--')) throw new Error('extract needs an output directory');
    const filter = flag('--filter') ? new RegExp(flag('--filter'), 'i') : null;
    let n = 0;
    for (const e of archive.entries) {
      const rel = archive.mount + e.name;
      if (filter && !filter.test(rel)) continue;
      const dest = path.join(outDir, ...rel.split('/'));
      if (!path.resolve(dest).startsWith(path.resolve(outDir))) throw new Error(`refusing path outside outDir: ${rel}`);
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      fs.writeFileSync(dest, readEntry(archive, e));
      n++;
    }
    console.log(`${path.basename(file)}: extracted ${n} files to ${outDir}`);
  } else {
    throw new Error(`unknown command ${cmd}`);
  }
}

if (import.meta.url === `file:///${process.argv[1].replace(/\\/g, '/')}` || process.argv[1]?.endsWith('gstool.mjs')) {
  main(process.argv.slice(2));
}
