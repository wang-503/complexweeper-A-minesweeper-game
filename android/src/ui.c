#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "assets.h"       // SP_* 槽位下标（由 tools/gen_ui_strings.js 生成）
#include "ui_strings.h"

// 显示值 D = |S|^2 → 数字贴图，直接来自生成的头文件（sp_num_by_D）。
// 早先这里手抄了一份按字母序猜的下标，结果整块界面都在贴错图。

// ---------------------------------------------------------------- 布局常量
// 尺寸都写成 "单位 × hud"，hud 是 HUD 自己的整数缩放（与棋盘单格尺寸无关）。
// 原始素材尺寸：LED 数字 13×23、格子/图标 16×16、人脸 24×24。
//
// 这几个常量要和 tools/ui_strings.js 里的字号一起调：
// 字号变大之后，面板/菜单栏必须跟着变高变宽，否则中文会顶到边框上。
// 当前字号是 sm17 / md23 / lg31（原来 12/16/22）。
#define L_FACE_SIZE 24
#define L_REAL_DIGITS 4
#define L_IMAG_DIGITS 3
#define L_PANEL_H 38        // 计雷器/计时器面板高度（hud=4 时 152px，装得下 23h 的 LED）
#define L_MENUBAR_UNIT 50   // 菜单栏高度（配 md29 的字）
#define L_MENU_ITEM_W 26    // 菜单按钮宽度 = 26h：md29 的两个汉字约 58px，左右各留约 23px
// 菜单栏高度/按钮尺寸的"文字保底"：菜单栏上下各留这么多余量（像素，非 ×hud）。
// 字号是固定像素、hud 只随窗口像素尺寸走，所以必须按字号保底，
// 否则高密度屏（像素多但 hud 不够大）上菜单中文会被裁掉。
#define L_MENUBAR_TEXT_PAD 8

// 棋盘视口的下限：极端窗口下也不至于只剩一两格。
// 单元格尺寸的安全钳制会用到它。
#define UI_MIN_VIS_COLS 6
#define UI_MIN_VIS_ROWS 5
// 侧栏最小宽度：比这窄就把 HUD 缩到 1。
#define UI_MIN_SIDE_W 200

static int32_t pow10i(int32_t n) {
    int32_t r = 1;
    for (int32_t i = 0; i < n; i++) r *= 10;
    return r;
}

static int32_t value_digits(int32_t base, int32_t v, bool has_v) {
    if (!has_v) return base;
    if (v >= 0) return (v <= pow10i(base) - 1) ? base : base + 1;
    return (v >= -(pow10i(base - 1) - 1)) ? base : base + 1;
}
static bool panel_imag(int32_t t) { return t == 3 || t == 4; }
static int32_t panel_value_digits(bool imag, int32_t v, bool has_v) {
    return value_digits(imag ? L_IMAG_DIGITS : L_REAL_DIGITS, v, has_v);
}
static int32_t panel_cells(bool imag, int32_t v, bool has_v) {
    return panel_value_digits(imag, v, has_v) + (imag ? 1 : 0);
}
// 计雷器面板宽度：图标 + 留白 + N 格 LED + 边框
static int32_t counter_width(int32_t h, int32_t digits) {
    return 16 * h + 2 * h + digits * 13 * h + 2 * h;
}
static int32_t timer_width(int32_t h) { return 4 * 13 * h + 2 * h; }
static int32_t counter_value(const Ui *ui, int32_t t) {
    const int32_t k = game_unmarked(&ui->game, (size_t)t);
    return (t == 2 || t == 4) ? -k : k;
}
static bool counter_shown(const Ui *ui, int32_t t, int32_t *out) {
    if (!ui->game.started) return false;
    *out = counter_value(ui, t);
    return true;
}
// 侧栏里最宽的那块计雷器（四块按最坏情况算：实雷四格数字，虚雷三格 + i）
static int32_t counters_width(const Ui *ui, int32_t h) {
    int32_t widest = 0;
    for (int32_t t = 1; t < 5; t++) {
        int32_t v = 0;
        const bool has = counter_shown(ui, t, &v);
        const int32_t cw = counter_width(h, panel_cells(panel_imag(t), v, has));
        if (cw > widest) widest = cw;
    }
    return widest;
}
// 侧栏里从上到下依次是：笑脸（大方块，点它重开）+ 四个计雷器 + 计时器
static int32_t sidebar_content_w(const Ui *ui, int32_t h) {
    int32_t w = counters_width(ui, h);
    // 笑脸按 1.5 格算：比一格大一点，好点又不会把侧栏撑满
    const int32_t face = ui->cell_px * 3 / 2;
    if (face > w) w = face;
    const int32_t tm = timer_width(h);
    if (tm > w) w = tm;
    return w;
}
// 侧栏内容块的高度（按真实 cell 算，因为笑脸是 1.5 格）
static int32_t sidebar_content_h(const Ui *ui, int32_t h) {
    const int32_t face = ui->cell_px * 3 / 2;
    return face + 2 * h + 4 * (L_PANEL_H * h) + 3 * h + L_PANEL_H * h + 2 * h;
}
// 整个侧栏内容块的顶边：在可用高度里垂直居中，别把东西都堆在上面、下面留一大片空。
// 内容比侧栏还高时贴顶（此时已经放不下，pick_hud_scale 会先一步把 hud 降下来）。
static int32_t sidebar_content_top(const Ui *ui, const UiLayout *L) {
    const int32_t content_h = sidebar_content_h(ui, L->hud);
    int32_t top = L->side_in_y + (L->side_in_h - content_h) / 2;
    if (top < L->side_in_y) top = L->side_in_y;
    return top;
}

static int32_t timer_seconds(const Ui *ui) {
    if (!ui->game.started) return 0;
    const uint32_t s = ui->game.elapsed_ms / 1000u;
    return (int32_t)(s > 9999u ? 9999u : s);
}

// 从大到小挑一个能让侧栏装下的 HUD 缩放（整数，避免 LED/人脸出现不均匀的块）
// 前向声明：pick_hud_scale 要用真实排版判断"装不装得下"（见那里的说明）
static void timer_rect_of(const Ui *ui, const UiLayout *L, UiRect *out);
// 前向声明：滚动条按下时要把滑块位置换算成偏移
static void ui_sb_apply(Ui *ui, int32_t pos);
// 滚动条矩形的**唯一定义**（绘制 / 命中 / ui_layout 的滑块换算都用它）。
// 定义在下面，这里先声明，避免出现"两份几何各自算一遍"的老毛病。
static void sb_h_rect(const UiLayout *L, UiRect *out);
static void sb_v_rect(const UiLayout *L, UiRect *out);
static int32_t sb_h_track(const UiLayout *L);
static int32_t sb_v_track(const UiLayout *L);

static int32_t pick_hud_scale(const Ui *ui, int32_t side_w, int32_t side_h) {
    // 上限 5：字号变大后 hud=5 能让整个 HUD（含文字）更醒目。
    //
    // **用真实排版来判断放不放得下**，不要另写一套高度公式：
    // 这里踩过两次同样的坑（浮层几何、侧栏高度），都是"两套公式各算各的"，
    // 结果只要有一边改了间距，另一边就悄悄失配 —— 表现为计时器被挤出屏幕。
    for (int32_t h = 5; h >= 1; h--) {
        const int32_t w = sidebar_content_w(ui, h) + 6 * h;   // 6h = 两边内边距
        if (w > side_w) continue;
        Ui probe = *ui;
        probe.hud_scale = h;                                  // 让 ui_layout 按这个 hud 排版
        const UiLayout L = ui_layout(&probe);
        UiRect tmr;
        timer_rect_of(&probe, &L, &tmr);
        if (tmr.y + tmr.h <= L.side_y + L.side_h) return h;
    }
    (void)side_h;
    return 1;
}

// ---------------------------------------------------------------- 拖动偏移的钳制
//
// 偏移**不再吸附到整格**：拖到哪就停在哪，屏幕边缘出现不完整的格子是正常的，
// 这样手感连续（"对齐"会让画面在松手瞬间跳一下，很出戏）。
//
// 只保留两件事：
//   1. 钳在合法区间（棋盘盖满视口，不露棋盘外的空白）
//   2. 棋盘比视口小时居中
//
// 历史：早先这里有个"吸附到整格"的版本，而且拖动中也用同一套，
// 于是任何不足一格的位移都被 `v - v % cell` 归零 —— 表现是"拖动完全没反应"。
// 现在统一成只钳制，拖动中与松手后行为一致。
static int32_t clamp_pan(int32_t v, int32_t span) {
    if (span <= 0) return span / 2;      // 装得下 → 居中（负值）
    if (v < 0) v = 0;
    if (v > span) v = span;
    return v;
}
static int32_t normalize_pan(int32_t v, int32_t span, int32_t cell) {
    (void)cell;                          // 保留参数只为兼容调用点，不再做整格吸附
    return clamp_pan(v, span);
}

//   - pan 吸附到整格并钳在 [0, 棋盘-视口]：拖动不会露白、也不会显示半格
//   - 棋盘比视口小的那一轴居中（用负偏移表示"居中"，对外仍是不可拖）

// ---------------------------------------------------------------- 字号层
// 三个档位（sm/md/lg）在每个 Ui 上都按 font_layer 解析。
// 为什么要这样：字形是**固定像素高度的光栅图**，不能任意缩放；
// 而不同 dpi 的设备要得到相同物理字号就需要不同像素高度，
// 所以预生成 100%/125%/150% 三层，运行时按密度挑最接近的一层。
static int32_t fs_of(const Ui *ui, int32_t kind) {
    return font_slot(ui ? ui->font_layer : 0, kind);
}
#define FS_SM(ui) fs_of((ui), 0)
#define FS_MD(ui) fs_of((ui), 1)
#define FS_LG(ui) fs_of((ui), 2)

// 基准单格（280dpi → 77px）：字号层的换算基准。
#define UI_BASE_CELL_PX 77
// 每个轴至少要留出这么多格的可拖行程。低于它拖动会"一碰就到底"、
// 之后画面完全不动（实测反馈"拖动完全不平滑"）。
#define MIN_DRAG_CELLS 4
// 该用哪一层：字号要跟单格尺寸成比例（单格是 0.7cm 的物理常量，
// 所以"跟单格成比例"就等于"跟屏幕密度成比例"）。
static int32_t font_layer_for_cell(int32_t base_cell) {
    if (base_cell < 1) base_cell = UI_BASE_CELL_PX;
    // pct = base_cell / 77 × 100 → 100 以下取 100；125 起二层；150 起三层
    const int32_t pct = (base_cell * 100 + UI_BASE_CELL_PX / 2) / UI_BASE_CELL_PX;
    if (pct >= 150) return 2;
    if (pct >= 125) return 1;
    return 0;
}

// 菜单栏高度与菜单按钮宽度：**按字号实测自适应**，不再只靠 × hud。
//
// 为什么要这么做（真机 + 多密度实测出来的）：
// 字号是固定像素高度（md23），而 hud 只随"窗口像素尺寸"变化、**不随屏幕密度变化**。
// 于是高密度手机上会出现"像素很多、但 hud 不够大"的组合：
// 2160×1080 @440dpi 时 hud=4，26h 的菜单栏只有 144px 高，而 md 的行高就有 31px ——
// 中文被上下裁掉。单元测试之所以没抓到，是因为它只看几何、不看"字是否放得下"。
//
// 现在把菜单栏高度与按钮宽度都按"当前字号实测"来定：
// 密度越大 → 字号占的像素越少相对而言 → hud 越大 → 菜单栏按比例更高，
// 而低 hud（小屏）时又保证至少装得下字。两头都稳。
static int32_t menubar_text_h(const Ui *ui) {
    // 用字形表实测"md 字号里所有字形的墨水总高度"（汉字比拉丁字母高）。
    // 不用行高：行高含给下伸部预留的空隙，拿它保底会让菜单栏偏高。
    return font_ink_height(FS_MD(ui));
}
// 菜单按钮宽度 = max(比例值, 装得下"游戏"两个字的最小值)
static int32_t menu_item_w(const Ui *ui, int32_t hh) {
    int32_t w = L_MENU_ITEM_W * hh;
    const int32_t tw = font_text_width(NULL, (const char *)ui_strings[UI_S_MENU_GAME].bytes, FS_MD(ui));
    const int32_t need = tw + 10 * hh;
    if (w < need) w = need;
    return w;
}

