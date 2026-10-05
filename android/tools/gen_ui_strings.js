/* 由 tools/ui_strings.js（唯一来源）生成 src/ui_strings.h 与 build/glyphs.json。
 *
 * 为什么生成 C 头而不是直接写中文字面量：C 源码的字符集取决于编译器和文件编码，
 * 而界面文案里有中文和 "−" 这种非 ASCII 符号。转成 UTF-8 字节数组就完全没有
 * "源文件按什么编码读"这个问题了，NDK clang / MSVC / zig cc 都只有一个答案。
 *
 * 用法: node tools/gen_ui_strings.js
 */
'use strict';

const fs = require('fs');
const path = require('path');

const manifest = require('./ui_strings.js');

const root = path.resolve(__dirname, '..');           // android/
const outH = path.join(root, 'src', 'ui_strings.h');
const outGlyphs = path.join(root, 'build', 'glyphs.json');

function cBytes(str) {
  const b = Buffer.from(str, 'utf8');
  const parts = [];
  for (const x of b) parts.push(x);
  return '{ ' + parts.join(', ') + ' }';
}

const lines = [];
lines.push('// 本文件由 tools/gen_ui_strings.js 自动生成，不要手改。');
lines.push('// 文案的唯一来源是 tools/ui_strings.js；改文案请改那里然后重新构建。');
lines.push('#ifndef CS_UI_STRINGS_H');
lines.push('#define CS_UI_STRINGS_H');
lines.push('');
lines.push('#include <stddef.h>');
lines.push('');
lines.push('// 版本号写成 CS_ 前缀的宏：早期版本这里叫 UI_APP_VERSION，结果和下面');
lines.push('// 枚举里的同名成员撞车（宏把枚举名替换掉，之后所有下标全错位），别改回去。');
lines.push('#define CS_APP_VERSION_STR "' + manifest.STRINGS.APP_VERSION + '"');
lines.push('');
lines.push('typedef struct { const unsigned char *bytes; unsigned int len; } UiStr;');
lines.push('');

// ---- 文案表 ----
// 枚举成员统一加 UI_S_ 前缀：这样文案下标（UI_S_*）和界面枚举（UI_*，例如
// ui.h 里的 UI_MENU_GAME）不会互相撞名。
const keys = Object.keys(manifest.STRINGS);
const maxLen = Math.max(...keys.map(k => Buffer.byteLength(manifest.STRINGS[k], 'utf8')));
lines.push('enum {');
for (const k of keys) lines.push('    UI_S_' + k + ',');
lines.push('    UI_STRING_COUNT');
lines.push('};');
lines.push('');
lines.push('// 每行留 maxLen+1：多出来那一格当结尾 0，于是 bytes 也可以当 C 字符串直接用');
lines.push('static const unsigned char ui_string_bytes[UI_STRING_COUNT][' + (maxLen + 1) + '] = {');
for (const k of keys) {
  lines.push('    /* UI_S_' + k + ' */ ' + cBytes(manifest.STRINGS[k]) + ',');
}
lines.push('};');
lines.push('');
lines.push('static const UiStr ui_strings[UI_STRING_COUNT] = {');
for (const k of keys) {
  const n = Buffer.byteLength(manifest.STRINGS[k], 'utf8');
  lines.push('    /* UI_S_' + k + ' */ { ui_string_bytes[UI_S_' + k + '], ' + n + 'u },');
}
lines.push('};');
lines.push('');
lines.push('// 段落分组：帮助与关于面板按行渲染');
lines.push('enum {');
const paraGroups = Object.keys(manifest.PARAGRAPHS);
for (const g of paraGroups) lines.push('    UI_PARA_' + g + '_COUNT = ' + manifest.PARAGRAPHS[g].length + ',');
lines.push('};');
lines.push('static const unsigned short ui_para_help[] = { ' +
  manifest.PARAGRAPHS.HELP.map(k => 'UI_S_' + k).join(', ') + ' };');
lines.push('static const unsigned short ui_para_about[] = { ' +
  manifest.PARAGRAPHS.ABOUT.map(k => 'UI_S_' + k).join(', ') + ' };');
lines.push('');
lines.push('#endif // CS_UI_STRINGS_H');
lines.push('');

