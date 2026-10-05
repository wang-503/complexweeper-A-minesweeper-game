// 宿主机预览与自检入口（Windows 控制台程序，用 zig cc 编）。
//
// 这个程序的用处是：**不碰安卓也能把界面看一遍、把规则跑一遍**。
// 它链接的是与 APK 完全相同的那几份 C 源码（game.c / render.c / font.c / ui.c），
// 所以在这里能出图、能过自检，安卓侧就是同一套逻辑在跑。
//
//   --selftest <报告>        规则自检（对照 正式版/src/selftest.zig）
//   --uitest  <报告>         界面与触屏手势自检
//   --shot    <PNG>          渲染一张 PNG 用于人工核对观感
//   --rules-dump <文件>      写出局面指纹，供 tools/parity_check.js 与 Windows 版对拍
//   --scale <n>              预览缩放倍率（默认 2，只为看清楚，不影响布局）
//   --demo <mid|lose|win|custom|flags>   构造一个可复现的局面
//   --zoom <1|2>            指定 HUD 缩放档（单格尺寸由物理尺寸定）
//   --list-demos             列出可用的 demo 名
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "font.h"
#include "game.h"
#include "render.h"
#include "selftest.h"
#include "strbuf.h"
#include "ui.h"

#include "font_meta.h"
#include "frame_buf.h"
#include "ui_strings.h"

// 图集与字形图集直接嵌进可执行文件：这样交叉验证时用的就是真正打进 APK 的那份数据，
// 不会出现"预览用的图和 APK 里的图不是一份"这种事。
static const unsigned char g_atlas[] = {
#include "atlas_blob.inc"
};
static const unsigned char g_font[] = {
#include "font_blob.inc"
};

static uint32_t now_ms(void) {
    return (uint32_t)((uint64_t)clock() * 1000ull / (uint64_t)CLOCKS_PER_SEC);
}

// ---------------------------------------------------------------- 自检
static uint32_t run_selftest(const char *path) {
    static char buf[256 * 1024];
    StrBuf sb;
    sb_init(&sb, buf, sizeof buf);
    const uint32_t fails = selftest_run(&sb, CS_APP_VERSION_STR);
    FILE *f = fopen(path, "wb");
    if (!f) return 2;
    fwrite(sb.buf, 1, sb.len, f);
    fclose(f);
    return fails;
}

// 界面自检：把布局不变量、格贴图选择、触屏手势状态机都过一遍。
// 这一套是 Windows 版 uitest.zig 的对应物，只是手势从鼠标换成了触屏。
// 返回失败项数（0 = 全过）。
static int32_t ui_uitest(StrBuf *sb);

static uint32_t run_uitest(const char *path) {
    static char buf[256 * 1024];
    StrBuf sb;
    sb_init(&sb, buf, sizeof buf);
    const int32_t fails = ui_uitest(&sb);
    FILE *f = fopen(path, "wb");
    if (!f) return 2;
    fwrite(sb.buf, 1, sb.len, f);
    fclose(f);
    return (fails == 0) ? 0u : 1u;
}

// ---------------------------------------------------------------- 局面构造
// 与 Windows 版 main.zig 的 setupDemo 保持同一套种子与开局格，方便两边对着看。
enum { DEMO_NONE = 0, DEMO_MID, DEMO_LOSE, DEMO_WIN, DEMO_CUSTOM, DEMO_FLAGS, DEMO_OPEN };

static void setup_demo(Ui *ui, int demo) {
    Game *g = &ui->game;
    switch (demo) {
        case DEMO_CUSTOM:
            g->w = 12;
            g->h = 12;
            g->mines = 14;
            for (size_t t = 0; t < 5; t++) g->type_count[t] = 0;
            g->type_count[3] = 7;
            g->type_count[4] = 7;
            game_new_game(g, 20260101);
            game_start_at(g, 6 * 12 + 6, 0);
            g->elapsed_ms = 42000;
            return;
        case DEMO_FLAGS:
            g->w = 16;
            g->h = 16;
            g->mines = 40;
            for (size_t t = 0; t < 5; t++) g->type_count[t] = 0;
            game_new_game(g, 20260101);
            game_start_at(g, 8 * 16 + 8, 0);
            g->elapsed_ms = 83000;
            {
                uint32_t flagged = 0;
                for (size_t i = 0; i < g->n && flagged < 12; i++) {
                    if (g->open[i] != 0) continue;
                    (void)game_set_flag(g, i, (uint8_t)(1 + (flagged % 4)));
                    flagged += 1;
                }
            }
            return;
        default: break;
    }
    g->w = 16;
    g->h = 16;
    g->mines = 40;
    for (size_t t = 0; t < 5; t++) g->type_count[t] = 0;
    game_new_game(g, 20260101);
    game_start_at(g, 8 * 16 + 8, 0);
    g->elapsed_ms = 83000;

    if (demo == DEMO_MID || demo == DEMO_OPEN) {
        uint32_t opened = 0;
        for (size_t i = 0; i < g->n && opened < 14; i++) {
            if (g->mine[i] != 0 || g->open[i] != 0) continue;
            game_reveal(g, i, 0);
            opened += 1;
        }
        uint32_t flagged = 0;
        for (size_t i = 0; i < g->n && flagged < 6; i++) {
            if (g->open[i] != 0) continue;
            (void)game_set_flag(g, i, (uint8_t)(1 + (flagged % 4)));
            flagged += 1;
        }
    }
    else if (demo == DEMO_WIN) {
        for (size_t i = 0; i < g->n; i++) {
            if (g->mine[i] == 0 && g->open[i] == 0) game_reveal(g, i, 0);
        }
    }
    else if (demo == DEMO_LOSE) {
        uint32_t flagged = 0;
        for (size_t i = 0; i < g->n && flagged < 4; i++) {
            if (g->open[i] != 0 || g->mine[i] != 0) continue;
            (void)game_set_flag(g, i, (uint8_t)(1 + (flagged % 4)));
            flagged += 1;
        }
        for (size_t i = 0; i < g->n; i++) {
            if (g->mine[i] == 2) {
                game_reveal(g, i, 0);
                break;
            }
        }
    }
}

static int demo_from_name(const char *s) {
    if (!strcmp(s, "mid")) return DEMO_MID;
    if (!strcmp(s, "lose")) return DEMO_LOSE;
    if (!strcmp(s, "win")) return DEMO_WIN;
    if (!strcmp(s, "custom")) return DEMO_CUSTOM;
    if (!strcmp(s, "flags")) return DEMO_FLAGS;
    if (!strcmp(s, "open")) return DEMO_OPEN;
    return DEMO_NONE;
}

// ---------------------------------------------------------------- 截图
// 预览用的帧缓冲与输出缓冲。放在文件作用域，别压在栈上。
// 尺寸按"专家盘在缩放 3"的最坏布局留（约 2000×3300），再乘预览放大倍率。
#define SHOT_MAX_W 4096
#define SHOT_MAX_H 4096
static uint32_t g_shot_fb[SHOT_MAX_W * SHOT_MAX_H];
static uint32_t g_shot_out[SHOT_MAX_W * SHOT_MAX_H];

// ---------------------------------------------------------------- 宿主机测试用的窗口参数
// 这些值刻意与 MuMu 真机一致（1920×1080，280dpi → 单格 77px），
// 这样自检里的几何断言与真机看到的是同一套数字，不会出现"测试过了真机不对"。
#define HOST_WIN_W 1920
#define HOST_WIN_H 1080
#define HOST_DPI 280
#define HOST_CELL_PX 77      // 0.7cm @ 280dpi

// 初始化一个与真机同参的界面（窗口尺寸 + 单格尺寸都要显式设置）
static void host_ui_init(Ui *ui, int32_t preset, int32_t win_w, int32_t win_h, int32_t cell_px) {
    ui_init(ui, win_w, win_h);
    if (cell_px > 0) ui_set_cell_px(ui, cell_px);
    if (preset >= 0) ui_cmd_preset(ui, preset);
}

// 棋盘格号 → 屏幕坐标中心。**必须走 pan/vis 这一套**：
// 棋盘比视口大时格子不在固定位置，直接拿 board_x 算会算到屏幕外。
static void host_cell_center(const Ui *ui, int32_t cell_idx, int32_t *ox, int32_t *oy) {
    const UiLayout L = ui_layout(ui);
    const int32_t cols = (int32_t)ui->game.w;
    const int32_t col = cell_idx % cols;
    const int32_t row = cell_idx / cols;
    *ox = L.vis_x + col * L.cell - L.pan_x + L.cell / 2;
    *oy = L.vis_y + row * L.cell - L.pan_y + L.cell / 2;
}

static bool render_shot(const char *path, int demo, int zoom, int scale, const char *overlay,
                        int32_t pan_x, int32_t pan_y) {
    Ui ui;
    host_ui_init(&ui, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
    // --zoom 现在只影响 HUD 缩放（单格尺寸由物理尺寸定，不再有档位）
    if (zoom > 0) ui.hud_scale = zoom;
    if (demo != DEMO_NONE) setup_demo(&ui, demo);
    ui_refresh_scale(&ui);
    // --pan 用 -1 表示"不指定"。**必须逐轴判断**，而且必须走 ui_pan_begin/to/end：
    // 直接写 ui.pan_x 会被 ui_refresh_scale 按"当前 pan"归一化（等于没设）。
    // 早先这里写成 `if (pan_x >= 0 || pan_y >= 0)`（pan_y 默认 -1，条件恒真）
    // 加上 `pan_x > 0 ? pan_x : ui.pan_x`（把 0 当"没给"），
    // 结果 --pan 从来没生效过 —— 每次比对的其实都是同一张 pan=0 的图。
    if (pan_x > 0 || pan_y > 0) {
        const int32_t sx = 10000, sy = 5000;
        ui_pan_begin(&ui, sx, sy);
        ui_pan_to(&ui, sx - (pan_x > 0 ? pan_x : 0), sy - (pan_y > 0 ? pan_y : 0));
        ui_pan_end(&ui);
    }
    if (overlay) {
        if (!strcmp(overlay, "help")) ui.overlay = UI_OVERLAY_HELP;
        else if (!strcmp(overlay, "about")) ui.overlay = UI_OVERLAY_ABOUT;
        else if (!strcmp(overlay, "best")) ui.overlay = UI_OVERLAY_BEST;
        else if (!strcmp(overlay, "custom")) ui_cmd_open_custom(&ui);
        else if (!strcmp(overlay, "menu")) ui.open_menu = UI_MENU_GAME;
    }
    const UiLayout L = ui_layout(&ui);

    // 画到一块与窗口等大的帧缓冲，再按整数倍最近邻放大到输出尺寸：
    // 这样预览图看起来和手机上的效果一致（手机上就是 1:1）。
    uint32_t *fb = g_shot_fb;
    if (L.win_w > SHOT_MAX_W || L.win_h > SHOT_MAX_H) {
        fprintf(stderr, "窗口 %dx%d 超出预览帧缓冲上限 %dx%d\n",
                L.win_w, L.win_h, SHOT_MAX_W, SHOT_MAX_H);
        return false;
    }

    Font font;
    Render r;
    render_init(&r, fb, L.win_w, L.win_h);
    render_load_atlas(&r, g_atlas, sizeof g_atlas);
    render_load_font(&r, g_font, sizeof g_font);
    font_init(&font, &r);
    r.pixels = fb;
    r.w = L.win_w;
    r.h = L.win_h;
    ui.now_hint = 1;
    ui_tick(&ui, 1);          // 让计时与手势状态稳定
    ui_paint(&r, &ui, &font, g_atlas, sizeof g_atlas, g_font, sizeof g_font);

    const int32_t ow = L.win_w * scale;
    const int32_t oh = L.win_h * scale;
    if (ow > SHOT_MAX_W || oh > SHOT_MAX_H) {
        fprintf(stderr, "输出 %dx%d 超出 %dx%d 上限\n", ow, oh, SHOT_MAX_W, SHOT_MAX_H);
        return false;
    }
    uint32_t *pixels = g_shot_out;
    for (int32_t y = 0; y < oh; y++) {
        const uint32_t *src = fb + (size_t)(y / scale) * (size_t)L.win_w;
        uint32_t *dst = pixels + (size_t)y * (size_t)ow;
        for (int32_t x = 0; x < ow; x++) dst[x] = src[x / scale];
    }
    const bool ok = render_write_png(path, pixels, ow, oh);
    if (ok) {
        printf("截图 %s  %d×%d（窗口 %d×%d，单格 %dpx，HUD %d×，可见 %d×%d 格，pan=(%d,%d)）\n",
               path, ow, oh, L.win_w, L.win_h, L.cell, L.hud, L.vis_cols, L.vis_rows,
               L.pan_x, L.pan_y);
    }
    return ok;
}

// ---------------------------------------------------------------- 规则指纹
// 与 Windows 版 main.zig 新增的 --rules-dump 输出同样的文本，逐字对比即可发现
// 两端规则实现飘了。这里刻意只用规则层，不碰界面。
static const uint32_t DUMP_SEEDS[] = {
    1, 2, 3, 12345, 999, 20260101, 4111, 4127, 4133, 4139, 4153, 4159, 7777, 31337,
};
static const struct { uint16_t w, h, m; uint16_t tc[5]; } DUMP_SHAPES[] = {
    { 9, 9, 10, { 0, 0, 0, 0, 0 } },
    { 16, 16, 40, { 0, 0, 0, 0, 0 } },
    { 30, 16, 99, { 0, 0, 0, 0, 0 } },
    { 40, 30, 60, { 0, 0, 0, 0, 0 } },
    { 12, 12, 14, { 0, 0, 7, 0, 7 } },
    { 12, 12, 10, { 0, 3, 2, 4, 1 } },
    { 20, 20, 40, { 0, 10, 10, 10, 10 } },
};
#define DUMP_SHAPE_COUNT (sizeof(DUMP_SHAPES) / sizeof(DUMP_SHAPES[0]))
#define DUMP_SEED_COUNT (sizeof(DUMP_SEEDS) / sizeof(DUMP_SEEDS[0]))

// 逐格签名：与 Windows 版 dumpBoardSignature 完全等价（格式必须一致，否则对拍会
// 因为格式差异误报）。"雷类型/显示值/状态"，状态 1 = 已翻开，2 = 插了旗。
static void dump_board_signature(const Game *g, StrBuf *sb) {
    for (size_t i = 0; i < g->n; i++) {
        if (i != 0) {
            if (i % g->w == 0) sb_addf(sb, "\n");
            else sb_addf(sb, " ");
        }
        const unsigned st = (g->open[i] != 0 ? 1u : 0u) + (g->flag[i] != 0 ? 2u : 0u);
        sb_addf(sb, "%u/%d/%u", (unsigned)g->mine[i], (int)g->clue[i], st);
    }
    sb_addf(sb, "\n");
}

// 一个局面指纹：把棋盘内容压成一句可比较的文本。
// 哈希必须与 Windows 版 dumpBoard 的 FNV-1a 逐字节一致（同样是小端逐字节处理），
// 否则对拍只会在这一列上误报 —— 真正要比的是下面那行逐格签名。
static void dump_board(const Game *g, StrBuf *sb) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < g->n; i++) {
        h ^= (uint32_t)g->mine[i];
        h *= 16777619u;
        h ^= (uint32_t)(uint8_t)((uint16_t)g->clue[i] & 0xFF);
        h *= 16777619u;
        h ^= (uint32_t)(uint8_t)(((uint16_t)g->clue[i] >> 8) & 0xFF);
        h *= 16777619u;
        h ^= (uint32_t)(g->open[i] | (g->flag[i] << 2));
        h *= 16777619u;
    }
    sb_addf(sb, "  board %ux%u m=%u tc=%u/%u/%u/%u start=%d opened=%u mines=%u fnv=%08x\n",
            g->w, g->h, g->mines, g->type_count[1], g->type_count[2], g->type_count[3], g->type_count[4],
            g->start_cell, (unsigned)game_opened_count(g), g->mines, h);
    dump_board_signature(g, sb);
}

