// 纯软件渲染：把界面画进一块 BGRA 帧缓冲。
//
// 为什么不用 Android 的 Canvas/Java 也不直接写 OpenGL 绘制：图形原语只有
// "贴一张图（可选最近邻缩放）"和"填一个矩形"，用软件画一遍最省事、最好测，
// 而且宿主机上能原样跑同一份代码把 PNG 导出来看。安卓端最后只做一件图形工作：
// 把这整块帧缓冲当一张纹理贴到一个全屏四边形上（GL_NEAREST，保持像素风）。
//
// 图集是 BGRA8、**预乘 alpha**（来自 GDI 的 32bpp DIB 语义），所以合成用
// 预乘公式：out = src + dst * (1 - a/255)。
#ifndef CS_RENDER_H
#define CS_RENDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint32_t *pixels; // 帧缓冲，BGRA8（小端下与 0xAARRGGBB 的 u32 一致）
    int32_t w;
    int32_t h;
    // 图集
    const uint8_t *atlas;
    size_t atlas_len;
    uint16_t atlas_w;
    uint16_t atlas_h;
    // 字形图集（1 位色多页）
    const uint8_t *font;
    size_t font_len;
} Render;

// 图集里的一个矩形
typedef struct { uint16_t x, y, w, h; } SpriteRect;

// 解析图集头部（CSAT），成功返回 true
bool render_load_atlas(Render *r, const uint8_t *data, size_t len);
// 解析字形图集（CSFT），成功返回 true
bool render_load_font(Render *r, const uint8_t *data, size_t len);

// 取第 i 个槽位的矩形
SpriteRect render_rect(const Render *r, uint16_t i);

void render_init(Render *r, uint32_t *pixels, int32_t w, int32_t h);

// 用不透明色填一个矩形（经典 Win95 面板色就这样铺）
void render_fill(Render *r, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bgra);

// 一个整数矩形（渲染层的裁剪框、界面层的版式查询都用它）
typedef struct { int32_t x, y, w, h; } UiRect;

// 贴一张图集里的图。dw/dh 与原始尺寸不同时做最近邻缩放。
void render_blit(const Render *r, uint16_t sprite, int32_t x, int32_t y, int32_t dw, int32_t dh);

// 同上，但额外裁剪到 clip 之内。棋盘滚动时用它，避免超出视口的格子盖住凹槽边框。
void render_blit_clipped(const Render *r, uint16_t sprite, int32_t x, int32_t y,
                         int32_t dw, int32_t dh, UiRect clip);

// 经典立体边框：raised = 左上亮、右下暗；sunken 反过来
void render_bevel(Render *r, int32_t x, int32_t y, int32_t w, int32_t h, int32_t t, bool raised);

// 1 位色字形：按 inkW/inkH 与掩码把字形染成指定颜色（用于菜单文字）
void render_glyph_mask(Render *r, const uint8_t *rows_hex, int32_t row_count,
                       int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bgra);

// 把帧缓冲写成 PNG（宿主机预览与自检用；安卓端不调）
bool render_write_png(const char *path, const uint32_t *pixels, int32_t w, int32_t h);

// ---- 经典 Win95 配色（与 Windows 版 win32.zig 里的 C_* 一致） ----
// 帧缓冲里的字节顺序是 B,G,R,A；用 0xAARRGGBB 这个 u32 写进去，在小端机上
// 正好落到 B,G,R,A，和 Windows 那边 GDI 的 32bpp DIB 语义一样。
#define CS_RGB(r, g, b) ((uint32_t)(0xFF000000u | ((uint32_t)(r)) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16)))
#define CS_C_BTNFACE CS_RGB(0xC0, 0xC0, 0xC0)
#define CS_C_HILIGHT CS_RGB(0xDF, 0xDF, 0xDF)
#define CS_C_SHADOW CS_RGB(0x80, 0x80, 0x80)
#define CS_C_BLACK CS_RGB(0x00, 0x00, 0x00)
#define CS_C_WHITE CS_RGB(0xFF, 0xFF, 0xFF)
#define CS_C_DARKGRAY CS_RGB(0x40, 0x40, 0x40)

#endif // CS_RENDER_H