UiLayout ui_layout(const Ui *ui) {
    UiLayout L;
    memset(&L, 0, sizeof(L));
    L.win_w = ui->win_w > 0 ? ui->win_w : 1;
    L.win_h = ui->win_h > 0 ? ui->win_h : 1;

    // ---- 顶部菜单栏（只占**左侧栏宽度**）+ 左侧信息栏 ----
    // 侧栏宽度 = 窗口**长边** / 5（横屏下即宽度的 1/5），并且至少 UI_MIN_SIDE_W
    const int32_t long_edge = L.win_w > L.win_h ? L.win_w : L.win_h;
    int32_t side_w = long_edge / 5;
    const int32_t side_w_cap = L.win_w / 2;   // 别把棋盘挤没了
    if (side_w > side_w_cap) side_w = side_w_cap;
    if (side_w < UI_MIN_SIDE_W) side_w = UI_MIN_SIDE_W;
    if (side_w > L.win_w - 32) side_w = L.win_w > 32 ? L.win_w - 32 : L.win_w;
    if (side_w < 1) side_w = 1;

    L.hud = ui->hud_scale > 0 ? ui->hud_scale : 1;
    // 菜单栏高度：比例值与"装得下中文字"两者取大。
    // 这样 hud 很大的高密度设备上不会被字顶穿，hud=1 的小屏也保证字放得下。
    {
        int32_t mh = L_MENUBAR_UNIT * L.hud;
        const int32_t need = menubar_text_h(ui) + 2 * L_MENUBAR_TEXT_PAD;
        if (mh < need) mh = need;
        L.menubar_h = mh;
    }

    // **菜单栏只在左侧栏那一条里**（不再横跨整个窗口顶部）。
    // 这样棋盘可以从窗口最上沿开始，把原来菜单栏右边那片空白吃进地图里，
    // 纵向多出整整一条菜单栏的高度（1920x1080 上是 150px ≈ 2 行格子）。
    L.side_x = 0;
    L.side_y = L.menubar_h;
    L.side_w = side_w;
    L.side_h = L.win_h - L.menubar_h;
    if (L.side_h < 1) L.side_h = 1;
    // 侧栏内部（去掉一圈边框）
    const int32_t sframe = 1 * L.hud;
    L.side_in_x = L.side_x + sframe;
    L.side_in_y = L.side_y + sframe;
    L.side_in_w = L.side_w - 2 * sframe;
    L.side_in_h = L.side_h - 2 * sframe;

    // ---- 右侧棋盘区：**满高**（从窗口最上沿一直到最下沿）----
    L.board_x = L.side_x + L.side_w;
    L.board_y = 0;
    L.board_w = L.win_w - L.board_x;
    L.board_h = L.win_h;
    if (L.board_w < 1) L.board_w = 1;
    if (L.board_h < 1) L.board_h = 1;
    L.cell = ui->cell_px > 0 ? ui->cell_px : 16;
    // 外框：固定 3h，**不随格子大小变**（"规范边框宽度"）。
    L.box = 3 * L.hud;
    // 单格微调（只在 ±2px 内）：让"整格视口 + 一格宽的滚动条"**正好铺满**棋盘区。
    //
    // 为什么需要：`(棋盘区 − 外框×2 − 一格) % 单格` 这个零头无论怎么取整都会剩下，
    // 屏幕上就是**格子与滚动条之间的一条灰缝**（1920x1080 实测 65px，
    // 用户反复报的"滚动条与游戏界面之间还是有灰框"）。既然要求滚动条恰好一格宽、
    // 可见格又必须是完整格，那就只能让单格稍微变一点点来配合。
    // ±2px 相对 77px 不到 3%，肉眼与手感都无差别，但缝没了。
    {
        const int32_t avail_w = L.board_w - 2 * L.box;
        const int32_t avail_h = L.board_h - 2 * L.box;
        int32_t best = L.cell, best_waste = -1;
        for (int32_t d = -2; d <= 2; d++) {
            const int32_t c = L.cell + d;
            if (c < 4) continue;
            if (avail_w < c || avail_h < c) continue;
            // 用"剩余空间占可用空间的比例"打分：越小越整齐
            const int32_t w1 = (avail_w - c) % c;
            const int32_t w2 = (avail_h - c) % c;
            const int32_t waste = w1 * avail_h + w2 * avail_w;   // 归一化到同一量级
            if (best_waste < 0 || waste < best_waste) { best_waste = waste; best = c; }
        }
        L.cell = best;
    }
    // 滚动条带：**宽度严格等于一个格子边长**（"规范边框宽度"）。
    L.sb_thick = L.cell;
    L.field_x = L.board_x + L.box;
    L.field_y = L.board_y + L.box;
    const int32_t cols = (int32_t)ui->game.w;
    const int32_t rows = (int32_t)ui->game.h;
    // 决定可见格数：可用空间 = 棋盘区 − 两边外框 − 滚动条那条带
    {
        const int32_t avail_w = L.board_w - 2 * L.box - L.sb_thick;
        const int32_t avail_h = L.board_h - 2 * L.box - L.sb_thick;
        int32_t vc = avail_w / L.cell;
        int32_t vr = avail_h / L.cell;
        if (vc < 1) vc = 1;
        if (vr < 1) vr = 1;
        if (vc > cols) vc = cols;      // 棋盘没这么宽就不撑满（小盘居中）
        if (vr > rows) vr = rows;
        L.vis_cols = vc;
        L.vis_rows = vr;
    }
    // ---- 保证"拖得动"：可见格数收到"至少剩 MIN_DRAG_CELLS 格可拖" ----
    //
    // 为什么需要：屏幕越大能显示的列越多，剩下的可拖行程就越少。极端情况是
    // 棋盘比屏幕只大一点点 —— 2712x1220 上专家盘可见 27 列、总 30 列，
    // **行程只有 231px**，手指挪 2cm 就撞到底、之后画面完全不动
    // （实测反馈"拖动完全不平滑"）。所以宁可少显示几列，也要留出拖动余量。
    // **只在这个轴真的放不下整盘时**才收窄。
    {
        const int32_t need_pan = MIN_DRAG_CELLS * L.cell;
        if (cols * L.cell > L.sb_thick + L.vis_cols * L.cell) {
            const int32_t max_vis = (cols * L.cell - need_pan) / L.cell;
            if (L.vis_cols > max_vis && max_vis >= 1) L.vis_cols = max_vis;
        }
        if (rows * L.cell > L.sb_thick + L.vis_rows * L.cell) {
            const int32_t max_vis = (rows * L.cell - need_pan) / L.cell;
            if (L.vis_rows > max_vis && max_vis >= 1) L.vis_rows = max_vis;
        }
        if (L.vis_cols > cols) L.vis_cols = cols;
        if (L.vis_rows > rows) L.vis_rows = rows;
    }
    // 视口贴在棋盘区左上角（外框之内）。
    L.vis_x = L.field_x;
    L.vis_y = L.field_y;
    // ---- 两条滚动条的位置：**起点都是视口边缘**（这样格子与滚动条之间没有缝） ----
    //   横向带：x 从视口右边到棋盘内右，y 从视口下边到棋盘内下
    //   纵向带：y 从视口下边到棋盘内下，x 从视口右边到棋盘内右
    // 两者**不重叠**（它们在右下角相接），所以命中不需要分先后。
    L.sb_x = L.field_x + L.vis_cols * L.cell;
    L.sb_y = L.field_y + L.vis_rows * L.cell;
    {
        const int32_t max_x = L.board_x + L.board_w - L.box - L.sb_thick;
        const int32_t max_y = L.board_y + L.board_h - L.box - L.sb_thick;
        if (L.sb_x > max_x) L.sb_x = max_x;
        if (L.sb_y > max_y) L.sb_y = max_y;
        if (L.sb_x < L.field_x) L.sb_x = L.field_x;
        if (L.sb_y < L.field_y) L.sb_y = L.field_y;
    }
    L.sb_w = L.sb_thick;   // 纵向带厚度（供命中/绘制共用）
    L.sb_h = L.sb_thick;   // 横向带厚度
    L.field_w = L.sb_w;
    L.field_h = L.sb_h;
    if (L.sb_w < 1) L.sb_w = 1;
    if (L.sb_h < 1) L.sb_h = 1;
    if (L.field_w < 1) L.field_w = 1;
    if (L.field_h < 1) L.field_h = 1;

    // ---- 棋盘原点（绘制与命中共用）：拖动 + 钳制 ----
    // 拖动范围**相对视口**算，不是相对落格区：
    // 要让棋盘始终盖满视口，origin 必须落在
    //     [vis_x + view_w - board_w , vis_x]
    // 这个区间里。若按落格区算，滚到最右时右边会露出一条棋盘外的空白
    // （真机实测就是"拖到底右边一片灰"）。
    const int32_t board_px_w = cols * L.cell;
    const int32_t board_px_h = rows * L.cell;
    const int32_t view_px_w = L.vis_cols * L.cell;
    const int32_t view_px_h = L.vis_rows * L.cell;
    if (board_px_w >= view_px_w) {
        int32_t o = L.vis_x - ui->pan_x;
        const int32_t lo = L.vis_x + view_px_w - board_px_w;   // 最右端
        const int32_t hi = L.vis_x;                            // 最左端
        if (o < lo) o = lo;
        if (o > hi) o = hi;
        L.board_origin_x = o;
        L.pan_x = L.vis_x - o;
    } else {
        // 棋盘比视口窄：居中，不拖动
        L.board_origin_x = L.vis_x + (view_px_w - board_px_w) / 2;
        L.pan_x = L.vis_x - L.board_origin_x;
    }
    if (board_px_h >= view_px_h) {
        int32_t o = L.vis_y - ui->pan_y;
        const int32_t lo = L.vis_y + view_px_h - board_px_h;
        const int32_t hi = L.vis_y;
        if (o < lo) o = lo;
        if (o > hi) o = hi;
        L.board_origin_y = o;
        L.pan_y = L.vis_y - o;
    } else {
        L.board_origin_y = L.vis_y + (view_px_h - board_px_h) / 2;
        L.pan_y = L.vis_y - L.board_origin_y;
    }
    // 棋盘实际可画/可点的区域 = 棋盘与视口的交集（可能不覆盖整个视口）
    {
        int32_t x0 = L.board_origin_x > L.vis_x ? L.board_origin_x : L.vis_x;
        int32_t y0 = L.board_origin_y > L.vis_y ? L.board_origin_y : L.vis_y;
        int32_t x1 = L.board_origin_x + board_px_w;
        int32_t y1 = L.board_origin_y + board_px_h;
        const int32_t vx1 = L.vis_x + view_px_w;
        const int32_t vy1 = L.vis_y + view_px_h;
        if (x1 > vx1) x1 = vx1;
        if (y1 > vy1) y1 = vy1;
        L.board_clip_x = x0;
        L.board_clip_y = y0;
        L.board_clip_w = x1 > x0 ? x1 - x0 : 0;
        L.board_clip_h = y1 > y0 ? y1 - y0 : 0;

        // ---- 本帧要画的格号区间 ----
        // 由"棋盘 ∩ 视口"反推，**不能**写成 first + vis_cols：
        // 拖动之后 first 已经不是 0，再加 vis_cols 会多算 first 格，
        // 而画布只画得下 vis_cols 格，于是右侧少画 first 格
        // （真机表现：拖到最右时右边空出整整一个拖动量）。
        // 这里一律按裁剪矩形的边界取整，与绘制/命中共用同一套几何。
        const int32_t b0x = L.board_clip_x;
        const int32_t b0y = L.board_clip_y;
        const int32_t b1x = L.board_clip_x + L.board_clip_w;
        const int32_t b1y = L.board_clip_y + L.board_clip_h;
        L.vis_first_col = b0x > L.board_origin_x ? (b0x - L.board_origin_x) / L.cell : 0;
        L.vis_first_row = b0y > L.board_origin_y ? (b0y - L.board_origin_y) / L.cell : 0;
        L.vis_end_col = L.board_clip_w > 0
                            ? (b1x - L.board_origin_x + L.cell - 1) / L.cell : 0;
        L.vis_end_row = L.board_clip_h > 0
                            ? (b1y - L.board_origin_y + L.cell - 1) / L.cell : 0;
        if (L.vis_end_col > cols) L.vis_end_col = cols;
        if (L.vis_end_row > rows) L.vis_end_row = rows;
        if (L.vis_end_col < L.vis_first_col) L.vis_end_col = L.vis_first_col;
        if (L.vis_end_row < L.vis_first_row) L.vis_end_row = L.vis_first_row;
    }

    // ---- 滚动条滑块 ----
    // 滑块长度 ∝ 可见比例：可见/全盘。全盘装得下时滑块铺满整条轨道
    // （那时本来也没得拖，滑块占满就表示"没有可滚动的部分"）。
    // 位置 ∝ 当前偏移/最大偏移。
    {
        // 轨道长度 = **视口尺寸**（与绘制/命中共用同一套定义）
        const int32_t track_x = sb_h_track(&L);
        const int32_t track_y = sb_v_track(&L);
        // 横向
        if (board_px_w > view_px_w && board_px_w > 0) {
            int32_t t = (int32_t)(((int64_t)track_x * view_px_w) / board_px_w);
            if (t < SB_MIN_THUMB) t = SB_MIN_THUMB;
            if (t > track_x) t = track_x;
            L.sb_thumb_h_w = t;
            const int32_t max_pan = board_px_w - view_px_w;
            int32_t p = L.pan_x;
            if (p < 0) p = 0;
            if (p > max_pan) p = max_pan;
            // 注意负偏移表示"居中"，这里只会走 max_pan > 0 的分支
            const int32_t span = track_x - t;
            L.sb_thumb_h_x = span > 0
                ? (int32_t)(((int64_t)span * p) / max_pan) : 0;
        }        else {
            L.sb_thumb_h_w = track_x;
            L.sb_thumb_h_x = 0;
        }
        // 纵向
        if (board_px_h > view_px_h && board_px_h > 0) {
            int32_t t = (int32_t)(((int64_t)track_y * view_px_h) / board_px_h);
            if (t < SB_MIN_THUMB) t = SB_MIN_THUMB;
            if (t > track_y) t = track_y;
            L.sb_thumb_v_h = t;
            const int32_t max_pan = board_px_h - view_px_h;
            int32_t p = L.pan_y;
            if (p < 0) p = 0;
            if (p > max_pan) p = max_pan;
            const int32_t span = track_y - t;
            L.sb_thumb_v_y = span > 0
                ? (int32_t)(((int64_t)span * p) / max_pan) : 0;
        }
        else {
            L.sb_thumb_v_h = track_y;
            L.sb_thumb_v_y = 0;
        }
    }
    return L;
}


// 侧栏里各部件的位置（都按 L->hud 缩放，且都装进 side_in_*）
static void face_rect_of(const Ui *ui, const UiLayout *L, UiRect *out) {
    // 笑脸做成方块：边长 = 1.5 格（比一格大一点，好点），
    // 水平居中、并在内容块最上面
    int32_t size = ui->cell_px * 3 / 2;
    if (size > L->side_in_w) size = L->side_in_w;
    out->w = size;
    out->h = size;
    out->x = L->side_in_x + (L->side_in_w - size) / 2;
    out->y = sidebar_content_top(ui, L) + 2 * L->hud;
}
static void counter_rect_of(const Ui *ui, const UiLayout *L, int32_t t, UiRect *out) {
    UiRect fr;
    face_rect_of(ui, L, &fr);
    int32_t v = 0;
    const bool has = counter_shown(ui, t, &v);
    const int32_t w = counter_width(L->hud, panel_cells(panel_imag(t), v, has));
    const int32_t row_h = L_PANEL_H * L->hud;
    out->w = w;
    out->h = row_h;
    out->x = L->side_in_x + (L->side_in_w - w) / 2;
    out->y = fr.y + fr.h + 2 * L->hud + (t - 1) * (row_h + 1 * L->hud);
}
static void timer_rect_of(const Ui *ui, const UiLayout *L, UiRect *out) {
    UiRect fr;
    face_rect_of(ui, L, &fr);
    const int32_t w = timer_width(L->hud);
    const int32_t row_h = L_PANEL_H * L->hud;
    out->w = w;
    out->h = row_h;
    out->x = L->side_in_x + (L->side_in_w - w) / 2;
    out->y = fr.y + fr.h + 2 * L->hud + 4 * (row_h + 1 * L->hud) + 2 * L->hud;
}

// ---------------------------------------------------------------- 绘制
static uint16_t flag_sprite(uint8_t t) {
    switch (t) {
        case 1: return SP_flag_1;
        case 2: return SP_flag_2;
        case 3: return SP_flag_3;
        default: return SP_flag_4;
    }
}
static uint16_t mine_sprite(uint8_t t) {
    switch (t) {
        case 1: return SP_mine_1;
        case 2: return SP_mine_2;
        case 3: return SP_mine_3;
        default: return SP_mine_4;
    }
}
static uint16_t boom_sprite(uint8_t t) {
    switch (t) {
        case 1: return SP_boom_1;
        case 2: return SP_boom_2;
        case 3: return SP_boom_3;
        default: return SP_boom_4;
    }
}
static uint16_t wrong_sprite(uint8_t t) {
    switch (t) {
        case 1: return SP_wrong_1;
        case 2: return SP_wrong_2;
        case 3: return SP_wrong_3;
        default: return SP_wrong_4;
    }
}
static uint16_t digit_sprite(uint8_t ch) {
    if (ch < '0' || ch > '9') return SP_led_9;
    return (uint16_t)(SP_led_0 + (ch - '0'));
}

