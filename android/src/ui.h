// 复扫雷 · 安卓界面层。
//
// 交互是触屏的（单指点按插旗 / 长按翻开 / 双击展开），布局是**左侧信息栏 + 右侧棋盘**：
//
//   ┌──────────┬───────────────────────────┐
//   │  菜单栏（横跨全宽）                    │
//   ├──────────┼───────────────────────────┤
//   │   笑脸    │                           │
//   │  计雷器1  │        棋盘（可拖动）       │
//   │  计雷器2  │                           │
//   │  计雷器3  │                           │
//   │  计雷器4  │                           │
//   │   计时器   │                           │
//   └──────────┴───────────────────────────┘
//     宽 = 长边/5            宽 = 剩下 4/5
//
// 两条关键约定（都是被真机问题逼出来的，改之前先读这里）：
//
// 1. **没有缩放层**。帧缓冲尺寸 == 窗口尺寸，软件渲染 1:1 上屏。所以布局坐标就是
//    设备像素，触摸坐标与绘制天然同源。单格边长直接由物理尺寸算出（0.7cm），
//    不存在"物理尺寸 → 整数倍率"的取整，也不会出现缩放对不上导致的点击偏移。
//
// 2. **单格边长（cell）与 HUD 缩放（hud）是两件事**。早先它们都由 zoom 驱动，
//    但格子改成按物理尺寸定之后，77px 的格子会把计雷器/人脸撑到荒谬的尺寸，
//    所以 HUD 用自己独立的整数缩放（挑"能让侧栏装下的最大整数"）。
//
// 3. **可见格数固定，超出部分靠拖动**。拖动偏移恒为单格整数倍、视口也取整数格，
//    这样屏幕上永远是若干个完整格子，不会出现半格。偏移会被钳在
//    [0, 棋盘尺寸 - 视口尺寸]；棋盘比视口小时该轴居中。
#ifndef CS_UI_H
#define CS_UI_H

#include <stdint.h>
#include <stdbool.h>

#include "game.h"
#include "render.h"
#include "font.h"

// 手势判定阈值（毫秒）。上手后想微调只改这几个数。
#define UI_LONG_PRESS_MS 350   // 按住多久算"长按翻开"
#define UI_DOUBLE_TAP_MS 450   // 两次点按间隔多久内算双击
#define UI_TAP_CONFIRM_MS 250  // 单指点按等到多久才真正插旗（给双击留判定时间）
#define UI_FACE_FLASH_MS 200   // 「脸扫雷」闪动时长
// 拖动判定阈值（设备像素）。
//
// 这个值只管一件事：**区分"按住不动"和"开始拖"**，不改动"拖动是否跟手"。
// 小于它就认为手指还按在原处（长按/点按照常可用，画面不动）；
// 一旦达到就切成拖动，画面**从按下点一比一跟手**（不丢位移、不跳）。
//
// 为什么是 2：早先取 8，真机手感是"拖动过程中很长时间都显示拖动前的画面"。
// 8px 在 280dpi 下是 0.73mm —— 听着很小，但换算到时间就不小了：
// 手指以 50px/s 慢慢拖动时要 160ms 才跨过，100px/s 也要 80ms，
// 再加上系统投递坐标本身还有几像素抖动/滞后，实际死区更大、每次都能感知到。
// 降到 2px（0.18mm）后，同样的慢速拖动只要 10~40ms 就进入跟手，基本无感；
// 而静止时手指的抖动远小于 2px，长按不会被误判成拖动。
#define UI_PAN_SLOP_PX 2

// 单格的目标物理尺寸：**0.7cm，这里写成 7**（即"厘米数 × 10"）。
// 换算：像素 = UI_CELL_TARGET_CM * dpi / 254，其中 254 = 2.54(cm/英寸) × 100。
//
// 这里踩过一个大坑：一开始把 7 当成"毫米"、却仍然除以 254，于是 280dpi 下算出
// 8px 的格子（0.07cm）——整块棋盘小得看不清；而且因为它太小，下面那套"安全钳制"
// 永远不触发，30×16 的盘被一屏全塞进去，"可见格数固定 + 拖动"形同虚设。
#define UI_CELL_TARGET_CM 7

