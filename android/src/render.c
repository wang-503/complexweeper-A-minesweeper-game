#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 图集头（与 tools/gen_atlas.js 写出的格式对应）：
//   magic "CSAT" | u16 version | u16 count | u16 w | u16 h | count×(x,y,w,h) | BGRA 像素
#define ATLAS_HEADER_FIXED 12
#define ATLAS_RECT_SIZE 8

// 字形图集头（与 tools/gen_font.js 对应）：
//   magic "CSFT" | u16 version | u16 page_count | u16 page_w | u16 page_h | u16 size_count | u32 保留
//   | page_count×u32 每页字节数 | 各页 1 位色数据
#define FONT_HEADER_FIXED 18

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool render_load_atlas(Render *r, const uint8_t *data, size_t len) {
    if (!data || len < ATLAS_HEADER_FIXED) return false;
    if (memcmp(data, "CSAT", 4) != 0) return false;
    const uint16_t count = rd16(data + 6);
    const uint16_t w = rd16(data + 8);
    const uint16_t h = rd16(data + 10);
    const size_t pix_off = ATLAS_HEADER_FIXED + (size_t)count * ATLAS_RECT_SIZE;
    const size_t need = pix_off + (size_t)w * h * 4;
    if (len < need) return false;
    r->atlas = data;
    r->atlas_len = len;
    r->atlas_w = w;
    r->atlas_h = h;
    return true;
}

bool render_load_font(Render *r, const uint8_t *data, size_t len) {
    if (!data || len < FONT_HEADER_FIXED) return false;
    if (memcmp(data, "CSFT", 4) != 0) return false;
    r->font = data;
    r->font_len = len;
    return true;
}

SpriteRect render_rect(const Render *r, uint16_t i) {
    SpriteRect out = { 0, 0, 0, 0 };
    const size_t off = ATLAS_HEADER_FIXED + (size_t)i * ATLAS_RECT_SIZE;
    if (!r->atlas || r->atlas_len < off + ATLAS_RECT_SIZE) return out;
    out.x = rd16(r->atlas + off);
    out.y = rd16(r->atlas + off + 2);
    out.w = rd16(r->atlas + off + 4);
    out.h = rd16(r->atlas + off + 6);
    return out;
}

void render_init(Render *r, uint32_t *pixels, int32_t w, int32_t h) {
    r->pixels = pixels;
    r->w = w;
    r->h = h;
    r->atlas = NULL;
    r->atlas_len = 0;
    r->atlas_w = 0;
    r->atlas_h = 0;
    r->font = NULL;
    r->font_len = 0;
}

void render_fill(Render *r, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bgra) {
    if (!r->pixels || w <= 0 || h <= 0) return;
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w; if (x1 > r->w) x1 = r->w;
    int32_t y1 = y + h; if (y1 > r->h) y1 = r->h;
    for (int32_t py = y0; py < y1; py++) {
        uint32_t *row = r->pixels + (size_t)py * (size_t)r->w;
        for (int32_t px = x0; px < x1; px++) row[px] = bgra;
    }
}

// 带裁剪的贴图。render_blit 就是 clip = 整个帧缓冲的特例。
void render_blit_clipped(const Render *r, uint16_t sprite, int32_t dx, int32_t dy,
                         int32_t dw, int32_t dh, UiRect clip) {
    if (!r->pixels || !r->atlas || dw <= 0 || dh <= 0) return;
    const SpriteRect s = render_rect(r, sprite);
    if (s.w == 0 || s.h == 0) return;

    const size_t pix_off = ATLAS_HEADER_FIXED +
        (size_t)rd16(r->atlas + 6) * ATLAS_RECT_SIZE;

    // 先算出与"目标矩形 ∩ 帧缓冲 ∩ clip"相交的部分，避免每像素都做裁剪判断
    int32_t cx0 = dx, cy0 = dy, cx1 = dx + dw, cy1 = dy + dh;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > r->w) cx1 = r->w;
    if (cy1 > r->h) cy1 = r->h;
    if (cx0 < clip.x) cx0 = clip.x;
    if (cy0 < clip.y) cy0 = clip.y;
    if (cx1 > clip.x + clip.w) cx1 = clip.x + clip.w;
    if (cy1 > clip.y + clip.h) cy1 = clip.y + clip.h;
    if (cx0 >= cx1 || cy0 >= cy1) return;

    for (int32_t py = cy0; py < cy1; py++) {
        // 最近邻：目标矩形内第 (py-dy) 行对应源图第 ((py-dy)*s.h/dh) 行
        const int32_t sy = s.y + (int32_t)(((int64_t)(py - dy)) * s.h / dh);
        uint32_t *dst = r->pixels + (size_t)py * (size_t)r->w;
        for (int32_t px = cx0; px < cx1; px++) {
            const int32_t sx = s.x + (int32_t)(((int64_t)(px - dx)) * s.w / dw);
            if (sy < s.y || sy >= s.y + s.h || sx < s.x || sx >= s.x + s.w) continue;
            const uint8_t *sp = r->atlas + pix_off +
                ((size_t)sy * r->atlas_w + (size_t)sx) * 4;
            const uint32_t a = sp[3];
            if (a == 0) continue;
            // 图集是预乘 alpha（GDI 的 32bpp DIB 语义），用预乘合成：
            //   out = src + dst * (1 - a/255)
            const uint32_t d = dst[px];
            const uint32_t ia = 255u - a;
            const uint32_t sb = sp[0], sg = sp[1], sr = sp[2];
            const uint32_t db = d & 0xFFu, dg = (d >> 8) & 0xFFu, dr = (d >> 16) & 0xFFu;
            const uint32_t ob = sb + (db * ia + 127u) / 255u;
            const uint32_t og = sg + (dg * ia + 127u) / 255u;
            const uint32_t or_ = sr + (dr * ia + 127u) / 255u;
            dst[px] = 0xFF000000u | (or_ << 16) | (og << 8) | ob;
        }
    }
}

