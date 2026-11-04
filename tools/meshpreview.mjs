// meshpreview - dependency-free helpers to check decoded meshes: a flat-shaded software rasterizer that writes PNG,
// and a minimal binary glTF 2.0 (.glb) writer. Used by assetparse.mjs; previews are for local verification.

import zlib from 'node:zlib';

// ---------- PNG ----------
const CRC = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function pngChunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'latin1'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
export function encodePNG(rgb, w, h) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 3 + 1)] = 0;
    rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // RGB
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk('IHDR', ihdr),
    pngChunk('IDAT', zlib.deflateSync(raw)),
    pngChunk('IEND', Buffer.alloc(0)),
  ]);
}

// ---------- rasterizer ----------
// positions: flat array [x,y,z,...]; indices: triangle list. Orthographic view rotated by yaw/pitch (radians),
// fitted to the image. Returns { png, stats }.
export function renderPreview(positions, indices, { size = 512, yaw = 0.8, pitch = 0.5, upAxis = 'y' } = {}) {
  const n = positions.length / 3;
  const P = new Float32Array(n * 3);
  const cy = Math.cos(yaw), sy = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch);
  for (let i = 0; i < n; i++) {
    let x = positions[3 * i], y = positions[3 * i + 1], z = positions[3 * i + 2];
    if (upAxis === 'z') [y, z] = [z, -y];
    // yaw around up, then pitch around x
    const x1 = cy * x + sy * z, z1 = -sy * x + cy * z;
    const y2 = cp * y - sp * z1, z2 = sp * y + cp * z1;
    P[3 * i] = x1;
    P[3 * i + 1] = y2;
    P[3 * i + 2] = z2;
  }
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  for (let i = 0; i < n; i++) {
    minX = Math.min(minX, P[3 * i]); maxX = Math.max(maxX, P[3 * i]);
    minY = Math.min(minY, P[3 * i + 1]); maxY = Math.max(maxY, P[3 * i + 1]);
  }
  const scale = (size * 0.9) / Math.max(maxX - minX, maxY - minY, 1e-9);
  const ox = size / 2 - ((minX + maxX) / 2) * scale, oy = size / 2 + ((minY + maxY) / 2) * scale;
  const rgb = Buffer.alloc(size * size * 3, 0xf0);
  const zbuf = new Float32Array(size * size).fill(-Infinity);
  const light = [0.4, 0.8, 0.45];
  const ll = Math.hypot(...light);
  let drawn = 0;
  for (let t = 0; t + 2 < indices.length; t += 3) {
    const [a, b, c] = [indices[t], indices[t + 1], indices[t + 2]];
    if (a >= n || b >= n || c >= n) continue;
    const ax = P[3 * a] * scale + ox, ay = oy - P[3 * a + 1] * scale, az = P[3 * a + 2];
    const bx = P[3 * b] * scale + ox, by = oy - P[3 * b + 1] * scale, bz = P[3 * b + 2];
    const cx = P[3 * c] * scale + ox, cyy = oy - P[3 * c + 1] * scale, cz = P[3 * c + 2];
    // face normal in view space for flat shading (two-sided)
    const ux = P[3 * b] - P[3 * a], uy = P[3 * b + 1] - P[3 * a + 1], uz = P[3 * b + 2] - P[3 * a + 2];
    const vx = P[3 * c] - P[3 * a], vy = P[3 * c + 1] - P[3 * a + 1], vz = P[3 * c + 2] - P[3 * a + 2];
    const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const nl = Math.hypot(nx, ny, nz) || 1;
    const shade = 0.25 + 0.75 * Math.abs((nx * light[0] + ny * light[1] + nz * light[2]) / (nl * ll));
    const col = [Math.round(70 + 150 * shade), Math.round(90 + 140 * shade), Math.round(120 + 120 * shade)];
    const x0 = Math.max(0, Math.floor(Math.min(ax, bx, cx))), x1 = Math.min(size - 1, Math.ceil(Math.max(ax, bx, cx)));
    const y0 = Math.max(0, Math.floor(Math.min(ay, by, cyy))), y1 = Math.min(size - 1, Math.ceil(Math.max(ay, by, cyy)));
    const area = (bx - ax) * (cyy - ay) - (by - ay) * (cx - ax);
    if (Math.abs(area) < 1e-12) continue;
    for (let y = y0; y <= y1; y++) {
      for (let x = x0; x <= x1; x++) {
        const px = x + 0.5, py = y + 0.5;
        const w0 = ((bx - px) * (cyy - py) - (by - py) * (cx - px)) / area;
        const w1 = ((cx - px) * (ay - py) - (cyy - py) * (ax - px)) / area;
        const w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const z = w0 * az + w1 * bz + w2 * cz;
        const k = y * size + x;
        if (z <= zbuf[k]) continue;
        zbuf[k] = z;
        rgb[3 * k] = col[0];
        rgb[3 * k + 1] = col[1];
        rgb[3 * k + 2] = col[2];
        drawn++;
      }
    }
  }
  return { png: encodePNG(rgb, size, size), stats: { vertices: n, triangles: Math.floor(indices.length / 3), pixelsDrawn: drawn } };
}

