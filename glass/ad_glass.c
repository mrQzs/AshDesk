/* ad_glass.c — 液态玻璃像素内核：标量参考实现、整图驱动与编译期 SIMD 选择 */
#include "ad_glass.h"

/* CMake 只在对应汇编进了同一个库时才定义这两个宏，声明与链接始终一致。
 * x86-64 的基线就有 SSE2、AArch64 的基线就有 NEON，所以不需要运行时 CPU 检测 */
#if defined(AD_GLASS_ASM_X64)
#define AD_GLASS_FAST(name) ad_glass_##name##_sse2
#define AD_GLASS_ISA "sse2"
#elif defined(AD_GLASS_ASM_A64)
#define AD_GLASS_FAST(name) ad_glass_##name##_neon
#define AD_GLASS_ISA "neon"
#endif
#ifdef AD_GLASS_FAST
void AD_GLASS_FAST(blur_h)(const uint32_t *src, uint32_t *dst, size_t count, size_t radius, uint16_t acc[4],
                           uint32_t mul);
void AD_GLASS_FAST(blur_v)(uint16_t *acc, const uint8_t *add, const uint8_t *sub, uint8_t *out, size_t bytes,
                           uint32_t mul);
void AD_GLASS_FAST(down4)(const uint32_t *src, ptrdiff_t stride, uint32_t *dst, size_t count);
void AD_GLASS_FAST(lerp)(const uint32_t *a, const uint32_t *b, int16_t *out, size_t count, uint32_t weights);
void AD_GLASS_FAST(expand4)(const int16_t *t, uint32_t *dst, size_t groups);
#endif

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static inline int32_t floor_div8(int32_t v) {
    return (v >= 0) ? (v / 8) : -((7 - v) / 8);
}

static inline uint32_t channel(uint32_t pixel, unsigned c) {
    return (pixel >> (8u * c)) & 0xFFu;
}

static inline uint32_t divide(uint32_t biased, uint32_t mul) {
    const uint32_t q = (biased * mul) >> 16;
    return (q > 255u) ? 255u : q;
}

/* ---------------- 标量参考行内核（与汇编逐位一致的定义） ---------------- */

static void blur_h_scalar(const uint32_t *src, uint32_t *dst, size_t count, size_t radius, uint16_t acc[4],
                          uint32_t mul) {
    const uint32_t *add = src + radius + 1u;
    const uint32_t *sub = src - radius;
    for (size_t i = 0; i < count; ++i) {
        uint32_t out = 0;
        for (unsigned c = 0; c < 4u; ++c) {
            out |= divide(acc[c], mul) << (8u * c);
            acc[c] = (uint16_t)(acc[c] + channel(add[i], c) - channel(sub[i], c));
        }
        dst[i] = out;
    }
}

static void blur_v_scalar(uint16_t *acc, const uint8_t *add, const uint8_t *sub, uint8_t *out, size_t bytes,
                          uint32_t mul) {
    for (size_t i = 0; i < bytes; ++i) {
        out[i] = (uint8_t)divide(acc[i], mul);
        acc[i] = (uint16_t)(acc[i] + add[i] - sub[i]);
    }
}

static void down4_scalar(const uint32_t *src, ptrdiff_t stride, uint32_t *dst, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t out = 0;
        for (unsigned c = 0; c < 4u; ++c) {
            uint32_t sum = 8u;
            for (ptrdiff_t row = 0; row < 4; ++row) {
                const uint32_t *block = src + (row * stride) + (ptrdiff_t)(4u * i);
                for (unsigned k = 0; k < 4u; ++k) sum += channel(block[k], c);
            }
            out |= (sum >> 4) << (8u * c);
        }
        dst[i] = out;
    }
}

static void lerp_scalar(const uint32_t *a, const uint32_t *b, int16_t *out, size_t count, uint32_t weights) {
    const uint32_t wa = weights & 0xFFFFu;
    const uint32_t wb = weights >> 16;
    for (size_t i = 0; i < count; ++i) {
        for (unsigned c = 0; c < 4u; ++c) out[(4u * i) + c] = (int16_t)((wa * channel(a[i], c)) + (wb * channel(b[i], c)));
    }
}

