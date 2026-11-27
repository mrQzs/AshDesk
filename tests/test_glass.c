/* test_glass.c — 液态玻璃像素内核：标量与 SIMD 行内核逐位一致；整图接口对照浮点 / 整数定义 */
#include "ad_glass.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static unsigned long long g_checks, g_fail;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t g_rng = 2463534242u;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void fill(void *p, size_t bytes) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < bytes; ++i) b[i] = (uint8_t)(rnd() >> 24);
}

static unsigned ch(uint32_t p, unsigned c) {
    return (p >> (8u * c)) & 0xFFu;
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static ad_glass_image image_of(uint32_t *px, int32_t w, int32_t h, int32_t stride) {
    const ad_glass_image image = {px, stride, w, h};
    return image;
}

static ad_glass_view view_of(const uint32_t *px, int32_t w, int32_t h, int32_t stride) {
    const ad_glass_view view = {px, stride, w, h};
    return view;
}

/* ---------------- 行内核：标量与 SIMD 逐位一致，范围外的保护字节不被写 ---------------- */

enum { GUARD = 16, MAXN = 333, MAXR = 63 };

static void test_blur_rows(const ad_glass_rows *s, const ad_glass_rows *f, size_t n, size_t r) {
    static uint32_t src[MAXN + (2 * MAXR) + 2], hs[MAXN + (2 * GUARD)], hf[MAXN + (2 * GUARD)];
    static uint16_t vs[MAXN * 16], vf[MAXN * 16];
    static uint8_t add[MAXN * 16], sub[MAXN * 16], os[(MAXN * 16) + (2 * GUARD)], of[(MAXN * 16) + (2 * GUARD)];
    const uint32_t window = (2u * (uint32_t)r) + 1u, mul = (65536u + window - 1u) / window;
    uint16_t as[4], af[4];
    fill(src, sizeof src);
    fill(hs, sizeof hs);
    memcpy(hf, hs, sizeof hs);
    for (unsigned c = 0; c < 4u; ++c) as[c] = af[c] = (uint16_t)((rnd() % ((255u * window) + 1u)) + (window / 2u));
    s->blur_h(src + r, hs + GUARD, n, r, as, mul);
    f->blur_h(src + r, hf + GUARD, n, r, af, mul);
    CHECK(memcmp(hs, hf, sizeof hs) == 0 && memcmp(as, af, sizeof as) == 0);

    const size_t bytes = n * 16u;
    for (size_t i = 0; i < bytes; ++i) vs[i] = vf[i] = (uint16_t)((rnd() % ((255u * window) + 1u)) + (window / 2u));
    fill(add, bytes);
    fill(sub, bytes);
    fill(os, sizeof os);
    memcpy(of, os, sizeof os);
    s->blur_v(vs, add, sub, os + GUARD, bytes, mul);
    f->blur_v(vf, add, sub, of + GUARD, bytes, mul);
    CHECK(memcmp(os, of, sizeof os) == 0 && memcmp(vs, vf, bytes * sizeof vs[0]) == 0);
}

static void test_rows(void) {
    const ad_glass_rows *s = &ad_glass_rows_scalar, *f = &ad_glass_rows_fast;
    static const size_t counts[] = {0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 64, MAXN};
    static const size_t radii[] = {1, 2, 3, 4, 13, MAXR};
    static uint32_t quad[4 * ((4 * MAXN) + 7)], a[4 * MAXN], b[4 * MAXN];
    static uint32_t ds[MAXN * 4 + (2 * GUARD)], df[MAXN * 4 + (2 * GUARD)];
    static int16_t t[4 * (MAXN + 2)], ls[(16 * MAXN) + (2 * GUARD)], lf[(16 * MAXN) + (2 * GUARD)];
    printf("glass rows: %s\n", f->isa);
    for (size_t ci = 0; ci < sizeof counts / sizeof counts[0]; ++ci) {
        const size_t n = counts[ci];
        for (size_t ri = 0; ri < sizeof radii / sizeof radii[0]; ++ri) test_blur_rows(s, f, n, radii[ri]);

        const ptrdiff_t stride = (ptrdiff_t)(4u * n) + 7;
        fill(quad, sizeof quad);
        fill(ds, sizeof ds);
        memcpy(df, ds, sizeof ds);
        s->down4(quad + 1, stride, ds + GUARD, n);
        f->down4(quad + 1, stride, df + GUARD, n);
        CHECK(memcmp(ds, df, sizeof ds) == 0);

        for (uint32_t fy = 0; fy < 8u; ++fy) {
            fill(a, sizeof a);
            fill(b, sizeof b);
            fill(ls, sizeof ls);
            memcpy(lf, ls, sizeof ls);
            s->lerp(a, b, ls + GUARD, 4u * n, (8u - fy) | (fy << 16));
            f->lerp(a, b, lf + GUARD, 4u * n, (8u - fy) | (fy << 16));
            CHECK(memcmp(ls, lf, sizeof ls) == 0);
        }

        for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i) t[i] = (int16_t)(rnd() % 2041u);
        fill(ds, sizeof ds);
        memcpy(df, ds, sizeof ds);
        s->expand4(t, ds + GUARD, n);
        f->expand4(t, df + GUARD, n);
        CHECK(memcmp(ds, df, sizeof ds) == 0);
    }
}