static bool chordable(const Ui *ui, size_t c) {
    const Game *g = &ui->game;
    if (c >= g->n) return false;
    if (g->over || g->open[c] == 0 || g->mine[c] != 0) return false;
    size_t buf[8];
    const size_t k = game_nbrs(g, c, buf);
    for (size_t i = 0; i < k; i++) {
        if (g->open[buf[i]] == 0 && g->flag[buf[i]] == 0) return true;
    }
    return false;
}
static bool chord_target(const Ui *ui, size_t i) {
    if (ui->press_cell < 0) return false;
    const size_t c = (size_t)ui->press_cell;
    if (!chordable(ui, c)) return false;
    const Game *g = &ui->game;
    if (g->open[i] != 0 || g->flag[i] != 0) return false;
    size_t buf[8];
    const size_t k = game_nbrs(g, c, buf);
    for (size_t x = 0; x < k; x++) {
        if (buf[x] == i) return true;
    }
    return false;
}
static uint16_t cell_sprite(const Ui *ui, size_t i) {
    const Game *g = &ui->game;
    // 按住不放的那一格：画成已翻开的空白（传统扫雷的按下预览），松手才真翻开
    if (ui->press_cell >= 0 && (size_t)ui->press_cell == i && g->open[i] == 0) return SP_blank;
    const bool revealed = g->over && !g->win;
    if (g->open[i] != 0) {
        if (g->mine[i] != 0) {
            if (g->boom == (int32_t)i) return boom_sprite(g->mine[i]);
            return mine_sprite(g->mine[i]);
        }
        const int32_t D = g->clue[i];
        if (D == 0 && game_nbr_mine_count(g, i) == 0) return SP_blank;
        if (D < 0 || D > 64) return SP_blank;
        const uint16_t s = sp_num_by_D[D];
        return s ? s : SP_blank;
    }
    if (g->flag[i] != 0) {
        const bool right = g->mine[i] == g->flag[i];
        if (revealed && !right) return wrong_sprite(g->flag[i]);
        return flag_sprite(g->flag[i]);
    }
    if (revealed && g->mine[i] != 0) return mine_sprite(g->mine[i]);
    return SP_closed;
}

static uint16_t face_sprite(const Ui *ui, uint32_t now) {
    const Game *g = &ui->game;
    if (ui->face_down) return SP_face_down;
    if (g->over) return g->win ? SP_face_win : SP_face_dead;
    if (ui->pointer_down || now < ui->face_flash_until) return SP_face_scan;
    return SP_face_normal;
}

// LED 数字串。v 无效（has_v = false）时画空格子。返回宽度。
static int32_t draw_led(Render *r, int32_t x, int32_t y, int32_t v, bool has_v,
                        int32_t digits, int32_t z) {
    const int32_t lw = 13 * z;
    const int32_t lh = 23 * z;
    int32_t cx = x;
    if (!has_v) {
        for (int32_t i = 0; i < digits; i++) {
            render_blit(r, SP_led_blank, cx, y, lw, lh);
            cx += lw;
        }
        return cx - x;
    }
    if (v < 0) {
        render_blit(r, SP_led_minus, cx, y, lw, lh);
        cx += lw;
        int32_t m = -v;
        const int32_t lim = pow10i(digits - 1) - 1;
        if (m > lim) m = lim;
        uint8_t buf[5];
        for (int32_t i = digits - 1; i >= 1; i--) {
            buf[i - 1] = (uint8_t)('0' + (m % 10));
            m /= 10;
        }
        for (int32_t i = 0; i < digits - 1; i++) {
            render_blit(r, digit_sprite(buf[i]), cx, y, lw, lh);
            cx += lw;
        }
    }
    else {
        int32_t m = v;
        const int32_t lim = pow10i(digits) - 1;
        if (m > lim) m = lim;
        uint8_t buf[5];
        for (int32_t i = digits - 1; i >= 0; i--) {
            buf[i] = (uint8_t)('0' + (m % 10));
            m /= 10;
        }
        for (int32_t i = 0; i < digits; i++) {
            render_blit(r, digit_sprite(buf[i]), cx, y, lw, lh);
            cx += lw;
        }
    }
    return cx - x;
}

// ---------------------------------------------------------------- 菜单
// 菜单结构：安卓的菜单画在客户区里（Windows 版是系统菜单栏，触屏上不合适）。
// 表里直接存文案下标（UI_S_*），省掉一层"键名字符串查表"，也就不会出现
// 键名和枚举撞名这种事。
typedef struct { uint16_t label; int32_t cmd; } MenuEntry;

enum {
    CMD_NONE = 0,
    CMD_NEW, CMD_BEGINNER, CMD_INTERMEDIATE, CMD_EXPERT, CMD_CUSTOM, CMD_BEST,
    // 1×/2×/3× 三个缩放项已从菜单里删掉：改成"单格固定 0.7cm + 拖动"之后，
    // 缩放档位不再参与布局，留着只会让人以为能调。CMD_ZOOM* 保留仅为兼容旧表项。
    CMD_ZOOM1, CMD_ZOOM2, CMD_ZOOM3,
    CMD_RULES, CMD_ABOUT,
};

static const MenuEntry MENU_GAME[] = {
    { UI_S_MENU_NEW, CMD_NEW },
    { UI_S_MENU_BEGINNER, CMD_BEGINNER },
    { UI_S_MENU_INTERMEDIATE, CMD_INTERMEDIATE },
    { UI_S_MENU_EXPERT, CMD_EXPERT },
    { UI_S_MENU_CUSTOM, CMD_CUSTOM },
    { UI_S_MENU_BEST, CMD_BEST },
};
static const MenuEntry MENU_HELP[] = {
    { UI_S_MENU_RULES, CMD_RULES },
    { UI_S_MENU_ABOUT, CMD_ABOUT },
};

static const MenuEntry *menu_entries(int32_t menu, int32_t *count) {
    if (menu == UI_MENU_GAME) { *count = (int32_t)(sizeof(MENU_GAME) / sizeof(MENU_GAME[0])); return MENU_GAME; }
    if (menu == UI_MENU_HELP) { *count = (int32_t)(sizeof(MENU_HELP) / sizeof(MENU_HELP[0])); return MENU_HELP; }
    *count = 0;
    return NULL;
}
static uint16_t menu_bar_label_idx(int32_t menu) {
    return (menu == UI_MENU_GAME) ? UI_S_MENU_GAME : UI_S_MENU_HELP;
}
static int32_t menu_count(void) { return 2; }

int32_t ui_menu_item_count(int32_t menu) {
    int32_t n = 0;
    menu_entries(menu, &n);
    return n;
}
const char *ui_menu_item_label(int32_t menu, int32_t idx) {
    int32_t n = 0;
    const MenuEntry *e = menu_entries(menu, &n);
    if (!e || idx < 0 || idx >= n) return "";
    return (const char *)ui_strings[e[idx].label].bytes;
}
static int32_t menu_item_cmd(int32_t menu, int32_t idx) {
    int32_t n = 0;
    const MenuEntry *e = menu_entries(menu, &n);
    if (!e || idx < 0 || idx >= n) return CMD_NONE;
    return e[idx].cmd;
}

// ---------------------------------------------------------------- 菜单栏
// 菜单栏横跨整个窗口顶部（侧栏之上），所以它的基准是窗口的 (0,0)。
static void menubar_button_rect(const Ui *ui, int32_t idx, int32_t *x, int32_t *y,
                                int32_t *w, int32_t *h) {
    const UiLayout L = ui_layout(ui);
    const int32_t hh = L.hud;
    const int32_t bow = menu_item_w(ui, hh);
    *x = 2 * hh + idx * (bow + 2 * hh);
    *y = 2 * hh;
    *w = bow;
    // 按钮高度按菜单栏算，但不小于"字放得下"的高度
    {
        int32_t bh = L.menubar_h - 4 * hh;
        const int32_t need = menubar_text_h(ui);
        if (bh < need) bh = need;
        *h = bh;
    }
}
static void dropdown_rect(const Ui *ui, int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    const UiLayout L = ui_layout(ui);
    const int32_t hh = L.hud;
    int32_t n = 0;
    menu_entries(ui->open_menu, &n);
    if (ui->open_menu < 0) n = 0;
    int32_t bx = 0, by = 0, bw = 0, bh = 0;
    menubar_button_rect(ui, ui->open_menu < 0 ? 0 : ui->open_menu, &bx, &by, &bw, &bh);
    *x = bx;
    *y = L.menubar_h;
    // 行高也按字号保底，免得高密度设备上文字挤在一起
    {
        int32_t row = (L_MENUBAR_UNIT - 4) * hh;
        const int32_t need = menubar_text_h(ui) + 2 * hh;
        if (row < need) row = need;
        *h = n * row + 4 * hh;
    }
    // 宽度按最长项算（用中号字量一遍，别让中文标题被截掉）。
    // 左右各留 6h：字号调大后 2*4h 显得太挤，"自定义"这种三项会贴边。
    int32_t widest = 0;
    for (int32_t i = 0; i < n; i++) {
        const char *label = ui_menu_item_label(ui->open_menu, i);
        const int32_t tw = 2 * 6 * hh + font_text_width(NULL, label, FS_MD(ui));
        if (tw > widest) widest = tw;
    }
    // 也不要比菜单按钮还窄（否则下拉开在按钮下面会显得突兀）
    if (widest < menu_item_w(ui, hh)) widest = menu_item_w(ui, hh);
    *w = widest;
    // 再给一点余量，并且不窄于菜单按钮的 1.5 倍 —— 字号调大后三项文案
    // （"自定义"）几乎贴到右边框，看着很局促。
    {
        const int32_t floor_w = 45 * hh;
        if (*w < floor_w) *w = floor_w;
    }
}

// ---------------------------------------------------------------- 浮层按钮
// 浮层里的按钮：按 index 顺序竖排（自定义面板除外，它的布局单独算）
typedef struct { int32_t x, y, w, h; int32_t cmd; } OverlayBtn;

// ---------------------------------------------------------------- 浮层几何
//
// **面板矩形只能有一份定义。** 这里踩过一个很典型的坑：
// `draw_overlay` 自己算一套面板位置（py = (win_h - panel_h)/2），
// 而 `overlay_buttons` 另算一套（cy = (menubar_h + win_h)/2）。
// 两套公式在 1080 高的屏上差了 128px，结果是：
//   - 帮助/关于/纪录的"确定"按钮画在**面板下边缘之外 104px**，看着像浮层底部空了一块
//   - 自定义面板的"确定"按钮同样在面板外，**点不到**（用户报的"不能点确定"）
// 现在统一用 overlay_panel_rect()，绘制与命中共用同一份几何。
typedef struct {
    int32_t x, y, w, h;      // 面板
    int32_t title_y;         // 标题（FS_MD(ui) 的行顶）
    int32_t body_y;          // 正文/字段起始 y
    int32_t row_h;           // 自定义面板一行的高度
    int32_t field_w, field_h;
    int32_t arrow_w, arrow_h;
    int32_t btn_y, btn_h;    // 底部按钮行
    int32_t btn_w;           // 底部按钮宽度（按最长文案实测，见 overlay_geom）
    int32_t err_y;           // 错误提示行（自定义面板用）
} OverlayGeom;

// 自定义面板里"最宽的标签"需要多少像素。
// 单独拎出来是为了打破循环依赖：面板宽度要由标签宽度决定，
// 而标签位置又依赖面板 —— 所以标签宽度只按字号量，不依赖面板。
static int32_t custom_label_w(int32_t layer) {
    static const uint16_t keys[6] = {
        UI_S_CUSTOM_HEIGHT, UI_S_CUSTOM_WIDTH, UI_S_CUSTOM_T1,
        UI_S_CUSTOM_T2, UI_S_CUSTOM_T3, UI_S_CUSTOM_T4,
    };
    int32_t w = 0;
    for (int32_t i = 0; i < 6; i++) {
        const int32_t t = font_text_width(NULL, (const char *)ui_strings[keys[i]].bytes,
                                          font_slot(layer, FONT_SM));
        if (t > w) w = t;
    }
    return w;
}

static OverlayGeom overlay_geom(const Ui *ui) {
    const UiLayout L = ui_layout(ui);
    const int32_t hh = L.hud > 0 ? L.hud : 1;
    const int32_t pad = 4 * hh;
    const int32_t title_h = font_line_h(FS_MD(ui)) + 2 * hh;
    OverlayGeom g;
    g.row_h = font_line_h(FS_SM(ui)) + 6 * hh;
    g.field_w = 12 * hh;
    g.field_h = font_line_h(FS_SM(ui)) + 2 * hh;
    g.arrow_w = 9 * hh;
    g.arrow_h = g.field_h / 2 - hh;      // 上下箭头各占一半高度
    if (g.arrow_h < 3 * hh) g.arrow_h = 3 * hh;
    g.btn_h = 14 * hh;
    // 按钮宽度**按最长文案实测**，否则"按合计均分"（5 个汉字）会被截。
    // 字号调大之后这个问题就暴露了：120px 的按钮装不下 145px 的文字。
    int32_t btn_w = 30 * hh;
    {
        static const uint16_t keys[3] = { UI_S_CUSTOM_SPLIT, UI_S_CUSTOM_OK, UI_S_CUSTOM_CANCEL };
        for (int32_t i = 0; i < 3; i++) {
            const int32_t t = font_text_width(NULL, (const char *)ui_strings[keys[i]].bytes, FS_MD(ui));
            if (t + 8 * hh > btn_w) btn_w = t + 8 * hh;
        }
    }
    g.btn_w = btn_w;

    if (ui->overlay == UI_OVERLAY_CUSTOM) {
        // 两列 × 三行（左：高度/宽度 → 右：四种雷），底下按钮行。
        // 列宽由"标签 + 值框 + 箭头"实测决定，保证标签一定放得下。
        //
        // 高度的算法要特别小心：行距必须**容纳 3 行字段**（第 3 行要完整落在
        // 按钮行上方，并给错误提示留一行）。早先用 11h 行距，第 3 行会掉到
        // 按钮行上（实测第 6 行 y=653..684 而面板底只有 678）。
        const int32_t rows = 3;
        const int32_t err_h = font_line_h(FS_SM(ui)) + 2 * hh;   // 错误提示单独占一行
        // 行距 = 字段高 + 一点间隙。**必须 ≥ 字段高**，否则同一列相邻两行会重叠。
        g.row_h = g.field_h + 3 * hh;
        const int32_t body_h = rows * g.row_h;
        const int32_t col_w = 4 * hh + custom_label_w(ui->font_layer) + 2 * hh + g.field_w + 2 * hh + g.arrow_w + 4 * hh;
        int32_t w = 2 * col_w + 2 * pad;
        const int32_t btn_row = 3 * btn_w + 3 * 4 * hh + 2 * pad;   // 均分/确定/取消 也必须在面板内
        if (w < btn_row) w = btn_row;
        // 高度**由内容算**：标题 + 字段区 + 错误行 + 按钮 + 内边距。
        // 注意这里只能用到"上面已经算好的量"，别再引用 g.body_y 之类
        // 需要靠 h 才能定的字段 —— 那会读到未初始化内存（踩过：面板高度
        // 变成 6 万多，箭头全跑到屏幕外，点哪都没反应）。
        const int32_t h = title_h + 2 * hh + body_h + 2 * hh + err_h + g.btn_h + 2 * pad;
        g.w = w; g.h = h;
        g.x = (L.win_w - w) / 2;
        g.y = (L.win_h - h) / 2;
        g.title_y = g.y + pad;
        g.body_y = g.title_y + title_h + 2 * hh;
        g.btn_y = g.y + h - pad - g.btn_h;
        g.err_y = g.btn_y - 2 * hh - font_line_h(FS_SM(ui));
        // 自检兜底：算出来的几何必须是合理的正整数，否则宁可夹回窗口内
        if (g.h < 1 || g.h > L.win_h) g.h = L.win_h / 2;
        if (g.w < 1 || g.w > L.win_w) g.w = L.win_w / 2;
        if (g.x < 0) g.x = 0;
        if (g.y < 0) g.y = 0;
    }
    else {
        // 帮助 / 关于 / 纪录：正文逐行 + 一个"确定"。
        // 宽度按最长一行实测，避免"面板很宽但文字只占左边一小条"。
        const bool is_help = (ui->overlay == UI_OVERLAY_HELP);
        int32_t lines = 0;
        int32_t widest = 0;
        if (ui->overlay == UI_OVERLAY_BEST) {
            lines = 3;
            static const uint16_t rows[3] = { UI_S_BEST_L1, UI_S_BEST_L2, UI_S_BEST_L3 };
            for (int32_t i = 0; i < 3; i++) {
                const int32_t t = font_text_width(NULL, (const char *)ui_strings[rows[i]].bytes, FS_SM(ui));
                if (t > widest) widest = t;
            }
            widest += 26 * hh;      // 右侧还有一列"用时"
        }
        else {
            lines = is_help ? UI_PARA_HELP_COUNT : UI_PARA_ABOUT_COUNT;
            for (int32_t i = 0; i < lines; i++) {
                const uint16_t k = is_help ? ui_para_help[i] : ui_para_about[i];
                const int32_t t = font_text_width(NULL, (const char *)ui_strings[k].bytes, FS_SM(ui));
                if (t > widest) widest = t;
            }
        }
        int32_t w = widest + 12 * hh;                 // 左右各 6h
        const int32_t min_w = 40 * hh;                // 太窄会显得局促
        const int32_t title_w = font_text_width(NULL, (const char *)ui_strings[
            is_help ? UI_S_HELP_TITLE : UI_S_ABOUT_TITLE].bytes, FS_MD(ui));
        if (w < min_w) w = min_w;
        if (w < title_w + 12 * hh) w = title_w + 12 * hh;
        // 至少放得下底部的"确定"按钮，并且左右各留 6h（否则按钮会贴着面板边）
        if (w < btn_w + 12 * hh) w = btn_w + 12 * hh;
        const int32_t h = title_h + 2 * hh + lines * g.row_h + 2 * hh + g.btn_h + 2 * pad;
        g.w = w; g.h = h;
        g.x = (L.win_w - w) / 2;
        g.y = (L.win_h - h) / 2;
        g.title_y = g.y + pad;
        g.body_y = g.title_y + title_h + 2 * hh;
        g.btn_y = g.y + h - pad - g.btn_h;
    }
    (void)btn_w;
    return g;
}