typedef enum {
    UI_OVERLAY_NONE = 0,
    UI_OVERLAY_CUSTOM,   // 自定义棋盘面板
    UI_OVERLAY_BEST,     // 最高分纪录
    UI_OVERLAY_HELP,     // 玩法
    UI_OVERLAY_ABOUT,    // 关于
} UiOverlay;

typedef enum { UI_MENU_NONE = -1, UI_MENU_GAME = 0, UI_MENU_HELP = 1 } UiMenu;

// 滚动条滑块的最小长度（设备像素）。太短就抓不住了。
// 注意它是绝对像素、不是 ×hud：滑块是"手指要按住拖"的目标，
// 和 HUD 的整数缩放无关。
#define SB_MIN_THUMB 56

typedef struct {
    int32_t win_w, win_h;

    // ---- HUD 缩放（独立于单格尺寸） ----
    int32_t hud;

    // ---- 顶部菜单栏（横跨全宽） ----
    int32_t menubar_h;

    // ---- 左侧信息栏 ----
    int32_t side_x, side_y, side_w, side_h;
    int32_t side_in_x, side_in_y, side_in_w, side_in_h;   // 去掉边框后的可用区

    // ---- 右侧棋盘区（含凹槽边框） ----
    int32_t board_x, board_y, board_w, board_h;
    int32_t field_x, field_y, field_w, field_h;           // 凹槽内部的落格区
    int32_t box;                                          // 凹槽边框厚度（固定 3h）
    // ---- 滚动条（横条贴视口下方、纵条贴视口右方；**厚度 = 一个格子边长**） ----
    int32_t sb_thick;                                     // 厚度 = cell
    int32_t sb_x, sb_y;                                   // 视口右/下边缘（= 条带起点）
    int32_t sb_w, sb_h;                                   // = sb_thick（保留字段，绘制请用 ui_scrollbar_rect）
    // 滑块（thumb）：长度按"可见部分 / 全盘"比例算，位置反映当前偏移
    int32_t sb_thumb_h_x, sb_thumb_h_w;                   // 横向滑块相对视口左边的 x 与宽
    int32_t sb_thumb_v_y, sb_thumb_v_h;                   // 纵向滑块相对视口上边的 y 与高

    // ---- 可见格视口（永远是完整的整数格） ----
    int32_t cell;                  // 单格边长（设备像素）
    int32_t vis_x, vis_y;          // 视口左上角（屏幕坐标）
    // 视口**容量**：落格区最多放得下几格（棋盘比视口大时就是屏幕上显示的格数）
    int32_t vis_cols, vis_rows;
    // 本帧需要绘制的格号区间（[first, end) 半开），由棋盘/裁剪/拖动一起决定。
    // 注意它**不等于** vis_cols：拖动之后起点不再是第 0 格，若还按 vis_cols 画，
    // 右侧就会少画整整一个拖动量（真机表现：拖到最右时右边空一大条）。
    int32_t vis_first_col, vis_end_col;
    int32_t vis_first_row, vis_end_row;
    int32_t pan_x, pan_y;          // 本帧实际使用的拖动偏移（钳制后的值）
    // 棋盘第 (0,0) 格左上角的屏幕坐标 = vis - pan。
    // 绘制与命中都**必须**用它，不要各自拿 vis/pan 再算一遍：
    // 拖动中 pan 可以停在非整格上，两处算法若不一致，点击就会偏。
    int32_t board_origin_x, board_origin_y;
    // 棋盘与视口的交集：真正可以画、可以点的区域。
    // 棋盘比视口小的时候它比视口小，用来把"棋盘外的空白"排除在命中之外。
    int32_t board_clip_x, board_clip_y, board_clip_w, board_clip_h;
} UiLayout;

