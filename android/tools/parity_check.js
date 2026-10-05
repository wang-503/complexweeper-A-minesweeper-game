/* 跨端规则对照：让 Windows 版（Zig 规则）和安卓版（C 规则）在**同样的种子、
 * 同样的开局格**下各跑一遍局面，然后把两份指纹逐字对比。
 *
 * 为什么需要这一步：规则有两份实现（正式版/src/rules.zig 与 android/src/game.c），
 * 这是"安卓侧不能链接 Zig 的 libc"逼出来的重复。两份实现在同种子下必须生成完全
 * 相同的棋盘，否则玩家会在两个平台上遇到不同的游戏。schedule 一次对拍能抓到的
 * 偏差包括：洗牌顺序、取整方式、遍历方向、边界判断。
 *
 * 用法: node tools/parity_check.js <windows.exe> <android_host.exe> <输出目录>
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const [winExe, hostExe, outDir] = process.argv.slice(2);
if (!winExe || !hostExe || !outDir) {
  console.error('用法: node tools/parity_check.js <windows.exe> <android_host.exe> <输出目录>');
  process.exit(2);
}

function runDump(exe, outPath) {
  // 参数单独传，路径里的空格由 Node 负责加引号；--rules-dump 是 Windows 版
  // 与宿主机自检程序共有的开关。
  const args = ['--rules-dump', outPath];
  if (exe.toLowerCase().endsWith('.exe') && path.basename(exe).startsWith('复扫雷')) {
    fs.mkdirSync(path.dirname(outPath), { recursive: true });
  }
  execFileSync(exe, args, { stdio: 'pipe' });
  if (!fs.existsSync(outPath)) throw new Error('没有产出 ' + outPath);
  return fs.readFileSync(outPath, 'utf8');
}

const winOut = path.join(outDir, 'parity_win.txt');
const andOut = path.join(outDir, 'parity_android.txt');

console.log('跨端规则对照：');
console.log('  Windows 版：' + winExe);
console.log('  安卓宿主机：' + hostExe);

let winText, andText;
try {
  winText = runDump(winExe, winOut);
} catch (e) {
  console.error('Windows 版跑 --rules-dump 失败：' + e.message);
  console.error('（确认 正式版/build.ps1 构建过，并且 main.zig 里有 --rules-dump 开关）');
  process.exit(2);
}
try {
  andText = runDump(hostExe, andOut);
} catch (e) {
  console.error('安卓宿主机跑 --rules-dump 失败：' + e.message);
  process.exit(2);
}

const winLines = winText.split(/\r?\n/);
const andLines = andText.split(/\r?\n/);

// 抬头里的版本号两边都应该一样；不一致要单独指出来
const winVer = (winText.match(/复扫雷 ([0-9.]+) · 规则指纹/) || [])[1];
const andVer = (andText.match(/复扫雷 ([0-9.]+) · 规则指纹/) || [])[1];
if (winVer && andVer && winVer !== andVer) {
  console.error('版本号不一致：Windows 版 ' + winVer + '，安卓版 ' + andVer);
  process.exit(1);
}

// 只对比"局面本体"：抬头、shape/seed/start 行、以及逐格签名。
// 刻意**跳过 board 那一行的 fnv= 尾段**：两端各自算的哈希只要有一点点实现差异
// 就会误报，而真正要守的是"逐格内容一致"。逐格签名已经逐字节覆盖了棋盘，
// 哈希带来的信息是冗余的，拿它当门禁只会制造假警报。
function canonical(lines, fnvLineRe) {
  return lines.map((l) => (fnvLineRe.test(l) ? l.replace(/fnv=[0-9a-f]+/, 'fnv=<忽略>') : l).trim());
}
const fnvRe = /^\s*board /;
const winCanon = canonical(winLines, fnvRe);
const andCanon = canonical(andLines, fnvRe);

// 哈希差异单独报一次，但不作为失败
const winHashes = [...winText.matchAll(/^\s*board .*fnv=([0-9a-f]+)$/gm)].map((m) => m[1]);
const andHashes = [...andText.matchAll(/^\s*board .*fnv=([0-9a-f]+)$/gm)].map((m) => m[1]);
const hashDiff = winHashes.filter((h, i) => h !== andHashes[i]).length;
if (hashDiff > 0) {
  console.log('  提示：' + hashDiff + ' 个局面的 fnv 值不同（两端哈希实现细节不同），');
  console.log('        逐格签名一致即视为通过 —— 签名才是内容等价性的判据。');
}

let mismatch = 0;
let firstMismatch = null;
const max = Math.max(winCanon.length, andCanon.length);
for (let i = 0; i < max; i++) {
  const a = winCanon[i] === undefined ? '<缺失>' : winCanon[i];
  const b = andCanon[i] === undefined ? '<缺失>' : andCanon[i];
  if (a === b) continue;
  mismatch += 1;
  if (!firstMismatch) firstMismatch = { line: i + 1, win: a, and: b };
}

const boardCount = (winText.match(/^\s*board /gm) || []).length;
console.log('  对比了 ' + boardCount + ' 个局面指纹，共 ' + winLines.length + ' 行');

if (mismatch > 0) {
  console.error('\n不一致 ' + mismatch + ' 行。第一处（第 ' + firstMismatch.line + ' 行）：');
  console.error('  Windows: ' + firstMismatch.win);
  console.error('  安卓   : ' + firstMismatch.and);
  console.error('\n这意味着 android/src/game.c 和 正式版/src/rules.zig 的规则飘了。');
  console.error('两边必须一起改，改完重新构建再跑这一步。');
  console.error('完整输出：' + winOut + ' 与 ' + andOut);
  process.exit(1);
}

console.log('跨端规则对照通过：' + boardCount + ' 个局面完全一致。');
