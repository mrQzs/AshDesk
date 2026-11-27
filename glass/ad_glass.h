/* ad_glass.h — 液态玻璃 CPU 像素内核（界面渲染，非网络路径）
 * 像素是 Qt 的 QRgb：uint32_t 0xAARRGGBB，小端内存序 B G R A；stride 以像素计。
 * 降采样、模糊、放大对四个通道一视同仁，RGB32 与预乘 ARGB32 都可用。
 * 全部整数定点：标量参考核与 SSE2（x86-64 NASM）/ NEON（AArch64）汇编逐位一致。 */
#ifndef AD_GLASS_H
#define AD_GLASS_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* NOLINTBEGIN(modernize-use-using, cppcoreguidelines-avoid-c-arrays): C 头文件，C++ 的 using / std::array 用不了 */

typedef struct {
    const uint32_t *pixels;
    ptrdiff_t stride;
    int32_t width, height;
} ad_glass_view;

typedef struct {
    uint32_t *pixels;
    ptrdiff_t stride;
    int32_t width, height;
} ad_glass_image;

/* 窗口 2r+1 ≤ 127 时，偏置累加和乘倒数的舍入误差 < 0.5，常数图模糊后不变 */
enum { AD_GLASS_MAX_RADIUS = 63 };

/* dst 必须是 max(1, w/4) × max(1, h/4)：每个像素取对应 4×4 块的四舍五入均值，末尾不足一块的行列不参与 */
void ad_glass_downsample4(ad_glass_view src, ad_glass_image dst);

/* 原地 passes 轮“水平 + 垂直”盒式模糊，边缘夹取；radius 夹到 [1, AD_GLASS_MAX_RADIUS]。
 * scratch 至少 ad_glass_blur_scratch(宽, 高) 字节、4 字节对齐 */
size_t ad_glass_blur_scratch(int32_t width, int32_t height);
void ad_glass_box_blur(ad_glass_image image, int32_t radius, int32_t passes, void *scratch);

/* RGB 饱和度乘 saturation_q8 / 256（夹到 ≥ 256），Rec.709 亮度近似不变，alpha 不变 */
void ad_glass_saturate(ad_glass_image image, int32_t saturation_q8);

/* 把 1/4 分辨率图双线性放大 4 倍，取全分辨率坐标 (x0 + x, y0 + y) 的值写满 dst。
 * 全分辨率像素 X 落在 1/4 图坐标 (X + 0.5) / 4 - 0.5，越界夹取。
 * scratch 至少 ad_glass_upsample_scratch(dst 宽) 个 int16_t */
size_t ad_glass_upsample_scratch(int32_t width);
void ad_glass_upsample4(ad_glass_view quarter, int32_t x0, int32_t y0, ad_glass_image dst, int16_t *scratch);

/* 斜面上的一个像素：面板合成时折射采样背景 */
typedef struct {
    int16_t x, y;      /* 面板内设备像素 */
    int16_t dx, dy;    /* 绿色通道折射偏移，1/256 像素 */
    int16_t red, blue; /* 色散：红、蓝偏移相对 dx/dy 的倍数，1/4096 */
    uint8_t frost;     /* 模糊权重，255 = 全模糊 */
    uint8_t cover;     /* 抗锯齿覆盖率，0 = 透明 */
    uint8_t reserved[2];
} ad_glass_edge;

typedef struct {
    ad_glass_view sharp;   /* 全分辨率清晰背景 */
    ad_glass_view blurred; /* 1/4 分辨率模糊背景 */
    int32_t origin_x, origin_y; /* 面板左上角在背景中的设备像素位置 */
    int32_t saturation_q8;
    int32_t disperse; /* 非 0：红、蓝通道按各自倍数另取清晰背景 */
} ad_glass_compose_params;

/* body 须已由 ad_glass_upsample4 填好模糊底色。逐个斜面像素折射采样，按 frost 混合清晰与模糊背景，
 * 清晰部分先做饱和，最后按 cover 预乘；cover = 0 的像素写成全透明 */
void ad_glass_compose_edges(const ad_glass_compose_params *params, const ad_glass_edge *edges, size_t count,
                            ad_glass_image body);

/* 行内核。导出只为测试逐位比较标量与 SIMD 实现，调用方用上面的整图接口。
 * 累加器预先加了 window/2 的偏置，输出 = min(255, (acc * mul) >> 16)，mul = ceil(65536 / window)。
 * blur_h：src 指向本段第一个输出像素，读 src[i - radius] 与 src[i + radius + 1]，二者都须在行内。
 * blur_v / lerp 的 SIMD 版只处理 16 字节的整数倍，余数由调用方交给标量核。 */
typedef struct {
    void (*blur_h)(const uint32_t *src, uint32_t *dst, size_t count, size_t radius, uint16_t acc[4], uint32_t mul);
    void (*blur_v)(uint16_t *acc, const uint8_t *add, const uint8_t *sub, uint8_t *out, size_t bytes, uint32_t mul);
    void (*down4)(const uint32_t *src, ptrdiff_t stride, uint32_t *dst, size_t count);
    /* out[4i + c] = wa * a[i].c + wb * b[i].c；weights = wa | wb << 16 */
    void (*lerp)(const uint32_t *a, const uint32_t *b, int16_t *out, size_t count, uint32_t weights);
    /* t 指向 T[k-1]；每组由 T[k-1]、T[k]、T[k+1] 生成全分辨率 4k..4k+3 四个像素 */
    void (*expand4)(const int16_t *t, uint32_t *dst, size_t groups);
    const char *isa;
} ad_glass_rows;

extern const ad_glass_rows ad_glass_rows_scalar;
/* 当前构建可用的最快实现；没有汇编的平台等同于 ad_glass_rows_scalar */
extern const ad_glass_rows ad_glass_rows_fast;

/* NOLINTEND(modernize-use-using, cppcoreguidelines-avoid-c-arrays) */
#ifdef __cplusplus
}
#endif
#endif