// 命中结果
enum {
    UI_HIT_NONE = 0,
    UI_HIT_MENU,      // 菜单栏空白
    UI_HIT_MENU_ITEM, // 菜单栏上的一个按钮
    UI_HIT_DROPDOWN,  // 展开的菜单里一项
    UI_HIT_FACE,      // 左侧栏最上面的笑脸
    UI_HIT_SIDEBAR,   // 侧栏其余部分（计雷器/计时器，目前没有交互，但要点得中）
    UI_HIT_BOARD,     // 棋盘区
    UI_HIT_OVERLAY_BTN,
    // 自定义面板上的加减按钮（安卓侧没有软键盘，数值只能靠按钮增减）
    UI_HIT_DLG_DEC,
    UI_HIT_DLG_INC,
    // 棋盘右侧/下方的滚动条（条宽 = 一个格子边长）
    UI_HIT_SB_H,      // 下方横向
    UI_HIT_SB_V,      // 右侧纵向
};

typedef struct {
    // ---- 游戏 ----
    Game game;    // ---- 视图 ----
    int32_t win_w, win_h;      // 窗口尺寸（= 帧缓冲尺寸，设备像素）
    int32_t cell_px;           // 单格边长（设备像素），平台层按 dpi 设置
    int32_t hud_scale;         // HUD 整数缩放（1 或 2），ui_layout 里自动选
    // 字号层（0/1/2）：按屏幕密度挑，让不同 dpi 的设备得到相近的**物理**字号。
    // 字形是固定像素高度的光栅图，不能任意缩放，所以预生成几层、运行时挑一层。
    int32_t font_layer;
    int32_t font_pct;          // 该层相对基准的百分比（100/125/150），仅用于日志
    // 目标单格（按密度算出的 0.7cm，**未被安全钳制**）。
    // 字号层用它来定层，这样换难度/换窗口都不会让字号跳档。
    int32_t cell_target_px;
    int32_t pan_x, pan_y;      // 拖动偏移（设备像素，恒为 cell_px 的整数倍）
    // ---- 滚动条拖动状态 ----
    // 与棋盘拖动分开记：滚动条拖动时"偏移 = 滑块位置"，逻辑完全不同。
    int32_t sb_drag;           // 0=没在拖；1=横向；2=纵向
    int32_t sb_grab;           // 按下点相对滑块起点的偏移（像素，保留备用）
    int32_t sb_base_x, sb_base_y;   // 按下点的屏幕坐标（拖动按"手指位移"推滑块）
    int32_t sb_track;          // 按下时的轨道长度（拖动中用，避免每帧重算）
    int32_t sb_thumb;          // 按下时的滑块长度
    int32_t sb_max_pan;        // 按下时的最大偏移（滑块行程 ↔ 偏移 的换算）
    // ---- 指针状态 ----
    bool pointer_down;
    int32_t pointer_cell;
    int32_t press_cell;    // 按下预览：那一格画成已翻开的空白
    int32_t pending_cell;  // 等双击窗口到期的点按
    uint32_t pending_since;
    int32_t last_tap_cell;
    uint32_t last_tap_ms;
    bool tap_armed;        // 有一次点按正在等窗口
    int32_t long_press_cell;
    uint32_t press_start_ms;
    bool long_fired;
    // 按下时命中的区域与下标（松开时要确认落在同一个地方）
    int32_t down_hit;
    int32_t down_idx;
    // 这次按下是否已经被长按吃掉（松手就不该再走点按/双击）
    bool last_press_consumed;
    // ---- 拖动 ----
    bool pan_active;               // 这一下已经产生了平移
    int32_t pan_start_x, pan_start_y;
    int32_t pan_base_x, pan_base_y;   // 按下时已有的偏移
    // 按下的原始位置与累计位移：用来在松手时判"这是点按还是拖动"。
    // 注意不能复用 pan_start_*：重贴基准时它会被 ui_pan_begin 改写。
    int32_t down_x, down_y;
    int32_t down_dx, down_dy;
    int32_t pan_restore_x, pan_restore_y;   // 按下时的偏移，点按要恢复它
    bool pan_reverted;                      // 已经恢复过（避免重复恢复）
    // ---- 人脸动画 ----
    bool face_down;
    uint32_t face_flash_until;
    // 最近一次心跳的时间：绘制人脸时要用（"动作刚做完 200ms 内保持脸扫雷"）
    uint32_t now_hint;
    // ---- 菜单与浮层 ----
    int32_t open_menu;     // UI_MENU_*
    int32_t menu_sel;      // 下拉里当前按着的一项
    int32_t overlay;
    // ---- 自定义面板状态 ----
    int32_t dlg_w, dlg_h, dlg_t1, dlg_t2, dlg_t3, dlg_t4;
    int32_t dlg_focus;     // 刚按过加减的那一行（0..5），-1 = 无。只用于高亮反馈
    int32_t dlg_err;       // 0 = 错误码
    // ---- 纪录 ----
    int32_t best[3];
    bool scores_dirty;
    // ---- 状态文字（脚注）：只在关键节点给一句提示，正常局面留空 ----
    int32_t note;          // 0 = 无
} Ui;