fs.mkdirSync(path.dirname(outH), { recursive: true });
fs.writeFileSync(outH, lines.join('\n'), 'utf8');

// ---- 字形清单（给 fetch_glyphs.ps1 与覆盖率门禁共用） ----
const glyphs = [...manifest.collectGlyphs()].sort((a, b) => a.codePointAt(0) - b.codePointAt(0));
fs.mkdirSync(path.dirname(outGlyphs), { recursive: true });
fs.writeFileSync(outGlyphs, JSON.stringify({
  sizes: manifest.SIZES,
  // 写成数组而不是以字符为键的对象：'A' 与 'a' 这种键在 PowerShell 的
  // ConvertFrom-Json 里会撞成"重复键"直接报错，数组没有这个问题。
  // 每个码点都带上来源，缺字时报错才说得清是缺了哪句话。
  glyphs: glyphs.map(g => ({
    ch: g,
    cp: g.codePointAt(0),
    // 哪些文案用到这个字（ASCII 全集是兜底加的，可能为空）
    from: keys.filter(k => manifest.STRINGS[k].includes(g)).map(k => 'UI_' + k),
  })),
}, null, 1), 'utf8');

// ---- 图集槽位名 → 下标（生成 C 侧的 SP_* 常量） ----
// 这一步很关键：槽位顺序就是 素材/图集.json 里 slots 的顺序，**不是字典序**。
// 早先 ui.c 里手抄了一份按字母序猜的下标，结果人脸、数字、旗帜全贴错了位置
// （红色方块满天飞）。下标必须由这份 json 生成，不能手抄。
const atlasJson = path.join(root, '..', '素材', '图集.json');
if (!fs.existsSync(atlasJson)) {
  console.error('缺少 素材/图集.json —— 安卓侧的槽位下标没法生成');
  process.exit(2);
}
const atlas = JSON.parse(fs.readFileSync(atlasJson, 'utf8'));
const hLines = [];
hLines.push('// 本文件由 tools/gen_ui_strings.js 自动生成，不要手改。');
hLines.push('// 槽位下标来自 素材/图集.json 的 slots 顺序（与 tools/gen_atlas.js 写进');
hLines.push('// atlas.bin 的顺序一致），所以手抄下标这种事不可能再发生。');
hLines.push('#ifndef CS_ASSETS_H');
hLines.push('#define CS_ASSETS_H');
hLines.push('');
hLines.push('#include <stdint.h>');
hLines.push('');
hLines.push('enum {');
atlas.slots.forEach((s, i) => hLines.push('    SP_' + s.name + ' = ' + i + ','));
hLines.push('    SP_COUNT = ' + atlas.slots.length);
hLines.push('};');
hLines.push('');
// 显示值 D = |S|^2 → 数字贴图槽位；表外为 0xFFFF
const ACHIEVABLE = [0, 1, 2, 4, 5, 8, 9, 10, 13, 16, 17, 18, 20, 25, 26, 29, 32, 34, 36, 37, 40, 49, 50, 64];
const numTab = new Array(65).fill(0xFFFF);
for (const d of ACHIEVABLE) {
  const i = atlas.slots.findIndex((s) => s.name === 'num_' + d);
  if (i < 0) throw new Error('素材/图集.json 里缺少 num_' + d);
  numTab[d] = i;
}
hLines.push('// 显示值 D = |S|^2 → 数字贴图槽位。0xFFFF = 表外（不该出现）。');
hLines.push('static const uint16_t sp_num_by_D[65] = {');
for (let d = 0; d < 65; d++) hLines.push('    [' + d + '] = ' + numTab[d] + ',');
hLines.push('};');
hLines.push('');
hLines.push('#endif // CS_ASSETS_H');
hLines.push('');
fs.writeFileSync(path.join(root, 'src', 'assets.h'), hLines.join('\n'), 'utf8');
console.log('槽位下标 ' + atlas.slots.length + ' 个 → src/assets.h');

console.log('文案: ' + keys.length + ' 条，字形 ' + glyphs.length + ' 个，字号档 ' +
  manifest.SIZES.map(s => s.name + '/' + s.px + 'px').join(' '));
console.log('写出 ' + path.relative(root, outH));
console.log('写出 ' + path.relative(root, outGlyphs));
