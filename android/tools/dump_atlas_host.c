// 调试工具：把构建出来的图集与字形图集渲染成 PNG，用来肉眼核对素材是否正确定位。
//
// 为什么需要它：界面出问题时，"到底是下标错了、还是字形数据错了、还是绘制代码错了"
// 这三件事靠读代码很难分辨。把两份图集直接铺出来看，一眼就能定位。
//
//   --atlas <atlas.bin> <out.png>     把每个槽位连同它的下标一起铺成网格
//   --font  <font_atlas.bin> <size> <out.png>   把某个字号的所有字形铺成网格
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "font.h"
#include "font_meta.h"
#include "render.h"

#define MAX_W 2048
#define MAX_H 2048
static uint32_t g_fb[MAX_W * MAX_H];

static const uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

// 把图集里每个槽位按原始尺寸铺成网格，格与格之间留 4 像素暗底
static int dump_atlas(const char *bin, const char *out) {
    size_t len = 0;
    const uint8_t *data = read_file(bin, &len);
    if (!data) { fprintf(stderr, "读不到 %s\n", bin); return 1; }
    Render r;
    render_init(&r, g_fb, MAX_W, MAX_H);
    if (!render_load_atlas(&r, data, len)) { fprintf(stderr, "图集解析失败\n"); return 1; }
    printf("atlas %ux%u\n", r.atlas_w, r.atlas_h);

    render_fill(&r, 0, 0, MAX_W, MAX_H, CS_RGB(0x30, 0x30, 0x60));
    int32_t x = 4, y = 4, row_h = 0;
    for (uint16_t i = 0; i < SP_COUNT; i++) {
        const SpriteRect s = render_rect(&r, i);
        if (x + s.w + 4 > MAX_W) { x = 4; y += row_h + 4; row_h = 0; }
        // 槽位底色：亮色棋盘格，方便看清透明区域
        render_fill(&r, x - 1, y - 1, s.w + 2, s.h + 2, CS_RGB(0x88, 0x88, 0x88));
        render_fill(&r, x, y, s.w, s.h, CS_RGB(0x00, 0x00, 0x00));
        render_blit(&r, i, x, y, s.w, s.h);
        x += s.w + 4;
        if (s.h > row_h) row_h = s.h;
    }
    const int32_t h = y + row_h + 4;
    const bool ok = render_write_png(out, g_fb, MAX_W, h);
    printf("写出 %s（%d×%d，%u 个槽位）\n", out, MAX_W, h, SP_COUNT);
    free((void *)data);
    return ok ? 0 : 1;
}

// 把某个字号的所有字形铺成网格（白底黑字），便于确认字形数据本身没问题
static int dump_font(const char *bin, int size, const char *out) {
    size_t flen = 0;
    const uint8_t *fdata = read_file(bin, &flen);
    if (!fdata) { fprintf(stderr, "读不到 %s\n", bin); return 1; }
    Render r;
    render_init(&r, g_fb, MAX_W, MAX_H);
    if (!render_load_font(&r, fdata, flen)) { fprintf(stderr, "字形图集解析失败\n"); return 1; }
    Font font;
    if (!font_init(&font, &r)) { fprintf(stderr, "字形图集头解析失败\n"); return 1; }
    printf("font pages=%u %ux%u sizes=%u\n", font.page_count, font.page_w, font.page_h, font.size_count);

    // 把原始页直接铺出来（每页一行），这是最直接的核对方式
    render_fill(&r, 0, 0, MAX_W, MAX_H, CS_RGB(0xFF, 0xFF, 0xFF));
    int32_t y = 4;
    for (uint16_t p = 0; p < font.page_count && p < 1; p++) {
        const uint16_t stride = (uint16_t)((font.page_w + 7) / 8);
        const uint8_t *page = font.pages[p];
        for (int32_t py = 0; py < font.page_h; py++) {
            for (int32_t px = 0; px < font.page_w; px++) {
                if ((page[(size_t)py * stride + (px >> 3)] & (0x80u >> (px & 7))) == 0) continue;
                g_fb[(size_t)(y + py) * MAX_W + px] = CS_RGB(0, 0, 0);
            }
        }
        y += font.page_h + 6;
    }

    // 再在下面按字号逐字铺一遍，确认 find_glyph + 绘制路径都对
    render_fill(&r, 0, y + 4, MAX_W, 2, CS_RGB(0xFF, 0, 0));
    int32_t cy = y + 12;
    render_fill(&r, 0, cy, MAX_W, (int32_t)font_line_h(size) + 4, CS_RGB(0xE0, 0xE0, 0xE0));
    int32_t cx = 4;
    for (uint32_t cp = 32; cp < 0x3000 && cx < MAX_W - 40; cp++) {
        if (!font_has(&font, size, cp)) continue;
        char buf[8];
        int32_t n = 0;
        if (cp < 0x80) buf[n++] = (char)cp;
        else if (cp < 0x800) { buf[n++] = (char)(0xC0 | (cp >> 6)); buf[n++] = (char)(0x80 | (cp & 0x3F)); }
        else { buf[n++] = (char)(0xE0 | (cp >> 12)); buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[n++] = (char)(0x80 | (cp & 0x3F)); }
        buf[n] = 0;
        font_draw(&r, &font, size, buf, cx, cy + 2, CS_RGB(0, 0, 0));
        cx += 26;
        if (cx > MAX_W - 40) { cx = 4; cy += font_line_h(size) + 4; render_fill(&r, 0, cy, MAX_W, (int32_t)font_line_h(size) + 4, CS_RGB(0xE0, 0xE0, 0xE0)); }
    }
    const int32_t h = cy + font_line_h(size) + 8;
    const bool ok = render_write_png(out, g_fb, MAX_W, h);
    printf("写出 %s（%d×%d，字号 %d）\n", out, MAX_W, h, size);
    free((void *)fdata);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "--atlas")) return dump_atlas(argv[2], argv[3]);
    if (argc >= 5 && !strcmp(argv[1], "--font")) return dump_font(argv[2], atoi(argv[3]), argv[4]);
    fprintf(stderr,
            "用法:\n"
            "  --atlas <atlas.bin> <out.png>\n"
            "  --font  <font_atlas.bin> <size 0|1|2> <out.png>\n");
    return 2;
}