/* ---------------- 4×4 降采样 ---------------- */

static void test_downsample(void) {
    static const int32_t sizes[][2] = {{1, 1}, {3, 2}, {2, 7}, {4, 4}, {7, 9}, {64, 33}, {130, 17}};
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; ++si) {
        const int32_t w = sizes[si][0], h = sizes[si][1], stride = w + 3;
        const int32_t qw = (w >= 4) ? w / 4 : 1, qh = (h >= 4) ? h / 4 : 1;
        uint32_t *src = malloc((size_t)stride * (size_t)h * sizeof *src);
        uint32_t *dst = malloc((size_t)qw * (size_t)qh * sizeof *dst);
        fill(src, (size_t)stride * (size_t)h * sizeof *src);
        ad_glass_downsample4(view_of(src, w, h, stride), image_of(dst, qw, qh, qw));
        int exact = 1;
        for (int32_t y = 0; y < qh; ++y) {
            for (int32_t x = 0; x < qw; ++x) {
                for (unsigned c = 0; c < 4u; ++c) {
                    unsigned sum = 8u;
                    for (int32_t j = 0; j < 16; ++j) {
                        const int32_t sy = clampi((4 * y) + (j / 4), 0, h - 1), sx = clampi((4 * x) + (j % 4), 0, w - 1);
                        sum += ch(src[(sy * stride) + sx], c);
                    }
                    exact &= (ch(dst[(y * qw) + x], c) == (sum >> 4));
                }
            }
        }
        CHECK(exact);
        free(src);
        free(dst);
    }
}

/* ---------------- 盒式模糊 ---------------- */

static void blur(uint32_t *px, int32_t w, int32_t h, int32_t r, int32_t passes) {
    void *scratch = malloc(ad_glass_blur_scratch(w, h));
    ad_glass_box_blur(image_of(px, w, h, w), r, passes, scratch);
    free(scratch);
}

static void test_blur_constant(void) {
    static const int32_t sizes[][2] = {{1, 1}, {2, 3}, {5, 1}, {1, 6}, {9, 9}, {130, 37}};
    static const int32_t radii[] = {1, 2, 4, 12, 63};
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; ++si) {
        for (size_t ri = 0; ri < sizeof radii / sizeof radii[0]; ++ri) {
            const int32_t w = sizes[si][0], h = sizes[si][1];
            const uint32_t value = (ri == 0) ? 0xFFFFFFFFu : ((ri == 1) ? 0u : rnd());
            uint32_t *px = malloc((size_t)w * (size_t)h * sizeof *px);
            for (int32_t i = 0; i < w * h; ++i) px[i] = value;
            blur(px, w, h, radii[ri], 3);
            int same = 1;
            for (int32_t i = 0; i < w * h; ++i) same &= (px[i] == value);
            CHECK(same);
            free(px);
        }
    }
}