static void expand4_scalar(const int16_t *t, uint32_t *dst, size_t groups) {
    for (size_t g = 0; g < groups; ++g) {
        const int16_t *k = t + (4u * g);
        uint32_t out[4] = {0, 0, 0, 0};
        for (unsigned c = 0; c < 4u; ++c) {
            const int32_t a = k[c], b = k[4u + c], n = k[8u + c];
            out[0] |= (uint32_t)(((3 * a) + (5 * b) + 32) >> 6) << (8u * c);
            out[1] |= (uint32_t)((a + (7 * b) + 32) >> 6) << (8u * c);
            out[2] |= (uint32_t)(((7 * b) + n + 32) >> 6) << (8u * c);
            out[3] |= (uint32_t)(((5 * b) + (3 * n) + 32) >> 6) << (8u * c);
        }
        for (unsigned j = 0; j < 4u; ++j) dst[(4u * g) + j] = out[j];
    }
}

const ad_glass_rows ad_glass_rows_scalar = {blur_h_scalar, blur_v_scalar, down4_scalar, lerp_scalar,
                                            expand4_scalar, "scalar"};
#ifdef AD_GLASS_FAST
const ad_glass_rows ad_glass_rows_fast = {AD_GLASS_FAST(blur_h), AD_GLASS_FAST(blur_v), AD_GLASS_FAST(down4),
                                          AD_GLASS_FAST(lerp), AD_GLASS_FAST(expand4), AD_GLASS_ISA};
#else
const ad_glass_rows ad_glass_rows_fast = {blur_h_scalar, blur_v_scalar, down4_scalar, lerp_scalar,
                                          expand4_scalar, "scalar"};
#endif

/* ---------------- 4×4 降采样 ---------------- */

/* 源图不足 4 像素宽或高时块会越界，逐像素夹取 */
static void downsample_clamped(ad_glass_view src, ad_glass_image dst, int32_t w, int32_t h) {
    for (int32_t y = 0; y < h; ++y) {
        uint32_t *out = dst.pixels + ((ptrdiff_t)y * dst.stride);
        for (int32_t x = 0; x < w; ++x) {
            uint32_t sum[4] = {8u, 8u, 8u, 8u};
            for (int32_t row = 0; row < 4; ++row) {
                const uint32_t *line = src.pixels + ((ptrdiff_t)clamp_i32((4 * y) + row, 0, src.height - 1) * src.stride);
                for (int32_t k = 0; k < 4; ++k) {
                    const uint32_t p = line[clamp_i32((4 * x) + k, 0, src.width - 1)];
                    for (unsigned c = 0; c < 4u; ++c) sum[c] += channel(p, c);
                }
            }
            out[x] = (sum[0] >> 4) | ((sum[1] >> 4) << 8) | ((sum[2] >> 4) << 16) | ((sum[3] >> 4) << 24);
        }
    }
}

void ad_glass_downsample4(ad_glass_view src, ad_glass_image dst) {
    if (src.width <= 0 || src.height <= 0) return;
    const int32_t w = clamp_i32(dst.width, 0, (src.width >= 4) ? src.width / 4 : 1);
    const int32_t h = clamp_i32(dst.height, 0, (src.height >= 4) ? src.height / 4 : 1);
    if (src.width < 4 || src.height < 4) {
        downsample_clamped(src, dst, w, h);
        return;
    }
    for (int32_t y = 0; y < h; ++y) {
        ad_glass_rows_fast.down4(src.pixels + ((ptrdiff_t)y * 4 * src.stride), src.stride,
                                 dst.pixels + ((ptrdiff_t)y * dst.stride), (size_t)w);
    }
}

/* ---------------- 盒式模糊 ---------------- */

/* 窗口越出行首行尾的位置逐像素夹取；与 blur_h_scalar 同一套算术 */
static void blur_h_clamped(const uint32_t *src, uint32_t *dst, size_t w, size_t r, size_t from, size_t to,
                           uint16_t acc[4], uint32_t mul) {
    for (size_t i = from; i < to; ++i) {
        const uint32_t add = src[((i + r + 1u) < w) ? (i + r + 1u) : (w - 1u)];
        const uint32_t sub = src[(i >= r) ? (i - r) : 0u];
        uint32_t out = 0;
        for (unsigned c = 0; c < 4u; ++c) {
            out |= divide(acc[c], mul) << (8u * c);
            acc[c] = (uint16_t)(acc[c] + channel(add, c) - channel(sub, c));
        }
        dst[i] = out;
    }
}

