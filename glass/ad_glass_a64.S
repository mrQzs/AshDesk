// ad_glass_a64.S — 液态玻璃行内核，AArch64 AdvSIMD (NEON，AArch64 基线指令集)
// 语义与 ad_glass.c 的标量参考核逐位一致，声明见 ad_glass.h 的 ad_glass_rows。
// AAPCS64：参数在 x0-x5。只用 x0-x7 与 v0-v7、v16-v19（调用者保存），不触碰 v8-v15。
// 构建：clang -c ad_glass_a64.S   （Apple 平台定义 AD_MACHO 以加下划线前缀）

#ifdef AD_MACHO
#define SYM(x) _##x
#else
#define SYM(x) x
#endif

        .text

// void ad_glass_blur_v_neon(uint16_t *acc, const uint8_t *add, const uint8_t *sub,
//                           uint8_t *out, size_t bytes, uint32_t mul)
// 每次 16 字节：out = min(255, acc * mul >> 16)，然后 acc += add - sub；bytes 的余数不处理
        .p2align 4
        .globl  SYM(ad_glass_blur_v_neon)
SYM(ad_glass_blur_v_neon):
        dup     v7.8h, w5
        lsr     x4, x4, #4
        cbz     x4, 2f
1:      ld1     {v0.8h, v1.8h}, [x0]
        umull   v2.4s, v0.4h, v7.4h
        umull2  v3.4s, v0.8h, v7.8h
        umull   v4.4s, v1.4h, v7.4h
        umull2  v5.4s, v1.8h, v7.8h
        shrn    v2.4h, v2.4s, #16
        shrn2   v2.8h, v3.4s, #16
        shrn    v4.4h, v4.4s, #16
        shrn2   v4.8h, v5.4s, #16
        uqxtn   v2.8b, v2.8h
        uqxtn2  v2.16b, v4.8h
        st1     {v2.16b}, [x3], #16
        ld1     {v16.16b}, [x1], #16
        ld1     {v17.16b}, [x2], #16
        usubl   v18.8h, v16.8b, v17.8b    // 按 16 位取模，与累加器的回绕一致
        usubl2  v19.8h, v16.16b, v17.16b
        add     v0.8h, v0.8h, v18.8h
        add     v1.8h, v1.8h, v19.8h
        st1     {v0.8h, v1.8h}, [x0], #32
        subs    x4, x4, #1
        b.ne    1b
2:      ret

// void ad_glass_blur_h_neon(const uint32_t *src, uint32_t *dst, size_t count, size_t radius,
//                           uint16_t acc[4], uint32_t mul)
// 每像素：dst[i] = min(255, acc * mul >> 16)，然后 acc += src[i + r + 1] - src[i - r]
        .p2align 4
        .globl  SYM(ad_glass_blur_h_neon)
SYM(ad_glass_blur_h_neon):
        dup     v7.4h, w5
        ld1     {v0.4h}, [x4]
        add     x6, x0, x3, lsl #2
        add     x6, x6, #4                // 进窗口的像素
        sub     x7, x0, x3, lsl #2        // 出窗口的像素
        cbz     x2, 2f
1:      umull   v1.4s, v0.4h, v7.4h
        shrn    v1.4h, v1.4s, #16
        uqxtn   v1.8b, v1.8h
        str     s1, [x1], #4
        ldr     s2, [x6], #4
        ldr     s3, [x7], #4
        usubl   v2.8h, v2.8b, v3.8b
        add     v0.4h, v0.4h, v2.4h
        subs    x2, x2, #1
        b.ne    1b
2:      st1     {v0.4h}, [x4]
        ret

// void ad_glass_down4_neon(const uint32_t *src, ptrdiff_t stride, uint32_t *dst, size_t count)
// 每个输出像素 = 4×4 块各通道 (和 + 8) >> 4；stride 以像素计
        .p2align 4
        .globl  SYM(ad_glass_down4_neon)
SYM(ad_glass_down4_neon):
        lsl     x1, x1, #2
        add     x4, x0, x1
        add     x5, x4, x1
        add     x6, x5, x1
        cbz     x3, 2f
