/* 中文字形图集：把 tools/ui_strings.js 里出现的字符位图化成 1 位色图集。
 *
 * 为什么用 System.Drawing 而不是自己写字体解析：安卓端没有可依赖的系统字体 API，
 * 而我们只需要"固定的一小撮字符 × 3 个字号"，在构建期用 Windows 自带的字体栈
 * 位图化一次就够了。用 SingleBitPerPixelGridFit 拿到的是硬边（不带抗锯齿灰度）
 * 的像素字，放大到整数倍后和经典扫雷的观感一致。
 *
 * 三段式（和仓库里 gen_atlas.js 的思路一致）：
 *   1. 本脚本建"要渲染哪些字"的 job.json，并负责最后的排版与生成 src/font_meta.h
 *   2. tools/fetch_glyphs.ps1 用 System.Drawing 渲染出 raw 数据 + 每字包围盒
 *   3. 本脚本把包围盒排进 1024×N 的整页（1 位色），拼成 src/font_atlas.bin
 * 数据格式（小端）：
 *   magic "CSFT" | u16 version=1 | u16 page_count | u16 page_w | u16 page_h | u16 size_count
 *   page_count 项：u32 该页字节数
 *   页码数据：page_h * (page_w/8) 字节，行内自左向右，MSB 在左
 *   字形表由 font_meta.h 携带（编译期常量），不需要运行时解析
 *
 * 用法: node tools/gen_font.js [--skip-fetch]
 */
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execFileSync } = require('child_process');

const root = path.resolve(__dirname, '..'); // android/
const srcDir = path.join(root, 'src');
const buildDir = path.join(root, 'build');

const argv = process.argv.slice(2);
const skipFetch = argv.includes('--skip-fetch');

const { SIZES } = require('./ui_strings.js');

const PAGE_W = 1024;
const MAX_PAGE_H = 2048;
const PAD = 1; // 字之间留 1 像素，避免放大后相邻字的边粘在一起

const glyphsPath = path.join(buildDir, 'glyphs.json');
if (!fs.existsSync(glyphsPath)) {
  throw new Error('缺少 build/glyphs.json —— 先跑 node tools/gen_ui_strings.js');
}
const glyphFile = JSON.parse(fs.readFileSync(glyphsPath, 'utf8'));
const glyphs = glyphFile.glyphs;

const rawPath = path.join(buildDir, 'glyphs_raw.txt');

// ---- 1. 渲染：把参数直接交给 fetch_glyphs.ps1，中间不过 JSON ----
if (!skipFetch) {
  console.log('[字体] 用 System.Drawing 位图化 ' + glyphs.length + ' 个字符 × ' +
    SIZES.length + ' 个字号…');
  const ps = path.join(__dirname, 'fetch_glyphs.ps1');
  const sizesArg = SIZES.map((s) => s.name + ':' + s.px).join(',');
  const cpsArg = glyphs.map((g) => g.cp).join(',');
  // 走 shell 并自己加引号：脚本路径里有空格（C:\Code Programs\...），
  // execFileSync 不会替我们引，Windows 上会直接找不到文件。
  execFileSync(
    `powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${ps}" ` +
    `-Sizes "${sizesArg}" -Codepoints "${cpsArg}" -Out "${rawPath}"`,
    { stdio: 'inherit', shell: true });
}

if (!fs.existsSync(rawPath)) throw new Error('渲染没有产出 ' + rawPath);

// ---- 2. 解析逐行输出 ----
const raw = { sizes: [] };
for (const line of fs.readFileSync(rawPath, 'utf8').split(/\r?\n/)) {
  if (!line) continue;
  const f = line.split(' ');
  if (f[0] === 'SIZE') {
    raw.sizes.push({ name: f[1], px: +f[2], ascent: +f[3], lineH: +f[4], glyphs: [] });
    continue;
  }
  if (f[0] === 'G') {
    if (!raw.sizes.length) throw new Error('渲染输出里 G 行出现在 SIZE 之前');
    const hex = f[7] === '-' ? [] : f[7].split(',');
    raw.sizes[raw.sizes.length - 1].glyphs.push({
      cp: +f[1], advance: +f[2], bx: +f[3], by: +f[4], inkW: +f[5], inkH: +f[6], bits: hex,
    });
  }
}
if (!raw.sizes.length) throw new Error('渲染输出里没有 SIZE 行：' + rawPath);


