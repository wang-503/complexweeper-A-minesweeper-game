#include "strbuf.h"

#include <stdio.h>
#include <string.h>

void sb_init(StrBuf *sb, char *buf, size_t cap) {
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->truncated = 0;
    if (cap > 0) buf[0] = '\0';
}

void sb_addn(StrBuf *sb, const char *s, size_t n) {
    if (sb->cap == 0) return;
    const size_t room = (sb->len < sb->cap - 1) ? (sb->cap - 1 - sb->len) : 0;
    const size_t take = (n < room) ? n : room;
    if (take > 0) {
        memcpy(sb->buf + sb->len, s, take);
        sb->len += take;
    }
    if (take < n) sb->truncated = 1;
    sb->buf[sb->len] = '\0';
}

void sb_add(StrBuf *sb, const char *s) {
    if (!s) return;
    sb_addn(sb, s, strlen(s));
}

void sb_vaddf(StrBuf *sb, const char *fmt, va_list ap) {
    if (sb->cap == 0) return;
    const size_t room = (sb->len < sb->cap - 1) ? (sb->cap - 1 - sb->len) : 0;
    if (room == 0) {
        sb->truncated = 1;
        return;
    }
    const int n = vsnprintf(sb->buf + sb->len, room + 1, fmt, ap);
    if (n < 0) {
        sb->truncated = 1;
        return;
    }
    if ((size_t)n > room) {
        sb->truncated = 1;
        sb->len = sb->cap - 1;
        return;
    }
    sb->len += (size_t)n;
}

void sb_addf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_vaddf(sb, fmt, ap);
    va_end(ap);
}

void sb_finish(StrBuf *sb) {
    if (sb->cap > 0) sb->buf[sb->len < sb->cap ? sb->len : sb->cap - 1] = '\0';
}
