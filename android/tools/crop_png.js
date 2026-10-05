/* 把 PNG 裁剪一块并整数倍放大，用于肉眼核对界面细节。
 *
 * 为什么自己写：这台机器上没有 ImageMagick / Python 图像库可用，而核对像素级
 * 细节（菜单栏文字、边框）必须放大看。只支持 render_write_png 写出的
 * 8 位真彩色、非隔行 PNG（我们自己的输出），够用。
 *
 * 用法: node tools/crop_png.js <in.png> <out.png> <x> <y> <w> <h> [scale]
 */
'use strict';

const fs = require('fs');
const zlib = require('zlib');

const [input, output, xs, ys, ws, hs, ss] = process.argv.slice(2);
if (!input || !output) {
  console.error('用法: node tools/crop_png.js <in.png> <out.png> <x> <y> <w> <h> [scale]');
  process.exit(2);
}

function readPNG(file) {
  const buf = fs.readFileSync(file);
  let off = 8, w = 0, h = 0, bitDepth = 0, colorType = 0, interlace = 0;
  const idat = [];
  while (off < buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString('ascii', off + 4, off + 8);
    const data = buf.subarray(off + 8, off + 8 + len);
    if (type === 'IHDR') {
      w = data.readUInt32BE(0); h = data.readUInt32BE(4);
      bitDepth = data[8]; colorType = data[9]; interlace = data[12];
    } else if (type === 'IDAT') idat.push(Buffer.from(data));
    else if (type === 'IEND') break;
    off += 12 + len;
  }
  if (bitDepth !== 8 || interlace) throw new Error('只支持 8 位非隔行 PNG');
  const channels = { 0: 1, 2: 3, 4: 2, 6: 4 }[colorType];
  if (!channels) throw new Error('不支持的颜色类型 ' + colorType);
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
  const rgb = Buffer.alloc(w * h * 3);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const s = y * stride + x * channels, d = (y * w + x) * 3;
    if (colorType === 2) { rgb[d] = out[s]; rgb[d + 1] = out[s + 1]; rgb[d + 2] = out[s + 2]; }
    else if (colorType === 6) { rgb[d] = out[s]; rgb[d + 1] = out[s + 1]; rgb[d + 2] = out[s + 2]; }
    else { rgb[d] = rgb[d + 1] = rgb[d + 2] = out[s]; }
  }
  return { w, h, rgb };
}

function writePNG(file, w, h, rgb) {
  const stride = 1 + w * 3;
  const raw = Buffer.alloc(stride * h);
  for (let y = 0; y < h; y++) {
    raw[y * stride] = 0;
    rgb.copy(raw, y * stride + 1, y * w * 3, (y + 1) * w * 3);
  }
  const deflated = zlib.deflateSync(raw);
  const chunks = [];
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
    const t = Buffer.from(type, 'ascii');
    const crcBuf = Buffer.concat([t, data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(crcBuf) >>> 0, 0);
    return Buffer.concat([len, t, data, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  chunks.push(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]));
  chunks.push(chunk('IHDR', ihdr));
  chunks.push(chunk('IDAT', deflated));
  chunks.push(chunk('IEND', Buffer.alloc(0)));
  fs.writeFileSync(file, Buffer.concat(chunks));
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

const src = readPNG(input);
const x = parseInt(xs || '0', 10), y = parseInt(ys || '0', 10);
const w = parseInt(ws || String(src.w), 10), h = parseInt(hs || String(src.h), 10);
const scale = parseInt(ss || '1', 10);
const cw = Math.min(w, src.w - x), chh = Math.min(h, src.h - y);
const outW = cw * scale, outH = chh * scale;
const outRGB = Buffer.alloc(outW * outH * 3);
for (let oy = 0; oy < outH; oy++) {
  for (let ox = 0; ox < outW; ox++) {
    const sx = x + Math.floor(ox / scale), sy = y + Math.floor(oy / scale);
    const s = (sy * src.w + sx) * 3, d = (oy * outW + ox) * 3;
    outRGB[d] = src.rgb[s]; outRGB[d + 1] = src.rgb[s + 1]; outRGB[d + 2] = src.rgb[s + 2];
  }
}
writePNG(output, outW, outH, outRGB);
console.log('裁剪 ' + cw + '×' + chh + ' →' + outW + '×' + outH + '，写出 ' + output);
