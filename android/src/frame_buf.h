// 上屏前的帧缓冲处理：只需把 alpha 位置 1，不做通道交换。
//
// 为什么值得单独一个文件加自检：真机上"一片纯黑"最可能的成因就在这里，
// 而宿主机的自检能直接把结论钉死。
//
// 结论（用 tools 里的探针实测出来的，别再凭记忆改）：
//   CS_RGB(0xFF,0,0) == 0xFF0000FF，在小端机上内存字节是 FF 00 00 FF —— 也就是
//   **R,G,B,A**。所以帧缓冲本来就是 GL 要的 RGBA 顺序，glTexImage2D 用 GL_RGBA
//   直接上传即可，**不需要任何通道交换**。
//   （这里曾经误以为帧缓冲是 BGRA、加了一次"交换"，结果只是把 R/B 换反。）
//
// 真正要做的只有一件事：帧缓冲的 alpha 是贴图预乘合成的残留值（大部分格子是
// 0 或别的数），必须统一置 1。否则在混合模式下会画出半透明的界面。
#ifndef CS_FRAME_BUF_H
#define CS_FRAME_BUF_H

#include <stdint.h>
#include <stddef.h>

// 把 count 个像素的 alpha 置成 255（其余通道原样不动）
static inline void cs_frame_opaque(uint32_t *px, size_t count) {
    for (size_t i = 0; i < count; i++) px[i] |= 0xFF000000u;
}

#endif // CS_FRAME_BUF_H