// 帮助/关于/纪录：底部只有一个"确定"
// 自定义：均分 / 确定 / 取消
static int32_t overlay_buttons(const Ui *ui, OverlayBtn *out, int32_t cap) {
    const OverlayGeom g = overlay_geom(ui);
    const UiLayout L = ui_layout(ui);
    const int32_t hh = L.hud > 0 ? L.hud : 1;
    const int32_t bw = g.btn_w;
    (void)cap;
    if (ui->overlay == UI_OVERLAY_CUSTOM) {
        int32_t n = 0;
        out[n].x = g.x + 4 * hh; out[n].y = g.btn_y; out[n].w = bw; out[n].h = g.btn_h; out[n].cmd = 100; n++;
        out[n].x = g.x + g.w - 2 * bw - 6 * hh; out[n].y = g.btn_y; out[n].w = bw; out[n].h = g.btn_h; out[n].cmd = 101; n++;
        out[n].x = g.x + g.w - bw - 4 * hh; out[n].y = g.btn_y; out[n].w = bw; out[n].h = g.btn_h; out[n].cmd = 102; n++;
        return n;
    }
    if (ui->overlay == UI_OVERLAY_BEST || ui->overlay == UI_OVERLAY_HELP ||
        ui->overlay == UI_OVERLAY_ABOUT) {
        // cmd 必须是"确定"（101）：早先这里写成 CMD_NONE，按下去什么都不做，浮层关不掉
        out[0].x = (L.win_w - bw) / 2; out[0].y = g.btn_y;
        out[0].w = bw; out[0].h = g.btn_h; out[0].cmd = 101;
        return 1;
    }
    return 0;
}

// 自定义面板上第 idx(0..5) 行的"值框"与上下箭头。
// 布局：两列三行 —— 左列 0=高度 1=宽度，右列 2..5 = 四种雷。
// 每列内部从左到右：4h 边距 → 标签(custom_label_w) → 2h → 值框 → 2h → 箭头 → 4h。
static void custom_field_rect(const Ui *ui, int32_t idx, int32_t *x, int32_t *y,
                              int32_t *w, int32_t *h) {
    const OverlayGeom g = overlay_geom(ui);
    const UiLayout L = ui_layout(ui);
    const int32_t hh = L.hud > 0 ? L.hud : 1;
    const int32_t col_w = g.w / 2;
    // 两列各 3 行：0,1,2 → 左列；3,4,5 → 右列。
    // 语义上左列是"棋盘尺寸"（高/宽），右列是"四种雷"，但 3+3 才让高度可控。
    const int32_t col = idx / 3;
    const int32_t r = idx % 3;
    const int32_t fx = g.x + col * col_w + 4 * hh + custom_label_w(ui->font_layer) + 2 * hh;
    *x = fx;
    *y = g.body_y + r * g.row_h;
    *w = g.field_w;
    *h = g.field_h;
}
// 该行的"加/减"按钮（在值框右侧，上下叠放）
static void custom_arrow_rect(const Ui *ui, int32_t idx, bool up, UiRect *out) {
    const OverlayGeom g = overlay_geom(ui);
    const UiLayout L = ui_layout(ui);
    const int32_t hh = L.hud > 0 ? L.hud : 1;
    int32_t fx, fy, fw, fh;
    custom_field_rect(ui, idx, &fx, &fy, &fw, &fh);
    out->w = g.arrow_w;
    out->h = (fh - hh) / 2;
    if (out->h < 3 * hh) out->h = 3 * hh;
    out->x = fx + fw + 2 * hh;
    out->y = up ? fy : (fy + fh - out->h);
}
// 该行的标签位置（在值框左边）
static int32_t custom_label_x(const Ui *ui, int32_t idx) {
    const OverlayGeom g = overlay_geom(ui);
    const UiLayout L = ui_layout(ui);
    const int32_t hh = L.hud > 0 ? L.hud : 1;
    const int32_t col_w = g.w / 2;
    const int32_t col = idx / 3;
    return g.x + col * col_w + 4 * hh;
}


// ---------------------------------------------------------------- 版式查询
// 这几个是给自检和宿主机预览用的"对外版式接口"：自检要能算出某个按钮的中心点
// 去模拟点按，不该靠猜坐标。
void ui_menubar_rect(const Ui *ui, int32_t idx, UiRect *out) {
    int32_t x, y, w, h;
    menubar_button_rect(ui, idx, &x, &y, &w, &h);
    out->x = x; out->y = y; out->w = w; out->h = h;
}
void ui_dropdown_rect(const Ui *ui, UiRect *out) {
    int32_t x, y, w, h;
    dropdown_rect(ui, &x, &y, &w, &h);
    out->x = x; out->y = y; out->w = w; out->h = h;
}
void ui_face_rect(const Ui *ui, UiRect *out) {
    const UiLayout L = ui_layout(ui);
    face_rect_of(ui, &L, out);
}
int32_t ui_counter_rect(const Ui *ui, int32_t t, UiRect *out) {
    if (t < 1 || t > 4) return 0;
    const UiLayout L = ui_layout(ui);
    counter_rect_of(ui, &L, t, out);
    return 4;
}
void ui_timer_rect(const Ui *ui, UiRect *out) {
    const UiLayout L = ui_layout(ui);
    timer_rect_of(ui, &L, out);
}
int32_t ui_overlay_button_rect(const Ui *ui, int32_t idx, UiRect *out) {
    OverlayBtn btns[4];
    const int32_t n = overlay_buttons(ui, btns, 4);
    if (idx < 0 || idx >= n) return n;
    out->x = btns[idx].x; out->y = btns[idx].y; out->w = btns[idx].w; out->h = btns[idx].h;
    return n;
}
// 按钮文案的顺序与 overlay_buttons 里 cmd 100/101/102 的顺序一致
const char *ui_overlay_button_label(int32_t idx) {
    switch (idx) {
        case 0: return (const char *)ui_strings[UI_S_CUSTOM_SPLIT].bytes;
        case 1: return (const char *)ui_strings[UI_S_CUSTOM_OK].bytes;
        default: return (const char *)ui_strings[UI_S_CUSTOM_CANCEL].bytes;
    }
}
int32_t ui_custom_field_rect(const Ui *ui, int32_t idx, UiRect *out) {
    if (idx < 0 || idx > 5) return 0;
    int32_t x, y, w, h;
    custom_field_rect(ui, idx, &x, &y, &w, &h);
    out->x = x; out->y = y; out->w = w; out->h = h;
    return 6;
}
void ui_custom_arrow_rect(const Ui *ui, int32_t idx, bool up, UiRect *out) {
    if (idx < 0 || idx > 5) { out->x = out->y = out->w = out->h = 0; return; }
    custom_arrow_rect(ui, idx, up, out);
}
void ui_overlay_panel_rect(const Ui *ui, UiRect *out) {
    const OverlayGeom g = overlay_geom(ui);
    out->x = g.x; out->y = g.y; out->w = g.w; out->h = g.h;
}
const char *ui_custom_field_label(int32_t idx) {
    static const uint16_t keys[6] = {
        UI_S_CUSTOM_HEIGHT, UI_S_CUSTOM_WIDTH, UI_S_CUSTOM_T1, UI_S_CUSTOM_T2, UI_S_CUSTOM_T3, UI_S_CUSTOM_T4,
    };
    if (idx < 0 || idx > 5) return "";
    return (const char *)ui_strings[keys[idx]].bytes;
}
int32_t ui_custom_field_value(const Ui *ui, int32_t idx) {
    switch (idx) {
        case 0: return ui->dlg_h;
        case 1: return ui->dlg_w;
        case 2: return ui->dlg_t1;
        case 3: return ui->dlg_t2;
        case 4: return ui->dlg_t3;
        case 5: return ui->dlg_t4;
        default: return 0;
    }
}

int32_t ui_counter_width(const Ui *ui, int32_t t) {
    if (t < 1 || t > 4) return 0;
    int32_t v = 0;
    const bool has = counter_shown(ui, t, &v);
    return counter_width(ui->hud_scale > 0 ? ui->hud_scale : 1,
                         panel_cells(panel_imag(t), v, has));
}

// ---------------------------------------------------------------- 绘制整帧
// 菜单栏**只在左侧栏那一条里**（不横跨窗口）。棋盘因此可以从窗口最上沿开始，
// 上方那片空白被并进地图（多出整整一条菜单栏的高度）。
static void draw_menubar(Render *r, Ui *ui, const Font *font, const UiLayout *L) {
    const int32_t hh = L->hud;
    render_fill(r, 0, 0, L->side_w, L->menubar_h, CS_C_BTNFACE);
    for (int32_t i = 0; i < menu_count(); i++) {
        int32_t x, y, w, h;
        menubar_button_rect(ui, i, &x, &y, &w, &h);
        // 侧栏太窄时，按钮不要画出去（宁可不画，也不要压到棋盘上）
        if (x + w > L->side_w) break;
        const bool active = (ui->open_menu == i);
        render_fill(r, x, y, w, h, active ? CS_C_HILIGHT : CS_C_BTNFACE);
        render_bevel(r, x, y, w, h, 1 * hh, !active);
        // 注意用 menu_bar_label_idx(i)，**不能**写成 ui_strings[UI_MENU_GAME]：
        // UI_MENU_GAME 是"菜单编号"，ui_strings 的下标要的是 UI_S_* 文案下标。
        // 两者数值恰好都是 0，混用就会把"游戏"画成程序标题（踩过这个坑）。
        const char *label = (const char *)ui_strings[menu_bar_label_idx(i)].bytes;
        const int32_t tw = font_text_width(font, label, FS_MD(ui));
        font_draw(r, font, FS_MD(ui), label, x + (w - tw) / 2, font_center_y_for_text(label, FS_MD(ui), y, h), CS_C_BLACK);
    }
    // 菜单栏下沿一条阴影，和经典 Windows 菜单栏一致（只到侧栏右边界）
    render_fill(r, 0, L->menubar_h - 1 * hh, L->side_w, 1 * hh, CS_C_SHADOW);
}

// ---------------------------------------------------------------- 左侧信息栏
// 从上到下：笑脸（方块，点它重开）→ 四个计雷器竖排 → 计时器。
// 全部按 L->hud 缩放，并保证装进 L->side_in_*（自检会核对）。
static void draw_sidebar(Render *r, Ui *ui, const UiLayout *L) {
    const int32_t hh = L->hud;
    const Game *g = &ui->game;

    // 侧栏底板（凹陷）+ 背景
    render_fill(r, L->side_x, L->side_y, L->side_w, L->side_h, CS_C_BTNFACE);
    render_bevel(r, L->side_x, L->side_y, L->side_w, L->side_h, hh, false);

    // 笑脸：贴图自带立体边，这里按方块居中放大
    UiRect fr;
    face_rect_of(ui, L, &fr);
    // 贴图本身 24×24，放大到 size×size；用最近邻保持硬边
    render_blit(r, face_sprite(ui, ui->now_hint), fr.x, fr.y, fr.w, fr.h);

    // 四个计雷器
    for (int32_t t = 1; t < 5; t++) {
        UiRect cr;
        counter_rect_of(ui, L, t, &cr);
        const int32_t val = 0;
        int32_t v = 0;
        const bool has = counter_shown(ui, t, &v);
        (void)val;
        const bool imag = panel_imag(t);
        render_bevel(r, cr.x, cr.y, cr.w, cr.h, 1 * hh, false);
        render_blit(r, flag_sprite((uint8_t)t), cr.x + 2 * hh,
                    cr.y + (cr.h - 16 * hh) / 2, 16 * hh, 16 * hh);
        const int32_t led_x = cr.x + 3 * hh + 16 * hh;
        const int32_t led_y = cr.y + (cr.h - 23 * hh) / 2;
        const int32_t vw = draw_led(r, led_x, led_y, v, has,
                                    panel_value_digits(imag, v, has), hh);
        if (imag) {
            render_blit(r, has ? SP_led_i : SP_led_blank, led_x + vw, led_y, 13 * hh, 23 * hh);
        }
    }

    // 计时器
    UiRect tr;
    timer_rect_of(ui, L, &tr);
    render_bevel(r, tr.x, tr.y, tr.w, tr.h, 1 * hh, false);
    (void)draw_led(r, tr.x + hh, tr.y + (tr.h - 23 * hh) / 2,
                   timer_seconds(ui), g->started, L_REAL_DIGITS, hh);
}

// ---------------------------------------------------------------- 右侧棋盘
// 只画可见范围内的整格。棋盘超出视口的部分靠 pan 偏移挪进视野。
//
// 裁剪：棋盘比视口小时居中；比视口大时只画视口覆盖的那几格，
// 不会越界写到侧栏上（虽然侧栏先画、棋盘后画，但溢出会盖住侧栏边框）。
static void draw_board(Render *r, Ui *ui, const UiLayout *L) {
    const Game *g = &ui->game;
    const int32_t hh = L->hud;

    // 棋盘区底板 + 凹槽
    render_fill(r, L->board_x, L->board_y, L->board_w, L->board_h, CS_C_BTNFACE);
    render_bevel(r, L->board_x, L->board_y, L->board_w, L->board_h, L->box, false);

    const int32_t cols = (int32_t)g->w;
    const int32_t rows = (int32_t)g->h;
    // 格子区间直接用布局算好的 vis_first_* / vis_end_*：
    // 它们是由"棋盘 ∩ 视口"反推的，拖动多少格就正好从那一格画到视口右边界。
    // 早先这里写成 `vc < L->vis_cols`，拖动后起点已经不是第 0 格，
    // 于是右侧少画了整整一个拖动量（真机表现：拖到最右时右边空一大条）。
    const int32_t origin_x = L->board_origin_x;
    const int32_t origin_y = L->board_origin_y;
    // 裁剪到"棋盘 ∩ 视口"，别让半格/棋盘外的部分盖住凹槽边框
    const UiRect clip = { L->board_clip_x, L->board_clip_y,
                          L->board_clip_w, L->board_clip_h };

    for (int32_t row = L->vis_first_row; row < L->vis_end_row; row++) {
        if (row < 0 || row >= rows) break;
        const int32_t dy = origin_y + row * L->cell;
        for (int32_t col = L->vis_first_col; col < L->vis_end_col; col++) {
            if (col < 0 || col >= cols) break;
            const int32_t dx = origin_x + col * L->cell;
            const size_t i = (size_t)row * (size_t)cols + (size_t)col;
            render_blit_clipped(r, cell_sprite(ui, i), dx, dy, L->cell, L->cell, clip);
        }
    }
    (void)hh;
}

