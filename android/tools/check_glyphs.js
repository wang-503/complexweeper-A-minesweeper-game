/* 字形覆盖率门禁：ui_strings.js 里出现的**每一个字符**都必须能在字形图集里找到。
 *
 * 为什么单独一步、而且要卡在编译之前：字形图集是构建期生成的，漏字的后果是
 * "装到手机上某个按钮显示成方块"，这种问题在开发机上不会自己冒出来。
 * 这里直接读实际生成的 src/font_meta.h（而不是重新算一遍清单），
 * 所以它检查的是"真正会被编进程序的那份数据"。
 *
 * 用法: node tools/check_glyphs.js
 */
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..'); // android/
const metaPath = path.join(root, 'src', 'font_meta.h');
const glyphsPath = path.join(root, 'build', 'glyphs.json');

if (!fs.existsSync(metaPath)) {
  console.error('缺少 src/font_meta.h —— 先跑 node tools/gen_font.js');
  process.exit(2);
}
if (!fs.existsSync(glyphsPath)) {
  console.error('缺少 build/glyphs.json —— 先跑 node tools/gen_ui_strings.js');
  process.exit(2);
}

const meta = fs.readFileSync(metaPath, 'utf8');
// 注意：#define 是独立一行，前面没有 "{",早期这里写成 /\{#define FONT_SIZE_COUNT (\d+)/
// 于是永远匹配不到、sizeCount 恒为 0，门禁变成"什么都不检查"。就是这一条曾经
// 让覆盖率检查一直安静地"通过"。
const sizeCountMatch = meta.match(/#define\s+FONT_SIZE_COUNT\s+(\d+)/);
const sizeCount = sizeCountMatch ? +sizeCountMatch[1] : 0;
if (sizeCount === 0) {
  console.error('读不到 FONT_SIZE_COUNT（src/font_meta.h 可能不完整）—— 门禁不能装作通过');
  process.exit(2);
}

// 解析 font_glyphs 表里的 (cp, size) 对
const covered = new Map(); // size -> Set(cp)
for (const m of meta.matchAll(/^\s*\{\s*(\d+)u,\s*(\d+),/gm)) {
  const cp = +m[1];
  const size = +m[2];
  if (!covered.has(size)) covered.set(size, new Set());
  covered.get(size).add(cp);
}

const list = JSON.parse(fs.readFileSync(glyphsPath, 'utf8'));
const wanted = list.glyphs; // [{ch, cp, from}]
const sizeNames = list.sizes.map((s) => s.name);

let missing = 0;
const missingBySize = new Map();
for (let size = 0; size < sizeCount; size++) {
  const set = covered.get(size) || new Set();
  for (const g of wanted) {
    if (set.has(g.cp)) continue;
    missing += 1;
    if (!missingBySize.has(size)) missingBySize.set(size, []);
    missingBySize.get(size).push(g);
  }
}

console.log('字形覆盖率：需要 ' + wanted.length + ' 个字符 × ' + sizeCount + ' 个字号 = ' +
  wanted.length * sizeCount + ' 个字形');
for (let size = 0; size < sizeCount; size++) {
  const set = covered.get(size) || new Set();
  console.log('  字号 ' + sizeNames[size] + '：图集里有 ' + set.size + ' 个字形');
}

if (missing > 0) {
  console.error('\n缺字形 ' + missing + ' 个：');
  for (const [size, arr] of missingBySize) {
    const sample = arr.slice(0, 20).map((g) => g.ch + '(U+' + g.cp.toString(16).toUpperCase() + ')').join(' ');
    console.error('  字号 ' + sizeNames[size] + '：' + arr.length + ' 个 —— ' + sample +
      (arr.length > 20 ? ' …' : ''));
    console.error('    这些字符出现在：' +
      [...new Set(arr.flatMap((g) => g.from))].slice(0, 6).join(', '));
  }
  console.error('\n修法：tools/ui_strings.js 改了文案之后，删掉 ' +
    'build/font.stamp 重新构建（或直接 node tools/gen_font.js），让图集重新生成。');
  process.exit(1);
}

console.log('覆盖率门禁通过：界面文案里每个字符都在图集里。');