// 初始化（默认高级盘）。窗口尺寸与单格尺寸都由平台层随后设置。
void ui_init(Ui *ui, int32_t win_w, int32_t win_h);
// 窗口尺寸变了（旋转/分屏）
void ui_resize(Ui *ui, int32_t win_w, int32_t win_h);
// 设置单格目标边长（设备像素）。平台层按 0.7cm × 屏幕密度算出后调用。
// 内部会做安全钳制：保证任一轴至少能显示 UI_MIN_VIS_COLS/ROWS 格、
// 且侧栏不至于窄到装不下 HUD。
void ui_set_cell_px(Ui *ui, int32_t cell_px);
// 目标单格边长（UI_CELL_TARGET_CM）在给定屏幕密度下是多少设备像素。
// 放在界面层而不是平台层，是为了让宿主机自检能直接把这条换算钉死 ——
// 它出过一次单位错误（算成 8px 而不是 77px），只在真机上才被发现。
int32_t ui_cell_px_for_dpi(int32_t dpi);
// 该 Ui 当前字号层对应的三档字号槽位（sm/md/lg），供自检按真实字号断言。
// 直接用 FONT_SM/MD/LG 是错的：那是最底层，高密度设备上并不用它。
int32_t ui_font_sm(const Ui *ui);
int32_t ui_font_md(const Ui *ui);
int32_t ui_font_lg(const Ui *ui);
// 重算 HUD 缩放、钳制单格与拖动偏移。窗口变化、单格变化、棋盘尺寸变化后都要调；
// 它不是内部细节而是**必须能被自检直接调**的一步（几何正确性全靠它保证）。
void ui_refresh_scale(Ui *ui);

// 当前布局（内部完成可见格数与拖动钳制，结果可直接用于绘制与命中）
UiLayout ui_layout(const Ui *ui);

// 把整帧画进帧缓冲
void ui_paint(Render *r, Ui *ui, const Font *font, const uint8_t *atlas, size_t atlas_len,
              const uint8_t *font_data, size_t font_len);

// ---- 触屏 ----
void ui_pointer_down(Ui *ui, int32_t x, int32_t y, uint32_t now);
void ui_pointer_move(Ui *ui, int32_t x, int32_t y, uint32_t now);
void ui_pointer_up(Ui *ui, int32_t x, int32_t y, uint32_t now);
// 松手时判定这一下是"拖动"还是"点按"：返回 true = 拖动（调用方不要再走点按逻辑）。
// 拖动从第一像素就生效（没有死区），"算不算拖动"留到这里判：
// 总位移不到 UI_PAN_SLOP_PX 就恢复按下时的偏移，当点按处理。
bool ui_pointer_settle(Ui *ui);
// 取消当前按格手势（拖动开始时调用，避免"拖完抬手顺手插一面旗"）
void ui_pointer_cancel(Ui *ui);
// 心跳：推进长按/双击/计时等与时间相关的状态
void ui_tick(Ui *ui, uint32_t now);