// ---- 2. 把每个字的 1 位图排进整页 ----
// 先按高度分桶再逐行摆放：同一个字号的行高一致，这样基本不浪费空间。
const entries = []; // 每个字形在页里的位置 + 度量
let page = { w: PAGE_W, h: 0, plan: [] };
let pages = [];
let cursor = { x: 0, y: 0, rowH: 0 };
let pageBytes = [];

function newPage() {
  if (cursor.y > 0) {
    pages.push({ w: PAGE_W, h: cursor.y, plan: page.plan.slice() });
  }
  page = { w: PAGE_W, h: 0, plan: [] };
  cursor = { x: 0, y: 0, rowH: 0 };
}

for (const size of raw.sizes) {
  for (const g of size.glyphs) {
    const w = g.inkW;
    const h = g.inkH;
    if (w === 0 || h === 0) {
      // 空格之类：没有墨水，但仍然要占一个字形条目（有前进宽度）
      entries.push({
        cp: g.cp, size: size.name, px: size.px,
        advance: g.advance, bx: 0, by: 0, x: 0, y: 0, w: 0, h: 0, page: pages.length,
        offset: 0,
      });
      continue;
    }
    if (cursor.x + w + PAD > PAGE_W) {
      cursor.x = 0;
      cursor.y += cursor.rowH + PAD;
      cursor.rowH = 0;
    }
    if (cursor.y + h > 2048) newPage();
    const x = cursor.x;
    const y = cursor.y;
    cursor.x += w + PAD;
    if (h > cursor.rowH) cursor.rowH = h;
    page.plan.push({ x, y, w, h, cp: g.cp, size: size.name });
    entries.push({
      cp: g.cp, size: size.name, px: size.px,
      advance: g.advance, bx: g.bx, by: g.by, x, y, w, h, page: -1, offset: 0,
    });
  }
}
// 收尾：把最后一页也提交
if (cursor.y > 0) pages.push({ w: PAGE_W, h: cursor.y, plan: page.plan.slice() });

// 把 plan 里的坐标回填给 entries
const planIndex = new Map(); // size|cp -> {page, x, y}
pages.forEach((p, pi) => {
  for (const it of p.plan) planIndex.set(it.size + '|' + it.cp, { page: pi, x: it.x, y: it.y });
});
for (const e of entries) {
  if (e.w === 0) { e.page = 0; e.offset = 0; continue; }
  const hit = planIndex.get(e.size + '|' + e.cp);
  e.page = hit.page;
  e.x = hit.x;
  e.y = hit.y;
}

// ---- 3. 按页拼 1 位色数据 ----
// raw 里每个字形的 bits 是"每行 inkW 位的十六进制串"（MSB 在左）
const rawIndex = new Map();
for (const size of raw.sizes) {
  for (const g of size.glyphs) rawIndex.set(size.name + '|' + g.cp, g.bits);
}

// **所有页必须补齐到同一高度**。
// 头部只记一个 page_h（= 各页最大高度），运行时按 page_w/8 * page_h 算每页步长，
// 所以每页数据长度必须**恰好等于** stride * page_h。
// 早先每页只分配"自己实际用到的高度"：单页时看不出来（那页就是最高页），
// 变成两页之后第 1 页短了 185728 字节，运行时从第 1 页取字形就读到错位数据 ——
// 表现为**整个界面文字乱码**（字形盒坐标指向不存在的像素）。
const pageH = pages.length ? Math.max(...pages.map((p) => p.h)) : 0;
pageBytes = pages.map((p) => {
  const stride = p.w / 8;
  const buf = Buffer.alloc(stride * pageH, 0);   // ← 统一高度，短的页补零
  for (const it of p.plan) {
    const bits = rawIndex.get(it.size + '|' + it.cp);
    if (!bits) throw new Error('渲染结果里缺少 ' + it.size + ' ' + it.cp);
    for (let y = 0; y < it.h; y++) {
      const row = bits[y];
      if (!row) continue;
      for (let x = 0; x < it.w; x++) {
        // row 是十六进制串，每个字符 4 位；取第 x 位（MSB 在左）
        const nib = parseInt(row[x >> 2], 16);
        const bit = (nib >> (3 - (x & 3))) & 1;
        if (!bit) continue;
        const px = it.x + x;
        const py = it.y + y;
        buf[py * stride + (px >> 3)] |= 0x80 >> (px & 7);
      }
    }
  }
  return buf;
});

