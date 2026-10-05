/* 把 aapt2 产出的 APK 再补上原生库，并且**以 stored（不压缩）方式**写入。
 *
 * 为什么不用 PowerShell 的 System.IO.Compression：实测在 Windows PowerShell 5.1
 * （.NET Framework）下，无论用 CreateEntry(..., NoCompression) 还是
 * CreateEntryFromFile(..., NoCompression)，写出来的中央目录里方法位都是 8（deflate）。
 * 而 Android 是直接从 APK 里 mmap 这些 .so 的，压缩过的库在老系统上装不上。
 * 与其跟那套 API 的怪脾气缠斗，不如自己拼 ZIP —— 格式本身很简单，而且完全可控。
 *
 * 只做两件事：
 *   1) 原样保留 aapt2 写出的条目（含它们各自的压缩方法）
 *   2) 追加 lib/<abi>/*.so 为 stored 条目
 *
 * 用法: node tools/pack_apk.js <输入的 apk> <输出的 apk> <so 文件> [更多 so...]
 *   so 文件按 "lib/<abi>/<名字>" 的相对路径给出：从 <so 文件> 里自动推导 ABI。
 */
'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const argv = process.argv.slice(2);
if (argv.length < 3) {
  console.error('用法: node tools/pack_apk.js <输入apk> <输出apk> <abi=路径.so> [更多...]');
  process.exit(2);
}
const [inApk, outApk, ...soSpecs] = argv;

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

// ---- 读 aapt2 产出的 ZIP 中央目录 ----
function readZip(file) {
  const buf = fs.readFileSync(file);
  let eocd = -1;
  for (let i = buf.length - 22; i >= 0 && i > buf.length - 66000; i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error(file + ': 不是合法的 ZIP');
  const count = buf.readUInt16LE(eocd + 10);
  let off = buf.readUInt32LE(eocd + 16);
  const entries = [];
  for (let i = 0; i < count; i++) {
    if (buf.readUInt32LE(off) !== 0x02014b50) throw new Error('中央目录损坏 @' + off);
    const e = {
      name: buf.toString('utf8', off + 46, off + 46 + buf.readUInt16LE(off + 28)),
      method: buf.readUInt16LE(off + 10),
      mtime: buf.readUInt16LE(off + 12),
      mdate: buf.readUInt16LE(off + 14),
      crc: buf.readUInt32LE(off + 16),
      compSize: buf.readUInt32LE(off + 20),
      uncompSize: buf.readUInt32LE(off + 24),
      localOff: buf.readUInt32LE(off + 42),
    };
    // 本地头里长度可能与中央目录不同（有 extra 字段），以本地头为准
    if (buf.readUInt32LE(e.localOff) !== 0x04034b50) throw new Error('本地头损坏 @' + e.localOff);
    const nameLen = buf.readUInt16LE(e.localOff + 26);
    const extraLen = buf.readUInt16LE(e.localOff + 28);
    e.dataStart = e.localOff + 30 + nameLen + extraLen;
    e.data = buf.subarray(e.dataStart, e.dataStart + e.compSize);
    entries.push(e);
    off += 46 + buf.readUInt16LE(off + 28) + buf.readUInt16LE(off + 30) + buf.readUInt16LE(off + 32);
  }
  return entries;
}

function dosDateTime() {
  const d = new Date();
  const date = (((d.getFullYear() - 1980) & 0x7F) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
  const time = (d.getHours() << 11) | (d.getMinutes() << 5) | (Math.floor(d.getSeconds() / 2) & 0x1F);
  return { date, time };
}

// ---- 写 ZIP：条目按 list 顺序，本地头与中央目录都用同一份元数据 ----
function writeZip(file, entries) {
  const { date, time } = dosDateTime();
  const locals = [];
  let offset = 0;
  const central = [];

  for (const e of entries) {
    const nameBuf = Buffer.from(e.name, 'utf8');
    const local = Buffer.alloc(30 + nameBuf.length);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);                       // version needed
    local.writeUInt16LE(0x0800, 6);                   // 语言编码标志：名字是 UTF-8
    local.writeUInt16LE(e.method, 8);
    local.writeUInt16LE(e.time !== undefined ? e.time : time, 10);
    local.writeUInt16LE(e.date !== undefined ? e.date : date, 12);
    local.writeUInt32LE(e.crc, 14);
    local.writeUInt32LE(e.compSize, 18);
    local.writeUInt32LE(e.uncompSize, 22);
    local.writeUInt16LE(nameBuf.length, 26);
    local.writeUInt16LE(0, 28);                       // extra len
    nameBuf.copy(local, 30);
    locals.push(local, e.data);
    if (offset > 0xFFFFFFFF) throw new Error('APK 太大，超出经典 ZIP 的 4GB 限制');

    const ch = Buffer.alloc(46 + nameBuf.length);
    ch.writeUInt32LE(0x02014b50, 0);
    ch.writeUInt16LE(0x031E, 4);                      // version made by（3 = UNIX）
    ch.writeUInt16LE(20, 6);
    ch.writeUInt16LE(0x0800, 8);
    ch.writeUInt16LE(e.method, 10);
    ch.writeUInt16LE(e.time !== undefined ? e.time : time, 12);
    ch.writeUInt16LE(e.date !== undefined ? e.date : date, 14);
    ch.writeUInt32LE(e.crc, 16);
    ch.writeUInt32LE(e.compSize, 20);
    ch.writeUInt32LE(e.uncompSize, 24);
    ch.writeUInt16LE(nameBuf.length, 28);
    ch.writeUInt16LE(0, 30);                          // extra
    ch.writeUInt16LE(0, 32);                          // comment
    ch.writeUInt16LE(0, 34);                          // disk number
    ch.writeUInt16LE(0, 36);                          // internal attrs
    ch.writeUInt32LE((0o100644 << 16) >>> 0, 38);     // external attrs：普通文件 644
    ch.writeUInt32LE(offset, 42);
    nameBuf.copy(ch, 46);
    central.push(ch);

    offset += local.length + e.data.length;
  }

  const centralStart = offset;
  const centralBuf = Buffer.concat(central);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(0, 4);
  eocd.writeUInt16LE(0, 6);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(centralBuf.length, 12);
  eocd.writeUInt32LE(centralStart, 16);
  eocd.writeUInt16LE(0, 20);

  fs.writeFileSync(file, Buffer.concat([...locals, centralBuf, eocd]));
}

// ---- 组装 ----
const entries = readZip(inApk);

// 万一 aapt2 已经带了 lib/ 下的东西，先清掉（以我们这次构建的为准）
const kept = entries.filter((e) => !e.name.startsWith('lib/'));

let added = 0;
for (const spec of soSpecs) {
  const eq = spec.indexOf('=');
  if (eq < 0) throw new Error('参数要写成 abi=路径.so：' + spec);
  const abi = spec.slice(0, eq);
  const soPath = spec.slice(eq + 1);
  const data = fs.readFileSync(soPath);
  kept.push({
    name: 'lib/' + abi + '/' + path.basename(soPath),
    method: 0,                     // stored：Android 才能直接 mmap
    crc: crc32(data),
    compSize: data.length,
    uncompSize: data.length,
    data,
  });
  added += 1;
  console.log('  stored: lib/' + abi + '/' + path.basename(soPath) + '  ' + data.length + ' 字节');
}
if (added === 0) throw new Error('没有要加入的 .so');

writeZip(outApk, kept);
console.log('  打包 ' + entries.length + ' 个原始条目 + ' + added + ' 个原生库 → ' +
  path.basename(outApk) + '（' + fs.statSync(outApk).size + ' 字节）');