/* 一轮水平 + 垂直，对照不取整的二维窗口均值（边缘夹取）：两次取整加乘倒数误差，半径 ≤ 12 时 ≤ 1.25 级 */
static void test_blur_reference(void) {
    static const int32_t sizes[][2] = {{61, 23}, {200, 7}, {3, 40}};
    static const int32_t radii[] = {1, 2, 4, 12};
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; ++si) {
        for (size_t ri = 0; ri < sizeof radii / sizeof radii[0]; ++ri) {
            const int32_t w = sizes[si][0], h = sizes[si][1], r = radii[ri];
            const size_t bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
            uint32_t *src = malloc(bytes), *px = malloc(bytes);
            fill(src, bytes);
            memcpy(px, src, bytes);
            blur(px, w, h, r, 1);
            double worst = 0;
            for (int32_t y = 0; y < h; ++y) {
                for (int32_t x = 0; x < w; ++x) {
                    for (unsigned c = 0; c < 4u; ++c) {
                        double sum = 0;
                        for (int32_t j = -r; j <= r; ++j)
                            for (int32_t i = -r; i <= r; ++i)
                                sum += ch(src[(clampi(y + j, 0, h - 1) * w) + clampi(x + i, 0, w - 1)], c);
                        const double exact = sum / ((2.0 * r + 1) * (2.0 * r + 1));
                        worst = fmax(worst, fabs((double)ch(px[(y * w) + x], c) - exact));
                    }
                }
            }
            CHECK(worst <= 1.25);
            free(src);
            free(px);
        }
    }
}

/* ---------------- 饱和 ---------------- */

static void test_saturate(void) {
    enum { N = 4096 };
    static uint32_t px[N], orig[N];
    fill(orig, sizeof orig);
    for (int32_t i = 0; i < 256; ++i) orig[i] = ((uint32_t)i << 24) | ((uint32_t)i * 0x010101u);
    memcpy(px, orig, sizeof px);
    ad_glass_saturate(image_of(px, N, 1, N), 256);
    CHECK(memcmp(px, orig, sizeof px) == 0);
    ad_glass_saturate(image_of(px, N, 1, N), 410);
    int gray = 1, alpha = 1;
    double worst = 0;
    for (int32_t i = 0; i < N; ++i) {
        alpha &= (ch(px[i], 3) == ch(orig[i], 3));
        if (i < 256) gray &= ((px[i] & 0xFFFFFFu) == ((uint32_t)i * 0x010101u));
        const double luma = (.2126 * ch(orig[i], 2)) + (.7152 * ch(orig[i], 1)) + (.0722 * ch(orig[i], 0));
        for (unsigned c = 0; c < 3u; ++c) {
            const double want = fmin(255, fmax(0, luma + ((ch(orig[i], c) - luma) * 410.0 / 256.0)));
            worst = fmax(worst, fabs((double)ch(px[i], c) - want));
        }
    }
    CHECK(gray && alpha);
    CHECK(worst <= 2.0);
}

/* ---------------- 4× 双线性放大：对照 (X + 0.5) / 4 - 0.5 处、坐标夹取的双线性插值 ---------------- */

static unsigned bilinear_ref(const uint32_t *q, int32_t qw, int32_t qh, int32_t x, int32_t y, unsigned c) {
    const double u = fmin(fmax(((x + 0.5) / 4.0) - 0.5, 0.0), qw - 1.0);
    const double v = fmin(fmax(((y + 0.5) / 4.0) - 0.5, 0.0), qh - 1.0);
    const int32_t x0 = (int32_t)u, y0 = (int32_t)v;
    const int32_t x1 = (x0 < qw - 1) ? x0 + 1 : x0, y1 = (y0 < qh - 1) ? y0 + 1 : y0;
    const double fx = u - x0, fy = v - y0;
    const double top = (ch(q[(y0 * qw) + x0], c) * (1 - fx)) + (ch(q[(y0 * qw) + x1], c) * fx);
    const double bottom = (ch(q[(y1 * qw) + x0], c) * (1 - fx)) + (ch(q[(y1 * qw) + x1], c) * fx);
    return (unsigned)floor((top * (1 - fy)) + (bottom * fy) + 0.5);
}