static void draw_dropdown(Render *r, Ui *ui, const Font *font, const UiLayout *L) {
    if (ui->open_menu < 0) return;
    int32_t x, y, w, h;
    dropdown_rect(ui, &x, &y, &w, &h);
    render_fill(r, x, y, w, h, CS_C_BTNFACE);
    render_bevel(r, x, y, w, h, 2 * L->hud, true);
    const int32_t n = ui_menu_item_count(ui->open_menu);
    const int32_t row = (L_MENUBAR_UNIT - 4) * L->hud;
    for (int32_t i = 0; i < n; i++) {
        const int32_t iy = y + 2 * L->hud + i * row;
        if (i == ui->menu_sel) render_fill(r, x + 2 * L->hud, iy, w - 4 * L->hud, row, CS_C_HILIGHT);
        const char *label = ui_menu_item_label(ui->open_menu, i);
        font_draw(r, font, FS_MD(ui), label, x + 6 * L->hud, font_center_y_for_text(label, FS_MD(ui), iy, row),
                  i == ui->menu_sel ? CS_C_BLACK : CS_C_DARKGRAY);    }
}

static void draw_text_center(Render *r, const Font *font, const char *s, int32_t cx, int32_t y,
                             int32_t size, uint32_t color) {
    const int32_t tw = font_text_width(font, s, size);
    font_draw(r, font, size, s, cx - tw / 2, y, color);
}

// 画一个按钮。取 UiRect + 字号，这样既能画浮层底部的大按钮，
// 也能画自定义面板里的小箭头按钮（同一个 UiRect 类型，不用两套函数）。
static void draw_button_font(Render *r, const Font *font, const UiLayout *L, const UiRect *b,
                             const char *label, bool pressed, int32_t fsize) {
    render_fill(r, b->x, b->y, b->w, b->h, pressed ? CS_C_SHADOW : CS_C_BTNFACE);
    render_bevel(r, b->x, b->y, b->w, b->h, 1 * L->hud, !pressed);
    const int32_t tw = font_text_width(font, label, fsize);
    font_draw(r, font, fsize, label, b->x + (b->w - tw) / 2,
              font_center_y_for_text(label, fsize, b->y, b->h), CS_C_BLACK);
}
static void draw_button(Render *r, const Font *font, const UiLayout *L, const OverlayBtn *b,
                        const char *label, bool pressed, int32_t layer) {
    UiRect rc = { b->x, b->y, b->w, b->h };
    draw_button_font(r, font, L, &rc, label, pressed, font_slot(layer, FONT_MD));
}

static void draw_overlay(Render *r, Ui *ui, const Font *font, const UiLayout *L) {
    if (ui->overlay == UI_OVERLAY_NONE) return;
    // 面板几何与 overlay_buttons/custom_field_rect 共用同一份（曾经的 bug 就是各算各的）
    const OverlayGeom g = overlay_geom(ui);
    const int32_t px = g.x, py = g.y, panel_w = g.w, panel_h = g.h;
    const int32_t hh = L->hud > 0 ? L->hud : 1;
    render_fill(r, px, py, panel_w, panel_h, CS_C_BTNFACE);
    render_bevel(r, px, py, panel_w, panel_h, 3 * hh, true);

    if (ui->overlay == UI_OVERLAY_CUSTOM) {
        draw_text_center(r, font, (const char *)ui_strings[UI_S_CUSTOM_TITLE].bytes,
                         L->win_w / 2, g.title_y, FS_MD(ui), CS_C_BLACK);
        const char *labels[6] = {
            (const char *)ui_strings[UI_S_CUSTOM_HEIGHT].bytes,
            (const char *)ui_strings[UI_S_CUSTOM_WIDTH].bytes,
            (const char *)ui_strings[UI_S_CUSTOM_T1].bytes,
            (const char *)ui_strings[UI_S_CUSTOM_T2].bytes,
            (const char *)ui_strings[UI_S_CUSTOM_T3].bytes,
            (const char *)ui_strings[UI_S_CUSTOM_T4].bytes,
        };
        const int32_t vals[6] = { ui->dlg_h, ui->dlg_w, ui->dlg_t1, ui->dlg_t2, ui->dlg_t3, ui->dlg_t4 };
        for (int32_t i = 0; i < 6; i++) {
            int32_t fx, fy, fw, fh;
            custom_field_rect(ui, i, &fx, &fy, &fw, &fh);
            const int32_t label_x = custom_label_x(ui, i);
            font_draw(r, font, FS_SM(ui), labels[i], label_x,
                      font_center_y_for_text(labels[i], FS_SM(ui), fy, fh), CS_C_BLACK);
            // 值框 + 上下箭头（安卓没软键盘，数值只能靠按钮增减）
            render_fill(r, fx, fy, fw, fh, CS_C_WHITE);
            render_bevel(r, fx, fy, fw, fh, 1 * hh, false);
            char buf[12];
            snprintf(buf, sizeof buf, "%d", vals[i]);
            const int32_t tw = font_text_width(font, buf, FS_SM(ui));
            font_draw(r, font, FS_SM(ui), buf, fx + (fw - tw) / 2,
                      font_center_y_for_text(buf, FS_SM(ui), fy, fh), CS_C_BLACK);
            const bool hot = (ui->dlg_focus == i);
            for (int32_t k = 0; k < 2; k++) {
                UiRect a;
                custom_arrow_rect(ui, i, k == 0, &a);
                draw_button_font(r, font, L, &a,
                                 (const char *)ui_strings[k == 0 ? UI_S_ARROW_UP : UI_S_ARROW_DOWN].bytes,
                                 hot, FS_SM(ui));
            }
        }
        if (ui->dlg_err != 0) {
            static const uint16_t err_idx[5] = { 0, UI_S_ERR_HEIGHT, UI_S_ERR_WIDTH, UI_S_ERR_SUM_ZERO, UI_S_ERR_SUM_BIG };
            const char *msg = (ui->dlg_err >= 1 && ui->dlg_err <= 4)
                                  ? (const char *)ui_strings[err_idx[ui->dlg_err]].bytes
                                  : "";
            font_draw(r, font, FS_SM(ui), msg, px + 4 * hh,
                      font_center_y_for_text(msg, FS_SM(ui), g.err_y, font_line_h(FS_SM(ui))), CS_C_BLACK);
        }
        OverlayBtn btns[4];
        const int32_t nb = overlay_buttons(ui, btns, 4);
        const char *blabels[3] = {
            (const char *)ui_strings[UI_S_CUSTOM_SPLIT].bytes,
            (const char *)ui_strings[UI_S_CUSTOM_OK].bytes,
            (const char *)ui_strings[UI_S_CUSTOM_CANCEL].bytes,
        };
        for (int32_t i = 0; i < nb; i++) draw_button(r, font, L, &btns[i], blabels[i], false, ui->font_layer);
        return;
    }

    if (ui->overlay == UI_OVERLAY_BEST) {
        const char *title = ui->note == 1 ? (const char *)ui_strings[UI_S_BEST_NEW].bytes
                                         : (const char *)ui_strings[UI_S_BEST_TITLE].bytes;
        draw_text_center(r, font, title, L->win_w / 2, g.title_y, FS_MD(ui), CS_C_BLACK);
        const uint16_t rows[3] = { UI_S_BEST_L1, UI_S_BEST_L2, UI_S_BEST_L3 };
        for (int32_t i = 0; i < 3; i++) {
            const int32_t y = g.body_y + i * g.row_h;
            const char *lab = (const char *)ui_strings[rows[i]].bytes;
            font_draw(r, font, FS_SM(ui), lab, px + 8 * hh,
                      font_center_y_for_text(lab, FS_SM(ui), y, g.row_h), CS_C_BLACK);
            char buf[24];
            if (ui->best[i] > 0) {
                snprintf(buf, sizeof buf, "%d %s", ui->best[i], (const char *)ui_strings[UI_S_BEST_SECONDS].bytes);
            }
            else {
                snprintf(buf, sizeof buf, "%s", (const char *)ui_strings[UI_S_BEST_NONE].bytes);
            }
            const int32_t tw = font_text_width(font, buf, FS_SM(ui));
            font_draw(r, font, FS_SM(ui), buf, px + panel_w - tw - 8 * hh,
                      font_center_y_for_text(buf, FS_SM(ui), y, g.row_h), CS_C_BLACK);
        }
        OverlayBtn btns[4];
        const int32_t nb = overlay_buttons(ui, btns, 4);
        const char *ok = (const char *)ui_strings[UI_S_CUSTOM_OK].bytes;
        for (int32_t i = 0; i < nb; i++) draw_button(r, font, L, &btns[i], ok, false, ui->font_layer);
        return;
    }

    // 帮助 / 关于：按段落逐行渲染
    const bool is_help = (ui->overlay == UI_OVERLAY_HELP);
    const char *title = is_help ? (const char *)ui_strings[UI_S_HELP_TITLE].bytes
                                : (const char *)ui_strings[UI_S_ABOUT_TITLE].bytes;
    draw_text_center(r, font, title, L->win_w / 2, g.title_y, FS_MD(ui), CS_C_BLACK);
    int32_t y = g.body_y;
    if (is_help) {
        for (int32_t i = 0; i < UI_PARA_HELP_COUNT; i++) {
            font_draw(r, font, FS_SM(ui), (const char *)ui_strings[ui_para_help[i]].bytes,
                      px + 6 * hh, y, CS_C_BLACK);
            y += g.row_h;
        }
    }
    else {
        for (int32_t i = 0; i < UI_PARA_ABOUT_COUNT; i++) {
            font_draw(r, font, FS_SM(ui), (const char *)ui_strings[ui_para_about[i]].bytes,
                      px + 6 * hh, y, CS_C_BLACK);
            y += g.row_h;
        }
    }
    OverlayBtn btns[4];
    const int32_t nb = overlay_buttons(ui, btns, 4);
    const char *ok = (const char *)ui_strings[UI_S_CUSTOM_OK].bytes;
    for (int32_t i = 0; i < nb; i++) draw_button(r, font, L, &btns[i], ok, false, ui->font_layer);
}

// 两条滚动条的矩形 —— **绘制与命中唯一共用**的定义（不要再各自算一遍）。
//
// 滚动条的几何分两层，**不要混**：
//
//   1) 轨道（track）：从视口边缘一直铺到棋盘内边缘 —— 用来吃掉
//      `(可用 − 一格) % 一格` 的零头（1920 竖向 87px、横向 18px）。
//      若只铺一格，那块零头就是一条什么都没画的纯面板灰边
//      （用户报的"画面下方还有灰边"）。
//   2) 滑块（thumb）：**厚度严格一格**（横块的高、纵块的宽）。
//      早先把整条带子跟着长度一起变厚，屏幕上就成了"滚动条有两格宽"
//      （用户报的）。厚度必须独立钳制在一格。
//
// 所以：轨道 = 整个剩余带；滑块 = 一格厚，贴在轨道靠视口的那一侧。
// 滑块换算仍按**视口尺寸**当轨道长度：滑块长度 ∝ 可见/全盘，
// 只有"轨道与视口一样长"时这个比例才等于可拖行程的比例。
static void sb_h_track_rect(const UiLayout *L, UiRect *out) {
    out->x = L->vis_x;                                    // 紧贴视口左下角
    out->y = L->vis_y + L->vis_rows * L->cell;
    out->w = L->board_x + L->board_w - L->box - out->x;   // 铺到棋盘内右边缘
    out->h = L->board_y + L->board_h - L->box - out->y;   // 铺到棋盘内下边缘
}
static void sb_v_track_rect(const UiLayout *L, UiRect *out) {
    out->x = L->vis_x + L->vis_cols * L->cell;
    out->y = L->vis_y;
    out->w = L->board_x + L->board_w - L->box - out->x;   // 铺到棋盘内右边缘
    out->h = L->board_y + L->board_h - L->box - out->y;   // 铺到棋盘内下边缘
}
// 滑块矩形：一格厚，贴在轨道靠视口/靠上的一侧
static void sb_h_rect(const UiLayout *L, UiRect *out) {
    sb_h_track_rect(L, out);
    out->h = L->sb_thick < out->h ? L->sb_thick : out->h;
}
static void sb_v_rect(const UiLayout *L, UiRect *out) {
    sb_v_track_rect(L, out);
    out->w = L->sb_thick < out->w ? L->sb_thick : out->w;
}
// 滑块换算用的"轨道长度"：**恒等于视口尺寸**（不是条带的实际长度）
static int32_t sb_h_track(const UiLayout *L) { return L->vis_cols * L->cell; }
static int32_t sb_v_track(const UiLayout *L) { return L->vis_rows * L->cell; }

void ui_scrollbar_rect(const Ui *ui, bool horizontal, UiRect *out) {
    const UiLayout L = ui_layout(ui);
    if (horizontal) sb_h_rect(&L, out);     // 滑块（一格厚）
    else sb_v_rect(&L, out);
}
void ui_scrollbar_track_rect(const Ui *ui, bool horizontal, UiRect *out) {
    const UiLayout L = ui_layout(ui);
    if (horizontal) sb_h_track_rect(&L, out);   // 轨道（铺满剩余带）
    else sb_v_track_rect(&L, out);
}

// 只画一圈边框（t 像素厚），不填充内部。
static void render_frame_border(Render *r, int32_t x, int32_t y, int32_t w, int32_t h,
                                int32_t t, uint32_t color) {
    if (w <= 0 || h <= 0 || t <= 0) return;
    if (t * 2 > w) t = w / 2 > 0 ? w / 2 : 1;
    render_fill(r, x, y, w, t, color);                       // 上
    render_fill(r, x, y + h - t, w, t, color);               // 下
    render_fill(r, x, y + t, t, h - 2 * t, color);           // 左
    render_fill(r, x + w - t, y + t, t, h - 2 * t, color);   // 右
}

