// 中文文字排版：把 1 位色字形按 UTF-8 文案排进帧缓冲。
//
// 不依赖系统字体、不做复杂排版：文案是固定的，只需要"逐字按前进宽度摆放"。
// 字形清单由 tools/gen_font.js 在构建期生成（只含界面上真正会出现的字符），
// 所以这里的查找表很小，缺字一眼就能看出来。
#ifndef CS_FONT_H
#define CS_FONT_H

// 字号与字形
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "render.h"

// 字号档（与 tools/ui_strings.js 的 SIZES 顺序一致）
// 字号档位。**分 3 个"缩放层"，每层 3 档**（sm/md/lg）：
//   层 0 = 基准（对应 280dpi、单格 77px）      1.00×
//   层 1 = 中密度设备                          1.25×
//   层 2 = 高密度设备（440dpi 左右）            1.50×
// 运行时由 ui.c 的 font_scale_pct() 按屏幕密度挑一层。
//
// 为什么要分层：字号是**固定像素高度**的光栅图，不能像矢量字那样任意缩放
// （缩放就糊了、失去像素风）。而不同 dpi 的设备需要不同像素高度才能得到
// 相同的物理字号，所以只能预生成几层、运行时挑最接近的一层。
// 早先只有一层，结果 440dpi 手机上中文只有 1.85mm（280dpi 上是 2.90mm）。
enum {
    FONT_SM = 0, FONT_MD = 1, FONT_LG = 2,
    FONT_LAYER_COUNT = 3,
    FONT_SIZE_COUNT = 9,
};
// 第 layer 层的某档字号
static inline int32_t font_slot(int32_t layer, int32_t kind) {
    if (layer < 0) layer = 0;
    if (layer > FONT_LAYER_COUNT - 1) layer = FONT_LAYER_COUNT - 1;
    return layer * 3 + kind;
}

typedef struct {
    bool loaded;
    uint16_t page_w, page_h;
    uint16_t page_count;
    uint16_t size_count;
    // 每页在 font 数据里的起始偏移（跳过头部与长度表）
    const uint8_t *pages[8];
    // 码点 → 字形下标（0xFFFFFFFF 表示没有这个字）
    uint32_t lookup[0x10000];
} Font;

// 绑定已解析好的字形图集数据，并建好查找表
bool font_init(Font *f, const Render *r);

// 一个字符串按指定字号排出来的宽度（像素）
int32_t font_text_width(const Font *f, const char *utf8, int size);

// 字号的行高
int32_t font_line_h(int size);
// 字号的"上伸部"像素数：从字形原点（行顶）到基线。用于需要按基线对齐的场合。
int32_t font_ascent(int size);
// 字号的像素高
int32_t font_px(int size);
// 这个字号里"墨水上边界"离字形原点的最小偏移（所有字形里最小的 by）。
int32_t font_ink_top(int size);
// 这个字号里"所有字形墨水范围"的总高度（从最小的 by 到最大的 by+h）。
// 用途：决定一个框最低要多高才能装下这号字 —— 菜单栏/面板按它保底，
// 免得固定像素的字号在某些屏幕密度下被裁掉（真机上出过这个问题）。
int32_t font_ink_height(int size);
// **这段文字**实际会画到的墨水范围（相对字形原点）。段落/按钮排版按真实范围算，
// 比按"整个字号所有字形"的合并范围紧得多（后者会被最深的那个字带到很低）。
// 没找到任何字形时返回 false。
bool font_text_ink_bounds(const char *utf8, int size, int32_t *out_top, int32_t *out_bottom);
// 一段文字在高度 h 的框里居中时，draw 的 y 应该是多少（原点 = 行顶）
int32_t font_center_y_for_text(const char *utf8, int size, int32_t box_y, int32_t box_h);
// 整个字号所有字形的墨水范围（保守，用于"至少要留多高"的自检）
int32_t font_center_y(int size, int32_t box_y, int32_t box_h);

// 在 (x, y_baseline_top) 处画一段 UTF-8 文本：y 是这一行的顶边。
// 返回画完之后笔位的 x。超出帧缓冲的部分由 render_fill 自己裁剪。
int32_t font_draw(Render *r, const Font *f, int size, const char *utf8,
                  int32_t x, int32_t y, uint32_t bgra);

// 把 UTF-8 解码成一个码点，返回消耗的字节数（0 = 结束或非法）
int32_t font_utf8_next(const char *s, uint32_t *cp);

// 诊断用：某个码点在指定字号里有没有字形
bool font_has(const Font *f, int size, uint32_t cp);

// 覆盖率自检：把一段 UTF-8 里所有字符都在图集里的情况检查一遍，
// 缺字写进 missing（调用方给缓冲）。返回缺字个数。
int32_t font_check_coverage(const Font *f, int size, const char *utf8, char *missing, size_t missing_cap);

#endif // CS_FONT_H