// ---- 4. 写 font_atlas.bin ----
// 头 18 字节：magic(4) + version(2) + page_count(2) + page_w(2) + page_h(2) + size_count(2) + 保留(4)
// 之后每页 4 字节的字节数，再之后是各页数据
const header = Buffer.alloc(18 + pages.length * 4);
header.write('CSFT', 0, 'ascii');
header.writeUInt16LE(1, 4);
header.writeUInt16LE(pages.length, 6);
header.writeUInt16LE(PAGE_W, 8);
header.writeUInt16LE(pages.length ? pageH : 0, 10);
header.writeUInt16LE(SIZES.length, 12);
pages.forEach((p, i) => header.writeUInt32LE(pageBytes[i].length, 18 + i * 4));

// **自检**：每页长度必须恰好等于 stride * page_h。
// 运行时就是按这个式子算步长的，不相等就一定读错位（参见上面补零的说明）。
// 这条断言是为了"以后再加字号层"时不会再悄悄踩同一个坑。
{
  const stride = PAGE_W / 8;
  pageBytes.forEach((b, i) => {
    if (b.length !== stride * pageH) {
      throw new Error(
        '字形图集第 ' + i + ' 页长度 ' + b.length + ' 字节，应为 ' +
        stride * pageH + '（= stride ' + stride + ' × page_h ' + pageH + '）');
    }
  });
}
// 字形图集直接写进 assets/：APK 里就是这个路径，运行时按 assets/font_atlas.bin 打开。
// （早先写在 src/ 下，结果 aapt2 打包时 assets/ 里没有它，装到手机上启动就报读不到。）
const binPath = path.join(root, 'assets', 'font_atlas.bin');
fs.mkdirSync(path.dirname(binPath), { recursive: true });
fs.writeFileSync(binPath, Buffer.concat([header, ...pageBytes]));

// ---- 5. 写 font_meta.h ----
// **把「生成源哈希」写进 font_meta.h 自己**，而不是单独放一个 stamp 文件。
//
// 为什么：构建脚本要靠"哈希对不对"来决定要不要重新位图化字形，而字形流水线依赖
// Windows 的 System.Drawing，仓库里提交了生成物就是为了让别人不用跑它。
// 早先用的是 build/font.stamp（已 gitignore）—— 新克隆里根本没有，
// 于是必然重新生成；后来改成 tools/font.stamp 入库，又出现「stamp 与脚本不同步」
// 的隐患：改了脚本忘了更新 stamp，构建就会一直复用旧图集（踩过一次，
// 表现为真机整屏乱码、排查很久）。
//
// 现在改成自洽：font_meta.h 里记着"我是由哪两个文件的哪个哈希生成的"，
// build.ps1 现场重算、对比自己。缺任何一个产物、或哈希不符，就重新生成。
//
// **哈希必须先归一化换行符**。这一条是踩出来的：同样的文件，我本机上
// tools/ui_strings.js 是 CRLF（6147 字节），而 git blob 里是 LF（6004 字节），
// 于是"我算出的哈希"和"任何人 clone 出来算出的哈希"必然不同 —— 每个新克隆
// 都会判定需要重新生成，而那条流水线依赖 Windows 的 System.Drawing，
// 别人根本跑不了。哈希的可复现性不能依赖"检出时的行尾"。
// 归一化只影响换行符；内容（含换行符以外的所有字节）仍然参与哈希。
const hashSrc = (p) => {
  const norm = fs.readFileSync(p, 'utf8').replace(/\r\n/g, '\n');
  return crypto.createHash('sha256').update(norm, 'utf8').digest('hex');
};
const hashUi = hashSrc(path.join(root, 'tools', 'ui_strings.js'));
const hashGen = hashSrc(__filename);
const srcHash = (hashUi + hashGen).toLowerCase();