static void draw_scrollbars(Render *r, const Ui *ui, const UiLayout *L) {
    const int32_t hh = L->hud > 0 ? L->hud : 1;
    UiRect ht, vt;      // 轨道：铺满剩余带
    UiRect hb, vb;      // 滑块：一格厚
    sb_h_track_rect(L, &ht);
    sb_v_track_rect(L, &vt);
    sb_h_rect(L, &hb);
    sb_v_rect(L, &vb);
    if (ht.w < 1 || ht.h < 1 || vt.w < 1 || vt.h < 1) return;

    // 轨道：**比面板更亮**的槽底 + 一圈深色边。
    //
    // 配色踩过一次坑：面板底色、格子底色、滑块如果都用同一个灰，屏幕上完全看不出
    // 滚动条，只剩一圈更深的边 —— 看着就像"游戏界面外面还有一道灰框"。
    const uint32_t track_bg = CS_C_HILIGHT;   // 比面板更亮的槽底
    render_fill(r, ht.x, ht.y, ht.w, ht.h, track_bg);
    render_frame_border(r, ht.x, ht.y, ht.w, ht.h, 1 * hh, CS_C_SHADOW);
    render_fill(r, vt.x, vt.y, vt.w, vt.h, track_bg);
    render_frame_border(r, vt.x, vt.y, vt.w, vt.h, 1 * hh, CS_C_SHADOW);

    // 滑块：凸起的方块 + 中间两道抓握线（和经典 Windows 滚动条一致）。
    // **厚度固定一格**（横块的高 = vb.w 方向的宽度 = sb_thick）。
    if (L->sb_thumb_h_w > 0 && hb.w >= L->sb_thumb_h_w) {
        const int32_t x = hb.x + L->sb_thumb_h_x;
        render_fill(r, x, hb.y, L->sb_thumb_h_w, hb.h, CS_C_BTNFACE);
        render_bevel(r, x, hb.y, L->sb_thumb_h_w, hb.h, 1 * hh, true);
        const int32_t mid = hb.y + hb.h / 2;
        const int32_t gx0 = x + L->sb_thumb_h_w / 5;
        const int32_t gx1 = x + L->sb_thumb_h_w - L->sb_thumb_h_w / 5;
        if (gx1 > gx0 && hb.h > 6 * hh) {
            render_fill(r, gx0, mid - 3 * hh, gx1 - gx0, hh, CS_C_SHADOW);
            render_fill(r, gx0, mid + 2 * hh, gx1 - gx0, hh, CS_C_SHADOW);
        }
    }
    if (L->sb_thumb_v_h > 0 && vb.h >= L->sb_thumb_v_h) {
        const int32_t y = vb.y + L->sb_thumb_v_y;
        render_fill(r, vb.x, y, vb.w, L->sb_thumb_v_h, CS_C_BTNFACE);
        render_bevel(r, vb.x, y, vb.w, L->sb_thumb_v_h, 1 * hh, true);
        const int32_t mid = vb.x + vb.w / 2;
        const int32_t gy0 = y + L->sb_thumb_v_h / 5;
        const int32_t gy1 = y + L->sb_thumb_v_h - L->sb_thumb_v_h / 5;
        if (gy1 > gy0 && vb.w > 6 * hh) {
            render_fill(r, mid - 3 * hh, gy0, hh, gy1 - gy0, CS_C_SHADOW);
            render_fill(r, mid + 2 * hh, gy0, hh, gy1 - gy0, CS_C_SHADOW);
        }
    }
}

static void draw_ui(Render *r, Ui *ui, const Font *font) {
    const UiLayout L = ui_layout(ui);

    render_fill(r, 0, 0, L.win_w, L.win_h, CS_C_BTNFACE);
    draw_menubar(r, ui, font, &L);
    draw_sidebar(r, ui, &L);
    draw_board(r, ui, &L);
    draw_scrollbars(r, ui, &L);
    draw_dropdown(r, ui, font, &L);
    draw_overlay(r, ui, font, &L);
}

void ui_paint(Render *r, Ui *ui, const Font *font, const uint8_t *atlas, size_t atlas_len,
              const uint8_t *font_data, size_t font_len) {
    if (!render_load_atlas(r, atlas, atlas_len)) return;
    if (font_data) render_load_font(r, font_data, font_len);
    draw_ui(r, ui, font);
}

// ---------------------------------------------------------------- 命令
// 帧缓冲尺寸上限（与 platform_android.c 的 FB_MAX_* 保持一致：
// ---------------------------------------------------------------- 初始化与几何
// ---------------------------------------------------------------- 新局种子
// "新局"必须换一个种子，否则每一把的雷区**完全一样**。
//
// 这里踩过一个很严重的坑：安卓侧所有"新局"入口都写死了 `seed = 1`
// （Windows 版用的是 `GetTickCount()`）。因为"第一次点击必安全"的规则是
// **按种子算出雷区之后再把那一格的雷挪走**，所以只要种子相同、第一次点的格子
// 也相同，挪雷的结果就完全相同 —— 玩家看到的是"点新局之后，开局点同一格，
// 雷区一模一样"。实测三次新局的局面指纹完全相同。
//
// 种子的来源：平台层每个心跳把当前毫秒数写进 now_hint（菜单命令/人脸重开都用它）。
// 自检里 ui_tick 其实不跑，now_hint 是 1 这种"不像时钟"的值，
// 那时退回到自增计数器 —— 保证"同一个进程里每次新局都不同"，
// 但不会破坏依赖固定种子的测试（测试用 ui_cmd_new_game 显式给种子）。
static uint32_t g_seed_counter = 0x9E3779B9u;   // 任意非零初值

uint32_t ui_fresh_seed(const Ui *ui) {
    g_seed_counter += 0x9E3779B9u;               // 每调一次必变
    // now_hint 看起来像"毫秒时间戳"才拿来混：否则用计数器独当一面
    if (ui->now_hint > 100000u) {
        // 时间戳与计数器混合：同一毫秒内连续新局也不会撞种子
        return ui->now_hint ^ g_seed_counter;
    }
    return g_seed_counter;
}

void ui_init(Ui *ui, int32_t win_w, int32_t win_h) {
    memset(ui, 0, sizeof(*ui));
    ui->win_w = win_w > 0 ? win_w : 1;
    ui->win_h = win_h > 0 ? win_h : 1;
    // 默认单格 16px：真实值由平台层按屏幕密度（0.7cm）通过 ui_set_cell_px 设置
    ui->cell_px = 16;
    ui->hud_scale = 1;
    ui->open_menu = UI_MENU_NONE;
    ui->menu_sel = -1;
    ui->overlay = UI_OVERLAY_NONE;
    ui->dlg_focus = -1;
    ui->press_cell = -1;
    ui->pending_cell = -1;
    ui->last_tap_cell = -1;
    ui->long_press_cell = -1;
    ui->pointer_cell = -1;
    ui->best[0] = ui->best[1] = ui->best[2] = 0;
    ui->game.w = game_presets[2].w;
    ui->game.h = game_presets[2].h;
    ui->game.mines = game_presets[2].mines;
    for (size_t t = 0; t < 5; t++) ui->game.type_count[t] = 0;
    game_new_game(&ui->game, ui_fresh_seed(ui));
    ui_refresh_scale(ui);
}

// 按当前窗口与单格尺寸，重算 HUD 缩放、钳制单格、钳制拖动偏移。
// 三件事必须一起做：单格变了会影响侧栏能否装下 HUD，也会影响可拖动范围。
void ui_refresh_scale(Ui *ui) {
    if (ui->win_w < 1) ui->win_w = 1;
    if (ui->win_h < 1) ui->win_h = 1;

    // 侧栏宽度 = 长边/5（与 ui_layout 里同一套规则，这里先算出来给 HUD 挑缩放）
    const int32_t long_edge = ui->win_w > ui->win_h ? ui->win_w : ui->win_h;
    int32_t side_w = long_edge / 5;
    const int32_t cap = ui->win_w / 2;
    if (side_w > cap) side_w = cap;
    if (side_w < UI_MIN_SIDE_W) side_w = UI_MIN_SIDE_W;
    if (side_w > ui->win_w - 32) side_w = ui->win_w > 32 ? ui->win_w - 32 : ui->win_w;
    if (side_w < 1) side_w = 1;
    const int32_t side_h = ui->win_h - L_MENUBAR_UNIT;   // 先按 hud=1 估，下面会再校正

    // 单格安全钳制：棋盘区（4/5 宽）至少要放得下 UI_MIN_VIS_COLS 格。
    // 极端窗口（分屏）下不至于只剩一两格，也不会让格子大到一屏显示不下。
    int32_t board_w = ui->win_w - side_w;
    int32_t board_h = ui->win_h - L_MENUBAR_UNIT;
    if (board_w < 1) board_w = 1;
    if (board_h < 1) board_h = 1;
    int32_t max_cell_x = board_w / UI_MIN_VIS_COLS;
    int32_t max_cell_y = board_h / UI_MIN_VIS_ROWS;
    int32_t max_cell = max_cell_x < max_cell_y ? max_cell_x : max_cell_y;
    if (max_cell < 1) max_cell = 1;
    if (ui->cell_px > max_cell) ui->cell_px = max_cell;
    if (ui->cell_px < 1) ui->cell_px = 1;

    // HUD 缩放：挑能让侧栏装下的最大整数（整数是为了 LED/人脸不出现不均匀的块）
    ui->hud_scale = pick_hud_scale(ui, side_w, side_h);
    if (ui->hud_scale < 1) ui->hud_scale = 1;
    // 字号层：按"目标单格"挑（用没被安全钳制过的原始值，保证换难度时字号稳定）
    ui->font_layer = font_layer_for_cell(ui->cell_target_px > 0 ? ui->cell_target_px : ui->cell_px);
    ui->font_pct = 100 + ui->font_layer * 25;

    // 拖动偏移落盘：吸附到整格 + 钳制到合法范围（静止时屏幕上必须是完整格子）。
    // ui_layout 内部会做同样的钳制，这里把结果写回状态，
    // 保证"状态里的 pan" 与 "布局用的 pan" 永远一致。
    const UiLayout L = ui_layout(ui);
    const int32_t span_x = (int32_t)ui->game.w * L.cell - L.vis_cols * L.cell;
    const int32_t span_y = (int32_t)ui->game.h * L.cell - L.vis_rows * L.cell;
    ui->pan_x = normalize_pan(ui->pan_x, span_x, L.cell);
    ui->pan_y = normalize_pan(ui->pan_y, span_y, L.cell);
}

// 目标单格边长（0.7cm）在给定密度下是多少设备像素。
//
// 换算链：0.7cm = 7mm；1mm = dpi/25.4 像素 → px = 7 × dpi / 25.4。
// 化成整数运算（分母放大 10 倍以消掉 25.4 的小数）：
//     px = (7 × 10 × dpi + 127) / 254 = (70 × dpi + 127) / 254
//  280dpi → (19600 + 127) / 254 = 77.6 → **77** ✓（0.70cm）
//
// 这个函数曾经写成 (7 × dpi + 127) / 254 —— 分子少乘了 10，280dpi 下得到 8px
// （0.07cm），棋盘小到看不清；而且因为格子太小，安全钳制永远不触发，
// 30×16 的盘被一屏塞满，"可见格数固定 + 拖动"完全失效。
// 单位错误在真机上才暴露，所以现在这条换算由宿主机自检直接钉住。
int32_t ui_cell_px_for_dpi(int32_t dpi) {
    if (dpi < 1) dpi = 280;
    const int32_t px = (int32_t)(((int64_t)UI_CELL_TARGET_CM * 10 * dpi + 127) / 254);
    return px < 1 ? 1 : px;
}

int32_t ui_font_sm(const Ui *ui) { return FS_SM(ui); }
int32_t ui_font_md(const Ui *ui) { return FS_MD(ui); }
int32_t ui_font_lg(const Ui *ui) { return FS_LG(ui); }

void ui_set_cell_px(Ui *ui, int32_t cell_px) {
    if (cell_px < 1) cell_px = 1;
    ui->cell_target_px = cell_px;   // 记下"原始目标"，供字号层换算
    ui->cell_px = cell_px;
    ui_refresh_scale(ui);
}

void ui_resize(Ui *ui, int32_t win_w, int32_t win_h) {
    ui->win_w = win_w > 0 ? win_w : 1;
    ui->win_h = win_h > 0 ? win_h : 1;
    ui_refresh_scale(ui);
}

// ---------------------------------------------------------------- 拖动
// 把滑块位置换算成棋盘偏移并写回。pos = 滑块起点（相对轨道起点）。
// 正向：thumb_x = (track - thumb) * pan / max_pan，所以反解就是下面这样。
// 钳制交给 ui_refresh_scale/ui_layout（它们会把 pan 夹回合法区间）。
static void ui_sb_apply(Ui *ui, int32_t pos) {
    if (ui->sb_drag == 0) return;
    const int32_t span = ui->sb_track - ui->sb_thumb;
    if (span <= 0 || ui->sb_max_pan <= 0) return;
    if (pos < 0) pos = 0;
    if (pos > span) pos = span;
    const int32_t pan = (int32_t)(((int64_t)pos * ui->sb_max_pan) / span);
    if (ui->sb_drag == 1) ui->pan_x = pan;
    else ui->pan_y = pan;
    // 立刻把布局里钳制过的值写回（滚动条拖到端点时 pan 会被夹）
    ui_refresh_scale(ui);
}

void ui_pan_begin(Ui *ui, int32_t x, int32_t y) {
    ui->pan_active = true;
    ui->pan_start_x = x;
    ui->pan_start_y = y;
    ui->pan_base_x = ui->pan_x;
    ui->pan_base_y = ui->pan_y;
}

void ui_pan_to(Ui *ui, int32_t x, int32_t y) {
    if (!ui->pan_active) return;
    UiLayout L = ui_layout(ui);
    const int32_t span_x = (int32_t)ui->game.w * L.cell - L.vis_cols * L.cell;
    const int32_t span_y = (int32_t)ui->game.h * L.cell - L.vis_rows * L.cell;
    // 手指往右拖 = 内容往右走 = 看左边的格子 = 偏移减小。
    // 只钳制、**不吸附到整格**：拖到哪就停在哪，屏幕边缘出现不完整的格子是正常的。
    // （早先拖动结束会吸附到整格，松手瞬间画面会跳一下；而且拖动中若也吸附，
    //   不足一格的位移会被直接归零，表现成"拖动完全没反应"。）
    if (span_x > 0) {
        ui->pan_x = normalize_pan(ui->pan_base_x - (x - ui->pan_start_x), span_x, L.cell);
    } else {
        ui->pan_x = L.pan_x;   // 该轴不需要拖动：保持居中值
    }
    if (span_y > 0) {
        ui->pan_y = normalize_pan(ui->pan_base_y - (y - ui->pan_start_y), span_y, L.cell);
    } else {
        ui->pan_y = L.pan_y;
    }
}

void ui_pan_end(Ui *ui) {
    ui->pan_active = false;
    // 收尾只做钳制（不吸附）：拖动结束的画面与手指离开时**完全一致**，不会跳。
    ui_refresh_scale(ui);
}

void ui_pan_reset(Ui *ui) {
    ui->pan_active = false;
    ui->pan_x = 0;
    ui->pan_y = 0;
    ui_refresh_scale(ui);
}

// ---------------------------------------------------------------- 命令
void ui_cmd_new_game(Ui *ui, uint32_t seed, bool use_seed) {
    ui->press_cell = -1;
    ui->pending_cell = -1;
    ui->long_press_cell = -1;
    ui->pointer_down = false;
    ui->face_down = false;
    ui->face_flash_until = 0;
    ui->last_press_consumed = false;
    ui->tap_armed = false;
    game_new_game(&ui->game, use_seed ? seed : (uint32_t)(seed ? seed : 1));
    ui->note = 0;
    ui->overlay = UI_OVERLAY_NONE;
    ui_pan_reset(ui);
}
void ui_cmd_preset(Ui *ui, int32_t idx) {
    if (idx < 0 || idx >= GAME_PRESET_COUNT) return;
    ui->game.w = game_presets[idx].w;
    ui->game.h = game_presets[idx].h;
    ui->game.mines = game_presets[idx].mines;
    for (size_t t = 0; t < 5; t++) ui->game.type_count[t] = 0;
    // 换难度同样要换种子（不能写死 1，否则每次进高级都是同一张图）
    ui_cmd_new_game(ui, ui_fresh_seed(ui), true);
}

