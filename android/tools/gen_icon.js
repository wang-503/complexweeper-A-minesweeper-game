/* 从图集里切出程序图标，生成各密度的 mipmap-* / ic_launcher.png。
 *
 * 图标本来就是图集里的 icon 槽位（32×32，带 alpha），所以不需要另画一份：
 * 直接把槽位抠出来、按最近邻缩放到各密度即可，视觉上与 Windows 版一致。
 *
 * 用法: node tools/gen_icon.js
 */
'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const root = path.resolve(__dirname, '..');            // android/
const atlasPng = path.join(root, '..', '素材', '图集.png');
const atlasJson = path.join(root, '..', '素材', '图集.json');
const outRoot = path.join(root, 'res');

// ---- 最小 PNG 解码（与 正式版/tools/gen_atlas.js 同样的实现，支持 8 位 RGB/RGBA/调色板） ----
function readPNG(file) {
  const buf = fs.readFileSync(file);
  let off = 8, w = 0, h = 0, bitDepth = 0, colorType = 0, interlace = 0;
  const idat = []; let palette = null, trns = null;
  while (off < buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString('ascii', off + 4, off + 8);
    const data = buf.subarray(off + 8, off + 8 + len);
    if (type === 'IHDR') {
      w = data.readUInt32BE(0); h = data.readUInt32BE(4);
      bitDepth = data[8]; colorType = data[9]; interlace = data[12];
    } else if (type === 'PLTE') palette = Buffer.from(data);
    else if (type === 'tRNS') trns = Buffer.from(data);
    else if (type === 'IDAT') idat.push(Buffer.from(data));
    else if (type === 'IEND') break;
    off += 12 + len;
  }
  if (bitDepth !== 8) throw new Error(file + ': 只支持 8 位色深');
  if (interlace) throw new Error(file + ': 不支持隔行扫描');
  const channels = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[colorType];
  if (!channels) throw new Error(file + ': 不支持的颜色类型 ' + colorType);
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = w * channels;
  const out = Buffer.alloc(stride * h);
  let pos = 0;
  for (let y = 0; y < h; y++) {
    const ft = raw[pos++];
    const line = raw.subarray(pos, pos + stride); pos += stride;
    const cur = out.subarray(y * stride, (y + 1) * stride);
    const prev = y > 0 ? out.subarray((y - 1) * stride, y * stride) : Buffer.alloc(stride);
    for (let i = 0; i < stride; i++) {
      const a = i >= channels ? cur[i - channels] : 0, b = prev[i], c = i >= channels ? prev[i - channels] : 0;
      let v = line[i];
      if (ft === 1) v += a; else if (ft === 2) v += b; else if (ft === 3) v += (a + b) >> 1;
      else if (ft === 4) {
        const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
      }
      cur[i] = v & 0xff;
    }
  }
  const rgba = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const s = y * stride + x * channels, d = (y * w + x) * 4;
    if (colorType === 6) { rgba[d] = out[s]; rgba[d + 1] = out[s + 1]; rgba[d + 2] = out[s + 2]; rgba[d + 3] = out[s + 3]; }
    else if (colorType === 2) { rgba[d] = out[s]; rgba[d + 1] = out[s + 1]; rgba[d + 2] = out[s + 2]; rgba[d + 3] = 255; }
    else if (colorType === 0) { rgba[d] = rgba[d + 1] = rgba[d + 2] = out[s]; rgba[d + 3] = 255; }
    else if (colorType === 4) { rgba[d] = rgba[d + 1] = rgba[d + 2] = out[s]; rgba[d + 3] = out[s + 1]; }
    else { const i = out[s]; rgba[d] = palette[i * 3]; rgba[d + 1] = palette[i * 3 + 1]; rgba[d + 2] = palette[i * 3 + 2];
           rgba[d + 3] = trns && i < trns.length ? trns[i] : 255; }
  }
  return { w, h, rgba };
}

let crcTable = null;
function crc32(buf) {
  if (!crcTable) {
    crcTable = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
      crcTable[n] = c;
    }
  }
  let c = 0xFFFFFFFF;
  for (let i = 0; i < buf.length; i++) c = crcTable[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

function writePNG(file, w, h, rgba) {
  const stride = 1 + w * 4;
  const raw = Buffer.alloc(stride * h);
  for (let y = 0; y < h; y++) {
    raw[y * stride] = 0;
    rgba.copy(raw, y * stride + 1, y * w * 4, (y + 1) * w * 4);
  }
  const deflated = zlib.deflateSync(raw, { level: 9 });
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
    const t = Buffer.from(type, 'ascii');
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(Buffer.concat([t, data])), 0);
    return Buffer.concat([len, t, data, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  const parts = [
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflated),
    chunk('IEND', Buffer.alloc(0)),
  ];
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.concat(parts));
}

// ---- 抠出 icon 槽位 ----
const meta = JSON.parse(fs.readFileSync(atlasJson, 'utf8'));
const slot = meta.slots.find((s) => s.name === 'icon');
if (!slot) throw new Error('素材/图集.json 里没有 icon 槽位');
const sheet = readPNG(atlasPng);
if (sheet.w !== meta.width || sheet.h !== meta.height) {
  throw new Error('整图尺寸与 json 不符');
}

const src = Buffer.alloc(slot.w * slot.h * 4);
for (let y = 0; y < slot.h; y++) {
  sheet.rgba.copy(src, y * slot.w * 4,
    ((slot.y + y) * sheet.w + slot.x) * 4,
    ((slot.y + y) * sheet.w + slot.x + slot.w) * 4);
}

// ---- 按密度最近邻放大 ----
// Android 的 mipmap 密度：mdpi=1x、hdpi=1.5x、xhdpi=2x、xxhdpi=3x、xxxhdpi=4x
const DENSITIES = [
  { dir: 'mipmap-mdpi', size: 48 },
  { dir: 'mipmap-hdpi', size: 72 },
  { dir: 'mipmap-xhdpi', size: 96 },
  { dir: 'mipmap-xxhdpi', size: 144 },
  { dir: 'mipmap-xxxhdpi', size: 192 },
];

for (const d of DENSITIES) {
  const out = Buffer.alloc(d.size * d.size * 4);
  for (let y = 0; y < d.size; y++) {
    for (let x = 0; x < d.size; x++) {
      // 最近邻：保持像素风的硬边，不要插值出糊边
      const sx = Math.min(slot.w - 1, Math.floor(x * slot.w / d.size));
      const sy = Math.min(slot.h - 1, Math.floor(y * slot.h / d.size));
      const s = (sy * slot.w + sx) * 4, t = (y * d.size + x) * 4;
      out[t] = src[s]; out[t + 1] = src[s + 1]; out[t + 2] = src[s + 2]; out[t + 3] = src[s + 3];
    }
  }
  const file = path.join(outRoot, d.dir, 'ic_launcher.png');
  writePNG(file, d.size, d.size, out);
  console.log('图标 ' + d.dir + '/ic_launcher.png  ' + d.size + '×' + d.size);
}
console.log('源：素材/图集.png 的 icon 槽位（' + slot.w + '×' + slot.h + '）');
