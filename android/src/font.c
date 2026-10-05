#include "font.h"

#include <string.h>

#include "font_meta.h"

// font_atlas.bin 的头（与 tools/gen_font.js 对应）：
//   magic "CSFT" | u16 version | u16 page_count | u16 page_w | u16 page_h
//   | u16 size_count | u16 保留 | page_count×u32 每页字节数 | 各页 1 位色数据
#define FONT_HDR_FIXED 18

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 字形表按 (size, cp) 升序生成，用二分找。
//
// **比较必须是 (size, cp) 的字典序**。早先写成"size 不等就只按 size 二分、
// size 相等才比 cp"，那是错的：数组是按 (size,cp) 排的，只按 size 比较会
// 在 mid 落在别的档位时把范围切错，找不到本来存在的字形。
// 这个 bug 在只有一个字号层（0/1/2 全被用到、且查的就是那几个）时不明显，
// 扩到 9 档之后就在设备上表现为**整个界面文字乱码**（找不到就退化成错误字形）。
static int glyph_cmp(const FontGlyph *g, int32_t size, uint32_t cp) {
    if ((int32_t)g->size != size) return (int32_t)g->size < size ? -1 : 1;
    if (g->cp != cp) return g->cp < cp ? -1 : 1;
    return 0;
}
static const FontGlyph *find_glyph(int size, uint32_t cp) {
    int32_t lo = 0;
    int32_t hi = FONT_GLYPH_COUNT - 1;
    while (lo <= hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        const int32_t c = glyph_cmp(&font_glyphs[mid], size, cp);
        if (c == 0) return &font_glyphs[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

bool font_init(Font *f, const Render *r) {
    memset(f, 0, sizeof(*f));
    if (!r->font || r->font_len < FONT_HDR_FIXED) return false;
    if (memcmp(r->font, "CSFT", 4) != 0) return false;
    f->page_count = rd16(r->font + 6);
    f->page_w = rd16(r->font + 8);
    f->page_h = rd16(r->font + 10);
    f->size_count = rd16(r->font + 12);
    if (f->page_count > 8) return false;
    const size_t page_stride = (size_t)((f->page_w + 7) / 8);
    const size_t page_bytes = page_stride * (size_t)f->page_h;
    size_t off = FONT_HDR_FIXED + (size_t)f->page_count * 4;
    for (uint16_t i = 0; i < f->page_count; i++) {
        const uint32_t n = rd32(r->font + FONT_HDR_FIXED + i * 4);
        if (off + n > r->font_len) return false;
        // **每一页都必须有整页的字节数**：取字形时是按 page_w/8 * page_h 算行步长的，
        // 页数据短了就会读到后面页的内容 —— 表现为整个界面文字乱码。
        // （生成脚本曾经每页只分配"自己用到的高度"，单页时看不出来，
        //   加到两页就炸了。见 tools/gen_font.js 里的补零与自检。）
        if ((size_t)n < page_bytes) return false;
        f->pages[i] = r->font + off;
        off += n;
    }
    // 码点 → 是否有字形（按 sd 档查就够了：三档的字形集合一致）
    for (uint32_t cp = 0; cp < 0x10000u; cp++) {
        f->lookup[cp] = find_glyph(FONT_SM, cp) ? 1u : 0u;
    }
    f->loaded = true;
    return true;
}

int32_t font_line_h(int size) {
    if (size < 0 || size >= FONT_SIZE_COUNT) size = FONT_SM;
    return font_sizes[size].line_h;
}

int32_t font_ascent(int size) {
    if (size < 0 || size >= FONT_SIZE_COUNT) size = FONT_SM;
    return font_sizes[size].ascent;
}

int32_t font_px(int size) {
    if (size < 0 || size >= FONT_SIZE_COUNT) size = FONT_SM;
    return font_sizes[size].px;
}

// 这个字号里最小的 by：也就是"墨水上边界"离字形原点最近的那一档。
// 用真实字形扫一遍而不是拿 ascent 硬算 —— 中文字形在微软雅黑里普遍有 1~2px 的
// 上下差异，硬算出来的居中会差一截。
int32_t font_ink_top(int size) {
    if (size < 0 || size >= FONT_SIZE_COUNT) size = FONT_SM;
    static int32_t cached[FONT_SIZE_COUNT];
    static bool done[FONT_SIZE_COUNT] = { false, false, false };
    if (done[size]) return cached[size];
    int32_t top = 1 << 20;
    for (int32_t i = 0; i < FONT_GLYPH_COUNT; i++) {
        const FontGlyph *g = &font_glyphs[i];
        if (g->size != (uint16_t)size) continue;
        if (g->w == 0 || g->h == 0) continue;   // 空格没有墨水，不参与
        if (g->by < top) top = g->by;
    }
    if (top == (1 << 20)) top = 0;
    cached[size] = top;
    done[size] = true;
    return top;
}

bool font_text_ink_bounds(const char *utf8, int size, int32_t *out_top, int32_t *out_bottom) {
    if (!utf8) return false;
    int32_t top = 1 << 20, bottom = -(1 << 20);
    const char *p = utf8;
    uint32_t cp = 0;
    int32_t n;
    while ((n = font_utf8_next(p, &cp)) > 0) {
        const FontGlyph *g = find_glyph(size, cp);
        if (g && g->w > 0 && g->h > 0) {
            if (g->by < top) top = g->by;
            const int32_t b = g->by + (int32_t)g->h;
            if (b > bottom) bottom = b;
        }
        p += n;
    }
    if (bottom < top) return false;   // 这段文字没有墨水（全是空格之类）
    if (out_top) *out_top = top;
    if (out_bottom) *out_bottom = bottom;
    return true;
}

int32_t font_center_y_for_text(const char *utf8, int size, int32_t box_y, int32_t box_h) {
    if (size < 0 || size >= FONT_SIZE_COUNT) size = FONT_SM;
    int32_t top = 0, bottom = font_sizes[size].line_h;
    if (!font_text_ink_bounds(utf8, size, &top, &bottom)) {
        // 空文字：按行高居中
        return box_y + (box_h - font_sizes[size].line_h) / 2;
    }
    // 让 [top, bottom] 这段墨水在框里居中
    return box_y + (box_h - (bottom - top)) / 2 - top;
}

// 这个字号里所有字形的墨水总高度（min by ～ max(by+h)）。
// 调大字号之后一定要用它给"框"保底：字号的像素高度是固定的，
// 而界面缩放只跟窗口尺寸走，某些屏幕密度下框会矮到把字裁掉。
int32_t font_ink_height(int size) {
    if (size < 0 || size >= FONT_SIZE_COUNT) size = FONT_SM;
    int32_t top = 1 << 20, bottom = 0;
    for (int32_t i = 0; i < FONT_GLYPH_COUNT; i++) {
        const FontGlyph *g = &font_glyphs[i];
        if (g->size != (uint16_t)size) continue;
        if (g->w == 0 || g->h == 0) continue;
        if (g->by < top) top = g->by;
        const int32_t b = g->by + (int32_t)g->h;
        if (b > bottom) bottom = b;
    }
    if (top > bottom) return font_sizes[size].line_h;   // 该字号没有字形，退回行高
    return bottom - top;
}

int32_t font_center_y(int size, int32_t box_y, int32_t box_h) {
    if (size < 0 || size >= FONT_SIZE_COUNT) size = FONT_SM;
    int32_t top = font_ink_top(size);
    int32_t bottom = 0;
    for (int32_t i = 0; i < FONT_GLYPH_COUNT; i++) {
        const FontGlyph *g = &font_glyphs[i];
        if (g->size != (uint16_t)size) continue;
        if (g->w == 0 || g->h == 0) continue;
        const int32_t b = g->by + (int32_t)g->h;
        if (b > bottom) bottom = b;
    }
    return box_y + (box_h - (bottom - top)) / 2 - top;
}

int32_t font_utf8_next(const char *s, uint32_t *cp) {
    const unsigned char *p = (const unsigned char *)s;
    if (!p || p[0] == 0) return 0;
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        return 4;
    }
    // 非法字节：跳过去，别让排版卡死
    *cp = 0xFFFD;
    return 1;
}

int32_t font_text_width(const Font *f, const char *utf8, int size) {
    (void)f; // 度量只依赖编译期生成的 font_glyphs 表，不需要运行时的 Font
    if (!utf8) return 0;
    int32_t x = 0;
    const char *p = utf8;
    uint32_t cp = 0;
    int32_t n;
    while ((n = font_utf8_next(p, &cp)) > 0) {
        const FontGlyph *g = find_glyph(size, cp);
        x += g ? g->advance : font_px(size) / 2;
        p += n;
    }
    return x;
}

bool font_has(const Font *f, int size, uint32_t cp) {
    (void)f;
    return find_glyph(size, cp) != NULL;
}

int32_t font_draw(Render *r, const Font *f, int size, const char *utf8,
                  int32_t x, int32_t y, uint32_t bgra) {
    if (!utf8 || !r || !r->pixels) return x;
    int32_t pen = x;
    const char *p = utf8;
    uint32_t cp = 0;
    int32_t n;
    const int32_t ascent = font_ascent(size);
    while ((n = font_utf8_next(p, &cp)) > 0) {
        const FontGlyph *g = find_glyph(size, cp);
        if (!g) {
            pen += font_px(size) / 2;
            p += n;
            continue;
        }
        if (g->w > 0 && g->h > 0 && g->page < f->page_count) {
            // 墨水盒在页里的位置 → 页内按行取 1 位数据。
            // by 是从"笔位行顶"量起的（DrawString 在 (0,0) 画，所以 by 就是
            // 相对行顶的偏移；ascent 只在需要基线对齐时用）。
            const int32_t gx = pen + g->bx;
            const int32_t gy = y + g->by;
            const uint16_t stride = (uint16_t)((f->page_w + 7) / 8);
            const uint8_t *page = f->pages[g->page];
            for (int32_t ry = 0; ry < (int32_t)g->h; ry++) {
                const int32_t py = gy + ry;
                if (py < 0 || py >= r->h) continue;
                const uint8_t *src_row = page + (size_t)(g->y + ry) * stride;
                uint32_t *dst = r->pixels + (size_t)py * (size_t)r->w;
                for (int32_t rx = 0; rx < (int32_t)g->w; rx++) {
                    const int32_t px = gx + rx;
                    if (px < 0 || px >= r->w) continue;
                    const int32_t sx = g->x + rx;
                    if ((src_row[sx >> 3] & (0x80u >> (sx & 7))) == 0) continue;
                    dst[px] = bgra;
                }
            }
        }
        pen += g->advance;
        p += n;
    }
    (void)ascent;
    return pen;
}

int32_t font_check_coverage(const Font *f, int size, const char *utf8, char *missing, size_t missing_cap) {
    (void)f;
    if (!utf8) return 0;
    int32_t n_missing = 0;
    size_t used = 0;
    const char *p = utf8;
    uint32_t cp = 0;
    int32_t n;
    while ((n = font_utf8_next(p, &cp)) > 0) {
        if (cp == '\n' || cp == '\r' || cp == ' ') {
            p += n;
            continue;
        }
        if (!find_glyph(size, cp)) {
            n_missing += 1;
            // 把缺的字符原样写进报告（可能就是这几行字的锅）
            if (used + (size_t)n + 1 < missing_cap) {
                memcpy(missing + used, p, (size_t)n);
                used += (size_t)n;
                missing[used] = '\0';
            }
        }
        p += n;
    }
    return n_missing;
}