// ---- 拖动 ----
// 平台层判定"这一下是拖动"之后调用。pan_to 内部把偏移吸附到整格并钳制。
void ui_pan_begin(Ui *ui, int32_t x, int32_t y);
void ui_pan_to(Ui *ui, int32_t x, int32_t y);
void ui_pan_end(Ui *ui);
void ui_pan_reset(Ui *ui);

// ---- 命令（菜单与自检共用） ----
void ui_cmd_new_game(Ui *ui, uint32_t seed, bool use_seed);
void ui_cmd_preset(Ui *ui, int32_t idx);
// 取一个"下次一定不同"的新局种子。所有"新局/换难度/人脸重开/应用自定义"入口
// 都必须用它 —— 写死种子的后果是每一把雷区完全一样（见 ui.c 里的说明）。
uint32_t ui_fresh_seed(const Ui *ui);
void ui_cmd_apply_custom(Ui *ui);
bool ui_cmd_open_custom(Ui *ui);
void ui_cmd_close_overlay(Ui *ui);

// ---- 自检与宿主机预览用的查询 ----
int32_t ui_menu_item_count(int32_t menu);
const char *ui_menu_item_label(int32_t menu, int32_t idx);
// 命中测试：返回 UI_HIT_*；命中菜单栏/下拉/浮层按钮/棋盘格时把下标写进 *idx
// （棋盘的 *idx 是格号，其余是按钮或菜单项下标）。
int32_t ui_hit_test(const Ui *ui, int32_t x, int32_t y, int32_t *idx);

void ui_menubar_rect(const Ui *ui, int32_t idx, UiRect *out);
void ui_dropdown_rect(const Ui *ui, UiRect *out);
void ui_face_rect(const Ui *ui, UiRect *out);
// 第 t 类雷（1..4）的计雷器面板矩形（自检用来核对"四块等宽且都装进侧栏"）
int32_t ui_counter_rect(const Ui *ui, int32_t t, UiRect *out);
// 计时器面板矩形
void ui_timer_rect(const Ui *ui, UiRect *out);
// 第 t 类雷的计雷器宽度（实雷四格数字，虚雷三格 + 一格 i 单位，四块等宽）
int32_t ui_counter_width(const Ui *ui, int32_t t);

int32_t ui_overlay_button_rect(const Ui *ui, int32_t idx, UiRect *out);
const char *ui_overlay_button_label(int32_t idx);
int32_t ui_custom_field_rect(const Ui *ui, int32_t idx, UiRect *out);
const char *ui_custom_field_label(int32_t idx);
int32_t ui_custom_field_value(const Ui *ui, int32_t idx);
// 自定义面板里按一次加/减（idx: 0=高 1=宽 2..5=四种雷）。步进会在硬边界内钳制。
void ui_dlg_step(Ui *ui, int32_t idx, int32_t delta);
// 第 idx 行的"加(up=true)/减"按钮矩形，给绘制与命中共用，也便于自检模拟点击
void ui_custom_arrow_rect(const Ui *ui, int32_t idx, bool up, UiRect *out);
// 浮层面板矩形（绘制与命中共用；自检用它断言"按钮必须落在面板内"）
void ui_overlay_panel_rect(const Ui *ui, UiRect *out);
// 两条滚动条的矩形（绘制与命中共用）。自检用它断言"滚动条紧贴视口，中间不许有缝"。
// ui_scrollbar_rect 给的是**滑块**（厚度固定一格）；
// ui_scrollbar_track_rect 给的是**轨道**（铺满视口之外的剩余带，负责吃掉零头）。
void ui_scrollbar_rect(const Ui *ui, bool horizontal, UiRect *out);
void ui_scrollbar_track_rect(const Ui *ui, bool horizontal, UiRect *out);

#endif // CS_UI_H