static void blur_row(const uint32_t *src, uint32_t *dst, size_t w, size_t r, uint32_t mul, uint16_t bias) {
    uint16_t acc[4];
    for (unsigned c = 0; c < 4u; ++c) {
        uint32_t sum = bias;
        for (size_t j = 0; j <= 2u * r; ++j) {
            /* 窗口初值覆盖 -r..r：前 r+1 个都夹到第 0 个像素 */
            const size_t at = (j <= r) ? 0u : (j - r);
            sum += channel(src[(at < w) ? at : (w - 1u)], c);
        }
        acc[c] = (uint16_t)sum;
    }
    if (w > (2u * r) + 1u) {
        blur_h_clamped(src, dst, w, r, 0, r, acc, mul);
        ad_glass_rows_fast.blur_h(src + r, dst + r, w - (2u * r) - 1u, r, acc, mul);
        blur_h_clamped(src, dst, w, r, w - r - 1u, w, acc, mul);
    } else {
        blur_h_clamped(src, dst, w, r, 0, w, acc, mul);
    }
}

static void blur_columns(const uint32_t *src, uint32_t *dst, ad_glass_image image, size_t r, uint32_t mul,
                         uint16_t bias, uint16_t *acc) {
    const size_t bytes = (size_t)image.width * 4u;
    const size_t bulk = bytes & ~(size_t)15u;
    const int32_t last = image.height - 1;
    for (size_t x = 0; x < bytes; ++x) acc[x] = bias;
    for (int32_t j = -(int32_t)r; j <= (int32_t)r; ++j) {
        const uint8_t *row = (const uint8_t *)(src + ((ptrdiff_t)clamp_i32(j, 0, last) * image.width));
        for (size_t x = 0; x < bytes; ++x) acc[x] = (uint16_t)(acc[x] + row[x]);
    }
    for (int32_t y = 0; y < image.height; ++y) {
        const uint8_t *add = (const uint8_t *)(src + ((ptrdiff_t)clamp_i32(y + (int32_t)r + 1, 0, last) * image.width));
        const uint8_t *sub = (const uint8_t *)(src + ((ptrdiff_t)clamp_i32(y - (int32_t)r, 0, last) * image.width));
        uint8_t *out = (uint8_t *)(dst + ((ptrdiff_t)y * image.stride));
        ad_glass_rows_fast.blur_v(acc, add, sub, out, bulk, mul);
        blur_v_scalar(acc + bulk, add + bulk, sub + bulk, out + bulk, bytes - bulk, mul);
    }
}

/* 布局：uint32_t 中间图 [w × h]，其后 uint16_t 列累加器 [4w] */
size_t ad_glass_blur_scratch(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return 0u;
    return ((size_t)width * (size_t)height * sizeof(uint32_t)) + ((size_t)width * 4u * sizeof(uint16_t));
}

void ad_glass_box_blur(ad_glass_image image, int32_t radius, int32_t passes, void *scratch) {
    if (image.width <= 0 || image.height <= 0) return;
    uint32_t *tmp = (uint32_t *)scratch;
    uint16_t *acc = (uint16_t *)(tmp + ((size_t)image.width * (size_t)image.height));
    const size_t r = (size_t)clamp_i32(radius, 1, AD_GLASS_MAX_RADIUS);
    const uint32_t window = (2u * (uint32_t)r) + 1u;
    const uint32_t mul = (65536u + window - 1u) / window;
    const uint16_t bias = (uint16_t)(window / 2u);
    const size_t w = (size_t)image.width;
    for (int32_t pass = 0; pass < passes; ++pass) {
        for (int32_t y = 0; y < image.height; ++y) {
            blur_row(image.pixels + ((ptrdiff_t)y * image.stride), tmp + ((size_t)y * w), w, r, mul, bias);
        }
        blur_columns(tmp, image.pixels, image, r, mul, bias, acc);
    }
}

/* ---------------- 饱和 ---------------- */

/* floor((c * s - luma * (s - 256) + 128) / 256)，先加 256 * (s - 256) 避免右移负数 */
static inline int32_t saturate_channel(int32_t c, int32_t luma, int32_t s) {
    return (((c * s) + ((256 - luma) * (s - 256)) + 128) >> 8) - (s - 256);
}

static inline int32_t luma_of(int32_t r, int32_t g, int32_t b) {
    return ((54 * r) + (183 * g) + (19 * b) + 128) >> 8;
}

void ad_glass_saturate(ad_glass_image image, int32_t saturation_q8) {
    const int32_t s = clamp_i32(saturation_q8, 256, 4096);
    for (int32_t y = 0; y < image.height; ++y) {
        uint32_t *row = image.pixels + ((ptrdiff_t)y * image.stride);
        for (int32_t x = 0; x < image.width; ++x) {
            const uint32_t p = row[x];
            const int32_t r = (int32_t)channel(p, 2), g = (int32_t)channel(p, 1), b = (int32_t)channel(p, 0);
            const int32_t luma = luma_of(r, g, b);
            row[x] = (p & 0xFF000000u) | ((uint32_t)clamp_i32(saturate_channel(r, luma, s), 0, 255) << 16) |
                     ((uint32_t)clamp_i32(saturate_channel(g, luma, s), 0, 255) << 8) |
                     (uint32_t)clamp_i32(saturate_channel(b, luma, s), 0, 255);
        }
    }
}

