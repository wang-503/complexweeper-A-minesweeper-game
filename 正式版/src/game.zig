// 复扫雷 · 规则与状态（Windows 版的入口）。
//
// 真正的实现在 rules.zig 里 —— 那个文件刻意不碰 libc，好让安卓版也能把同一份
// 规则编进 APK。这里只做一层再导出：主程序照旧 `@import("game.zig")`，
// `g.Game` / `g.PRESETS` / `g.splitEvenly` 这些名字和在 rules.zig 里写的完全一样，
// 所以对 main.zig、selftest.zig、uitest.zig 来说这次拆分是透明的。
pub usingnamespace @import("rules.zig");
