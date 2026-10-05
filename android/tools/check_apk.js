/* APK 结构校验：把"能装、能启动"的前提条件逐条查一遍。
 *
 * 这些都是实测踩过的点，装到手机上才发现代价太大：
 *   - 没有 classes.dex 就必须 android:hasCode="false"，否则安装器直接拒绝
 *   - .so 必须以 **stored（不压缩）** 方式放进 APK，压缩过的库在老系统上装不上
 *   - android:exported 在 API 31+ 是必填项，缺了安装就失败
 *   - minSdk/targetSdk 决定系统给什么兼容行为，写错了会在新系统上出各种怪现象
 *
 * 用法: node tools/check_apk.js <apk> <aapt2.exe> <apksigner.jar> <java.exe>
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const zlib = require('zlib');

const [apk, aapt2, apksigner, javaExe] = process.argv.slice(2);
if (!apk || !aapt2 || !apksigner) {
  console.error('用法: node tools/check_apk.js <apk> <aapt2> <apksigner.jar> [java]');
  process.exit(2);
}

let fails = 0;
function check(cond, what, detail) {
  if (cond) {
    console.log('  [ok]   ' + what + (detail ? '  ' + detail : ''));
  } else {
    fails += 1;
    console.error('  [FAIL] ' + what + (detail ? '  ' + detail : ''));
  }
}

console.log('APK 校验：' + path.basename(apk) + '（' + fs.statSync(apk).size + ' 字节）');

// ---- 1. 逐条列出 ZIP 内容 ----
// 自己解 ZIP 的中央目录，不依赖外部工具，顺便能看压缩方法（.so 必须 stored）
function listZipEntries(file) {
  const buf = fs.readFileSync(file);
  // 找 End of Central Directory（从尾部往前找，兼容有注释的情况）
  let eocd = -1;
  for (let i = buf.length - 22; i >= 0 && i > buf.length - 66000; i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('不是合法的 ZIP/APK（找不到 EOCD）');
  const count = buf.readUInt16LE(eocd + 10);
  let off = buf.readUInt32LE(eocd + 16);
  const entries = [];
  for (let i = 0; i < count; i++) {
    if (buf.readUInt32LE(off) !== 0x02014b50) throw new Error('中央目录损坏');
    const method = buf.readUInt16LE(off + 10);
    const compSize = buf.readUInt32LE(off + 20);
    const uncompSize = buf.readUInt32LE(off + 24);
    const nameLen = buf.readUInt16LE(off + 28);
    const extraLen = buf.readUInt16LE(off + 30);
    const commentLen = buf.readUInt16LE(off + 32);
    const name = buf.toString('utf8', off + 46, off + 46 + nameLen);
    entries.push({ name, method, compSize, uncompSize });
    off += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

const entries = listZipEntries(apk);
console.log('  内容：');
for (const e of entries) {
  const how = e.method === 0 ? 'stored' : e.method === 8 ? 'deflate' : 'method' + e.method;
  console.log('    ' + e.name + '  ' + e.uncompSize + ' 字节 (' + how + ')');
}

// hasCode=false 的纯原生应用不该有 dex
check(!entries.some((e) => e.name.endsWith('.dex')),
  '没有 classes.dex（hasCode=false 的应用不该有）');

// .so 必须不压缩
const soEntries = entries.filter((e) => e.name.startsWith('lib/') && e.name.endsWith('.so'));
check(soEntries.length > 0, '至少有一个原生库', soEntries.map((e) => e.name).join(' '));
check(soEntries.every((e) => e.method === 0),
  '所有 .so 都是 stored（未压缩），系统才能直接 mmap',
  soEntries.filter((e) => e.method !== 0).map((e) => e.name).join(' ') || '全部未压缩');
check(soEntries.every((e) => e.name.startsWith('lib/arm64-v8a/') ||
  e.name.startsWith('lib/armeabi-v7a/') || e.name.startsWith('lib/x86_64/')),
  '原生库放在合法的 ABI 目录下');
check(entries.some((e) => e.name === 'AndroidManifest.xml'), '包含 AndroidManifest.xml');
check(entries.some((e) => e.name === 'resources.arsc'), '包含 resources.arsc');
const assets = entries.filter((e) => e.name.startsWith('assets/')).map((e) => e.name);
check(assets.includes('assets/atlas.bin'), '包含 assets/atlas.bin（图集）');
check(assets.includes('assets/font_atlas.bin'), '包含 assets/font_atlas.bin（字形图集）');

// ---- 2. 用 aapt2 读 manifest 关键属性 ----
let badging = '';
try {
  badging = execFileSync(aapt2, ['dump', 'badging', apk], { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });
} catch (e) {
  console.error('  [FAIL] aapt2 dump badging 失败：' + e.message);
  fails += 1;
}
if (badging) {
  const m = (re) => (badging.match(re) || [])[null] || null;
  const pkg = (badging.match(/package: name='([^']+)'/) || [])[1];
  const verName = (badging.match(/versionName='([^']+)'/) || [])[1];
  const verCode = (badging.match(/versionCode='([^']+)'/) || [])[1];
  const minSdk = (badging.match(/minSdkVersion:'([^']+)'/) || [])[1];
  const targetSdk = (badging.match(/targetSdkVersion:'([^']+)'/) || [])[1];
  const label = (badging.match(/application-label:'([^']*)'/) || [])[1];
  const launch = (badging.match(/launchable-activity: name='([^']+)'/) || [])[1];
  const portrait = /android.hardware.screen.portrait/.test(badging);

  console.log('  manifest：package=' + pkg + ' version=' + verName + '(' + verCode + ') minSdk=' +
    minSdk + ' targetSdk=' + targetSdk);
  check(!!pkg, '有包名', pkg);
  check(!!verName && /^\d+\.\d+\.\d+$/.test(verName), '版本号形如 x.y.z', verName);
  check(!!minSdk && +minSdk >= 24, 'minSdk 不低于 24', minSdk);
  check(!!targetSdk && +targetSdk >= 33, 'targetSdk 不低于 33（新系统要求）', targetSdk);
  check(launch === 'android.app.NativeActivity', '入口是系统的 NativeActivity', launch);
  check(!!label, '有应用名', label);
  // 固定横屏：不该出现 portrait 特性
  check(!portrait, '没有声明竖屏特性（专家盘需要横屏）');
  check(/supports-gl-texture|uses-gl-es: '0x20000'|glEsVersion/.test(badging) ||
    /uses-gl-es/.test(badging), '声明了 OpenGL ES 2.0');
  check(/android.app.lib_name|native-code/.test(badging) || badging.includes('native-code'),
    '识别到原生代码', (badging.match(/native-code: ?'([^']*)'/) || [])[1] || '');
}

// ---- 3. 签名校验 ----
if (javaExe && fs.existsSync(apksigner)) {
  try {
    const out = execFileSync(javaExe, ['-jar', apksigner, 'verify', '--print-certs', apk],
      { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 });
    const dn = (out.match(/certificate DN: (.*)/) || [])[1] || '';
    check(true, '签名可校验', dn.trim());
  } catch (e) {
    check(false, '签名可校验', (e.stdout || '') + (e.stderr || '') || e.message);
  }
} else {
  console.log('  [skip] 没有 java/apksigner，跳过签名校验');
}

if (fails > 0) {
  console.error('\nAPK 校验失败：' + fails + ' 项不合格。');
  process.exit(1);
}
console.log('\nAPK 校验通过。');