static void test_upsample(void) {
    static const int32_t quarters[][2] = {{1, 1}, {7, 5}, {101, 9}};
    static const int32_t widths[] = {1, 2, 3, 4, 5, 9, 16, 37, 83};
    static const int32_t origins[] = {-9, -1, 0, 1, 2, 3, 5, 13};
    for (size_t qi = 0; qi < sizeof quarters / sizeof quarters[0]; ++qi) {
        const int32_t qw = quarters[qi][0], qh = quarters[qi][1];
        uint32_t *q = malloc((size_t)qw * (size_t)qh * sizeof *q);
        fill(q, (size_t)qw * (size_t)qh * sizeof *q);
        for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; ++wi) {
            for (size_t oi = 0; oi < sizeof origins / sizeof origins[0]; ++oi) {
                const int32_t w = widths[wi], h = 1 + (int32_t)(oi % 5u), x0 = origins[oi], y0 = origins[(oi + 3u) % 8u];
                int16_t *scratch = malloc(ad_glass_upsample_scratch(w) * sizeof *scratch);
                const int32_t stride = w + 3;
                const size_t count = (size_t)stride * (size_t)h;
                uint32_t *storage = malloc((count + 2u) * sizeof *storage);
                for (size_t i = 0; i < count + 2u; ++i) storage[i] = 0xDEADBEEFu;
                uint32_t *dst = storage + 1;
                ad_glass_upsample4(view_of(q, qw, qh, qw), x0, y0, image_of(dst, w, h, stride), scratch);
                int exact = 1;
                for (int32_t y = 0; y < h; ++y) {
                    for (int32_t x = 0; x < w; ++x)
                        for (unsigned c = 0; c < 4u; ++c)
                            exact &= (ch(dst[(y * stride) + x], c) == bilinear_ref(q, qw, qh, x0 + x, y0 + y, c));
                    for (int32_t x = w; x < stride; ++x) exact &= (dst[(y * stride) + x] == 0xDEADBEEFu);
                }
                CHECK(exact);
                CHECK(storage[0] == 0xDEADBEEFu && storage[count + 1u] == 0xDEADBEEFu);
                free(scratch);
                free(storage);
            }
        }
        free(q);
    }
}

/* ---------------- 斜面折射合成 ---------------- */

static uint32_t compose_one(const ad_glass_compose_params *p, ad_glass_edge e, uint32_t below) {
    uint32_t body[4] = {below, below, below, below};
    e.x = 1;
    e.y = 1;
    ad_glass_compose_edges(p, &e, 1, image_of(body, 2, 2, 2));
    return body[3];
}

static void test_compose(void) {
    enum { W = 32, H = 8 };
    static uint32_t sharp[W * H], blurred[(W / 4) * (H / 4)];
    /* 清晰背景：R = 4x，G = 4x + 1，B = 100，按列变化；模糊背景是常数 */
    for (int32_t y = 0; y < H; ++y)
        for (int32_t x = 0; x < W; ++x) sharp[(y * W) + x] = 0xFF000000u | ((uint32_t)(4 * x) << 16) | ((uint32_t)(4 * x + 1) << 8) | 100u;
    for (size_t i = 0; i < sizeof blurred / sizeof blurred[0]; ++i) blurred[i] = 0xFF0A141Eu;
    ad_glass_compose_params p = {view_of(sharp, W, H, W), view_of(blurred, W / 4, H / 4, W / 4), 9, 2, 256, 0};
    const ad_glass_edge plain = {0, 0, 0, 0, 4096, 4096, 255, 255, {0, 0}};
    ad_glass_edge e = plain;

    e.cover = 0;
    CHECK(compose_one(&p, e, 0xFF123456u) == 0u);
    e = plain;
    e.cover = 128; /* 不折射、全模糊：保留底色，只按覆盖率预乘 */
    CHECK(compose_one(&p, e, 0xFF0080FFu) == 0x80004080u);
    e = plain;
    e.dx = 3 * 256; /* 折射后全模糊：取模糊背景 */
    CHECK(compose_one(&p, e, 0xFF000000u) == 0xFF0A141Eu);
    e.frost = 0; /* 全清晰：取 (9 + 1 + 3, 2 + 1) 处的清晰背景 */
    CHECK(compose_one(&p, e, 0u) == (0xFF000000u | (52u << 16) | (53u << 8) | 100u));
    e.dx = 3 * 256 + 128; /* 半像素：相邻两列的平均 */
    CHECK(compose_one(&p, e, 0u) == (0xFF000000u | (54u << 16) | (55u << 8) | 100u));
    e.dx = -100 * 256; /* 远出左边界：夹到第 0 列 */
    CHECK(compose_one(&p, e, 0u) == (0xFF000000u | (1u << 8) | 100u));
    e = plain;
    e.dx = 2 * 256;
    e.frost = 0;
    e.red = 2 * 4096;
    e.blue = 0;
    p.disperse = 1; /* 红取 2 倍偏移，绿取 1 倍，蓝不偏 */
    CHECK(compose_one(&p, e, 0u) == (0xFF000000u | ((4u * 14u) << 16) | ((4u * 12u + 1u) << 8) | 100u));
    p.disperse = 0;
    p.saturation_q8 = 410;
    e = plain;
    e.frost = 0;
    e.dx = 256; /* 灰色经饱和仍是灰色 */
    for (size_t i = 0; i < (size_t)W * H; ++i) sharp[i] = 0xFF808080u;
    CHECK(compose_one(&p, e, 0u) == 0xFF808080u);
}

