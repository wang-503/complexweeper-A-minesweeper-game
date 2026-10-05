// 规则自检（安卓版）。实现在 selftest.c，是 正式版/src/selftest.zig 的移植。
#ifndef CS_SELFTEST_H
#define CS_SELFTEST_H

#include <stdint.h>

#include "strbuf.h"

// 跑一遍全部规则不变量，报告写进 out。返回失败项数（0 = 全过）。
uint32_t selftest_run(StrBuf *out, const char *app_version);

#endif // CS_SELFTEST_H
