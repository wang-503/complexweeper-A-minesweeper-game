/* 界面文案与字形清单的**唯一来源**（single source of truth）。
 *
 * 为什么要有这个文件：GDI 版的中文是系统画的，安卓版得自己准备字形图集。
 * 字形图集必须"刚好覆盖界面上会出现的字"，而字又散落在菜单、自定义面板、
 * 纪录窗、帮助、关于里。如果文案和字形清单各写一份，早晚会出现"某个字没被
 * 生成进图集，装到手机上变成方块"这种事。
 *
 * 所以：文案写在这里 → 生成 src/ui_strings.zig（给 C 用，C11 不便声明 UTF-8 字面量）
 *      → 同一份文案抽出所有字符作为字形清单 → fetch_glyphs.ps1 只位图化这些字。
 * 构建时再回头校验"清单里每个字符都在图集里"，缺一个就让构建失败。
 *
 * 加一条新文案的步骤：只改这个文件，然后重新构建（不需要手改任何图集）。
 */
'use strict';

/* 字号档位：像素高度。菜单/按钮用大号，正文用中号，小字（状态、提示）用小号。
 * 三档都从系统字体（Microsoft YaHei）位图化，1 位色（SingleBitPerPixelGridFit），
 * 这样放大到整数倍时仍然是硬边的像素风，和经典扫雷的观感一致。
 *
 * **分 3 个缩放层，每层 sm/md/lg 三档**（顺序必须与 src/font.h 的
 * FONT_LAYER_COUNT / font_slot() 一致）：
 *   层 0（100%）= 基准，对应 280dpi、单格 77px 的设备
 *   层 1（125%）= 中密度设备
 *   层 2（150%）= 高密度设备（440dpi 左右）
 * 运行时由 ui.c 的 font_scale_pct() 按屏幕密度挑一层。
 *
 * 为什么必须分层：字号是**固定像素高度的光栅图**，不能像矢量字那样任意缩放
 * （缩放就糊了、丢掉像素风）。而不同 dpi 的设备要得到相同**物理**字号，
 * 就需要不同的像素高度 —— 只能预生成几层、运行时挑最接近的一层。
 * 早先只有一层，结果 440dpi 手机上中文只有 1.85mm（280dpi 上是 2.90mm）。 */
const SIZES = [
  // 层 0：基准（名字必须唯一 —— 图集/覆盖率检查是按名字查表的，
  // 三层都叫 sm/md/lg 会让后两层读成 0 个字形，构建时门禁就会失败）
  { name: 'sm0', px: 21 },
  { name: 'md0', px: 29 },
  { name: 'lg0', px: 39 },
  // 层 1：125%
  { name: 'sm1', px: 26 },
  { name: 'md1', px: 36 },
  { name: 'lg1', px: 49 },
  // 层 2：150%
  { name: 'sm2', px: 32 },
  { name: 'md2', px: 44 },
  { name: 'lg2', px: 59 },
];