static uint32_t run_rules_dump(const char *path) {
    // 缓冲区要够大：一开始给 512KB，指纹正好写不下被截断，parity_check 报
    // "不一致 3855 行"，看起来像规则飘了，其实只是报告没写完。
    static char buf[8 * 1024 * 1024];
    StrBuf sb;
    sb_init(&sb, buf, sizeof buf);
    sb_addf(&sb, "复扫雷 %s · 规则指纹\n", CS_APP_VERSION_STR);
    sb_addf(&sb, "==========================\n");

    static Game g;
    for (size_t si = 0; si < DUMP_SHAPE_COUNT; si++) {
        const uint16_t *tc = DUMP_SHAPES[si].tc;
        uint16_t sum = 0;
        for (size_t t = 1; t < 5; t++) sum += tc[t];
        const uint16_t mines = (sum > 0) ? sum : DUMP_SHAPES[si].m;
        for (size_t di = 0; di < DUMP_SEED_COUNT; di++) {
            // 角、上边中点、正中、右下角：覆盖不同的扩散顺序
            const size_t starts[4] = {
                0,
                DUMP_SHAPES[si].w / 2,
                (size_t)(DUMP_SHAPES[si].h / 2) * DUMP_SHAPES[si].w + DUMP_SHAPES[si].w / 2,
                (size_t)DUMP_SHAPES[si].w * DUMP_SHAPES[si].h - 1,
            };
            for (size_t ti = 0; ti < 4; ti++) {
                memset(&g, 0, sizeof g);
                g.w = DUMP_SHAPES[si].w;
                g.h = DUMP_SHAPES[si].h;
                g.mines = mines;
                for (size_t t = 0; t < 5; t++) g.type_count[t] = tc[t];
                game_new_game(&g, DUMP_SEEDS[di]);
                game_start_at(&g, starts[ti], 0);
                sb_addf(&sb, "shape=%u seed=%u start=%u\n", (unsigned)si, DUMP_SEEDS[di], (unsigned)starts[ti]);
                dump_board(&g, &sb);
            }
        }
    }

    // 再来一段操作序列的指纹：插旗 → 撤旗 → 展开 → 踩雷，确认操作语义也一致
    {
        memset(&g, 0, sizeof g);
        g.w = 12;
        g.h = 12;
        g.mines = 24;
        for (size_t t = 0; t < 5; t++) g.type_count[t] = 0;
        game_new_game(&g, 4242);
        game_start_at(&g, 70, 0);
        sb_addf(&sb, "ops seed=4242\n");
        for (size_t i = 0; i < g.n; i++) {
            if (g.open[i] == 0) {
                (void)game_cycle_flag(&g, i);
                (void)game_cycle_flag(&g, i);
            }
        }
        dump_board(&g, &sb);
        for (size_t i = 0; i < g.n; i++) (void)game_set_flag(&g, i, 0);
        dump_board(&g, &sb);
        for (size_t i = 0; i < g.n; i++) {
            if (g.open[i] != 0) game_try_expand(&g, i);
        }
        dump_board(&g, &sb);
    }

    sb_addf(&sb, "dump_truncated=%d\n", sb.truncated);
    FILE *f = fopen(path, "wb");
    if (!f) return 2;
    fwrite(sb.buf, 1, sb.len, f);
    fclose(f);
    printf("规则指纹写出 %s（%u 字节）\n", path, (unsigned)sb.len);
    return 0;
}

// ---------------------------------------------------------------- 界面自检
static int32_t g_ui_checks = 0;
static int32_t g_ui_fails = 0;
static void uexpect(StrBuf *sb, bool cond, const char *name) {
    g_ui_checks += 1;
    if (!cond) {
        g_ui_fails += 1;
        sb_addf(sb, "  [失败] %s\n", name);
    }
}