1:      ld1     {v0.16b}, [x0], #16
        ld1     {v1.16b}, [x4], #16
        ld1     {v2.16b}, [x5], #16
        ld1     {v3.16b}, [x6], #16
        uaddl   v4.8h, v0.8b, v1.8b
        uaddl2  v5.8h, v0.16b, v1.16b
        uaddw   v4.8h, v4.8h, v2.8b
        uaddw2  v5.8h, v5.8h, v2.16b
        uaddw   v4.8h, v4.8h, v3.8b
        uaddw2  v5.8h, v5.8h, v3.16b
        add     v4.8h, v4.8h, v5.8h       // 两组列和：像素 0+2、1+3
        ext     v5.16b, v4.16b, v4.16b, #8
        add     v4.4h, v4.4h, v5.4h
        uqrshrn v4.8b, v4.8h, #4          // (和 + 8) >> 4
        str     s4, [x2], #4
        subs    x3, x3, #1
        b.ne    1b
2:      ret

// void ad_glass_lerp_neon(const uint32_t *a, const uint32_t *b, int16_t *out, size_t count,
//                         uint32_t weights)
// out[4i + c] = wa * a[i].c + wb * b[i].c；每次 4 个像素，count 的余数不处理。wa、wb ≤ 8
        .p2align 4
        .globl  SYM(ad_glass_lerp_neon)
SYM(ad_glass_lerp_neon):
        dup     v6.16b, w4
        lsr     w5, w4, #16
        dup     v7.16b, w5
        lsr     x3, x3, #2
        cbz     x3, 2f
1:      ld1     {v0.16b}, [x0], #16
        ld1     {v1.16b}, [x1], #16
        umull   v2.8h, v0.8b, v6.8b
        umull2  v3.8h, v0.16b, v6.16b
        umlal   v2.8h, v1.8b, v7.8b
        umlal2  v3.8h, v1.16b, v7.16b
        st1     {v2.8h, v3.8h}, [x2], #32
        subs    x3, x3, #1
        b.ne    1b
2:      ret

// void ad_glass_expand4_neon(const int16_t *t, uint32_t *dst, size_t groups)
// t 指向 T[k-1]（每项 4 个 int16）。a=T[k-1] b=T[k] c=T[k+1]：
//   (3a+5b, a+7b, 7b+c, 5b+3c) 各 +32 >> 6 → 4 个像素。只读到 T[k+1]，不越界
        .p2align 4
        .globl  SYM(ad_glass_expand4_neon)
SYM(ad_glass_expand4_neon):
        movi    v4.4h, #3
        movi    v6.4h, #1
        mov     v4.d[1], v6.d[0]          // 3|1
        movi    v5.4h, #5
        movi    v6.4h, #7
        mov     v5.d[1], v6.d[0]          // 5|7
        cbz     x2, 2f
1:      ld1r    {v0.2d}, [x0], #8         // a|a，x0 前进到 T[k]
        ld1r    {v1.2d}, [x0]             // b|b
        add     x3, x0, #8
        ld1r    {v2.2d}, [x3]             // c|c
        mul     v0.8h, v0.8h, v4.8h       // 3a|a
        mul     v1.8h, v1.8h, v5.8h       // 5b|7b
        mul     v2.8h, v2.8h, v4.8h       // 3c|c
        add     v0.8h, v0.8h, v1.8h       // 像素 0|1
        ext     v1.16b, v1.16b, v1.16b, #8
        ext     v2.16b, v2.16b, v2.16b, #8
        add     v1.8h, v1.8h, v2.8h       // 像素 2|3
        uqrshrn v0.8b, v0.8h, #6
        uqrshrn2 v0.16b, v1.8h, #6
        st1     {v0.16b}, [x1], #16
        subs    x2, x2, #1
        b.ne    1b
2:      ret

#if defined(__linux__) && defined(__ELF__)
        .section .note.GNU-stack,"",%progbits
#endif
