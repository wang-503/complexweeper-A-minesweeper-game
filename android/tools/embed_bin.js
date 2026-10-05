/* 把二进制图集转成一个 C 头文件里的字节数组（十六进制文本）。
 *
 * 为什么不直接 fwrite 成 .inc 里的 \xNN 转义串：十六进制数字看起来更整齐，
 * 也避免了 \x 后面紧跟十六进制字符时被吃掉一位这种经典坑。
 *
 * 用法: node tools/embed_bin.js <输入.bin> <输出.inc> <数组名>
 */
'use strict';

const fs = require('fs');
const path = require('path');

const [input, output, name] = process.argv.slice(2);
if (!input || !output || !name) {
  console.error('用法: node tools/embed_bin.js <输入.bin> <输出.inc> <数组名>');
  process.exit(2);
}

const data = fs.readFileSync(input);
const lines = [];
lines.push('/* 本文件由 tools/embed_bin.js 自动生成，不要手改。 */');
lines.push('/* 来源: ' + path.basename(input) + '（' + data.length + ' 字节） */');
const PER = 16;
for (let i = 0; i < data.length; i += PER) {
  const chunk = [];
  for (let j = i; j < Math.min(i + PER, data.length); j++) {
    chunk.push('0x' + data[j].toString(16).padStart(2, '0'));
  }
  lines.push('    ' + chunk.join(', ') + ',');
}
lines.push('    /* 结束标记：嵌入式数组不能为空，补一个 0 */');
lines.push('    0x00,');
lines.push('');

fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, lines.join('\n'), 'utf8');
console.log('嵌入 ' + path.basename(input) + '（' + data.length + ' 字节）→ ' + output);