/* ---------------- 行内核吞吐：标量与 SIMD，每像素纳秒 ---------------- */

/* MinGW 的 msvcrt 没有 timespec_get */
#ifdef _WIN32
static double seconds(void) {
    LARGE_INTEGER t;
    LARGE_INTEGER f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
}
#else
static double seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
}
#endif

static void bench(const ad_glass_rows *rows) {
    enum { N = 1920, REPS = 2000 };
    static uint32_t src[N + 64], dst[N + 64], quad[4 * 4 * N];
    static uint16_t acc[4 * N];
    static int16_t t[4 * (N + 2)];
    fill(src, sizeof src);
    fill(quad, sizeof quad);
    memset(acc, 0, sizeof acc);
    for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i) t[i] = (int16_t)(rnd() % 2041u);
    uint16_t h[4] = {0, 0, 0, 0};
    double ns[5];
    double start = seconds();
    for (int i = 0; i < REPS; ++i) rows->blur_h(src + 4, dst, N, 4, h, 7282u);
    ns[0] = (seconds() - start) * 1e9 / ((double)N * REPS);
    start = seconds();
    for (int i = 0; i < REPS; ++i) rows->blur_v(acc, (const uint8_t *)src, (const uint8_t *)quad, (uint8_t *)dst, (size_t)4u * N, 7282u);
    ns[1] = (seconds() - start) * 1e9 / ((double)N * REPS);
    start = seconds();
    for (int i = 0; i < REPS; ++i) rows->down4(quad, (ptrdiff_t)4 * N, dst, N / 4);
    ns[2] = (seconds() - start) * 1e9 / ((double)N * REPS);
    start = seconds();
    for (int i = 0; i < REPS; ++i) rows->lerp(src, quad, (int16_t *)quad + ((ptrdiff_t)8 * N), N, 5u | (3u << 16));
    ns[3] = (seconds() - start) * 1e9 / ((double)N * REPS);
    start = seconds();
    for (int i = 0; i < REPS; ++i) rows->expand4(t, dst, N / 4);
    ns[4] = (seconds() - start) * 1e9 / ((double)N * REPS);
    printf("  %-6s ns/px: blur_h %.2f blur_v %.2f down4(per src col) %.2f lerp %.2f expand4 %.2f\n", rows->isa,
           ns[0], ns[1], ns[2], ns[3], ns[4]);
}

int main(void) {
    test_rows();
    test_downsample();
    test_blur_constant();
    test_blur_reference();
    test_saturate();
    test_upsample();
    test_compose();
    bench(&ad_glass_rows_scalar);
    if (ad_glass_rows_fast.blur_h != ad_glass_rows_scalar.blur_h) bench(&ad_glass_rows_fast);
    printf("checks=%llu failures=%llu\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