// ---------- glTF ----------
// mesh: { positions: Float32Array, normals?: Float32Array, uvs?: Float32Array, indices: Uint32Array, name? }
export function writeGLB(mesh) {
  const parts = [];
  const views = [];
  const accessors = [];
  let offset = 0;
  const add = (typed, target, type, componentType, extra = {}) => {
    const buf = Buffer.from(typed.buffer, typed.byteOffset, typed.byteLength);
    const pad = (4 - (buf.length % 4)) % 4;
    parts.push(buf, Buffer.alloc(pad));
    views.push({ buffer: 0, byteOffset: offset, byteLength: buf.length, target });
    offset += buf.length + pad;
    const count = typed.length / { SCALAR: 1, VEC2: 2, VEC3: 3 }[type];
    accessors.push({ bufferView: views.length - 1, componentType, count, type, ...extra });
    return accessors.length - 1;
  };
  const p = mesh.positions;
  const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < p.length; i += 3) for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], p[i + k]); max[k] = Math.max(max[k], p[i + k]); }
  const attributes = { POSITION: add(p, 34962, 'VEC3', 5126, { min, max }) };
  if (mesh.normals) attributes.NORMAL = add(mesh.normals, 34962, 'VEC3', 5126);
  if (mesh.uvs) attributes.TEXCOORD_0 = add(mesh.uvs, 34962, 'VEC2', 5126);
  const indices = add(mesh.indices, 34963, 'SCALAR', 5125);
  const bin = Buffer.concat(parts);
  const json = {
    asset: { version: '2.0', generator: 'cim-re assetparse' },
    scene: 0, scenes: [{ nodes: [0] }], nodes: [{ mesh: 0, name: mesh.name ?? 'mesh' }],
    meshes: [{ name: mesh.name ?? 'mesh', primitives: [{ attributes, indices, mode: 4 }] }],
    buffers: [{ byteLength: bin.length }], bufferViews: views, accessors,
  };
  let js = Buffer.from(JSON.stringify(json));
  js = Buffer.concat([js, Buffer.alloc((4 - (js.length % 4)) % 4, 0x20)]);
  const header = Buffer.alloc(12);
  header.writeUInt32LE(0x46546c67, 0);
  header.writeUInt32LE(2, 4);
  header.writeUInt32LE(12 + 8 + js.length + 8 + bin.length, 8);
  const jh = Buffer.alloc(8);
  jh.writeUInt32LE(js.length, 0);
  jh.writeUInt32LE(0x4e4f534a, 4);
  const bh = Buffer.alloc(8);
  bh.writeUInt32LE(bin.length, 0);
  bh.writeUInt32LE(0x004e4942, 4);
  return Buffer.concat([header, jh, js, bh, bin]);
}
