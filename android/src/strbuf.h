// 一个很小的固定缓冲文本拼接器。
// 自检报告、规则指纹、预览调试信息都往它里面写；不用动态分配，
// 因为安卓侧这份代码要在 NativeActivity 里跑，越少活动部件越好。
#ifndef CS_STRBUF_H
#define CS_STRBUF_H

#include <stdarg.h>
#include <stddef.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int truncated; // 写满过就置 1，报告里会写明
} StrBuf;

void sb_init(StrBuf *sb, char *buf, size_t cap);
void sb_add(StrBuf *sb, const char *s);
void sb_addn(StrBuf *sb, const char *s, size_t n);
void sb_addf(StrBuf *sb, const char *fmt, ...);
void sb_vaddf(StrBuf *sb, const char *fmt, va_list ap);
// 结束符（没写满时一定补上 0）
void sb_finish(StrBuf *sb);

#endif // CS_STRBUF_H