// ---------------------------------------------------------------- 菜单命令
// CMD_* 与菜单表里的 cmd 对应（见上面的 MENU_GAME / MENU_HELP）
static void run_cmd(Ui *ui, int32_t cmd) {
    switch (cmd) {
        case CMD_NEW: ui_cmd_new_game(ui, ui_fresh_seed(ui), true); break;
        case CMD_BEGINNER: ui_cmd_preset(ui, 0); break;
        case CMD_INTERMEDIATE: ui_cmd_preset(ui, 1); break;
        case CMD_EXPERT: ui_cmd_preset(ui, 2); break;
        case CMD_CUSTOM: ui_cmd_open_custom(ui); break;
        case CMD_BEST: ui->overlay = UI_OVERLAY_BEST; break;
        case CMD_RULES: ui->overlay = UI_OVERLAY_HELP; break;
        case CMD_ABOUT: ui->overlay = UI_OVERLAY_ABOUT; break;
        default: break;
    }
    ui->open_menu = UI_MENU_NONE;
    ui->menu_sel = -1;
}

// 自定义面板里按一次"加/减"：只改数值，**不做最终校验**（校验留给"确定"，
// 这样用户可以先调一格再调别的，不会中途被错误提示打断）。
// 但每一步都钳在硬边界内，避免出现"9 行"这种一减就越界的值。
//   0 = 高度(9..GAME_MAX_H)  1 = 宽度(9..GAME_MAX_W)  2..5 = 四种雷(0..格子数-9)
void ui_dlg_step(Ui *ui, int32_t idx, int32_t delta) {
    if (idx < 0 || idx > 5) return;
    int32_t *v = NULL;
    switch (idx) {
        case 0: v = &ui->dlg_h; break;
        case 1: v = &ui->dlg_w; break;
        case 2: v = &ui->dlg_t1; break;
        case 3: v = &ui->dlg_t2; break;
        case 4: v = &ui->dlg_t3; break;
        default: v = &ui->dlg_t4; break;
    }
    int32_t lo = 0, hi = 0;
    if (idx == 0) { lo = 9; hi = GAME_MAX_H; }
    else if (idx == 1) { lo = 9; hi = GAME_MAX_W; }
    else {
        // 每种雷的上限：总格数减去 9（首点必安全的 3x3 邻域）
        const int32_t cells = ui->dlg_w * ui->dlg_h;
        hi = cells > 9 ? cells - 9 : 0;
        lo = 0;
    }
    int32_t nv = *v + delta;
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    *v = nv;
}

// 打开自定义面板，预填"当前尺寸 + 按总雷数均分"的配比
bool ui_cmd_open_custom(Ui *ui) {
    ui->dlg_h = ui->game.h;
    ui->dlg_w = ui->game.w;
    uint16_t split[5];
    game_split_evenly(ui->game.mines, split);
    ui->dlg_t1 = split[1];
    ui->dlg_t2 = split[2];
    ui->dlg_t3 = split[3];
    ui->dlg_t4 = split[4];
    ui->dlg_focus = -1;
    ui->dlg_err = 0;
    ui->overlay = UI_OVERLAY_CUSTOM;
    return true;
}

void ui_cmd_close_overlay(Ui *ui) {
    ui->overlay = UI_OVERLAY_NONE;
    ui->dlg_err = 0;
    ui->note = 0;
}

// 自定义面板的"确定"：校验通过才生效（尺寸/配比都有硬边界）
void ui_cmd_apply_custom(Ui *ui) {
    const int32_t sum = ui->dlg_t1 + ui->dlg_t2 + ui->dlg_t3 + ui->dlg_t4;
    const int32_t cells = ui->dlg_w * ui->dlg_h;
    if (ui->dlg_h < 9 || ui->dlg_h > GAME_MAX_H) { ui->dlg_err = 1; return; }
    if (ui->dlg_w < 9 || ui->dlg_w > GAME_MAX_W) { ui->dlg_err = 2; return; }
    if (sum < 1) { ui->dlg_err = 3; return; }
    if (sum > cells - 9) { ui->dlg_err = 4; return; }
    ui->game.w = (uint16_t)ui->dlg_w;
    ui->game.h = (uint16_t)ui->dlg_h;
    ui->game.mines = (uint16_t)sum;
    ui->game.type_count[1] = (uint16_t)ui->dlg_t1;
    ui->game.type_count[2] = (uint16_t)ui->dlg_t2;
    ui->game.type_count[3] = (uint16_t)ui->dlg_t3;
    ui->game.type_count[4] = (uint16_t)ui->dlg_t4;
    ui_cmd_new_game(ui, ui_fresh_seed(ui), true);   // 自定义盘同样要换种子
    ui->overlay = UI_OVERLAY_NONE;
    ui->dlg_err = 0;
}
// ---------------------------------------------------------------- 命中测试
// 顺序很重要：浮层 > 下拉 > 菜单栏 > 侧栏 > 棋盘。棋盘在最后，
// 保证侧栏上的点**永远**不会被判成棋盘格（这是"误触"最容易出的地方）。
int32_t ui_hit_test(const Ui *ui, int32_t x, int32_t y, int32_t *idx) {
    if (idx) *idx = -1;
    const UiLayout L = ui_layout(ui);
    if (x < 0 || y < 0 || x >= L.win_w || y >= L.win_h) return UI_HIT_NONE;

    if (ui->overlay != UI_OVERLAY_NONE) {
        OverlayBtn btns[4];
        const int32_t nb = overlay_buttons(ui, btns, 4);
        for (int32_t i = 0; i < nb; i++) {
            if (x >= btns[i].x && y >= btns[i].y && x < btns[i].x + btns[i].w && y < btns[i].y + btns[i].h) {
                if (idx) *idx = i;
                return UI_HIT_OVERLAY_BTN;
            }
        }
        if (ui->overlay == UI_OVERLAY_CUSTOM) {
            // 先判上下箭头（它们比值框更靠右，且更小，先判避免被别的吞掉）
            for (int32_t i = 0; i < 6; i++) {
                for (int32_t k = 0; k < 2; k++) {
                    UiRect a;
                    custom_arrow_rect(ui, i, k == 0, &a);
                    if (x >= a.x && y >= a.y && x < a.x + a.w && y < a.y + a.h) {
                        if (idx) *idx = i;
                        return k == 0 ? UI_HIT_DLG_INC : UI_HIT_DLG_DEC;
                    }
                }
            }
            // 值框本身不再接受点击（没有软键盘，点它没有任何作用）
        }
        return UI_HIT_NONE;
    }

    if (ui->open_menu >= 0) {
        int32_t dx, dy, dw, dh;
        dropdown_rect(ui, &dx, &dy, &dw, &dh);
        if (x >= dx && y >= dy && x < dx + dw && y < dy + dh) {
            const int32_t hh = L.hud;
            const int32_t row = (L_MENUBAR_UNIT - 4) * hh;
            const int32_t i = row > 0 ? (y - dy - 2 * hh) / row : -1;
            if (i >= 0 && i < ui_menu_item_count(ui->open_menu)) {
                if (idx) *idx = i;
            }
            return UI_HIT_DROPDOWN;
        }
    }

    for (int32_t i = 0; i < menu_count(); i++) {
        int32_t mx, my, mw, mh;
        menubar_button_rect(ui, i, &mx, &my, &mw, &mh);
        if (mx + mw > L.side_w) break;   // 画不出来的按钮也不该能点到
        if (x >= mx && y >= my && x < mx + mw && y < my + mh) {
            if (idx) *idx = i;
            return UI_HIT_MENU_ITEM;
        }
    }
    // 菜单栏空白**只算侧栏那一条**：右侧上方现在是棋盘，别把点格子判成点菜单
    if (y < L.menubar_h && x < L.side_w) return UI_HIT_MENU;

    // 侧栏：先判笑脸，其余算侧栏空白
    if (x < L.side_x + L.side_w && y >= L.side_y) {
        UiRect fr;
        face_rect_of(ui, &L, &fr);
        if (x >= fr.x && y >= fr.y && x < fr.x + fr.w && y < fr.y + fr.h) return UI_HIT_FACE;
        return UI_HIT_SIDEBAR;
    }

    // 滚动条：**整条轨道都可点**（点轨道 = 跳转；点滑块 = 抓着拖）。
    // 用轨道矩形（铺满到内边缘）而不是滑块矩形，这样槽里任何位置都点得到。
    {
        UiRect hb, vb;
        sb_h_track_rect(&L, &hb);
        sb_v_track_rect(&L, &vb);
        if (hb.w > 0 && hb.h > 0 &&
            x >= hb.x && y >= hb.y && x < hb.x + hb.w && y < hb.y + hb.h) {
            return UI_HIT_SB_H;
        }
        if (vb.w > 0 && vb.h > 0 &&
            x >= vb.x && y >= vb.y && x < vb.x + vb.w && y < vb.y + vb.h) {
            return UI_HIT_SB_V;
        }
    }

    // 棋盘：只认可见视口内的完整格。
    // 格号由 board_origin（与绘制同源）反推：拖动中 pan 可以停在非整格上，
    // 这里若另算一遍就会和绘制错位。
    // 边界用 board_clip（棋盘 ∩ 视口），这样棋盘小于视口时
    // 旁边那块空白不会被判成棋盘格。
    if (x >= L.board_clip_x && y >= L.board_clip_y &&
        x < L.board_clip_x + L.board_clip_w && y < L.board_clip_y + L.board_clip_h) {
        const int32_t col = (x - L.board_origin_x) / L.cell;
        const int32_t row = (y - L.board_origin_y) / L.cell;
        if (col >= 0 && col < (int32_t)ui->game.w && row >= 0 && row < (int32_t)ui->game.h) {
            if (idx) *idx = row * (int32_t)ui->game.w + col;
            return UI_HIT_BOARD;
        }
    }
    return UI_HIT_NONE;
}

// ---------------------------------------------------------------- 触屏
// 屏幕坐标 → 格号。**只走 ui_hit_test 这一条路**：
// 早先这里另写了一份换算，改为缩放后两边就对不上了（点这里翻那里）。
static int32_t board_cell_at(const Ui *ui, int32_t x, int32_t y) {
    int32_t idx = -1;
    if (ui_hit_test(ui, x, y, &idx) != UI_HIT_BOARD) return -1;
    return idx;
}
static void flash_face(Ui *ui, uint32_t now) {
    ui->face_flash_until = now + UI_FACE_FLASH_MS;
}

static void do_reveal(Ui *ui, int32_t cell, uint32_t now) {
    Game *g = &ui->game;
    if (cell < 0) return;
    const size_t c = (size_t)cell;
    if (g->over || g->open[c] != 0 || g->flag[c] != 0) return;
    if (!g->started) {
        game_start_at(g, c, now);
    }
    else {
        game_reveal(g, c, now);
    }
    if (!g->over) flash_face(ui, now);
    // 通关且是标准三档：结算纪录
    if (g->over && g->win) {
        for (int32_t i = 0; i < GAME_PRESET_COUNT; i++) {
            if (g->w == game_presets[i].w && g->h == game_presets[i].h && g->mines == game_presets[i].mines) {
                int32_t sec = (int32_t)(g->elapsed_ms / 1000u);
                if (sec < 1) sec = 1;
                if (ui->best[i] == 0 || sec < ui->best[i]) {
                    ui->best[i] = sec;
                    ui->scores_dirty = true;
                    ui->note = 1;
                    ui->overlay = UI_OVERLAY_BEST;
                }
                break;
            }
        }
    }
}

// 拖动开始时调用：把"按格手势"整条清掉。
// 不清的话，拖完手指抬起时会走到 ui_pointer_up 的按下分支，
// 顺手给路径上那一格插一面旗（真机上很容易出现，很难查）。
//
// **但绝对不要动 pointer_down**：它表示"手指还按在屏幕上"这个物理事实。
// 这里踩过一个很隐蔽的坑：拖动一开始就把 pointer_down 置 false，
// 于是下一次 ACTION_MOVE 进来时 ui_pointer_move 开头 `if (!pointer_down) return;`
// 直接返回 —— 拖动只处理了第一个 MOVE 事件就再也没反应了，
// 表现是"拖不动"，而所有单步日志看起来都正常（pan_active 也确实成了 1）。
void ui_pointer_cancel(Ui *ui) {
    ui->face_down = false;
    ui->sb_drag = 0;      // 系统取消（来电/手势冲突）时别把滚动条卡在拖动状态
    ui->press_cell = -1;
    ui->long_press_cell = -1;
    ui->pending_cell = -1;
    ui->pointer_cell = -1;
    ui->long_fired = false;
    ui->last_press_consumed = false;
    ui->tap_armed = false;
    ui->menu_sel = -1;
}
void ui_pointer_down(Ui *ui, int32_t x, int32_t y, uint32_t now) {
    int32_t hit_idx = -1;
    const int32_t hit = ui_hit_test(ui, x, y, &hit_idx);
    ui->now_hint = now;
    ui->pointer_down = true;
    ui->press_start_ms = now;
    ui->long_fired = false;
    ui->menu_sel = -1;
    ui->down_hit = hit;
    ui->down_idx = hit_idx;
    // 拖动基准。注意两套值：
    //   pan_start/base_*  —— 当前这一次拖动段的基准（重贴时会被重置）
    //   pan_restore_*     —— 按下那一刻的偏移，松手时若判定为"点按"就恢复它
    ui->pan_start_x = x;
    ui->pan_start_y = y;
    ui->pan_base_x = ui->pan_x;
    ui->pan_base_y = ui->pan_y;
    ui->down_x = x;
    ui->down_y = y;
    ui->down_dx = 0;
    ui->down_dy = 0;
    ui->pan_restore_x = ui->pan_x;
    ui->pan_restore_y = ui->pan_y;
    ui->pan_reverted = false;

    if (ui->overlay != UI_OVERLAY_NONE) {
        // 浮层里按下的位置已经由 ui_hit_test 判定；这里只记住"按在哪个按钮/输入框上"
        if (hit == UI_HIT_OVERLAY_BTN) ui->down_idx = hit_idx;
        return;
    }

    if (hit == UI_HIT_DROPDOWN || hit == UI_HIT_MENU_ITEM || hit == UI_HIT_MENU) {
        return; // 菜单相关的手势留给 pointer_up 处理
    }

    if (hit == UI_HIT_FACE) {
        ui->face_down = true;
        return;
    }

    // ---- 滚动条：按下即开始拖动滑块 ----
    // 点滑块 → 抓在按下点（细调）；点轨道 → 滑块中心对到手指（粗跳）。
    // 拖动中"偏移 ↔ 滑块位置"的换算参数在这里存下来，避免每帧重算。
    if (hit == UI_HIT_SB_H || hit == UI_HIT_SB_V) {
        const UiLayout L = ui_layout(ui);
        const bool horiz = (hit == UI_HIT_SB_H);
        UiRect bar;
        if (horiz) sb_h_rect(&L, &bar); else sb_v_rect(&L, &bar);
        const int32_t track = horiz ? bar.w : bar.h;
        const int32_t thumb = horiz ? L.sb_thumb_h_w : L.sb_thumb_v_h;
        if (track > 0 && thumb > 0 && track - thumb > 0) {
            const int32_t base = horiz ? bar.x : bar.y;   // 条带自身起点
            const int32_t pos = (horiz ? x : y) - base;
            const int32_t t0 = horiz ? L.sb_thumb_h_x : L.sb_thumb_v_y;
            ui->sb_drag = horiz ? 1 : 2;
            ui->sb_track = track;
            ui->sb_thumb = thumb;
            ui->sb_max_pan = (horiz ? (int32_t)ui->game.w * L.cell - L.vis_cols * L.cell
                                    : (int32_t)ui->game.h * L.cell - L.vis_rows * L.cell);
            ui->sb_grab = (pos >= t0 && pos < t0 + thumb) ? (pos - t0) : (thumb / 2);
            // 拖动中用"手指相对按下点的位移"推滑块位置，所以这里要记住按下点的
            // **屏幕坐标**（不是相对轨道的偏移 —— 早先存成绝对偏移又按相对位移用，
            // 两者符号相反、正好抵消，滑块纹丝不动）。
            ui->sb_base_x = x;
            ui->sb_base_y = y;
            // 点轨道：立刻把滑块中心对到手指
            if (!(pos >= t0 && pos < t0 + thumb)) {
                ui_sb_apply(ui, pos - ui->sb_grab);
            }
        }
        return;
    }

    // 棋盘：记住按住的那一格（长按判定与"松手时还在同一格"都看它）。
    // 未翻开的格子顺带做按下预览。
    const int32_t cell = (hit == UI_HIT_BOARD) ? hit_idx : -1;
    ui->pointer_cell = cell;
    if (cell >= 0 && !ui->game.over) {
        ui->long_press_cell = cell;
        ui->press_cell = (ui->game.open[cell] == 0) ? cell : -1;
    }
    else {
        ui->long_press_cell = -1;
        ui->press_cell = -1;
    }
}

