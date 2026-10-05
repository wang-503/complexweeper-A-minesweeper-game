/* 字形图集结构门禁：**每页数据长度必须恰好等于 stride × page_h**。
 *
 * 为什么需要这个检查：
 *   font_atlas.bin 的头部只记了**一个** page_h（各页最大高度），运行时按
 *   `page_w/8 * page_h` 算每页步长去取字形。
 *   如果某一页实际只写了"自己用到的高度"（比 page_h 短），那么从这一页取字形
 *   就会读到后面一段内存（往往是别的数据或零），屏幕上就是**整个界面文字乱码**。
 *
 * 这个 bug 只在**多页**时才会暴露：单页时那一页本身就是最高的页，长度正好相等。
 * 本项目曾经只有 1 页（993 个字形），扩到 9 档变 2 页之后立刻中招。
 *
 * 除了长度，这里还顺手检查：
 *   - magic / 版本 / 尺寸字段与 src/font_meta.h 一致
 *   - 所有字形的 page 下标合法、页内包围盒不越界
 */
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const binPath = path.join(root, 'assets', 'font_atlas.bin');
const metaPath = path.join(root, 'src', 'font_meta.h');

const fail = (msg) => { console.error('  [x] ' + msg); process.exitCode = 1; };

if (!fs.existsSync(binPath)) { fail('找不到 ' + binPath); process.exit(1); }
if (!fs.existsSync(metaPath)) { fail('找不到 ' + metaPath); process.exit(1); }

const bin = fs.readFileSync(binPath);
const meta = fs.readFileSync(metaPath, 'utf8');

const num = (re) => {
  const m = meta.match(re);
  if (!m) return null;
  return parseInt(m[1], 10);
};
const mPageW = num(/^#define FONT_PAGE_W (\d+)/m);
const mPageH = num(/^#define FONT_PAGE_H (\d+)/m);
const mPages = num(/^#define FONT_PAGE_COUNT (\d+)/m);
const mSizes = num(/^#define FONT_SIZE_COUNT (\d+)/m);
const mGlyphs = num(/^#define FONT_GLYPH_COUNT (\d+)/m);

if (bin.length < 18) fail('图集文件太小');
if (bin.toString('ascii', 0, 4) !== 'CSFT') fail('magic 不是 CSFT');

const pageCount = bin.readUInt16LE(6);
const pageW = bin.readUInt16LE(8);
const pageH = bin.readUInt16LE(10);
const sizeCount = bin.readUInt16LE(12);

const eq = (name, binV, metaV) => {
  if (metaV === null) { fail('font_meta.h 里读不到 ' + name); return; }
  if (binV !== metaV) fail(name + ' 不一致：图集 ' + binV + ' vs font_meta.h ' + metaV);
};
eq('page_count', pageCount, mPages);
eq('page_w', pageW, mPageW);
eq('page_h', pageH, mPageH);
eq('size_count', sizeCount, mSizes);

const stride = Math.ceil(pageW / 8);
const needPerPage = stride * pageH;
let off = 18 + pageCount * 4;
for (let i = 0; i < pageCount; i++) {
  const n = bin.readUInt32LE(18 + i * 4);
  if (n !== needPerPage) {
    fail('第 ' + i + ' 页只有 ' + n + ' 字节，应为 ' + needPerPage +
         '（= stride ' + stride + ' × page_h ' + pageH + '）。' +
         '页数据短了会让运行时从这一页取到错位内存，屏幕上就是文字乱码。');
  }
  if (off + n > bin.length) fail('第 ' + i + ' 页数据超出文件末尾');
  off += n;
}
if (off !== bin.length) {
  fail('页数据总长 ' + off + ' 与文件长度 ' + bin.length + ' 不符（有多余或缺失的字节）');
}

// 字形包围盒：解析 font_meta.h 里的 font_glyphs[] 粗查（只取 x,y,w,h,page 五个字段）
if (mGlyphs && mGlyphs > 0) {
  const body = meta.slice(meta.indexOf('font_glyphs[] = {'));
  const rows = body.match(/\{\s*\d+u\s*,[^}]*\}/g) || [];
  if (rows.length !== mGlyphs) {
    fail('font_meta.h 里解析到 ' + rows.length + ' 条字形，声明的是 ' + mGlyphs + ' 条');
  }
  else {
    let badPage = 0, oob = 0;
    for (const r of rows) {
      const f = r.replace(/[{}]/g, '').split(',').map((s) => parseInt(s, 10));
      // { cp, size, advance, bx, by, x, y, w, h, page }
      const x = f[5], y = f[6], w = f[7], h = f[8], pg = f[9];
      if (w === 0 || h === 0) continue;
      if (!(pg >= 0 && pg < pageCount)) { badPage++; continue; }
      if (x + w > pageW || y + h > pageH) oob++;
    }
    if (badPage) fail('有 ' + badPage + ' 个字形 page 下标越界');
    if (oob) fail('有 ' + oob + ' 个字形页内包围盒越界');
  }
}

if (process.exitCode) {
  console.error('字形图集结构检查失败。');
} else {
  console.log('  图集结构 OK：' + pageCount + ' 页 × ' + pageW + '×' + pageH +
              '，每页 ' + needPerPage + ' 字节，共 ' + bin.length + ' 字节');
}