// 不裁剪的贴图 = 裁剪到整块帧缓冲的特例
void render_blit(const Render *r, uint16_t sprite, int32_t x, int32_t y, int32_t dw, int32_t dh) {
    const UiRect all = { 0, 0, r->w, r->h };
    render_blit_clipped(r, sprite, x, y, dw, dh, all);
}

void render_bevel(Render *r, int32_t x, int32_t y, int32_t w, int32_t h, int32_t t, bool raised) {
    if (t <= 0) return;
    const uint32_t a = raised ? CS_C_HILIGHT : CS_C_SHADOW;
    const uint32_t b = raised ? CS_C_SHADOW : CS_C_HILIGHT;
    render_fill(r, x, y, w, t, a);                 // 上
    render_fill(r, x, y, t, h, a);                 // 左
    render_fill(r, x, y + h - t, w, t, b);         // 下
    render_fill(r, x + w - t, y, t, h, b);         // 右
}

void render_glyph_mask(Render *r, const uint8_t *rows_hex, int32_t row_count,
                       int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bgra) {
    if (!r->pixels || !rows_hex || w <= 0 || h <= 0) return;
    for (int32_t ry = 0; ry < h && ry < row_count; ry++) {
        const uint8_t *row = rows_hex + (size_t)ry * (size_t)((w + 3) / 4);
        const int32_t py = y + ry;
        if (py < 0 || py >= r->h) continue;
        uint32_t *dst = r->pixels + (size_t)py * (size_t)r->w;
        for (int32_t rx = 0; rx < w; rx++) {
            const int32_t px = x + rx;
            if (px < 0 || px >= r->w) continue;
            const uint8_t nib = row[rx >> 2];
            if (((nib >> (3 - (rx & 3))) & 1) == 0) continue;
            dst[px] = bgra;
        }
    }
}

// ---------------------------------------------------------------- PNG 输出
// 只为了宿主机预览：写一个最小的 8 位 RGB PNG（不用 libpng，也不依赖 zlib：
// 用 stored（未压缩）deflate 块，文件大一点但实现短、绝不会错）。
static uint32_t crc32_of(const uint8_t *buf, size_t len, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool png_chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    uint8_t hdr[8];
    put_be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return false;
    if (len && fwrite(data, 1, len, f) != len) return false;
    uint32_t crc = crc32_of((const uint8_t *)type, 4, 0);
    if (len) crc = crc32_of(data, len, crc);
    uint8_t tail[4];
    put_be32(tail, crc);
    return fwrite(tail, 1, 4, f) == 4;
}

bool render_write_png(const char *path, const uint32_t *pixels, int32_t w, int32_t h) {
    if (!pixels || w <= 0 || h <= 0) return false;
    // 原始像素：每行 1 字节 filter(0) + w*3 字节 RGB
    const size_t stride = 1 + (size_t)w * 3;
    const size_t raw_len = stride * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return false;
    for (int32_t y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * stride;
        row[0] = 0;
        const uint32_t *src = pixels + (size_t)y * (size_t)w;
        for (int32_t x = 0; x < w; x++) {
            const uint32_t p = src[x];
            row[1 + (size_t)x * 3 + 0] = (uint8_t)((p >> 16) & 0xFF); // R
            row[1 + (size_t)x * 3 + 1] = (uint8_t)((p >> 8) & 0xFF);  // G
            row[1 + (size_t)x * 3 + 2] = (uint8_t)(p & 0xFF);         // B
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(raw);
        return false;
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    bool ok = fwrite(sig, 1, 8, f) == 8;

    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;   // 位深
    ihdr[9] = 2;   // 真彩色 RGB
    ihdr[10] = 0;  // 压缩方法
    ihdr[11] = 0;  // 过滤方法
    ihdr[12] = 0;  // 非隔行
    ok = ok && png_chunk(f, "IHDR", ihdr, sizeof ihdr);

    // zlib 流先拼进内存再作为一个 IDAT 写出去：IDAT 的长度必须写在数据前面，
    // 边算边流式写就没法回头填长度（第一版就是这么错的，PNG 打不开）。
    const size_t nblocks = (raw_len + 65534) / 65535;
    const size_t zlen = 2 + nblocks * 5 + raw_len + 4;
    uint8_t *z = (uint8_t *)malloc(zlen);
    if (!z) {
        fclose(f);
        free(raw);
        return false;
    }
    size_t zp = 0;
    z[zp++] = 0x78;
    z[zp++] = 0x01;
    size_t roff = 0;
    while (roff < raw_len) {
        const size_t n = (raw_len - roff) > 65535 ? 65535 : (raw_len - roff);
        z[zp++] = (roff + n >= raw_len) ? 1 : 0; // BFINAL
        z[zp++] = (uint8_t)(n & 0xFF);
        z[zp++] = (uint8_t)((n >> 8) & 0xFF);
        z[zp++] = (uint8_t)(~n & 0xFF);
        z[zp++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zp, raw + roff, n);
        zp += n;
        roff += n;
    }
    uint32_t ad_a = 1, ad_b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        ad_a = (ad_a + raw[i]) % 65521u;
        ad_b = (ad_b + ad_a) % 65521u;
    }
    put_be32(z + zp, (ad_b << 16) | ad_a);
    zp += 4;

    ok = ok && png_chunk(f, "IDAT", z, zp);
    free(z);

    ok = ok && png_chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0) ok = false;
    free(raw);
    return ok;
}