/* 界面文案。键名会被转成 Zig 常量名（UI_<KEY>） */
const STRINGS = {
  APP_TITLE: '复扫雷 Complexweeper',
  APP_VERSION: '1.0.13',

  /* 顶部菜单栏 */
  MENU_GAME: '游戏',
  MENU_HELP: '帮助',
  MENU_NEW: '新局',
  MENU_BEGINNER: '初级',
  MENU_INTERMEDIATE: '中级',
  MENU_EXPERT: '高级',
  MENU_CUSTOM: '自定义',
  MENU_BEST: '纪录',
  MENU_ZOOM: '缩放',
  MENU_ABOUT: '关于',
  MENU_RULES: '玩法',

  /* 顶部菜单展开后的条目（缩进一层，用同一个大字号） */
  ITEM_ZOOM_1: '1×',
  ITEM_ZOOM_2: '2×',
  ITEM_ZOOM_3: '3×',

  /* 自定义棋盘面板 */
  CUSTOM_TITLE: '自定义棋盘',
  CUSTOM_HEIGHT: '高度(H)',
  CUSTOM_WIDTH: '宽度(W)',
  CUSTOM_RANGE_H: '9 – 30 行',
  CUSTOM_RANGE_W: '9 – 40 列',
  CUSTOM_T1: '正实雷',
  CUSTOM_T2: '负实雷',
  CUSTOM_T3: '正虚雷',
  CUSTOM_T4: '负虚雷',
  CUSTOM_SPLIT: '按合计均分',
  CUSTOM_OK: '确定',
  CUSTOM_CANCEL: '取消',
  /* 自定义面板的加减按钮：安卓侧没有软键盘（hasCode=false 的 NativeActivity），
   * 所以数值不能靠"点输入框打字"，只能用按钮增减。 */
  ARROW_UP: '▲',
  ARROW_DOWN: '▼',
  ERR_HEIGHT: '高度要在 9 – 30 之间。',
  ERR_WIDTH: '宽度要在 9 – 40 之间。',
  ERR_SUM_ZERO: '四种雷合计至少 1 颗。',
  ERR_SUM_BIG: '合计超过上限（格数 − 9）。',

  /* 纪录窗 */
  BEST_TITLE: '最高分纪录',
  BEST_NEW: '新纪录！',
  BEST_L1: '初级',
  BEST_L2: '中级',
  BEST_L3: '高级',
  BEST_SECONDS: '秒',
  BEST_NONE: '———',

  /* 帮助 */
  HELP_TITLE: '玩法',
  HELP_L1: '雷区里有四种雷，分别是正实雷、负实雷、正虚雷、负虚雷。',
  HELP_L2: '一个格子上的数字是它周围所有雷之和的模长。',
  HELP_L3: '点一下可以循环插旗，长按翻开格子，双击数字格可以展开周围。',
  HELP_L4: '数字格周围的旗子插对之后，双击才会展开；插错则没有反应。',
  HELP_L5: '翻开所有非雷的格子就赢了，不需要把所有雷都插上旗。',

  /* 关于 */
  ABOUT_TITLE: '关于',
  ABOUT_L1: '复扫雷 Complexweeper',
  ABOUT_L2: '扫雷，但雷是复数。',
  ABOUT_L3: '原生 Windows 版用 Zig + Win32 写成，这是它的安卓版。',
  ABOUT_L4: '图像素材：Microsoft（原版扫雷）与青月晓（新增部分）。',
  ABOUT_L5: '本程序与 Microsoft 公司无隶属关系。',
  ABOUT_L6: '代码按 GPL-3.0 授权。',

  /* 界面上的杂项 */
  MISC_BOOM: '踩雷了',
  MISC_WIN: '通关！',
  MISC_PAUSED: '已暂停',
  MISC_ZOOM: '缩放',
};

/* 只有帮助与关于会被分页/滚动显示，这里给它们一个换行用的段落列表 */
const PARAGRAPHS = {
  HELP: ['HELP_L1', 'HELP_L2', 'HELP_L3', 'HELP_L4', 'HELP_L5'],
  ABOUT: ['ABOUT_L1', 'ABOUT_L2', 'ABOUT_L3', 'ABOUT_L4', 'ABOUT_L5', 'ABOUT_L6'],
};

/* 收集所有需要字形化的字符：STRINGS 的值 + PRESET_LABELS。
 * 注意也要收 ASCII 全集 —— 计时器、计雷器、版本号、边框上的数字都会用到。 */
function collectGlyphs() {
  const chars = new Set();
  for (const v of Object.values(STRINGS)) {
    for (const ch of v) chars.add(ch);
  }
  // 数字与常用 ASCII 一律备齐（版本号、秒数、判据调试文本都可能出现）
  for (let c = 0x20; c <= 0x7e; c++) chars.add(String.fromCharCode(c));
  return chars;
}

module.exports = { SIZES, STRINGS, PARAGRAPHS, collectGlyphs };