const names = SIZES.map((s) => s.name);
const lines = [];
lines.push('// 本文件由 tools/gen_font.js 自动生成，不要手改。');
lines.push('// 字形来自 tools/ui_strings.js 的文案清单，用构建机的系统字体位图化。');
lines.push('//');
lines.push('// 下面这个哈希 = sha256(tools/ui_strings.js) + sha256(tools/gen_font.js)，');
lines.push('// 小写十六进制。build.ps1 会现场重算并对比它，决定要不要重新生成字形：');
lines.push('//   · 一致 且 font_atlas.bin 在 → 直接复用（别人 clone 下来就是这样）');
lines.push('//   · 不一致 / 产物缺失       → 跑字形流水线（需要 Windows 的 System.Drawing）');
lines.push('#define FONT_SRC_HASH "' + srcHash + '"');
lines.push('');
lines.push('#ifndef CS_FONT_META_H');
lines.push('#define CS_FONT_META_H');
lines.push('');
lines.push('#include <stdint.h>');
lines.push('');
lines.push('#define FONT_PAGE_W ' + PAGE_W);
lines.push('#define FONT_PAGE_H ' + header.readUInt16LE(10));
lines.push('#define FONT_PAGE_COUNT ' + pages.length);
lines.push('#define FONT_SIZE_COUNT ' + SIZES.length);
lines.push('');
lines.push('typedef struct {');
lines.push('    uint32_t cp;      // 码点');
lines.push('    uint16_t size;    // 字号档下标');
lines.push('    int16_t advance;  // 前进宽度（像素）');
lines.push('    int16_t bx;       // 墨水盒相对笔位的偏移');
lines.push('    int16_t by;');
lines.push('    uint16_t x, y, w, h; // 在页里的位置');
lines.push('    uint16_t page;');
lines.push('} FontGlyph;');
lines.push('');
lines.push('static const FontGlyph font_glyphs[] = {');
entries.sort((a, b) => (a.size === b.size ? a.cp - b.cp : a.size - b.size));
for (const e of entries) {
  lines.push('    { ' + e.cp + 'u, ' + names.indexOf(e.size) + ', ' + (e.advance | 0) + ', ' +
    (e.bx | 0) + ', ' + (e.by | 0) + ', ' + e.x + ', ' + e.y + ', ' + e.w + ', ' + e.h + ', ' +
    e.page + ' },');
}
lines.push('};');
lines.push('#define FONT_GLYPH_COUNT ' + entries.length);
lines.push('');
lines.push('// 每个字号的度量：ascent 用来把笔位换算成墨水盒顶边');
lines.push('typedef struct { int16_t px; int16_t ascent; int16_t line_h; } FontSizeInfo;');
lines.push('static const FontSizeInfo font_sizes[FONT_SIZE_COUNT] = {');
for (const s of raw.sizes) {
  lines.push('    { ' + s.px + ', ' + (s.ascent | 0) + ', ' + (s.lineH | 0) + ' }, // ' + s.name);
}
lines.push('};');
lines.push('');
lines.push('#endif // CS_FONT_META_H');
lines.push('');

fs.writeFileSync(path.join(srcDir, 'font_meta.h'), lines.join('\n'), 'utf8');

const nonEmpty = entries.filter((e) => e.w > 0).length;
console.log('字形: ' + entries.length + ' 条（有墨水的 ' + nonEmpty + '），' +
  pages.length + ' 页 × ' + PAGE_W + '×' + header.readUInt16LE(10) + ' 位图，bin ' +
  fs.statSync(binPath).size + ' 字节');
console.log('写出 ' + path.relative(root, binPath));
console.log('写出 ' + path.relative(root, path.join(srcDir, 'font_meta.h')));