/* ---------------- 4× 双线性放大 ---------------- */

size_t ad_glass_upsample_scratch(int32_t width) {
    return (width > 0) ? 4u * (((size_t)width / 4u) + 4u) : 0u;
}

/* T[k - kmin] = wa * a[k] + wb * b[k]，k ∈ [kmin, kmax]，列号夹到图内 */
static void lerp_span(const uint32_t *a, const uint32_t *b, int32_t qw, int32_t kmin, int32_t kmax, uint32_t weights,
                      int16_t *t) {
    const int32_t in0 = (kmin > 0) ? kmin : 0;
    const int32_t in1 = (kmax < qw - 1) ? kmax : (qw - 1);
    for (int32_t k = kmin; k <= kmax; ++k) {
        if (k < in0 || k > in1) {
            const int32_t col = clamp_i32(k, 0, qw - 1);
            lerp_scalar(a + col, b + col, t + ((ptrdiff_t)4 * (k - kmin)), 1u, weights);
        }
    }
    if (in0 <= in1) {
        const size_t n = (size_t)(in1 - in0) + 1u;
        const size_t bulk = n & ~(size_t)3u;
        int16_t *out = t + ((ptrdiff_t)4 * (in0 - kmin));
        ad_glass_rows_fast.lerp(a + in0, b + in0, out, bulk, weights);
        lerp_scalar(a + in0 + bulk, b + in0 + bulk, out + (4u * bulk), n - bulk, weights);
    }
}

static inline uint32_t expand_pixel(const int16_t *t, int32_t kmin, int32_t x) {
    const int32_t u8 = (2 * x) - 3;
    const int32_t ix = floor_div8(u8);
    const int32_t fx = u8 - (8 * ix);
    const int16_t *left = t + ((ptrdiff_t)4 * (ix - kmin));
    uint32_t out = 0;
    for (unsigned c = 0; c < 4u; ++c) {
        out |= (uint32_t)((((8 - fx) * left[c]) + (fx * left[4u + c]) + 32) >> 6) << (8u * c);
    }
    return out;
}

static void expand_row(const int16_t *t, int32_t kmin, int32_t x0, uint32_t *out, int32_t width) {
    const int32_t end = x0 + width;
    int32_t x = x0;
    for (; x < end && ((uint32_t)x & 3u) != 0u; ++x) out[x - x0] = expand_pixel(t, kmin, x);
    const int32_t groups = (end - x) / 4;
    if (groups > 0) {
        ad_glass_rows_fast.expand4(t + ((ptrdiff_t)4 * ((x / 4) - 1 - kmin)), out + (x - x0), (size_t)groups);
        x += 4 * groups;
    }
    for (; x < end; ++x) out[x - x0] = expand_pixel(t, kmin, x);
}

void ad_glass_upsample4(ad_glass_view quarter, int32_t x0, int32_t y0, ad_glass_image dst, int16_t *scratch) {
    if (quarter.width <= 0 || quarter.height <= 0 || dst.width <= 0 || dst.height <= 0) return;
    const int32_t kmin = floor_div8((2 * x0) - 3);
    const int32_t kmax = floor_div8((2 * (x0 + dst.width - 1)) - 3) + 1;
    for (int32_t y = 0; y < dst.height; ++y) {
        const int32_t v8 = (2 * (y0 + y)) - 3;
        const int32_t iy = floor_div8(v8);
        const uint32_t fy = (uint32_t)(v8 - (8 * iy));
        const uint32_t *a = quarter.pixels + ((ptrdiff_t)clamp_i32(iy, 0, quarter.height - 1) * quarter.stride);
        const uint32_t *b = quarter.pixels + ((ptrdiff_t)clamp_i32(iy + 1, 0, quarter.height - 1) * quarter.stride);
        lerp_span(a, b, quarter.width, kmin, kmax, (8u - fy) | (fy << 16), scratch);
        expand_row(scratch, kmin, x0, dst.pixels + ((ptrdiff_t)y * dst.stride), dst.width);
    }
}

/* ---------------- 斜面折射合成 ---------------- */

typedef struct {
    int32_t r, g, b;
} ad_rgb;