static int32_t ui_uitest(StrBuf *sb) {
    g_ui_checks = 0;
    g_ui_fails = 0;
    static uint32_t fb[1800 * 1200];
    static Game g_backup;

    sb_addf(sb, "复扫雷 %s · 界面与触屏自检（安卓版）\n==========================\n", CS_APP_VERSION_STR);

    // ---- 字号与字形覆盖率：文案里每个字符都必须在图集里 ----
    {
        Ui ui;
        Font font;
        Render r;
        ui_init(&ui, 2400, 1600);
        render_init(&r, fb, 1200, 800);
        render_load_font(&r, g_font, sizeof g_font);
        font_init(&font, &r);
        int32_t missing_total = 0;
        char missing[256];
        missing[0] = 0;
        static char all_missing[4096];
        all_missing[0] = 0;
        for (int32_t i = 0; i < UI_STRING_COUNT; i++) {
            char tmp[256];
            tmp[0] = 0;
            const int32_t n = font_check_coverage(&font, ui_font_md(&ui), (const char *)ui_strings[i].bytes,
                                                  tmp, sizeof tmp);
            if (n > 0) {
                missing_total += n;
                strncat(all_missing, tmp, sizeof all_missing - strlen(all_missing) - 1);
            }
        }
        uexpect(sb, missing_total == 0, "界面文案里不能有字形图集没覆盖的字符");
        sb_addf(sb, "0 字形覆盖率：%s（缺 %d 个）\n", missing_total == 0 ? "通过" : all_missing, missing_total);
        uexpect(sb, font_init(&font, &r), "字形图集能正确解析");
        uexpect(sb, font.pages[0] != NULL, "字形图集至少有一页");
        uexpect(sb, font.page_w == FONT_PAGE_W && font.page_h == FONT_PAGE_H, "字形图集尺寸与 font_meta.h 一致");
        (void)missing;
    }

    // ---- 布局不变量（左侧信息栏 + 右侧棋盘） ----
    {
        Ui ui;
        host_ui_init(&ui, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        for (int32_t hi = 1; hi <= 2; hi++) {
            Ui probe = ui;
            probe.hud_scale = hi;
            const UiLayout L = ui_layout(&probe);
            char tag[128];

            // 单格尺寸**与 HUD 缩放无关**：在 hud 2..5 这个范围里，单格必须完全一样。
            // 注意 hud 会改变落格区大小（菜单栏/边框变粗），极端 hud=1 时
            // "整盘入屏"的收缩规则可以合法地算出不同值，所以只比 2..5。
            //
            // 区间要容纳 **±2px 的单格微调**：为了让"整格视口 + 一格滚动条"正好铺满、
            // 消掉格子与滚动条之间的灰缝，ui_layout 会在目标值附近挑一个最整齐的单格。
            snprintf(tag, sizeof tag, "单格尺寸不被 HUD 缩放影响（hud=%d 在合理区间内）", hi);
            uexpect(sb, L.cell >= HOST_CELL_PX * 60 / 100 && L.cell <= HOST_CELL_PX + 2, tag);
            {
                Ui probe2 = ui;
                probe2.hud_scale = 5;
                const UiLayout L5 = ui_layout(&probe2);
                Ui probe3 = ui;
                probe3.hud_scale = 2;
                const UiLayout L2b = ui_layout(&probe3);
                // 允许 ±2px：单格会在目标值附近做"最整齐"的微调（消灰缝），
                // 而 hud 会改变外框厚度 → 可用空间不同 → 微调结果可能差 1~2px。
                // 这里要防的是"单格跟着 hud 乱变"这种真 bug，不是这 2px。
                const int32_t d = L5.cell - L2b.cell;
                snprintf(tag, sizeof tag, "hud 变化时单格基本不变（hud=%d：%d vs %d）",
                         hi, L5.cell, L2b.cell);
                uexpect(sb, d >= -2 && d <= 2, tag);
            }
            uexpect(sb, L.win_w > 0 && L.win_h > 0, "窗口尺寸为正");

            // 侧栏宽度 = 窗口长边 / 5
            const int32_t long_edge = L.win_w > L.win_h ? L.win_w : L.win_h;
            snprintf(tag, sizeof tag, "侧栏宽 = 长边/5（hud=%d：%d）", hi, L.side_w);
            uexpect(sb, L.side_w == long_edge / 5, tag);
            snprintf(tag, sizeof tag, "棋盘区 = 窗口减侧栏（hud=%d）", hi);
            uexpect(sb, L.board_x == L.side_w && L.board_w == L.win_w - L.side_w, tag);
            uexpect(sb, L.side_y == L.menubar_h, "侧栏在菜单栏下面");
            // 菜单栏只占侧栏那一条，所以棋盘**从窗口最上沿开始、满高**。
            // （原来菜单栏横跨全宽，棋盘只能从 menubar_h 开始，上方那片空白是浪费的。）
            uexpect(sb, L.board_y == 0 && L.board_h == L.win_h, "棋盘满高（顶部空白并进地图）");
            uexpect(sb, L.menubar_h > 0 && L.menubar_h < L.win_h, "菜单栏高度合理");
            uexpect(sb, L.side_x == 0, "侧栏贴左边界");
            uexpect(sb, L.board_x + L.board_w == L.win_w, "棋盘贴右边界");
            uexpect(sb, L.board_y + L.board_h == L.win_h, "棋盘贴下边界");

            // 可见格：正整数、且是完整格（不能有半格）。
            // 注意 field_w/field_h 现在表示**滚动条带的长度**（不是视口容量），
            // 所以按"视口 + 条带 + 外框 ≤ 棋盘区"来断言。
            snprintf(tag, sizeof tag, "可见格数为正（hud=%d）", hi);
            uexpect(sb, L.vis_cols >= 1 && L.vis_rows >= 1, tag);
            snprintf(tag, sizeof tag, "可见格是完整格（hud=%d：%d 列 + 条带 %d）",
                     hi, L.vis_cols, L.sb_w);
            uexpect(sb, L.vis_cols * L.cell + L.sb_w + 2 * L.box <= L.board_w &&
                        L.vis_rows * L.cell + L.sb_h + 2 * L.box <= L.board_h, tag);

            // 侧栏里所有部件都要装得下
            UiRect fr;
            ui_face_rect(&probe, &fr);
            snprintf(tag, sizeof tag, "笑脸在侧栏内（hud=%d）", hi);
            uexpect(sb, fr.x >= L.side_x && fr.x + fr.w <= L.side_x + L.side_w, tag);
            uexpect(sb, fr.y >= L.side_y && fr.y + fr.h <= L.side_y + L.side_h, tag);
            uexpect(sb, fr.w == fr.h && fr.w > 0, "笑脸是正方形");
            for (int32_t t = 1; t < 5; t++) {
                UiRect cr;
                ui_counter_rect(&probe, t, &cr);
                snprintf(tag, sizeof tag, "计雷器 %d 在侧栏内且不压笑脸（hud=%d）", t, hi);
                uexpect(sb, cr.x >= L.side_x && cr.x + cr.w <= L.side_x + L.side_w, tag);
                uexpect(sb, cr.y >= fr.y + fr.h, tag);
                uexpect(sb, cr.y + cr.h <= L.side_y + L.side_h, tag);
                if (t > 1) {
                    UiRect prev;
                    ui_counter_rect(&probe, t - 1, &prev);
                    snprintf(tag, sizeof tag, "计雷器 %d 不与上一块重叠（hud=%d）", t, hi);
                    uexpect(sb, cr.y >= prev.y + prev.h, tag);
                }
            }
            UiRect tr;
            ui_timer_rect(&probe, &tr);
            UiRect last;
            ui_counter_rect(&probe, 4, &last);
            snprintf(tag, sizeof tag, "计时器在四个计雷器下面且在侧栏内（hud=%d）", hi);
            uexpect(sb, tr.y >= last.y + last.h, tag);
            uexpect(sb, tr.x >= L.side_x && tr.x + tr.w <= L.side_x + L.side_w, tag);
            uexpect(sb, tr.y + tr.h <= L.side_y + L.side_h, tag);
            // 四块计雷器等宽（实雷四格数字、虚雷三格 + i 单位）
            for (int32_t t = 1; t <= 4; t++) {
                UiRect cr;
                ui_counter_rect(&probe, t, &cr);
                snprintf(tag, sizeof tag, "计雷器 %d 与 ui_counter_width 一致（hud=%d）", t, hi);
                uexpect(sb, cr.w == ui_counter_width(&probe, t), tag);
            }

            // 菜单栏按钮：能放下两倍宽、文字不被裁
            UiRect br;
            ui_menubar_rect(&probe, 0, &br);
            UiRect br1;
            ui_menubar_rect(&probe, 1, &br1);
            snprintf(tag, sizeof tag, "两个菜单按钮不重叠且在窗口内（hud=%d）", hi);
            uexpect(sb, br1.x >= br.x + br.w, tag);
            uexpect(sb, br1.x + br1.w <= L.win_w, tag);
            uexpect(sb, br.y + br.h <= L.menubar_h, "菜单按钮在菜单栏内");
            for (int32_t menu = 0; menu < 2; menu++) {
                ui_menubar_rect(&probe, menu, &br);
                const char *label = (const char *)ui_strings[menu == 0 ? UI_S_MENU_GAME : UI_S_MENU_HELP].bytes;
                const int32_t tw = font_text_width(NULL, label, ui_font_md(&ui));
                // 文字要能装进按钮，并左右各留出至少 2px 余量
                // （字号调大后这里会先失败，正好当"字号与按钮宽度没配套"的哨兵）
                uexpect(sb, tw <= br.w - 4, menu == 0 ? "「游戏」两个字装得进按钮" : "「帮助」两个字装得进按钮");
                const int32_t ty = font_center_y_for_text(label, ui_font_md(&ui), br.y, br.h);
                int32_t itop = 0, ibot = 0;
                const bool has_ink = font_text_ink_bounds(label, ui_font_md(&ui), &itop, &ibot);
                uexpect(sb, has_ink, "菜单文字有墨水");
                uexpect(sb, ty + itop >= br.y && ty + ibot <= br.y + br.h,
                        menu == 0 ? "「游戏」两个字纵向完整落在按钮内" : "「帮助」两个字纵向完整落在按钮内");
            }
        }
        sb_addf(sb, "1 布局不变量（侧栏 + 棋盘）：通过\n");
    }

    // ---- 换难度：棋盘尺寸变了，拖动偏移要被重新钳制 ----
    {
        Ui ui;
        host_ui_init(&ui, 0, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        uexpect(sb, ui.game.w == 9 && ui.game.h == 9 && ui.game.mines == 10, "初级盘参数正确");
        ui_cmd_preset(&ui, 2);
        uexpect(sb, ui.game.w == 30 && ui.game.h == 16 && ui.game.mines == 99, "高级盘参数正确");
        // 换难度后 pan 必须仍被钳在合法范围
        const UiLayout L2 = ui_layout(&ui);
        const int32_t span_x = (int32_t)ui.game.w * L2.cell - L2.vis_cols * L2.cell;
        const int32_t span_y = (int32_t)ui.game.h * L2.cell - L2.vis_rows * L2.cell;
        uexpect(sb, ui.pan_x >= 0 && ui.pan_x <= (span_x > 0 ? span_x : 0), "换难度后 pan_x 合法");
        uexpect(sb, ui.pan_y >= 0 && ui.pan_y <= (span_y > 0 ? span_y : 0), "换难度后 pan_y 合法");
        sb_addf(sb, "2 换难度与拖动重置：通过\n");
    }

    // ---- 自定义面板的校验规则 ----
    {
        Ui ui;
        ui_init(&ui, 2400, 1600);
        ui_cmd_open_custom(&ui);
        uexpect(sb, ui.overlay == UI_OVERLAY_CUSTOM, "能打开自定义面板");
        ui.dlg_h = 8;
        ui_cmd_apply_custom(&ui);
        uexpect(sb, ui.dlg_err == 1, "高度 8 应报错（下限 9）");
        ui.dlg_h = 31;
        ui_cmd_apply_custom(&ui);
        uexpect(sb, ui.dlg_err == 1, "高度 31 应报错（上限 30）");
        ui.dlg_h = 16;
        ui.dlg_w = 8;
        ui_cmd_apply_custom(&ui);
        uexpect(sb, ui.dlg_err == 2, "宽度 8 应报错（下限 9）");
        ui.dlg_w = 41;
        ui_cmd_apply_custom(&ui);
        uexpect(sb, ui.dlg_err == 2, "宽度 41 应报错（上限 40）");
        ui.dlg_w = 16;
        ui.dlg_t1 = ui.dlg_t2 = ui.dlg_t3 = ui.dlg_t4 = 0;
        ui_cmd_apply_custom(&ui);
        uexpect(sb, ui.dlg_err == 3, "合计 0 颗应报错");
        ui.dlg_t1 = 250;
        ui.dlg_t2 = 250;
        ui_cmd_apply_custom(&ui);
        uexpect(sb, ui.dlg_err == 4, "合计超过 格数−9 应报错");
        // 合法值应该真的生效
        ui.dlg_t1 = 20;
        ui.dlg_t2 = 20;
        ui.dlg_t3 = 3;
        ui.dlg_t4 = 3;
        ui_cmd_apply_custom(&ui);
        uexpect(sb, ui.overlay == UI_OVERLAY_NONE, "合法输入后关闭面板");
        uexpect(sb, ui.game.w == 16 && ui.game.h == 16 && ui.game.mines == 46, "自定义尺寸与雷数生效");
        uexpect(sb, ui.game.type_count[1] == 20 && ui.game.type_count[4] == 3, "自定义配比生效");
        sb_addf(sb, "3 自定义面板校验：通过\n");
    }

    // ---- 判据相关的界面行为 ----
    {
        Ui ui;
        ui_init(&ui, 2400, 1600);
        ui_cmd_preset(&ui, 0);
        ui_cmd_new_game(&ui, 12345, true);
        game_start_at(&ui.game, 40, 0);
        uexpect(sb, ui.game.started && !ui.game.over, "开局后局面进行中");
        sb_addf(sb, "4 换难度与开局：通过\n");
    }

    // ---- 触屏手势状态机 ----
    {
        Ui ui;
        host_ui_init(&ui, 0, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        ui_cmd_new_game(&ui, 12345, true);
        game_start_at(&ui.game, 40, 0);
        // 找一格未翻开、且**在可见视口内**的格子（棋盘比视口大时不能随便挑）
        const UiLayout L = ui_layout(&ui);
        int32_t target = -1;
        for (int32_t vr = 0; vr < L.vis_rows && target < 0; vr++) {
            for (int32_t vc = 0; vc < L.vis_cols; vc++) {
                const int32_t col = vc + (L.pan_x > 0 ? L.pan_x / L.cell : 0);
                const int32_t row = vr + (L.pan_y > 0 ? L.pan_y / L.cell : 0);
                if (col >= (int32_t)ui.game.w || row >= (int32_t)ui.game.h) continue;
                const int32_t i = row * (int32_t)ui.game.w + col;
                if (ui.game.open[i] == 0 && ui.game.flag[i] == 0) { target = i; break; }
            }
        }
        uexpect(sb, target >= 0, "能找到一格未翻开且在视口内的格子");
        int32_t cx = 0, cy = 0;
        host_cell_center(&ui, target, &cx, &cy);
        int32_t hit_idx = -1;
        uexpect(sb, ui_hit_test(&ui, cx, cy, &hit_idx) == UI_HIT_BOARD, "点棋盘被判定为棋盘区域");

        // ---- 单点点按 = 插旗（等窗口过去） ----
        const uint32_t t0 = 1000;
        ui_pointer_down(&ui, cx, cy, t0);
        uexpect(sb, ui.press_cell == target, "按下时记住了那一格（按下预览）");
        ui_pointer_up(&ui, cx, cy, t0 + 50);   // 50ms < 长按阈值
        uexpect(sb, ui.game.flag[target] == 0, "刚松手时旗子还没插（在等双击窗口）");
        ui_tick(&ui, t0 + 50 + UI_TAP_CONFIRM_MS + 1);
        uexpect(sb, ui.game.flag[target] == 1, "单点点按之后插上第 1 种旗");
        uexpect(sb, ui.game.open[target] == 0, "单点点按不翻开格子");

        // 再来四次：旗子应该按 +1 → −1 → +i → −i → 空 循环。
        // 注意每次 tick 的时间必须比"松手时刻"晚过 UI_TAP_CONFIRM_MS —— 早先是
        // 从按下时刻起算的，只晚了 200ms，于是插旗永远不触发（测试自己写错了）。
        for (int32_t k = 2; k <= 5; k++) {
            const uint32_t base = t0 + k * 1000;
            ui_pointer_down(&ui, cx, cy, base);
            ui_pointer_up(&ui, cx, cy, base + 50);
            ui_tick(&ui, base + 50 + UI_TAP_CONFIRM_MS + 1);
            uexpect(sb, ui.game.flag[target] == (uint8_t)(k % 5),
                    k == 5 ? "第五次点按 → 回到无旗" : "点按应让旗子按 1→2→3→4→0 循环");
        }
        sb_addf(sb, "5 单点拨旗循环：通过（1→2→3→4→0）\n");

        // ---- 长按 = 翻开 ----
        {
            Ui u2;
            host_ui_init(&u2, 0, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
            ui_cmd_new_game(&u2, 12345, true);
            game_start_at(&u2.game, 40, 0);
            const UiLayout L2 = ui_layout(&u2);
            int32_t t2 = -1;
            for (int32_t vr = 0; vr < L2.vis_rows && t2 < 0; vr++) {
                for (int32_t vc = 0; vc < L2.vis_cols; vc++) {
                    const int32_t col = vc + (L2.pan_x > 0 ? L2.pan_x / L2.cell : 0);
                    const int32_t row = vr + (L2.pan_y > 0 ? L2.pan_y / L2.cell : 0);
                    if (col >= (int32_t)u2.game.w || row >= (int32_t)u2.game.h) continue;
                    const int32_t i = row * (int32_t)u2.game.w + col;
                    if (u2.game.open[i] == 0 && u2.game.flag[i] == 0) { t2 = i; break; }
                }
            }
            uexpect(sb, t2 >= 0, "长按用例能找到一格未翻开且在视口内的格子");
            int32_t x2 = 0, y2 = 0;
            host_cell_center(&u2, t2, &x2, &y2);
            ui_pointer_down(&u2, x2, y2, 0);
            ui_tick(&u2, UI_LONG_PRESS_MS + 1);
            uexpect(sb, u2.game.open[t2] == 1, "长按之后格子被翻开");
            ui_pointer_up(&u2, x2, y2, UI_LONG_PRESS_MS + 20);
            uexpect(sb, u2.game.flag[t2] == 0, "长按不该插旗（松手后也不再走点按）");
            ui_tick(&u2, UI_LONG_PRESS_MS + 1000);
            uexpect(sb, u2.game.flag[t2] == 0, "长按之后过了点按窗口也不该补插旗");
            sb_addf(sb, "6 长按翻开：通过\n");
        }

        // ---- 双击 = 展开 ----
        {
            Ui u3;
            host_ui_init(&u3, 0, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
            ui_cmd_new_game(&u3, 12345, true);
            game_start_at(&u3.game, 40, 0);
            // 找一格已翻开的数字格，且周围还有没插旗的邻格
            int32_t num = -1;
            for (size_t i = 0; i < u3.game.n; i++) {
                if (u3.game.open[i] == 0 || u3.game.mine[i] != 0) continue;
                size_t buf[8];
                const size_t k = game_nbrs(&u3.game, i, buf);
                bool has_un = false;
                for (size_t x = 0; x < k; x++) {
                    if (u3.game.open[buf[x]] == 0 && u3.game.flag[buf[x]] == 0) has_un = true;
                }
                if (has_un) { num = (int32_t)i; break; }
            }
            uexpect(sb, num >= 0, "能找到一格可展开的数字格");
            int32_t x3 = 0, y3 = 0;
            host_cell_center(&u3, num, &x3, &y3);
            // 先什么都不插：邻域全是未插旗的格，判据必然不通过，双击必须不做任何事。
            // （注意 tryExpand 在"邻域没有可翻的格"时会直接返回、连 judge_fail 都不留，
            //   所以这里必须保证邻域确实还有未插旗的格子。）
            {
                size_t buf[8];
                const size_t k = game_nbrs(&u3.game, (size_t)num, buf);
                for (size_t x = 0; x < k; x++) (void)game_set_flag(&u3.game, buf[x], 0);
                const size_t before_fail = game_opened_count(&u3.game);
                ui_pointer_down(&u3, x3, y3, 100);
                ui_pointer_up(&u3, x3, y3, 140);
                ui_pointer_down(&u3, x3, y3, 200);
                ui_pointer_up(&u3, x3, y3, 240);
                uexpect(sb, game_opened_count(&u3.game) == before_fail,
                        "判据不通过时双击展开不能改变棋盘");
                uexpect(sb, u3.game.flag[num] == 0, "双击数字格不该给数字格本身插旗");
                uexpect(sb, u3.game.msg == GAME_MSG_JUDGE_FAIL, "判据不通过时应留下 judge_fail");
                u3.game.msg = GAME_MSG_NONE;   // 清掉，免得把下一段的结论冲掉
                sb_addf(sb, "7a 判据不通过时不展开：通过（翻开 %u 不变）\n", (unsigned)before_fail);
            }

            // 再把邻域插成正确的旗，双击这次必须真的展开
            {
                size_t buf[8];
                const size_t k = game_nbrs(&u3.game, (size_t)num, buf);
                for (size_t x = 0; x < k; x++) {
                    if (u3.game.open[buf[x]] == 0) {
                        (void)game_set_flag(&u3.game, buf[x], u3.game.mine[buf[x]]);
                    }
                }
                uexpect(sb, game_match_combo_truth(&u3.game, (size_t)num),
                        "把邻域插成正确的旗之后判据应通过");
                const size_t before_ok = game_opened_count(&u3.game);
                ui_pointer_down(&u3, x3, y3, 1000);
                ui_pointer_up(&u3, x3, y3, 1040);
                ui_pointer_down(&u3, x3, y3, 1100);
                ui_pointer_up(&u3, x3, y3, 1140);
                const size_t after_ok = game_opened_count(&u3.game);
                uexpect(sb, after_ok > before_ok, "判据通过时双击必须真的展开");
                sb_addf(sb, "7 双击展开：通过（判据通过后 %u→%u）\n",
                        (unsigned)before_ok, (unsigned)after_ok);
            }
        }

        // ---- 人脸单点重开 ----
        {
            Ui u4;
            host_ui_init(&u4, 0, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
            ui_cmd_new_game(&u4, 12345, true);
            game_start_at(&u4.game, 40, 0);
            uexpect(sb, u4.game.started, "重开前局面已开局");
            UiRect fr;
            ui_face_rect(&u4, &fr);
            const int32_t fx = fr.x + fr.w / 2;
            const int32_t fy = fr.y + fr.h / 2;
            int32_t hi = -1;
            uexpect(sb, ui_hit_test(&u4, fx, fy, &hi) == UI_HIT_FACE, "人脸区域能被命中");
            ui_pointer_down(&u4, fx, fy, 5000);
            uexpect(sb, u4.face_down, "按下人脸时切到按下脸");
            ui_pointer_up(&u4, fx, fy, 5040);
            // 重开 = 回到"未开局"，而且是一个干净的盘
            uexpect(sb, !u4.game.started && !u4.game.over && u4.game.moves == 0,
                    "单点人脸之后回到未开局的新盘");
            sb_addf(sb, "8 人脸重开：通过\n");
        }

        // ---- 菜单栏与下拉 ----
        {
            Ui u5;
            host_ui_init(&u5, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
            int32_t idx = -1;
            UiRect mr;
            ui_menubar_rect(&u5, 0, &mr);
            const int32_t cx5 = mr.x + mr.w / 2;
            const int32_t cy5 = mr.y + mr.h / 2;
            uexpect(sb, ui_hit_test(&u5, cx5, cy5, &idx) == UI_HIT_MENU_ITEM && idx == 0,
                    "菜单栏第一个按钮能被命中");
            ui_pointer_down(&u5, cx5, cy5, 0);
            ui_pointer_up(&u5, cx5, cy5, 30);
            uexpect(sb, u5.open_menu == UI_MENU_GAME, "点一下展开「游戏」菜单");
            uexpect(sb, ui_menu_item_count(UI_MENU_GAME) == 6, "游戏菜单有 6 项（1×/2×/3× 已删）");
            // 下拉里的第一项是"新局"
            UiRect dr;
            ui_dropdown_rect(&u5, &dr);
            const int32_t row = (26 - 4) * u5.hud_scale;
            const int32_t iy = dr.y + 2 * u5.hud_scale + row / 2;
            uexpect(sb, ui_hit_test(&u5, dr.x + dr.w / 2, iy, &idx) == UI_HIT_DROPDOWN && idx == 0,
                    "下拉第一项能被命中");
            ui_pointer_down(&u5, dr.x + dr.w / 2, iy, 100);
            ui_pointer_up(&u5, dr.x + dr.w / 2, iy, 130);
            uexpect(sb, u5.open_menu == UI_MENU_NONE, "选中菜单项之后下拉收起");
            // 帮助菜单能打开"玩法"浮层
            ui_menubar_rect(&u5, 1, &mr);
            ui_pointer_down(&u5, mr.x + mr.w / 2, mr.y + mr.h / 2, 200);
            ui_pointer_up(&u5, mr.x + mr.w / 2, mr.y + mr.h / 2, 230);
            uexpect(sb, u5.open_menu == UI_MENU_HELP, "点一下展开「帮助」菜单");
            ui_dropdown_rect(&u5, &dr);
            const int32_t iy2 = dr.y + 2 * u5.hud_scale + row / 2;
            ui_pointer_down(&u5, dr.x + dr.w / 2, iy2, 300);
            ui_pointer_up(&u5, dr.x + dr.w / 2, iy2, 330);
            uexpect(sb, u5.overlay == UI_OVERLAY_HELP, "「玩法」打开帮助浮层");
            // 浮层的确定按钮能关掉它
            UiRect br;
            ui_overlay_button_rect(&u5, 0, &br);
            ui_pointer_down(&u5, br.x + br.w / 2, br.y + br.h / 2, 400);
            ui_pointer_up(&u5, br.x + br.w / 2, br.y + br.h / 2, 430);
            uexpect(sb, u5.overlay == UI_OVERLAY_NONE, "浮层确定按钮能关闭浮层");
            sb_addf(sb, "9 菜单栏与下拉：通过\n");
        }
    }

    // ---- 渲染不崩、且画出的是经典配色 ----
    {
        Ui ui;
        host_ui_init(&ui, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        setup_demo(&ui, DEMO_MID);
        const UiLayout L = ui_layout(&ui);
        Font font;
        Render r;
        render_init(&r, fb, L.win_w, L.win_h);
        render_load_atlas(&r, g_atlas, sizeof g_atlas);
        render_load_font(&r, g_font, sizeof g_font);
        font_init(&font, &r);
        ui.now_hint = 1;
        ui_paint(&r, &ui, &font, g_atlas, sizeof g_atlas, g_font, sizeof g_font);
        // 左上角第一个像素是外框的高光（不是面板灰）：这是经典 3D 边框的画法。
        // 所以检查按钮面色要避开：① 外框的 3D 高光；② 菜单按钮自己的 3D 边框。
        // 取纵向中心、横向取到两个按钮右侧的空白处，那里才是纯面板灰。
        {
            UiRect mr0, mr1;
            ui_menubar_rect(&ui, 0, &mr0);
            ui_menubar_rect(&ui, 1, &mr1);
            const int32_t probe_x = mr1.x + mr1.w + 6;
            const int32_t probe_y = mr0.y + mr0.h / 2;
            uexpect(sb, probe_x < L.win_w, "探针点落在窗口内");
            uexpect(sb, (fb[(size_t)probe_y * L.win_w + probe_x] & 0xFFFFFFu) == (CS_C_BTNFACE & 0xFFFFFFu),
                    "菜单栏空白处是经典面板灰");
        }
        // 左上角现在是菜单栏本体：不该是黑的、也不该是背景留白（说明整屏都被界面铺满）
        uexpect(sb, (fb[0] & 0xFFFFFFu) != 0x000000u, "左上角不是未初始化的黑");
        // 棋盘中央应该有已翻开的格子（颜色和面板灰不同）
        uint32_t distinct = 0;
        uint32_t seen[64];
        for (int32_t y = L.vis_y; y < L.vis_y + L.vis_rows * L.cell && distinct < 64; y += 3) {
            for (int32_t x = L.vis_x; x < L.vis_x + L.vis_cols * L.cell && distinct < 64; x += 3) {
                const uint32_t p = fb[(size_t)y * L.win_w + x] & 0xFFFFFFu;
                bool found = false;
                for (uint32_t i = 0; i < distinct; i++) {
                    if (seen[i] == p) { found = true; break; }
                }
                if (!found) seen[distinct++] = p;
            }
        }
        uexpect(sb, distinct >= 4, "棋盘上画出了多种颜色（格子/数字/旗子）");
        sb_addf(sb, "10 渲染与配色：通过（棋盘上 %u 种颜色）\n", distinct);
    }

    // ---- 四种雷的计雷器宽度必须一致（五块面板等宽） ----
    {
        Ui ui;
        host_ui_init(&ui, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        game_start_at(&ui.game, 100, 0);
        int32_t widths[4];
        for (int32_t t = 1; t < 5; t++) {
            widths[t - 1] = ui_counter_width(&ui, t);

        }
        // 正负实雷四格、正负虚雷三格+i，所以四块等宽
        uexpect(sb, widths[0] == widths[1] && widths[1] == widths[2] && widths[2] == widths[3],
                "四块计雷器面板必须等宽");
        sb_addf(sb, "11 计雷器等宽：通过（%d / %d / %d / %d）\n",
                widths[0], widths[1], widths[2], widths[3]);
    }

    // ---- 上屏前的帧缓冲（真机"全黑"最可能的成因，在宿主机上把结论钉死） ----
    {
        // 先确认帧缓冲的内存顺序：CS_RGB(0xFF,0,0) 必须是 FF 00 00 FF（即 R,G,B,A）。
        // 这一条是"要不要做通道交换"的全部依据 —— 顺序若与 GL_RGBA 一致就不用换。
        // （这里曾经误判成 BGRA 并把 R/B 换反，所以专门加一条断言把它锁住。）
        uint32_t red = CS_RGB(0xFF, 0x00, 0x00);
        const uint8_t *rb = (const uint8_t *)&red;
        uexpect(sb, rb[0] == 0xFF && rb[1] == 0x00 && rb[2] == 0x00,
                "帧缓冲内存顺序是 R,G,B（与 GL_RGBA 一致，不需要通道交换）");
        uint32_t blue = CS_RGB(0x00, 0x00, 0xFF);
        const uint8_t *bb = (const uint8_t *)&blue;
        uexpect(sb, bb[0] == 0x00 && bb[1] == 0x00 && bb[2] == 0xFF, "纯蓝同上");

        // alpha 必须被就地置 1：帧缓冲里的 alpha 是贴图预乘残留值，
        // 留着会在混合模式下画出半透明。
        uint32_t px[3] = { 0x00000011u, CS_RGB(0xC0, 0x80, 0x40) & 0x00FFFFFFu, 0x7F000000u };
        cs_frame_opaque(px, 3);
        uexpect(sb, (px[0] >> 24) == 0xFF && (px[1] >> 24) == 0xFF && (px[2] >> 24) == 0xFF,
                "alpha 一律被置成 255");
        uexpect(sb, (px[0] & 0x00FFFFFFu) == 0x00000011u &&
                    (px[1] & 0x00FFFFFFu) == (CS_RGB(0xC0, 0x80, 0x40) & 0x00FFFFFFu),
                "置 alpha 不能动其它三个通道");
        sb_addf(sb, "12 上屏帧缓冲（顺序 + alpha）：通过\n");
    }

    // ---- 拖动：钳制、整格吸附、棋盘小于视口时居中 ----
    {
        static const struct { const char *name; int32_t preset; } BOARDS[] = {
            { "初级 9×9", 0 }, { "中级 16×16", 1 }, { "专家 30×16", 2 },
        };
        for (size_t bi = 0; bi < sizeof(BOARDS) / sizeof(BOARDS[0]); bi++) {
            Ui u;
            host_ui_init(&u, BOARDS[bi].preset, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
            const UiLayout L = ui_layout(&u);
            const int32_t board_px_w = (int32_t)u.game.w * L.cell;
            const int32_t board_px_h = (int32_t)u.game.h * L.cell;
            const int32_t view_px_w = L.vis_cols * L.cell;
            const int32_t view_px_h = L.vis_rows * L.cell;
            const int32_t span_x = board_px_w - view_px_w;
            const int32_t span_y = board_px_h - view_px_h;
            char tag[128];

            // 初始状态永远合法：需要拖动的轴在 [0,span]，装得下的轴是居中值（可负）
            snprintf(tag, sizeof tag, "%s：初始 pan 在合法范围", BOARDS[bi].name);
            uexpect(sb, (span_x > 0 ? (u.pan_x >= 0 && u.pan_x <= span_x) : u.pan_x == span_x / 2) &&
                        (span_y > 0 ? (u.pan_y >= 0 && u.pan_y <= span_y) : u.pan_y == span_y / 2), tag);
            snprintf(tag, sizeof tag, "%s：初始 pan 是单格整数倍（%d, %d）",
                     BOARDS[bi].name, u.pan_x, u.pan_y);
            uexpect(sb, u.pan_x % L.cell == 0 && u.pan_y % L.cell == 0, tag);

            // 往两边拖到极限：必须被钳住，且始终是整格
            for (int32_t dir = -1; dir <= 1; dir += 2) {
                ui_pan_begin(&u, 1000, 500);
                ui_pan_to(&u, 1000 + dir * 100000, 500 + dir * 100000);
                ui_pan_end(&u);
                const UiLayout Lp = ui_layout(&u);
                snprintf(tag, sizeof tag, "%s：拖到极限后被钳住（dir=%d, pan=%d,%d）",
                         BOARDS[bi].name, dir, u.pan_x, u.pan_y);
                if (span_x > 0) uexpect(sb, u.pan_x >= 0 && u.pan_x <= span_x, tag);
                else uexpect(sb, u.pan_x == span_x / 2, tag);   // 装得下 → 居中
                snprintf(tag, sizeof tag, "%s：拖动后仍是整格（dir=%d）", BOARDS[bi].name, dir);
                uexpect(sb, u.pan_x % Lp.cell == 0 && u.pan_y % Lp.cell == 0, tag);
                // 拖动不能把棋盘拖出视野：内容左边界不得超过视口右边界，
                // 内容右边界不得小于视口左边界（否则屏幕上就是一片空白）
                snprintf(tag, sizeof tag, "%s：拖动后棋盘仍在视野内（dir=%d）", BOARDS[bi].name, dir);
                {
                    const int32_t content_l = Lp.vis_x - Lp.pan_x;
                    const int32_t content_r = content_l + (int32_t)u.game.w * Lp.cell;
                    const int32_t content_t = Lp.vis_y - Lp.pan_y;
                    const int32_t content_b = content_t + (int32_t)u.game.h * Lp.cell;
                    const bool visible_x = (content_l <= Lp.vis_x + Lp.vis_cols * Lp.cell) &&
                                           (content_r >= Lp.vis_x);
                    const bool visible_y = (content_t <= Lp.vis_y + Lp.vis_rows * Lp.cell) &&
                                           (content_b >= Lp.vis_y);
                    uexpect(sb, visible_x && visible_y, tag);
                }
            }

            // 两种轴各自的"是否需要拖动"必须与几何一致
            snprintf(tag, sizeof tag, "%s：水平拖动需求与几何一致", BOARDS[bi].name);
            uexpect(sb, (span_x > 0) == (board_px_w > view_px_w), tag);
            sb_addf(sb, "    %s：盘 %d×%d px，视口 %d×%d px，可拖 (%d,%d)，pan=(%d,%d)\n",
                    BOARDS[bi].name, board_px_w, board_px_h, view_px_w, view_px_h,
                    span_x > 0 ? span_x : 0, span_y > 0 ? span_y : 0, u.pan_x, u.pan_y);
        }
        sb_addf(sb, "13 拖动钳制与整格吸附：通过\n");
    }

    // ---- 触摸坐标与绘制严格同源 + 侧栏绝不误判成棋盘 ----
    {
        static const struct { int32_t w, h; } WINS[] = {
            { 1920, 1080 }, { 1280, 720 }, { 2560, 1440 },
        };
        for (size_t wi = 0; wi < sizeof(WINS) / sizeof(WINS[0]); wi++) {
            Ui u;
            host_ui_init(&u, 2, WINS[wi].w, WINS[wi].h, HOST_CELL_PX);
            const UiLayout L = ui_layout(&u);
            char tag[128];

            // 每个可见格的中心点都必须命中它自己那一格。
            // 注意：棋盘比视口小时，视口会有一部分落在棋盘外（那边不画格子），
            // 这一部分**不该**被判成棋盘，所以只检查真正对应棋盘格的 vc/vr。
            bool all_ok = true;
            int32_t checked = 0;
            int32_t bad_vc = -1, bad_vr = -1, bad_got = -1, bad_want = -1;
            for (int32_t vr = 0; vr < L.vis_rows && all_ok; vr++) {
                for (int32_t vc = 0; vc < L.vis_cols; vc++) {
                    const int32_t col = vc + (L.pan_x > 0 ? L.pan_x / L.cell : 0);
                    const int32_t row = vr + (L.pan_y > 0 ? L.pan_y / L.cell : 0);
                    if (col >= (int32_t)u.game.w || row >= (int32_t)u.game.h) continue;
                    const int32_t x = L.vis_x + vc * L.cell + L.cell / 2;
                    const int32_t y = L.vis_y + vr * L.cell + L.cell / 2;
                    int32_t idx = -1;
                    const int32_t want = row * (int32_t)u.game.w + col;
                    const int32_t got = ui_hit_test(&u, x, y, &idx);
                    if (got != UI_HIT_BOARD || idx != want) {
                        all_ok = false;
                        bad_vc = vc; bad_vr = vr; bad_got = (got == UI_HIT_BOARD) ? idx : -got;
                        bad_want = want;
                        break;
                    }
                    checked++;
                }
            }
            snprintf(tag, sizeof tag, "%dx%d：每个可见格中心都命中自己（查了 %d 格）",
                     WINS[wi].w, WINS[wi].h, checked);
            uexpect(sb, all_ok && checked > 0, tag);
            if (!all_ok) {
                sb_addf(sb, "      [诊断] vc=%d vr=%d 期望格 %d，实得 %d；"
                            "vis=(%d,%d) %d×%d cell=%d pan=(%d,%d) side_w=%d hd=%d 盘=%ux%u\n",
                        bad_vc, bad_vr, bad_want, bad_got,
                        L.vis_x, L.vis_y, L.vis_cols, L.vis_rows, L.cell, L.pan_x, L.pan_y,
                        L.side_w, L.hud, u.game.w, u.game.h);
            }
            // 可见范围内对应的棋盘格应当**一个不漏**都能被命中
            const int32_t pcol = L.pan_x > 0 ? L.pan_x / L.cell : 0;
            const int32_t prow = L.pan_y > 0 ? L.pan_y / L.cell : 0;
            int32_t expect_hits = 0;
            for (int32_t r = prow; r < prow + L.vis_rows && r < (int32_t)u.game.h; r++) {
                for (int32_t c = pcol; c < pcol + L.vis_cols && c < (int32_t)u.game.w; c++) expect_hits++;
            }
            snprintf(tag, sizeof tag, "%dx%d：棋盘可见格一个不漏（%d 格，盘共 %u）",
                     WINS[wi].w, WINS[wi].h, expect_hits, (unsigned)u.game.n);
            uexpect(sb, checked == expect_hits, tag);
            // 棋盘比视口大时，可见格数必须真的小于棋盘（否则"要拖动"就是假的）
            if ((int32_t)u.game.w > L.vis_cols || (int32_t)u.game.h > L.vis_rows) {
                snprintf(tag, sizeof tag, "%dx%d：盘比视口大时可见格确实更少", WINS[wi].w, WINS[wi].h);
                uexpect(sb, expect_hits < (int32_t)u.game.n, tag);
            }

            // 侧栏/菜单栏上的点**永远**不能是棋盘格（误触就出在这里）
            bool leak = false;
            for (int32_t x = 0; x < L.side_w && !leak; x += 7) {
                for (int32_t y = L.menubar_h; y < L.win_h && !leak; y += 7) {
                    int32_t idx = -1;
                    if (ui_hit_test(&u, x, y, &idx) == UI_HIT_BOARD) leak = true;
                }
            }
            // 菜单栏**只占侧栏那一条**：所以这个循环也只能扫侧栏宽度。
            // 早先扫满全宽 —— 那在"菜单栏横跨全宽"的旧布局下是对的，
            // 现在右侧上方是棋盘，扫过去当然会（也**应该**）命中棋盘格。
            for (int32_t x = 0; x < L.side_w && !leak; x += 7) {
                for (int32_t y = 0; y < L.menubar_h && !leak; y += 7) {
                    int32_t idx = -1;
                    if (ui_hit_test(&u, x, y, &idx) == UI_HIT_BOARD) leak = true;
                }
            }
            snprintf(tag, sizeof tag, "%dx%d：侧栏与菜单栏上的点不会被判成棋盘格", WINS[wi].w, WINS[wi].h);
            uexpect(sb, !leak, tag);
            // 反过来：**棋盘区里（x ≥ board_x）没有任何点会被判成菜单/侧栏** ——
            // 这就是"顶部空白并进地图"的可检验表述。
            {
                bool bad = false;
                for (int32_t x = L.board_x; x < L.win_w && !bad; x += 11) {
                    for (int32_t y = 0; y < L.win_h && !bad; y += 11) {
                        const int32_t h = ui_hit_test(&u, x, y, NULL);
                        if (h == UI_HIT_MENU || h == UI_HIT_MENU_ITEM || h == UI_HIT_SIDEBAR) bad = true;
                    }
                }
                snprintf(tag, sizeof tag, "%dx%d：棋盘区里没有点被判成菜单/侧栏", WINS[wi].w, WINS[wi].h);
                uexpect(sb, !bad, tag);
            }

            // 棋盘区右侧以外的区域（棋盘小于视口时）也不能算棋盘格
            const int32_t board_right = L.vis_x + (int32_t)u.game.w * L.cell - L.pan_x;
            if (board_right < L.win_w) {
                int32_t idx = -1;
                const int32_t px = board_right + 2;
                const int32_t py = L.vis_y + L.cell / 2;
                snprintf(tag, sizeof tag, "%dx%d：棋盘右侧空白不是棋盘格", WINS[wi].w, WINS[wi].h);
                uexpect(sb, ui_hit_test(&u, px, py, &idx) != UI_HIT_BOARD, tag);
            }
        }
        sb_addf(sb, "14 触摸坐标同源 + 侧栏不误判：通过\n");
    }

    // ---- 拖动确实换了格子（点同一个屏幕位置，拖动前后命中的格号必须不同） ----
    {
        Ui u;
        host_ui_init(&u, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        const UiLayout L0 = ui_layout(&u);
        const int32_t probe_x = L0.vis_x + L0.cell / 2;
        const int32_t probe_y = L0.vis_y + L0.cell / 2;
        int32_t before = -1, after = -1;
        ui_hit_test(&u, probe_x, probe_y, &before);
        // 往左拖两格：内容左移，同一个屏幕点应当落到更右边两格的格子上
        ui_pan_begin(&u, probe_x, probe_y);
        ui_pan_to(&u, probe_x - 2 * L0.cell, probe_y);
        ui_pan_end(&u);
        ui_hit_test(&u, probe_x, probe_y, &after);
        uexpect(sb, before >= 0 && after >= 0, "拖动前后命中点都在棋盘内");
        uexpect(sb, after == before + 2, "往左拖两格后，同一屏幕点命中右移两格的格子");
        const UiLayout L1 = ui_layout(&u);
        uexpect(sb, L1.pan_x == 2 * L1.cell, "两格拖动产生两格偏移");
        // 反向拖两格回到原位。注意 ui_pan_begin 会把当前 pan 记为基准，
        // 位移是相对**当次起点**算的，所以这里必须重新 begin。
        ui_pan_begin(&u, probe_x, probe_y);
        ui_pan_to(&u, probe_x + 2 * L1.cell, probe_y);
        ui_pan_end(&u);
        uexpect(sb, u.pan_x == 0, "反向拖两格后偏移归零");
        sb_addf(sb, "15 拖动改变命中格：通过（拖前 %d → 拖后 %d）\n", before, after);
    }

    // ---- 拖动绝不能顺带翻格/插旗（这是拖动功能最容易引入的误触） ----
    {
        Ui u;
        host_ui_init(&u, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        const UiLayout L = ui_layout(&u);
        const int32_t sx = L.vis_x + L.cell / 2;
        const int32_t sy = L.vis_y + L.cell / 2;
        ui_pointer_down(&u, sx, sy, 1000);
        // 越过阈值 → 变成拖动
        ui_pointer_move(&u, sx + 4 * UI_PAN_SLOP_PX, sy, 1010);
        uexpect(sb, u.pan_active, "位移越过阈值后进入拖动");
        // 继续滑过好几格，再抬手
        ui_pointer_move(&u, sx + 4 * UI_PAN_SLOP_PX + 3 * L.cell, sy + 2 * L.cell, 1020);
        ui_pointer_up(&u, sx + 4 * UI_PAN_SLOP_PX + 3 * L.cell, sy + 2 * L.cell, 1100);
        uexpect(sb, !u.pan_active, "抬手后拖动结束");
        int32_t flagged = 0, opened = 0;
        for (size_t i = 0; i < u.game.n; i++) {
            if (u.game.flag[i] != 0) flagged++;
            if (u.game.open[i] != 0) opened++;
        }
        uexpect(sb, flagged == 0, "拖动路径上没有插上任何旗");
        uexpect(sb, opened == 0, "拖动路径上没有翻开任何格子");
        sb_addf(sb, "16 拖动不误触：通过\n");
    }

    // ---- 单格物理尺寸的换算（出过一次单位错误，只在真机上才被发现） ----
    {
        // 0.7cm 在常见密度下应该有多少像素：px = 0.7 * dpi / 2.54
        static const struct { int32_t dpi, want; } CASES[] = {
            { 160, 44 }, { 240, 66 }, { 280, 77 }, { 320, 88 }, { 420, 116 }, { 480, 132 },
        };
        for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
            const int32_t got = ui_cell_px_for_dpi(CASES[i].dpi);
            char tag[128];
            snprintf(tag, sizeof tag, "%ddpi 下单格 %dpx（0.7cm 应为 %dpx）",
                     CASES[i].dpi, got, CASES[i].want);
            uexpect(sb, got == CASES[i].want, tag);
            // 反过来校验物理尺寸确实落在 0.7cm 附近（±1px 的取整误差）
            const double cm = (double)got * 2.54 / (double)CASES[i].dpi;
            snprintf(tag, sizeof tag, "%ddpi 下单格物理尺寸 %.3fcm 接近 0.70cm", CASES[i].dpi, cm);
            uexpect(sb, cm > 0.67 && cm < 0.73, tag);
        }
        // 极端/异常密度不能算出 0 或负数
        uexpect(sb, ui_cell_px_for_dpi(0) == 77, "密度为 0 时退回 280dpi 的 77px");
        uexpect(sb, ui_cell_px_for_dpi(1) >= 1, "极低密度也至少 1px");
        sb_addf(sb, "17 单格物理尺寸换算：通过（0.7cm @280dpi = 77px）\n");
    }

    // ---- 拖动中"不足一格的位移"必须生效，且**不再吸附到整格** ----
    {
        Ui u;
        host_ui_init(&u, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        const UiLayout L = ui_layout(&u);
        uexpect(sb, L.vis_cols < (int32_t)u.game.w, "专家盘的可见列数确实少于总列数（需要拖动）");
        const int32_t half = L.cell / 2;
        const int32_t third = L.cell / 3;

        // 拖半格：偏移必须真的等于半格，而不是被吸附掉
        ui_pan_begin(&u, 1000, 500);
        ui_pan_to(&u, 1000 - half, 500);
        ui_pan_end(&u);
        uexpect(sb, u.pan_x == half, "拖半格后偏移正好是半格（不吸附、不归零）");
        // 半格偏移下，屏幕两侧应当各露出一个不完整的格子
        {
            const UiLayout Lh = ui_layout(&u);
            const int32_t first_px = Lh.board_origin_x + Lh.vis_first_col * Lh.cell;
            const int32_t right_px = Lh.board_origin_x + Lh.vis_end_col * Lh.cell;
            const int32_t view_r = Lh.vis_x + Lh.vis_cols * Lh.cell;
            uexpect(sb, first_px < Lh.vis_x, "半格偏移：左侧第一个可见格是不完整的（被裁掉一部分）");
            uexpect(sb, right_px >= view_r, "半格偏移：右侧仍然铺满到视口边界");
        }

        // 再拖三分之一格：偏移仍然是精确的位移量
        ui_pan_reset(&u);
        ui_pan_begin(&u, 1000, 500);
        ui_pan_to(&u, 1000 - third, 500);
        ui_pan_end(&u);
        uexpect(sb, u.pan_x == third, "拖三分之一格后偏移精确等于三分之一格");

        // 松手不跳：ui_pan_to 的值与 ui_pan_end 之后的值必须一模一样
        ui_pan_reset(&u);
        ui_pan_begin(&u, 1000, 500);
        ui_pan_to(&u, 1000 - half - third, 500);
        const int32_t during = u.pan_x;
        ui_pan_end(&u);
        uexpect(sb, u.pan_x == during, "松手后偏移不变（画面不跳）");
        uexpect(sb, during == half + third, "拖动量被完整保留");

        // 拖动中与松手后命中同一格（绘制/命中共用 board_origin）
        {
            const UiLayout Ld = ui_layout(&u);
            int32_t idx = -1;
            const int32_t sx = Ld.vis_x + Ld.cell / 2;
            const int32_t sy = Ld.vis_y + Ld.cell / 2;
            uexpect(sb, ui_hit_test(&u, sx, sy, &idx) == UI_HIT_BOARD, "偏移后视口左上角仍在棋盘内");
            const int32_t want_col = (sx - Ld.board_origin_x) / Ld.cell;
            uexpect(sb, idx == want_col, "命中格号与 board_origin 推导一致");
            sb_addf(sb, "    半格偏移 %dpx、三分之一格 %dpx、合计 %dpx；"
                        "该点命中第 %d 列\n", half, third, during, idx);
        }
        sb_addf(sb, "18 拖动不吸附到整格（边缘显示不完整格子）：通过\n");
    }

    // ---- 完整的多事件拖动序列（真机上"拖不动"就出在这里） ----
    {
        // 逐事件走一遍：DOWN → 多个 MOVE → UP。
        // 这里的坑很隐蔽：拖动开始时调 ui_pointer_cancel 若把 pointer_down 置 false，
        // 下一次 MOVE 进来时 ui_pointer_move 开头的 `if (!pointer_down) return;`
        // 会直接返回，于是"拖动只处理了第一个 MOVE 就死了"。
        // 单看每一步的日志都正常（pan_active 也确实成了 1），所以必须要整串断言。
        Ui u;
        host_ui_init(&u, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        const UiLayout L = ui_layout(&u);
        const int32_t sx = L.vis_x + 5 * L.cell;
        const int32_t sy = L.vis_y + 5 * L.cell;
        ui_pointer_down(&u, sx, sy, 1000);
        uexpect(sb, u.pointer_down, "DOWN 之后 pointer_down = true");
        uexpect(sb, ui_hit_test(&u, sx, sy, NULL) == UI_HIT_BOARD, "起点在棋盘上");

        const int32_t steps[] = { -10, -40, -80, -120, -160, -200, -240 };
        bool drag_started = false;
        for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
            const int32_t x = sx + steps[i];
            ui_pointer_move(&u, x, sy, 1000 + (uint32_t)(i + 1) * 30);
            if (u.pan_active) drag_started = true;
            char tag[128];
            snprintf(tag, sizeof tag, "第 %d 次 MOVE 后 pointer_down 仍为 true（拖动不能被自己掐断）",
                     (int)i + 1);
            uexpect(sb, drag_started ? u.pointer_down : true, tag);
        }
        uexpect(sb, u.pan_active, "整串 MOVE 之后仍在拖动状态");
        // 位移是最后一步相对 pan_begin 那一步算的：越过阈值的那一步是 -40（相对 -10）
        uexpect(sb, u.pan_x > 0, "整串拖动之后偏移真的变了（不是一直 0）");
        const UiLayout Ld = ui_layout(&u);
        uexpect(sb, Ld.board_origin_x != Ld.vis_x, "布局里的棋盘原点跟着偏移走了");
        // 抬手：拖动收尾，绝不能插旗/翻格
        ui_pointer_up(&u, sx + steps[6], sy, 2000);
        uexpect(sb, !u.pan_active, "UP 之后拖动结束");
        int32_t flagged = 0, opened = 0;
        for (size_t i = 0; i < u.game.n; i++) {
            if (u.game.flag[i] != 0) flagged++;
            if (u.game.open[i] != 0) opened++;
        }
        uexpect(sb, flagged == 0 && opened == 0, "整串拖动没有插旗、也没有翻格");
        sb_addf(sb, "19 多事件拖动序列：通过（偏移 %dpx）\n", u.pan_x);
    }

    // ---- 拖到两端时棋盘必须始终盖满视口（不能露出棋盘外的空白） ----
    {
        Ui u;
        host_ui_init(&u, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        for (int32_t dir = 0; dir < 2; dir++) {
            // 拖到左端 / 右端
            ui_pan_begin(&u, 1000, 500);
            ui_pan_to(&u, dir == 0 ? 1000 + 100000 : 1000 - 100000, 500);
            ui_pan_end(&u);
            const UiLayout L = ui_layout(&u);
            const int32_t board_r = L.board_origin_x + (int32_t)u.game.w * L.cell;
            const int32_t board_b = L.board_origin_y + (int32_t)u.game.h * L.cell;
            const int32_t view_r = L.vis_x + L.vis_cols * L.cell;
            const int32_t view_b = L.vis_y + L.vis_rows * L.cell;
            char tag[128];
            snprintf(tag, sizeof tag, "拖到%s端：棋盘左边界不越过视口右边界（%d vs %d）",
                     dir == 0 ? "左" : "右", L.board_origin_x, view_r);
            uexpect(sb, L.board_origin_x <= view_r, tag);
            snprintf(tag, sizeof tag, "拖到%s端：棋盘右边界盖满视口右边界（%d vs %d）",
                     dir == 0 ? "左" : "右", board_r, view_r);
            uexpect(sb, board_r >= view_r, tag);
            snprintf(tag, sizeof tag, "拖到%s端：棋盘上边界不越过视口下边界", dir == 0 ? "左" : "右");
            uexpect(sb, L.board_origin_y <= view_b, tag);
            snprintf(tag, sizeof tag, "拖到%s端：棋盘下边界盖满视口下边界（%d vs %d）",
                     dir == 0 ? "左" : "右", board_b, view_b);
            uexpect(sb, board_b >= view_b, tag);
            sb_addf(sb, "    拖到%s端：origin=(%d,%d) 盘右/下=(%d,%d) 视口右/下=(%d,%d)\n",
                    dir == 0 ? "左" : "右", L.board_origin_x, L.board_origin_y,
                    board_r, board_b, view_r, view_b);
        }
        sb_addf(sb, "20 拖到两端不露空白：通过\n");
    }

    // ---- 拖动后必须把格子画到视口边界（这条以前是坏的：右侧空出整个拖动量） ----
    {
        Ui u;
        host_ui_init(&u, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        const UiLayout L0 = ui_layout(&u);
        // 逐个 pan 位置检查：绘制区间必须覆盖到裁剪矩形右/下边界
        for (int32_t i = 0; i <= 4; i++) {
            const int32_t pan = (L0.vis_cols < (int32_t)u.game.w)
                                    ? (i * ((int32_t)u.game.w * L0.cell - L0.vis_cols * L0.cell)) / 4
                                    : 0;
            ui_pan_reset(&u);
            ui_pan_begin(&u, 1000, 500);
            ui_pan_to(&u, 1000 - pan, 500);
            ui_pan_end(&u);
            const UiLayout L = ui_layout(&u);
            const int32_t clip_r = L.board_clip_x + L.board_clip_w;
            const int32_t clip_b = L.board_clip_y + L.board_clip_h;
            // 最后一列格子的右边界
            const int32_t last_r = L.board_origin_x + L.vis_end_col * L.cell;
            const int32_t last_b = L.board_origin_y + L.vis_end_row * L.cell;
            char tag[160];
            snprintf(tag, sizeof tag,
                     "pan=%d：画到的格子必须盖到裁剪右边界（画到 %d，边界 %d）",
                     pan, last_r, clip_r);
            uexpect(sb, last_r >= clip_r, tag);
            snprintf(tag, sizeof tag,
                     "pan=%d：画到的格子必须盖到裁剪下边界（画到 %d，边界 %d）",
                     pan, last_b, clip_b);
            uexpect(sb, last_b >= clip_b, tag);
            // 起点不能跳过裁剪左边界（否则左边会缺格）
            snprintf(tag, sizeof tag, "pan=%d：第一列必须覆盖裁剪左边界", pan);
            uexpect(sb, L.board_origin_x + L.vis_first_col * L.cell <= L.board_clip_x, tag);
            // 不能多画到棋盘外
            snprintf(tag, sizeof tag, "pan=%d：绘制区间不越过棋盘", pan);
            uexpect(sb, L.vis_first_col >= 0 && L.vis_end_col <= (int32_t)u.game.w, tag);
            if (i == 4 || pan == 0) {
                sb_addf(sb, "    pan=%d：画第 %d..%d 列（共 %d 列），裁剪宽 %d，盘 %u 列\n",
                        pan, L.vis_first_col, L.vis_end_col - 1,
                        L.vis_end_col - L.vis_first_col, L.board_clip_w, (unsigned)u.game.w);
            }
        }
        sb_addf(sb, "21 拖动后格子画到视口边界：通过\n");
    }

    // ---- 拖动死区必须小到无感（实测反馈出来才加的） ----
    {
        Ui u;
        host_ui_init(&u, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        const UiLayout L = ui_layout(&u);
        const int32_t sx = L.vis_x + 6 * L.cell;
        const int32_t sy = L.vis_y + 6 * L.cell;
        ui_pointer_down(&u, sx, sy, 1000);
        uexpect(sb, !u.pan_active, "刚按下还不算拖动（先给长按/点按留判定）");

        // 阈值之下一律不平移（保护长按），到了阈值立刻跟手
        for (int32_t d = 1; d < UI_PAN_SLOP_PX; d++) {
            ui_pointer_move(&u, sx - d, sy, 1000 + 20 * d);
            char tag[128];
            snprintf(tag, sizeof tag, "挪 %dpx（< 阈值 %d）时画面不动，保护长按",
                     d, UI_PAN_SLOP_PX);
            uexpect(sb, u.pan_x == 0, tag);
        }
        ui_pointer_move(&u, sx - UI_PAN_SLOP_PX, sy, 1000 + 20 * UI_PAN_SLOP_PX);
        uexpect(sb, u.pan_active, "达到阈值后进入拖动");
        uexpect(sb, u.pan_x == UI_PAN_SLOP_PX,
                "进入拖动的那一帧就补上全部位移（不丢像素）");

        // 之后必须一比一跟手：再挪多少就走多少
        for (int32_t d = UI_PAN_SLOP_PX + 1; d <= UI_PAN_SLOP_PX + 6; d++) {
            ui_pointer_move(&u, sx - d, sy, 1000 + 20 * d);
            char tag[128];
            snprintf(tag, sizeof tag, "挪 %dpx 时偏移必须正好是 %dpx（一比一跟手）", d, d);
            uexpect(sb, u.pan_x == d, tag);
        }
        const int32_t moved = UI_PAN_SLOP_PX + 6;
        const bool is_drag = ui_pointer_settle(&u);
        uexpect(sb, is_drag, "总位移超过阈值 → 判定为拖动");
        ui_pointer_up(&u, sx - moved, sy, 1200);
        uexpect(sb, u.pan_x == moved, "拖动松手后偏移完整保留（不吸附、不跳）");

        // 死区换算成人能感知的时间：阈值越小越好
        {
            // 280dpi 下 1px ≈ 0.0907mm
            const double mm = UI_PAN_SLOP_PX * 25.4 / 280.0;
            sb_addf(sb, "    死区 %dpx（≈%.2fmm）：50px/s 慢速拖动 ≈ %.0fms 后跟手；"
                        "100px/s ≈ %.0fms\n",
                    UI_PAN_SLOP_PX, mm, 1000.0 * UI_PAN_SLOP_PX / 50.0,
                    1000.0 * UI_PAN_SLOP_PX / 100.0);
            uexpect(sb, 1000.0 * UI_PAN_SLOP_PX / 50.0 <= 60.0,
                    "最慢的拖动（50px/s）也要在 60ms 内进入跟手（否则能感知到卡顿）");
        }

        // ---- 手指只是抖一下 → 必须按点按处理，并把画面恢复原样 ----
        ui_pan_reset(&u);
        const int32_t before = u.pan_x;
        ui_pointer_down(&u, sx, sy, 2000);
        ui_pointer_move(&u, sx - 1, sy, 2010);       // 抖 1px
        uexpect(sb, u.pan_x == before, "抖动 1px 时画面纹丝不动（不会挪了再弹回）");
        uexpect(sb, !ui_pointer_settle(&u), "总位移 1px < 阈值 → 不算拖动");

        // ---- 按下后轻微抖动，长按仍然要能触发 ----
        ui_pan_reset(&u);
        ui_pointer_down(&u, sx, sy, 3000);
        ui_pointer_move(&u, sx - 1, sy, 3010);        // 抖 1px
        ui_tick(&u, 3000 + UI_LONG_PRESS_MS + 1);
        uexpect(sb, u.long_fired, "只抖 1px 的情况下长按仍能触发（抖动不该吃掉长按）");
        sb_addf(sb, "22 拖动死区小到无感 + 抖动仍算点按：通过\n");
    }

    // ---- 新局必须换种子：连续新局 + 点同一格，雷区不能一样 ----
    // 这条是真机上发现的严重 bug：所有"新局"入口都写死了 seed=1，
    // 而"第一次点击必安全"是按种子铺完雷再把那一格挪走，
    // 于是"点新局 -> 点同一格"每次都得到完全相同的局面。
    {
        // 局面指纹：把雷位/翻开/提示值全混进去，足够区分两张图
        #define CS_SEED_FP(g) ({                                    \
            uint32_t h_ = 2166136261u;                              \
            for (size_t i_ = 0; i_ < (g)->n; i_++) {                \
                h_ ^= (uint32_t)(g)->mine[i_] * 131u +              \
                      (uint32_t)(g)->open[i_] * 17u +               \
                      (uint32_t)(g)->clue[i_];                      \
                h_ *= 16777619u;                                    \
            }                                                       \
            h_;                                                     \
        })

        Ui u;
        host_ui_init(&u, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        const int32_t start_cell = 200;      // 每次都点同一格
        uint32_t seen[4];
        for (int32_t round = 0; round < 4; round++) {
            // 模拟"点新局"：走 CMD_NEW 的同一条路径
            ui_cmd_new_game(&u, ui_fresh_seed(&u), true);
            game_start_at(&u.game, (size_t)start_cell, 1000);
            seen[round] = CS_SEED_FP(&u.game);
        }
        char tag[128];
        for (int32_t i = 0; i < 4; i++) {
            for (int32_t j = i + 1; j < 4; j++) {
                snprintf(tag, sizeof tag,
                         "第 %d 局与第 %d 局雷区必须不同（指纹 %08x vs %08x）",
                         i + 1, j + 1, seen[i], seen[j]);
                uexpect(sb, seen[i] != seen[j], tag);
            }
        }
        sb_addf(sb, "    连续 4 次新局（都点第 %d 格）指纹：%08x %08x %08x %08x\n",
                start_cell, seen[0], seen[1], seen[2], seen[3]);

        // 换难度也必须换种子。
        // 注意：雷区要等**第一次翻开**才铺（"首点必安全"就是这么实现的），
        // 所以指纹必须在 game_start_at 之后取 —— 否则每局都是"空盘"，指纹恒等。
        Ui v;
        host_ui_init(&v, 0, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        uint32_t p1 = 0, p2 = 0;
        ui_cmd_preset(&v, 2); game_start_at(&v.game, 200, 0); p1 = CS_SEED_FP(&v.game);
        ui_cmd_preset(&v, 2); game_start_at(&v.game, 200, 0); p2 = CS_SEED_FP(&v.game);
        uexpect(sb, p1 != p2, "连续两次选同一个难度，雷区也必须不同");

        // 自定义盘同样
        Ui w;
        host_ui_init(&w, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        uint32_t c1 = 0, c2 = 0;
        ui_cmd_open_custom(&w); ui_cmd_apply_custom(&w);
        game_start_at(&w.game, 200, 0); c1 = CS_SEED_FP(&w.game);
        ui_cmd_open_custom(&w); ui_cmd_apply_custom(&w);
        game_start_at(&w.game, 200, 0); c2 = CS_SEED_FP(&w.game);
        uexpect(sb, c1 != c2, "连续两次应用同一个自定义盘，雷区也必须不同");
        #undef CS_SEED_FP
        sb_addf(sb, "23 新局换种子（连续新局雷区不同）：通过\n");
    }

    // ---- 跨屏幕自适应：任意窗口尺寸 × 任意屏幕密度，文字都必须放得下 ----
    // 这条是设备上多密度实测后补的：字号是固定像素，而 hud 只随窗口像素尺寸走、
    // 不随密度走，于是"像素多但 hud 小"的组合（例如 2160×1080 @440dpi）里
    // 菜单栏装不下中文、字被上下裁掉。原来只断言几何，抓不到"字放不下"。
    {
        static const struct { int32_t w, h; } WINS[] = {
            { 2160, 1080 }, { 1920, 1080 }, { 1600, 720 }, { 1280, 720 },
            { 2560, 1600 }, { 1024, 600 }, { 2400, 1080 }, { 960, 540 },
        };
        static const int32_t DPIS[] = { 160, 240, 280, 320, 395, 420, 440, 480, 560 };
        int32_t cases = 0;
        for (size_t wi = 0; wi < sizeof WINS / sizeof WINS[0]; wi++) {
            for (size_t di = 0; di < sizeof DPIS / sizeof DPIS[0]; di++) {
                Ui u;
                ui_init(&u, WINS[wi].w, WINS[wi].h);
                ui_set_cell_px(&u, ui_cell_px_for_dpi(DPIS[di]));
                ui_cmd_preset(&u, 2);
                ui_refresh_scale(&u);
                const UiLayout L = ui_layout(&u);
                char tag[176];

                // 单格物理尺寸：目标 0.7cm（被安全钳制时可以更小，但不能为 0）
                uexpect(sb, L.cell >= 1, "单格至少 1px");
                // 可见格数与侧栏
                snprintf(tag, sizeof tag, "%dx%d@%ddpi：可见格数为正",
                         WINS[wi].w, WINS[wi].h, DPIS[di]);
                uexpect(sb, L.vis_cols >= 1 && L.vis_rows >= 1, tag);

                // 菜单栏必须装得下菜单文字（这一条才是真正抓 bug 的）
                for (int32_t m = 0; m < 2; m++) {
                    UiRect br;
                    ui_menubar_rect(&u, m, &br);
                    const char *label = (const char *)ui_strings[
                        m == 0 ? UI_S_MENU_GAME : UI_S_MENU_HELP].bytes;
                    int32_t itop = 0, ibot = 0;
                    const bool ink = font_text_ink_bounds(label, ui_font_md(&u), &itop, &ibot);
                    const int32_t ty = font_center_y_for_text(label, ui_font_md(&u), br.y, br.h);
                    snprintf(tag, sizeof tag, "%dx%d@%ddpi：菜单%d 文字纵向完整落在按钮内",
                             WINS[wi].w, WINS[wi].h, DPIS[di], m);
                    uexpect(sb, ink && ty + itop >= br.y && ty + ibot <= br.y + br.h, tag);
                    snprintf(tag, sizeof tag, "%dx%d@%ddpi：菜单%d 文字横向装得进按钮",
                             WINS[wi].w, WINS[wi].h, DPIS[di], m);
                    uexpect(sb, font_text_width(NULL, label, ui_font_md(&u)) <= br.w, tag);
                }
                // 菜单栏本身必须够高
                snprintf(tag, sizeof tag, "%dx%d@%ddpi：菜单栏高度容得下文字",
                         WINS[wi].w, WINS[wi].h, DPIS[di]);
                {
                    int32_t itop = 0, ibot = 0;
                    font_text_ink_bounds((const char *)ui_strings[UI_S_MENU_GAME].bytes,
                                         ui_font_md(&u), &itop, &ibot);
                    uexpect(sb, L.menubar_h >= ibot - itop, tag);
                }
                // 侧栏宽度为正、棋盘区为正
                snprintf(tag, sizeof tag, "%dx%d@%ddpi：侧栏与棋盘区都为正",
                         WINS[wi].w, WINS[wi].h, DPIS[di]);
                uexpect(sb, L.side_w > 0 && L.board_w > 0 && L.hud >= 1, tag);
                cases++;
            }
        }
        sb_addf(sb, "    覆盖 %d 种「窗口 × 密度」组合\n", cases);
        sb_addf(sb, "24 跨屏幕自适应（文字放得下）：通过\n");
    }

    // ---- 窗口尺寸必须真的驱动布局（设备上 2712x1220 只画了左上 1920x1080） ----
    // 这条是用户报"不同分辨率设备上依然不行"之后查出来的真 bug：
    // ui_resize() 早就写好了，但**平台层从来没调用过**，布局一直是
    // ui_init 那个写死的 1920x1080。于是非 1920x1080 的设备上，帧缓冲是真实窗口
    // 尺寸、内容却只占左上角一块（实测 2712x1220 的机器右侧 792 列纯黑）。
    {
        static const struct { int32_t w, h; } SIZES[] = {
            { 2712, 1220 }, { 1920, 1080 }, { 1280, 720 }, { 2440, 1440 },
            { 1600, 720 }, { 1024, 600 }, { 2560, 1600 },
        };
        for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
            // 模拟平台层：先按默认值 init，再同步真实窗口尺寸
            Ui u;
            ui_init(&u, 1920, 1080);
            ui_set_cell_px(&u, ui_cell_px_for_dpi(280));
            ui_cmd_preset(&u, 2);
            ui_resize(&u, SIZES[i].w, SIZES[i].h);
            const UiLayout L = ui_layout(&u);
            char tag[160];

            snprintf(tag, sizeof tag, "ui_resize 后布局宽度 = 窗口宽度（%dx%d）",
                     SIZES[i].w, SIZES[i].h);
            uexpect(sb, L.win_w == SIZES[i].w, tag);
            snprintf(tag, sizeof tag, "ui_resize 后布局高度 = 窗口高度（%dx%d）",
                     SIZES[i].w, SIZES[i].h);
            uexpect(sb, L.win_h == SIZES[i].h, tag);

            // 棋盘区右/下边界必须落在窗口内，并且**要贴到右边**（不能只占左上角）
            snprintf(tag, sizeof tag, "%dx%d：棋盘区右边贴到窗口右侧", SIZES[i].w, SIZES[i].h);
            uexpect(sb, L.board_x + L.board_w == SIZES[i].w, tag);
            snprintf(tag, sizeof tag, "%dx%d：棋盘区下边贴到窗口底部", SIZES[i].w, SIZES[i].h);
            uexpect(sb, L.board_y + L.board_h == SIZES[i].h, tag);
            // 侧栏 = 长边/5（允许被下限/上限钳制）
            {
                const int32_t long_edge = SIZES[i].w > SIZES[i].h ? SIZES[i].w : SIZES[i].h;
                const int32_t want = long_edge / 5;
                snprintf(tag, sizeof tag, "%dx%d：侧栏宽 = 长边/5（= %d）",
                         SIZES[i].w, SIZES[i].h, want);
                uexpect(sb, L.side_w == want, tag);
            }
        }
        sb_addf(sb, "    覆盖 %d 种窗口尺寸\n", (int32_t)(sizeof SIZES / sizeof SIZES[0]));
        sb_addf(sb, "25 窗口尺寸驱动布局（ui_resize）：通过\n");
    }

    // ---- 拖动行程不能"短到没用"（用户报"拖动完全不平滑"的真因） ----
    // 棋盘比屏幕只大一点点时，能拖的行程会短到只有几格（实测 2712x1220 上
    // 高级盘只有 231px ≈ 手指挪 2cm 就撞底），之后画面完全不动。
    // ui.c 的处理是"这种退化情况就让单格缩一点、整盘入屏"。
    // 这里断言：任何「窗口 × 难度」组合下，要么不需要拖，要么行程足够长。
    {
        static const struct { int32_t w, h; } WINS[] = {
            { 2712, 1220 }, { 1920, 1080 }, { 1600, 720 }, { 1280, 720 },
            { 1024, 600 }, { 2560, 1600 }, { 2340, 1080 }, { 960, 540 },
        };
        static const int32_t DPIS[] = { 240, 280, 320, 395, 440 };
        int32_t cases = 0, panned = 0, shrunk = 0;
        for (size_t wi = 0; wi < sizeof WINS / sizeof WINS[0]; wi++) {
            for (size_t di = 0; di < sizeof DPIS / sizeof DPIS[0]; di++) {
                for (int32_t preset = 0; preset < GAME_PRESET_COUNT; preset++) {
                    Ui u;
                    ui_init(&u, WINS[wi].w, WINS[wi].h);
                    const int32_t target = ui_cell_px_for_dpi(DPIS[di]);
                    ui_set_cell_px(&u, target);
                    ui_cmd_preset(&u, preset);
                    ui_refresh_scale(&u);
                    const UiLayout L = ui_layout(&u);
                    const int32_t span_x = ((int32_t)u.game.w - L.vis_cols) * L.cell;
                    const int32_t span_y = ((int32_t)u.game.h - L.vis_rows) * L.cell;
                    char tag[192];

                    if (L.cell < target) shrunk++;
                    if (span_x > 0 || span_y > 0) panned++;

                    // 真正的设计属性：某个轴真的放不下整盘时，可见格数要收到
                    // "至少留出 4 格可拖行程"，否则拖动会一碰到底（画面随后完全不动）。
                    if ((int32_t)u.game.w * L.cell > L.sb_w + L.vis_cols * L.cell) {
                        const int32_t over = (int32_t)u.game.w * L.cell - L.vis_cols * L.cell;
                        snprintf(tag, sizeof tag,
                                 "%dx%d@%ddpi 预置%d：横向行程 ≥ 4 格（%d ≥ %d）",
                                 WINS[wi].w, WINS[wi].h, DPIS[di], preset, over, 4 * L.cell);
                        uexpect(sb, over >= 4 * L.cell, tag);
                    }
                    if ((int32_t)u.game.h * L.cell > L.sb_h + L.vis_rows * L.cell) {
                        const int32_t over = (int32_t)u.game.h * L.cell - L.vis_rows * L.cell;
                        snprintf(tag, sizeof tag,
                                 "%dx%d@%ddpi 预置%d：纵向行程 ≥ 4 格（%d ≥ %d）",
                                 WINS[wi].w, WINS[wi].h, DPIS[di], preset, over, 4 * L.cell);
                        uexpect(sb, over >= 4 * L.cell, tag);
                    }
                        // （"行程够不够长"由上面的"≥ 4 格"断言覆盖）

                    // 单格不允许缩到目标的一半以下（点不准）
                    snprintf(tag, sizeof tag, "%dx%d@%ddpi 预置%d：单格不低于目标的 60%%",
                             WINS[wi].w, WINS[wi].h, DPIS[di], preset);
                    uexpect(sb, L.cell >= target * 60 / 100, tag);
                    cases++;
                }
            }
        }
        sb_addf(sb, "    覆盖 %d 种「窗口 × 密度 × 难度」；其中需拖 %d 种、收缩 %d 种\n",
                cases, panned, shrunk);
        sb_addf(sb, "26 拖动行程不会短到没用：通过\n");
    }

    // ---- 浮层几何：按钮必须在面板内；自定义面板的加减按钮要能用 ----
    //
    // 用户报的两个问题就是这组断言要锁的：
    //   1) 玩法/关于的提示框"大小和文字不匹配" —— 面板与按钮各算各的几何，
    //      按钮画到了面板外 104px
    //   2) 自定义面板"不能改数字也不能点确定" —— 确定按钮同样在面板外（点不到），
    //      而且安卓没有软键盘，点输入框根本打不出字
    {
        static const struct { int32_t w, h; } WINS[] = {
            { 1920, 1080 }, { 2712, 1220 }, { 1600, 720 }, { 1280, 720 }, { 1024, 600 },
        };
        const int32_t kinds[4] = { UI_OVERLAY_HELP, UI_OVERLAY_ABOUT, UI_OVERLAY_BEST, UI_OVERLAY_CUSTOM };
        const char *kn[4] = { "玩法", "关于", "纪录", "自定义" };
        int32_t checked = 0;
        for (size_t wi = 0; wi < sizeof WINS / sizeof WINS[0]; wi++) {
            for (int32_t ki = 0; ki < 4; ki++) {
                Ui u;
                ui_init(&u, WINS[wi].w, WINS[wi].h);
                ui_set_cell_px(&u, ui_cell_px_for_dpi(280));
                ui_cmd_preset(&u, 2);
                ui_refresh_scale(&u);
                if (kinds[ki] == UI_OVERLAY_CUSTOM) ui_cmd_open_custom(&u);
                else u.overlay = kinds[ki];

                UiRect p;
                ui_overlay_panel_rect(&u, &p);
                char tag[192];

                // 面板必须落在窗口内
                snprintf(tag, sizeof tag, "%dx%d %s：面板在窗口内", WINS[wi].w, WINS[wi].h, kn[ki]);
                uexpect(sb, p.x >= 0 && p.y >= 0 && p.x + p.w <= WINS[wi].w && p.y + p.h <= WINS[wi].h, tag);

                // 每个按钮都必须完整落在面板内（这是原来坏掉的地方）
                UiRect b0;
                const int32_t nb = ui_overlay_button_rect(&u, 0, &b0);
                uexpect(sb, nb >= 1, "浮层至少有一个按钮");
                for (int32_t i = 0; i < nb; i++) {
                    UiRect b;
                    ui_overlay_button_rect(&u, i, &b);
                    snprintf(tag, sizeof tag, "%dx%d %s：按钮%d(%s) 完整在面板内",
                             WINS[wi].w, WINS[wi].h, kn[ki], i, ui_overlay_button_label(i));
                    uexpect(sb, b.x >= p.x && b.y >= p.y &&
                                b.x + b.w <= p.x + p.w && b.y + b.h <= p.y + p.h, tag);
                }

                if (kinds[ki] == UI_OVERLAY_CUSTOM) {
                    const int32_t hh = ui_layout(&u).hud;
                    for (int32_t i = 0; i < 6; i++) {
                        UiRect f, au, ad;
                        ui_custom_field_rect(&u, i, &f);
                        ui_custom_arrow_rect(&u, i, true, &au);
                        ui_custom_arrow_rect(&u, i, false, &ad);
                        // 值框、上下箭头都要在面板内
                        snprintf(tag, sizeof tag, "%dx%d 自定义：第%d行 值框在面板内",
                                 WINS[wi].w, WINS[wi].h, i);
                        uexpect(sb, f.x >= p.x && f.x + f.w <= p.x + p.w &&
                                    f.y >= p.y && f.y + f.h <= p.y + p.h, tag);
                        snprintf(tag, sizeof tag, "%dx%d 自定义：第%d行 上下箭头都在面板内且不重叠",
                                 WINS[wi].w, WINS[wi].h, i);
                        uexpect(sb, au.x + au.w <= p.x + p.w && ad.x + ad.w <= p.x + p.w &&
                                    au.y + au.h <= ad.y && ad.y + ad.h <= f.y + f.h + 1 * hh, tag);
                        // 箭头要有可点面积（别小到点不中）
                        snprintf(tag, sizeof tag, "%dx%d 自定义：第%d行 箭头不小于 3h×3h",
                                 WINS[wi].w, WINS[wi].h, i);
                        uexpect(sb, au.w >= 3 * hh && au.h >= 3 * hh, tag);
                        // 标签不能被值框压住
                        {
                            const int32_t lw = font_text_width(NULL, ui_custom_field_label(i), ui_font_sm(&u));
                            UiRect pl;
                            ui_overlay_panel_rect(&u, &pl);
                            const int32_t col_w = pl.w / 2;
                            const int32_t col = i / 3;
                            const int32_t lx = pl.x + col * col_w + 4 * hh;
                            snprintf(tag, sizeof tag, "%dx%d 自定义：第%d行 标签不压值框",
                                     WINS[wi].w, WINS[wi].h, i);
                            uexpect(sb, lx + lw <= f.x, tag);
                        }
                        // **字段不能压到底部按钮行**（这条原来漏了，第 6 行掉到按钮上了）
                        snprintf(tag, sizeof tag, "%dx%d 自定义：第%d行 不压底部按钮行",
                                 WINS[wi].w, WINS[wi].h, i);
                        uexpect(sb, f.y + f.h < p.y + p.h - 12 * hh, tag);
                    }
                    // 最后一行底部与按钮行上沿之间要留出错误提示的位置
                    {
                        UiRect last, b1;
                        ui_custom_field_rect(&u, 5, &last);
                        ui_overlay_button_rect(&u, 1, &b1);
                        snprintf(tag, sizeof tag, "%dx%d 自定义：最后一行与按钮行之间放得下错误提示",
                                 WINS[wi].w, WINS[wi].h);
                        uexpect(sb, last.y + last.h <= b1.y - font_line_h(ui_font_sm(&u)), tag);
                    }
                }
                checked++;
            }
        }
        sb_addf(sb, "    覆盖 %d 种「窗口 × 浮层」\n", checked);
        sb_addf(sb, "27 浮层几何（按钮必须在面板内）：通过\n");
    }

    // ---- 自定义面板：点上下箭头必须真的能改数字，确定必须能生效 ----
    {
        Ui u;
        host_ui_init(&u, 2, HOST_WIN_W, HOST_WIN_H, HOST_CELL_PX);
        ui_cmd_open_custom(&u);

        // 模拟"点一下箭头"：直接用命中判定算出的矩形中心，走真实指针路径
        struct { int32_t idx; bool up; } taps[] = {
            { 0, true }, { 0, true }, { 0, false },      // 高 +1 +1 -1
            { 1, true },                                  // 宽 +1
            { 2, true }, { 2, true }, { 2, true },        // 正实雷 +3
            { 3, false },                                 // 负实雷 -1
        };
        const int32_t h0 = u.dlg_h, w0 = u.dlg_w, t20 = u.dlg_t2, t30 = u.dlg_t3;
        const int32_t t10 = u.dlg_t1, t40 = u.dlg_t4;
        for (size_t i = 0; i < sizeof taps / sizeof taps[0]; i++) {
            UiRect a;
            ui_custom_arrow_rect(&u, taps[i].idx, taps[i].up, &a);
            const int32_t cx = a.x + a.w / 2, cy = a.y + a.h / 2;
            ui_pointer_down(&u, cx, cy, 100 + (int32_t)i * 10);
            ui_pointer_up(&u, cx, cy, 120 + (int32_t)i * 10);
        }
        uexpect(sb, u.dlg_h == h0 + 1, "点两次加、一次减之后高度 +1");
        uexpect(sb, u.dlg_w == w0 + 1, "宽度 +1");
        uexpect(sb, u.dlg_t1 == t10 + 3, "正实雷 +3");
        uexpect(sb, u.dlg_t2 == t20 - 1, "负实雷 -1");
        uexpect(sb, u.dlg_t3 == t30, "没碰过的项不变（正虚雷）");
        uexpect(sb, u.dlg_t4 == t40, "没碰过的项不变（负虚雷）");

        // 边界钳制：高度减到底不能低于 9；每种雷不能减到负数
        for (int32_t i = 0; i < 40; i++) {
            UiRect a;
            ui_custom_arrow_rect(&u, 0, false, &a);
            ui_pointer_down(&u, a.x + a.w / 2, a.y + a.h / 2, 5000 + i * 10);
            ui_pointer_up(&u, a.x + a.w / 2, a.y + a.h / 2, 5005 + i * 10);
        }
        uexpect(sb, u.dlg_h == 9, "高度连减到底被钳在 9");
        for (int32_t i = 0; i < 40; i++) {
            UiRect a;
            ui_custom_arrow_rect(&u, 2, false, &a);
            ui_pointer_down(&u, a.x + a.w / 2, a.y + a.h / 2, 6000 + i * 10);
            ui_pointer_up(&u, a.x + a.w / 2, a.y + a.h / 2, 6005 + i * 10);
        }
        uexpect(sb, u.dlg_t1 == 0, "雷数连减到底被钳在 0");

        // "确定"必须真的生效并关掉面板
        ui_cmd_open_custom(&u);          // 重新给一个合法盘面
        UiRect ok;
        ui_overlay_button_rect(&u, 1, &ok);
        uexpect(sb, strcmp(ui_overlay_button_label(1), (const char *)ui_strings[UI_S_CUSTOM_OK].bytes) == 0,
                "第二个按钮确实是「确定」");
        ui_pointer_down(&u, ok.x + ok.w / 2, ok.y + ok.h / 2, 7000);
        ui_pointer_up(&u, ok.x + ok.w / 2, ok.y + ok.h / 2, 7030);
        uexpect(sb, u.overlay == UI_OVERLAY_NONE, "点「确定」能关掉面板");
        uexpect(sb, u.game.w == u.dlg_w && u.game.h == u.dlg_h, "点「确定」后生效到盘面");

        // "取消"不生效
        const int32_t gw = u.game.w;
        ui_cmd_open_custom(&u);
        UiRect cc;
        ui_overlay_button_rect(&u, 2, &cc);
        {
            UiRect a;
            ui_custom_arrow_rect(&u, 1, true, &a);   // 先改一下宽度
            ui_pointer_down(&u, a.x + a.w / 2, a.y + a.h / 2, 8000);
            ui_pointer_up(&u, a.x + a.w / 2, a.y + a.h / 2, 8010);
        }
        ui_pointer_down(&u, cc.x + cc.w / 2, cc.y + cc.h / 2, 8100);
        ui_pointer_up(&u, cc.x + cc.w / 2, cc.y + cc.h / 2, 8130);
        uexpect(sb, u.overlay == UI_OVERLAY_NONE, "点「取消」关掉面板");
        uexpect(sb, u.game.w == gw, "「取消」不改变盘面宽度");
        sb_addf(sb, "28 自定义面板加减按钮与确定：通过\n");
    }

    // ---- 滚动条：带子宽度 = 一个格子；拖动滑块必须能移动地图 ----
    // 用户的要求：把"落格区放不下整格时剩下的零头灰边"（不同分辨率下 23~476px 不等）
    // 换成宽度规范（= 一个格子边长）的滚动条，并且拖动它来移动地图。
    {
        static const struct { int32_t w, h; } WINS[] = {
            { 1920, 1080 }, { 2712, 1220 }, { 2400, 1080 }, { 1600, 720 },
            { 1280, 720 }, { 1024, 600 }, { 2560, 1600 }, { 2208, 1768 },
        };
        int32_t rolled = 0;
        for (size_t wi = 0; wi < sizeof WINS / sizeof WINS[0]; wi++) {
            Ui u;
            ui_init(&u, WINS[wi].w, WINS[wi].h);
            ui_set_cell_px(&u, ui_cell_px_for_dpi(280));
            ui_cmd_preset(&u, 2);
            ui_refresh_scale(&u);
            const UiLayout L = ui_layout(&u);
            char tag[192];

            // 滚动条带厚度 = 单格边长；轨道长度 = 条带矩形的长边
            {
                UiRect hb, vbr;
                ui_scrollbar_rect(&u, true, &hb);
                ui_scrollbar_rect(&u, false, &vbr);
                snprintf(tag, sizeof tag, "%dx%d：滚动条厚度 ≥ 一格（不窄于手指目标）",
                         WINS[wi].w, WINS[wi].h);
                uexpect(sb, L.sb_thick == L.cell && hb.h >= L.cell && vbr.w >= L.cell, tag);

                const int32_t view_l = L.vis_x;
                const int32_t view_t = L.vis_y;
                const int32_t view_r = view_l + L.vis_cols * L.cell;
                const int32_t view_b = view_t + L.vis_rows * L.cell;
                // **格子与滚动条之间不许有缝**：
                //   横条贴在视口正下方 → 左端 = 视口左边、上端 = 视口下边
                //   纵条贴在视口正右方 → 上端 = 视口上边、左端 = 视口右边
                snprintf(tag, sizeof tag, "%dx%d：横条在视口正下方（x=%d y=%d）",
                         WINS[wi].w, WINS[wi].h, view_l, view_b);
                uexpect(sb, hb.x == view_l && hb.y == view_b, tag);
                snprintf(tag, sizeof tag, "%dx%d：纵条在视口正右方（x=%d y=%d）",
                         WINS[wi].w, WINS[wi].h, view_r, view_t);
                uexpect(sb, vbr.x == view_r && vbr.y == view_t, tag);
                // **轨道**要铺满到棋盘内边缘：否则剩下的零头是一条什么都没画的
                // 纯面板灰边（用户报的"画面下方还有灰边"）。
                // **滑块厚度**必须严格一格：跟着轨道一起变厚就成了"滚动条有两格宽"
                // （用户报的）。两层几何分开，别混。
                const int32_t inner_r = L.board_x + L.board_w - L.box;
                const int32_t inner_b = L.board_y + L.board_h - L.box;
                UiRect htr, vtr;
                ui_scrollbar_track_rect(&u, true, &htr);
                ui_scrollbar_track_rect(&u, false, &vtr);
                snprintf(tag, sizeof tag, "%dx%d：横轨道铺满到棋盘内右边缘（%d）",
                         WINS[wi].w, WINS[wi].h, inner_r);
                uexpect(sb, htr.x + htr.w == inner_r, tag);
                snprintf(tag, sizeof tag, "%dx%d：横轨道铺满到棋盘内下边缘（%d）",
                         WINS[wi].w, WINS[wi].h, inner_b);
                uexpect(sb, htr.y + htr.h == inner_b, tag);
                snprintf(tag, sizeof tag, "%dx%d：纵轨道铺满到棋盘内右下边缘", WINS[wi].w, WINS[wi].h);
                uexpect(sb, vtr.x + vtr.w == inner_r && vtr.y + vtr.h == inner_b, tag);
                snprintf(tag, sizeof tag, "%dx%d：横滑块厚度 = 一格（%d == %d）",
                         WINS[wi].w, WINS[wi].h, hb.h, L.cell);
                uexpect(sb, hb.h == L.cell, tag);
                snprintf(tag, sizeof tag, "%dx%d：纵滑块厚度 = 一格（%d == %d）",
                         WINS[wi].w, WINS[wi].h, vbr.w, L.cell);
                uexpect(sb, vbr.w == L.cell, tag);
                // 两条在右下角重叠时，命中必须归**横向** —— 与绘制顺序（横向后画）一致
                {
                    const int32_t ox = vbr.x + 4;
                    const int32_t oy = htr.y + htr.h - 4;   // 轨道最底部（含零头）
                    snprintf(tag, sizeof tag, "%dx%d：轨道底部（零头处）命中横向", WINS[wi].w, WINS[wi].h);
                    uexpect(sb, ui_hit_test(&u, ox, oy, NULL) == UI_HIT_SB_H, tag);
                    snprintf(tag, sizeof tag, "%dx%d：纵条上方段命中纵向", WINS[wi].w, WINS[wi].h);
                    uexpect(sb, ui_hit_test(&u, ox, vtr.y + 4, NULL) == UI_HIT_SB_V, tag);
                }
                // 轨道非空、滑块长度合法（滑块按视口尺寸换算）
                snprintf(tag, sizeof tag, "%dx%d：轨道与滑块长度合法", WINS[wi].w, WINS[wi].h);
                uexpect(sb, htr.w > 0 && vtr.h > 0 &&
                            L.sb_thumb_h_w > 0 && L.sb_thumb_h_w <= htr.w &&
                            L.sb_thumb_v_h > 0 && L.sb_thumb_v_h <= vtr.h, tag);
            }

            // 有行程就必须"够长"，否则拖动会一碰到底（见 MIN_DRAG_CELLS）
            const int32_t over_x = (int32_t)u.game.w * L.cell - L.vis_cols * L.cell;
            const int32_t over_y = (int32_t)u.game.h * L.cell - L.vis_rows * L.cell;
            snprintf(tag, sizeof tag, "%dx%d：横向行程要么为 0 要么 ≥ 4 格", WINS[wi].w, WINS[wi].h);
            uexpect(sb, over_x == 0 || over_x >= 4 * L.cell, tag);
            snprintf(tag, sizeof tag, "%dx%d：纵向行程要么为 0 要么 ≥ 4 格", WINS[wi].w, WINS[wi].h);
            uexpect(sb, over_y == 0 || over_y >= 4 * L.cell, tag);

            // 菜单栏只占侧栏那一条：右侧上方现在是棋盘（点击要落到棋盘格上），
            // 菜单栏高度之外的一切都不该被当成菜单/侧栏。
            {
                int32_t idx = -1;
                snprintf(tag, sizeof tag, "%dx%d：棋盘区最上方是棋盘格而非菜单", WINS[wi].w, WINS[wi].h);
                uexpect(sb, ui_hit_test(&u, L.vis_x + 2, L.vis_y + 2, &idx) == UI_HIT_BOARD, tag);
                snprintf(tag, sizeof tag, "%dx%d：侧栏右边界之外、菜单栏下方的点不是侧栏",
                         WINS[wi].w, WINS[wi].h);
                uexpect(sb, ui_hit_test(&u, L.side_w + 2, 2, &idx) != UI_HIT_MENU &&
                            ui_hit_test(&u, L.side_w + 2, 2, &idx) != UI_HIT_SIDEBAR, tag);
            }

            // 棋盘超出视口时：滑块必须短于轨道，且拖动滑块能改变偏移
            if (over_x > 0) {
                rolled++;
                Ui t = u;
                const UiLayout T0 = ui_layout(&t);
                UiRect hb0;
                ui_scrollbar_rect(&t, true, &hb0);
                const int32_t track = hb0.w;
                const int32_t thumb = T0.sb_thumb_h_w;
                const int32_t cy = hb0.y + hb0.h / 2;
                snprintf(tag, sizeof tag, "%dx%d：横向可滚动时滑块短于轨道", WINS[wi].w, WINS[wi].h);
                uexpect(sb, thumb < track, tag);
                // 按住滑块中心，再往右拖到 3/4 处。
                // 注意：**点滑块本身不该跳转**（那是正确行为），必须"按住 + 拖动"。
                const int32_t grab_x = hb0.x + T0.sb_thumb_h_x + thumb / 2;
                ui_pointer_down(&t, grab_x, cy, 100);
                uexpect(sb, t.sb_drag == 1, "按在横向滚动条上会进入滚动条拖动");
                const int32_t want = (track - thumb) * 3 / 4;
                ui_pointer_move(&t, hb0.x + want + thumb / 2, cy, 150);
                const UiLayout T1 = ui_layout(&t);
                snprintf(tag, sizeof tag, "%dx%d：把滑块拖到 3/4 处，pan 也到 3/4 附近（实得 %d/%d）",
                         WINS[wi].w, WINS[wi].h, T1.pan_x, over_x);
                uexpect(sb, T1.pan_x > over_x / 2 && T1.pan_x <= over_x, tag);
                // 继续拖到底：pan 到最大
                ui_pointer_move(&t, hb0.x + track + 500, cy, 180);
                const UiLayout T2 = ui_layout(&t);
                snprintf(tag, sizeof tag, "%dx%d：滑块拖到底 pan = 最大偏移", WINS[wi].w, WINS[wi].h);
                uexpect(sb, T2.pan_x == over_x, tag);
                ui_pointer_up(&t, hb0.x + track, cy, 200);
                uexpect(sb, t.sb_drag == 0, "松手后退出滚动条拖动");
                // 松手不该把偏移弹回去
                const UiLayout T3 = ui_layout(&t);
                uexpect(sb, T3.pan_x == over_x, "松手后偏移保持（不弹回）");
            }
            if (over_y > 0) {
                Ui t = u;
                const UiLayout T0 = ui_layout(&t);
                UiRect vb0;
                ui_scrollbar_rect(&t, false, &vb0);
                const int32_t cx = vb0.x + vb0.w / 2;
                const int32_t thumb = T0.sb_thumb_v_h;
                // 按住滑块中心（保证落在这条滚动条内），再往下拖到底。
                // 与横向一样用"相对位移"，不是把手指移到目标值。
                const int32_t cy0 = vb0.y + thumb / 2;
                ui_pointer_down(&t, cx, cy0, 300);
                uexpect(sb, t.sb_drag == 2, "按在纵向滚动条上会进入纵向拖动");
                ui_pointer_move(&t, cx, cy0 + vb0.h + thumb + 1000, 350);
                const UiLayout T1 = ui_layout(&t);
                snprintf(tag, sizeof tag, "%dx%d：纵向滑块拖到底 pan = 最大偏移", WINS[wi].w, WINS[wi].h);
                uexpect(sb, T1.pan_y == over_y, tag);
                ui_pointer_up(&t, cx, cy0 + vb0.h + thumb + 1000, 400);
            }
        }
        sb_addf(sb, "    覆盖 %d 种窗口（其中 %d 种横向需要滚动）\n",
                (int32_t)(sizeof WINS / sizeof WINS[0]), rolled);
        sb_addf(sb, "29 滚动条（带宽 = 一格、可拖动移图）：通过\n");
    }

    (void)g_backup;
    sb_addf(sb, "\n断言 %d 项，失败 %d 项\n", g_ui_checks, g_ui_fails);
    sb_addf(sb, "%s\n", g_ui_fails == 0 ? "全部通过" : "存在失败");
    return g_ui_fails;
}

// ---------------------------------------------------------------- main
int main(int argc, char **argv) {
    const char *selftest_path = NULL;
    const char *uitest_path = NULL;
    const char *shot_path = NULL;
    const char *dump_path = NULL;
    const char *overlay = NULL;
    int demo = DEMO_NONE;
    int zoom = 0;          // HUD 缩放档（单格尺寸由物理尺寸定，不再有档位概念）
    int scale = 2;         // 预览放大倍率（只为看清楚，不影响布局）
    int32_t pan_x = -1, pan_y = -1;   // --pan x y：预览拖动后的画面

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest") && i + 1 < argc) selftest_path = argv[++i];
        else if (!strcmp(argv[i], "--uitest") && i + 1 < argc) uitest_path = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
        else if (!strcmp(argv[i], "--rules-dump") && i + 1 < argc) dump_path = argv[++i];
        else if (!strcmp(argv[i], "--overlay") && i + 1 < argc) overlay = argv[++i];
        else if (!strcmp(argv[i], "--demo") && i + 1 < argc) demo = demo_from_name(argv[++i]);
        else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) zoom = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pan") && i + 2 < argc) {
            pan_x = atoi(argv[++i]);
            pan_y = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--list-demos")) {
            printf("mid lose win custom flags open\n");
            return 0;
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("用法: cs_android_host [--selftest 报告] [--uitest 报告] [--shot PNG]\n"
                   "       [--rules-dump 文件] [--demo mid|lose|win|custom|flags|open]\n"
                   "       [--overlay help|about|best|custom|menu] [--zoom 1|2] [--scale n]\n"
                   "       [--pan x y]（预览拖动后的画面；x/y 为设备像素偏移）\n");
            return 0;
        }
    }

    int rc = 0;
    if (selftest_path) {
        const uint32_t fails = run_selftest(selftest_path);
        printf("规则自检：%s（报告 %s）\n", fails == 0 ? "全部通过" : "存在失败", selftest_path);
        if (fails) rc = 1;
    }
    if (uitest_path) {
        const uint32_t fails = run_uitest(uitest_path);
        printf("界面自检：%s（报告 %s）\n", fails == 0 ? "全部通过" : "存在失败", uitest_path);
        if (fails) rc = 1;
    }
    if (dump_path) {
        rc |= (int)run_rules_dump(dump_path);
    }
    if (shot_path) {
        if (!render_shot(shot_path, demo, zoom, scale, overlay, pan_x, pan_y)) {
            fprintf(stderr, "渲染截图失败\n");
            rc = 1;
        }
    }
    if (!selftest_path && !uitest_path && !dump_path && !shot_path) {
        printf("没有指定动作。用 --help 看用法。\n");
    }
    return rc;
}