void ui_pointer_move(Ui *ui, int32_t x, int32_t y, uint32_t now) {
    (void)now;
    if (!ui->pointer_down) return;
    if (ui->overlay != UI_OVERLAY_NONE) return;
    // 下拉与菜单只在按下/松开时判定，移动只负责取消棋盘预览
    if (ui->open_menu >= 0) return;

    // ---- 滚动条拖动：优先于棋盘拖动 ----
    // 滚动条的语义是"滑块跟手"，不能用棋盘那套（从按下点算偏移）。
    if (ui->sb_drag != 0) {
        const UiLayout L = ui_layout(ui);
        UiRect bar;
        if (ui->sb_drag == 1) sb_h_rect(&L, &bar); else sb_v_rect(&L, &bar);
        const int32_t base = (ui->sb_drag == 1) ? bar.x : bar.y;
        // 滑块在轨道里的新位置 = 按下时滑块的位置 + 手指相对按下点走了多远
        const int32_t t0 = base + ((ui->sb_drag == 1) ? L.sb_thumb_h_x : L.sb_thumb_v_y);
        const int32_t d = (ui->sb_drag == 1) ? (x - ui->sb_base_x) : (y - ui->sb_base_y);
        ui_sb_apply(ui, t0 + d - base);
        return;
    }

    // ---- 拖动：一有位移就立刻跟手 ----
    //
    // 这里**没有**"必须先挪够 N 像素才算拖动"的门槛。早先有那个门槛
    // （UI_PAN_SLOP_PX 那套），实测手感的抱怨是：
    // "拖动过程中很长时间都显示拖动前的画面，必须拖过一段距离才突然进入平滑拖动"。
    // 原因就是门槛之下一帧都不平移 —— 死区。而且真机上系统投递的坐标本身
    // 还有几像素的抖动/滞后，实际死区比常量更大。
    //
    // 现在改成：只要有位移就立刻平移，**"是点按还是拖动"留到松手时再判**
    // （见 ui_pointer_up：总位移小于阈值就恢复原位、按点按处理）。
    // 代价是极小的：手指在格子上微动时画面会跟着动一两像素，松手后恢复；
    // 收益是"按下即可拖动"，没有死区。
    if (ui->down_hit != UI_HIT_BOARD) return;   // 从侧栏/菜单起手不许拖棋盘

    // 相对**按下点**的总位移（这个要一直累计，不受重贴基准影响）
    ui->down_dx = x - ui->down_x;
    ui->down_dy = y - ui->down_y;
    const int32_t adx = ui->down_dx < 0 ? -ui->down_dx : ui->down_dx;
    const int32_t ady = ui->down_dy < 0 ? -ui->down_dy : ui->down_dy;

    // 平移基准就是**按下点**，不重贴。
    // 早先的"重贴"（ui_pan_begin 用当前点做基准）会把基准之前的位移丢掉，
    // 那种写法在跨过阈值的那一帧会"跳"一下；以按下点为基准才是一比一跟手。
    if (!ui->pan_active) ui->pan_active = true;

    // 手指只是轻轻抖一下（总位移还在阈值内）时**不算拖动**：
    // 保留按格手势（长按/点按都要能用），但不跟手 —— 否则每点一下就
    // 因为一两像素的抖动把画面挪走、松手再弹回来，看着很脏。
    // 一越过阈值就立刻切成拖动：取消按格手势，画面开始跟手。
    if (adx >= UI_PAN_SLOP_PX || ady >= UI_PAN_SLOP_PX) {
        ui_pointer_cancel(ui);
        ui_pan_to(ui, x, y);
    }
}

void ui_tick(Ui *ui, uint32_t now) {
    Game *g = &ui->game;
    ui->now_hint = now;
    // 拖动期间不推进长按/点按：手指在滑，不该顺手翻格或插旗。
    // 但**按下的"抖动"不算拖动**：拖动现在从第一像素就生效（为了没有死区），
    // 若只按 pan_active 判，手指在格子上轻轻一动就会吃掉长按。
    // 所以这里看的是"总位移有没有超过阈值" —— 与松手时判点按/拖动同一把尺子。
    if (ui->pan_active && (ui->down_dx >= UI_PAN_SLOP_PX || -ui->down_dx >= UI_PAN_SLOP_PX ||
                           ui->down_dy >= UI_PAN_SLOP_PX || -ui->down_dy >= UI_PAN_SLOP_PX)) {
        return;
    }

    // ---- 长按判定：按住不放，且指针还在原来那一格上 ----
    // 注意判据用 long_press_cell 而不是 press_cell：已翻开的数字格没有"按下预览"
    // （press_cell 是 -1），但长按展开仍然要能用。
    if (ui->pointer_down && !ui->long_fired && ui->long_press_cell >= 0) {
        if ((uint32_t)(now - ui->press_start_ms) >= UI_LONG_PRESS_MS) {
            ui->long_fired = true;
            ui->last_press_consumed = true;
            const size_t c = (size_t)ui->long_press_cell;
            if (g->open[c] != 0 && g->mine[c] == 0) {
                // 已翻开的数字格：长按 = 展开（和双击等价）
                game_try_expand(g, c);
                if (!g->over) flash_face(ui, now);
            }
            else if (g->open[c] == 0 && g->flag[c] == 0) {
                do_reveal(ui, ui->press_cell, now);
            }
            ui->press_cell = -1;
            // 长按吃掉了这一格的点按与双击
            ui->pending_cell = -1;
            ui->last_tap_cell = -1;
            ui->tap_armed = false;
        }
    }

    // ---- 单击点按：等双击窗口过去再插旗 ----
    if (ui->tap_armed && ui->pending_cell >= 0 &&
        (uint32_t)(now - ui->pending_since) >= UI_TAP_CONFIRM_MS) {
        if (game_cycle_flag(g, (size_t)ui->pending_cell)) flash_face(ui, now);
        ui->pending_cell = -1;
        ui->tap_armed = false;
    }

    // 计时
    if (g->started && !g->over) g->elapsed_ms = now - g->t0;
}

// 松手时判定：这一下到底是"拖动"还是"点按"。
//
// 拖动**从第一像素就生效**（这样没有死区、按下即跟手），
// 但"要不要把它当成拖动"留到这里判：总位移还不到阈值，就说明手指只是
// 在格子上抖了一下，应当按点按处理 —— 把画面恢复成按下时的样子，
// 让调用方继续走点按/长按的收尾逻辑。
//
// 返回 true 表示"确实是一次拖动"（调用方不要再走点按逻辑）。
bool ui_pointer_settle(Ui *ui) {
    if (!ui->pan_active) return false;
    const int32_t adx = ui->down_dx < 0 ? -ui->down_dx : ui->down_dx;
    const int32_t ady = ui->down_dy < 0 ? -ui->down_dy : ui->down_dy;
    if (adx >= UI_PAN_SLOP_PX || ady >= UI_PAN_SLOP_PX) return true;   // 真拖动
    // 只是抖动：把偏移恢复成按下时的值，并让调用方继续按点按处理。
    // 用"总位移"判而不是看 pan_active：抖动时按格手势没被取消，
    // 长按/点按都要照常可用。
    if (!ui->pan_reverted) {
        ui->pan_x = ui->pan_restore_x;
        ui->pan_y = ui->pan_restore_y;
        ui->pan_reverted = true;
    }
    ui->pan_active = false;
    return false;
}

void ui_pointer_up(Ui *ui, int32_t x, int32_t y, uint32_t now) {
    // 滚动条：松手即结束，不参与点按/长按
    if (ui->sb_drag != 0) {
        ui->sb_drag = 0;
        ui->pointer_down = false;
        ui->pointer_cell = -1;
        ui->press_cell = -1;
        ui->long_press_cell = -1;
        ui->now_hint = now;
        ui_refresh_scale(ui);
        return;
    }
    // 先判定"拖动 or 点按"。是点按的话 ui_pointer_settle 已经恢复了原偏移，
    // 下面继续走正常的点按收尾（插旗/人脸/菜单）。
    if (ui_pointer_settle(ui)) {
        // 确认是拖动：这一下**只是拖动**，绝不能顺带翻格或插旗。
        ui->pan_active = false;
        ui->pointer_down = false;
        ui->pointer_cell = -1;
        ui->press_cell = -1;
        ui->long_press_cell = -1;
        ui->now_hint = now;
        ui_pan_end(ui);
        return;
    }
    if (!ui->pointer_down) return;
    ui->pointer_down = false;
    ui->face_down = false;
    ui->now_hint = now;

    // ---- 浮层：按钮 / 自定义面板的加减按钮 ----
    if (ui->overlay != UI_OVERLAY_NONE) {
        int32_t idx = -1;
        const int32_t hit = ui_hit_test(ui, x, y, &idx);
        if (ui->overlay == UI_OVERLAY_CUSTOM &&
            (hit == UI_HIT_DLG_INC || hit == UI_HIT_DLG_DEC) && idx >= 0) {
            // 安卓侧没有软键盘，数值只能这样增减。
            // 长短按都只用"按一次加一次"，步进 1；范围在 ui_dlg_step 里钳。
            ui_dlg_step(ui, idx, hit == UI_HIT_DLG_INC ? 1 : -1);
            ui->dlg_focus = idx;
            ui->dlg_err = 0;
            return;
        }
        if (hit == UI_HIT_OVERLAY_BTN && idx >= 0) {
            OverlayBtn btns[4];
            const int32_t nb = overlay_buttons(ui, btns, 4);
            if (idx < nb) {
                const int32_t cmd = btns[idx].cmd;
                if (cmd == 100) {
                    // 按合计均分
                    uint16_t split[5];
                    const int32_t sum = ui->dlg_t1 + ui->dlg_t2 + ui->dlg_t3 + ui->dlg_t4;
                    game_split_evenly((uint16_t)(sum < 1 ? 1 : sum), split);
                    ui->dlg_t1 = split[1];
                    ui->dlg_t2 = split[2];
                    ui->dlg_t3 = split[3];
                    ui->dlg_t4 = split[4];
                    ui->dlg_err = 0;
                }
                else if (cmd == 101) {
                    if (ui->overlay == UI_OVERLAY_CUSTOM) ui_cmd_apply_custom(ui);
                    else ui_cmd_close_overlay(ui);
                }
                else if (cmd == 102) {
                    ui_cmd_close_overlay(ui);
                }
                return;
            }
        }
        // 点在面板外面：取消操作（自定义面板不关，避免误触丢掉填了一半的数值）
        if (hit != UI_HIT_OVERLAY_BTN && ui->overlay == UI_OVERLAY_CUSTOM) {
            return;
        }
        if (ui->overlay != UI_OVERLAY_CUSTOM) ui_cmd_close_overlay(ui);
        return;
    }

    // ---- 菜单栏：按下和松开都在同一个按钮上才算数 ----
    if (ui->down_hit == UI_HIT_MENU_ITEM) {
        int32_t idx = -1;
        const int32_t up_hit = ui_hit_test(ui, x, y, &idx);
        if (up_hit == UI_HIT_MENU_ITEM && idx == ui->down_idx) {
            const bool was_open = (ui->open_menu == idx);
            ui->open_menu = was_open ? UI_MENU_NONE : idx;
        }
        ui->press_cell = -1;
        ui->long_press_cell = -1;
        ui->last_press_consumed = false;
        return;
    }
    if (ui->open_menu >= 0) {
        // 下拉展开时松开：命中的那一项执行命令
        int32_t idx = -1;
        const int32_t up_hit = ui_hit_test(ui, x, y, &idx);
        const int32_t menu = ui->open_menu;
        if (up_hit == UI_HIT_DROPDOWN && idx >= 0) {
            run_cmd(ui, menu_item_cmd(menu, idx));
            return;
        }
        if (up_hit != UI_HIT_MENU_ITEM && up_hit != UI_HIT_DROPDOWN) {
            ui->open_menu = UI_MENU_NONE;
            ui->menu_sel = -1;
        }
        ui->press_cell = -1;
        ui->long_press_cell = -1;
        return;
    }
    ui->menu_sel = -1;

    // ---- 人脸：单点即重开（不参与双击判定） ----
    if (ui->down_hit == UI_HIT_FACE) {
        int32_t idx = -1;
        const int32_t up_hit = ui_hit_test(ui, x, y, &idx);
        if (up_hit == UI_HIT_FACE) {
            // 人脸按钮：单点即重开。用新鲜种子（时间戳混计数器），
            // 同一毫秒内连点两次也不会拿到同一张图。
            ui_cmd_new_game(ui, now ^ ui_fresh_seed(ui), true);
            ui->last_press_consumed = false;
            return;
        }
    }

    const int32_t cell = ui->long_press_cell;
    const bool consumed = ui->last_press_consumed;
    ui->press_cell = -1;
    ui->long_press_cell = -1;
    ui->last_press_consumed = false;

    if (consumed) return;   // 长按已经处理过这一格
    if (cell < 0) return;
    // 松开时必须还在同一格上（拖走就当作取消）
    if (board_cell_at(ui, x, y) != cell) return;

    // ---- 双击 = 展开 ----
    if (ui->last_tap_cell == cell && (uint32_t)(now - ui->last_tap_ms) <= UI_DOUBLE_TAP_MS) {
        ui->last_tap_cell = -1;
        ui->tap_armed = false;
        if (ui->game.open[(size_t)cell] != 0) {
            game_try_expand(&ui->game, (size_t)cell);
            if (!ui->game.over) flash_face(ui, now);
        }
        return;
    }
    // ---- 单指点按：等双击窗口过去再插旗 ----
    ui->last_tap_cell = cell;
    ui->last_tap_ms = now;
    ui->pending_cell = cell;
    ui->pending_since = now;
    ui->tap_armed = true;
}