/* (x, y) 是已夹到图内的 1/256 像素坐标 */
static inline ad_rgb bilinear(ad_glass_view image, int32_t x, int32_t y) {
    const int32_t ix = x >> 8, iy = y >> 8;
    const uint32_t fx = (uint32_t)x & 255u, fy = (uint32_t)y & 255u;
    const int32_t ix1 = (ix < image.width - 1) ? ix + 1 : ix;
    const int32_t iy1 = (iy < image.height - 1) ? iy + 1 : iy;
    const uint32_t *top = image.pixels + ((ptrdiff_t)iy * image.stride);
    const uint32_t *bottom = image.pixels + ((ptrdiff_t)iy1 * image.stride);
    const uint32_t w00 = (256u - fx) * (256u - fy), w01 = fx * (256u - fy);
    const uint32_t w10 = (256u - fx) * fy, w11 = fx * fy;
    int32_t v[3];
    for (unsigned c = 0; c < 3u; ++c) {
        const uint32_t sum = (channel(top[ix], c) * w00) + (channel(top[ix1], c) * w01) +
                             (channel(bottom[ix], c) * w10) + (channel(bottom[ix1], c) * w11) + 32768u;
        v[c] = (int32_t)(sum >> 16);
    }
    const ad_rgb rgb = {v[2], v[1], v[0]};
    return rgb;
}

/* 全分辨率 1/256 像素坐标 → 清晰背景 */
static inline ad_rgb sample_sharp(ad_glass_view sharp, int32_t x, int32_t y) {
    return bilinear(sharp, clamp_i32(x, 0, (sharp.width - 1) * 256), clamp_i32(y, 0, (sharp.height - 1) * 256));
}

/* 全分辨率 1/256 像素坐标 → 1/4 模糊背景：u = (X + 0.5) / 4 - 0.5，即 (x - 384) / 4 */
static inline ad_rgb sample_blurred(ad_glass_view blurred, int32_t x, int32_t y) {
    return bilinear(blurred, clamp_i32(x - 384, 0, (blurred.width - 1) * 1024) >> 2,
                    clamp_i32(y - 384, 0, (blurred.height - 1) * 1024) >> 2);
}

static inline uint32_t premultiply(int32_t value, uint32_t cover) {
    return (((uint32_t)clamp_i32(value, 0, 255) * cover) + 127u) / 255u;
}

static inline uint32_t compose_one(const ad_glass_compose_params *p, const ad_glass_edge *e, uint32_t below) {
    const int32_t x = ((p->origin_x + e->x) * 256) + e->dx;
    const int32_t y = ((p->origin_y + e->y) * 256) + e->dy;
    ad_rgb color = {(int32_t)channel(below, 2), (int32_t)channel(below, 1), (int32_t)channel(below, 0)};
    if (e->dx != 0 || e->dy != 0) color = sample_blurred(p->blurred, x, y);
    if (e->frost < 255u) {
        ad_rgb crisp = sample_sharp(p->sharp, x, y);
        if (p->disperse != 0) {
            const int32_t bx = (p->origin_x + e->x) * 256, by = (p->origin_y + e->y) * 256;
            crisp.r = sample_sharp(p->sharp, bx + ((e->dx * e->red) / 4096), by + ((e->dy * e->red) / 4096)).r;
            crisp.b = sample_sharp(p->sharp, bx + ((e->dx * e->blue) / 4096), by + ((e->dy * e->blue) / 4096)).b;
        }
        const int32_t s = clamp_i32(p->saturation_q8, 256, 4096);
        const int32_t luma = luma_of(crisp.r, crisp.g, crisp.b);
        const int32_t keep = 255 - (int32_t)e->frost, frost = e->frost;
        color.r = ((saturate_channel(crisp.r, luma, s) * keep) + (color.r * frost) + 127) / 255;
        color.g = ((saturate_channel(crisp.g, luma, s) * keep) + (color.g * frost) + 127) / 255;
        color.b = ((saturate_channel(crisp.b, luma, s) * keep) + (color.b * frost) + 127) / 255;
    }
    return ((uint32_t)e->cover << 24) | (premultiply(color.r, e->cover) << 16) | (premultiply(color.g, e->cover) << 8) |
           premultiply(color.b, e->cover);
}

void ad_glass_compose_edges(const ad_glass_compose_params *params, const ad_glass_edge *edges, size_t count,
                            ad_glass_image body) {
    if (params->sharp.width <= 0 || params->sharp.height <= 0 || params->blurred.width <= 0 ||
        params->blurred.height <= 0) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const ad_glass_edge *e = &edges[i];
        uint32_t *target = body.pixels + ((ptrdiff_t)e->y * body.stride) + e->x;
        *target = (e->cover == 0u) ? 0u : compose_one(params, e, *target);
    }
}
