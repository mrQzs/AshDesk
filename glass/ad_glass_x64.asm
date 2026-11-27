; ad_glass_x64.asm — 液态玻璃行内核，SSE2（x86-64 基线指令集，无需 CPU 检测），NASM 语法
; 语义与 ad_glass.c 的标量参考核逐位一致，声明见 ad_glass.h 的 ad_glass_rows。
; 构建：Win64  nasm -f win64 -DAD_WIN64 ad_glass_x64.asm
;       SysV   nasm -f elf64 ad_glass_x64.asm      (macOS: -f macho64 -DAD_MACHO)
;
; 入口宏把参数归一化到：a1=r10 a2=r11 a3=r8 a4=r9 a5=rax a6=rdx，rcx 作临时。
; 只用两种 ABI 下都易失的 rax rcx rdx r8-r11 与 xmm0-xmm5，不动 rsp：
; Win64 下是合法叶函数，无需 unwind 信息。所有访存都用 movdqu/movd/movq，不要求对齐。

        default rel
        section .text

%macro ENTRY 2                          ; 函数名, 参数个数
        align 16
%ifdef AD_MACHO
        global _%1
_%1:
%else
        global %1
%1:
%endif
%ifdef AD_WIN64
        mov     r10, rcx
        mov     r11, rdx
%if %2 >= 5
        mov     rax, [rsp + 40]          ; 返回地址 8 B + shadow space 32 B
%endif
%if %2 >= 6
        mov     rdx, [rsp + 48]
%endif
%else
        mov     r10, rdi
        mov     r11, rsi
%if %2 >= 5
        mov     rax, r8
%endif
        mov     r8, rdx
%if %2 >= 6
        mov     rdx, r9
%endif
        mov     r9, rcx
%endif
%endmacro

; xmm<%1> 的 8 个字 = 低 4 个 %2、高 4 个 %3；破坏 ecx 与 xmm<%4>
%macro WORDS 4
        mov     ecx, (%2) * 0x00010001
        movd    xmm%1, ecx
        punpckldq xmm%1, xmm%1
        mov     ecx, (%3) * 0x00010001
        movd    xmm%4, ecx
        punpckldq xmm%4, xmm%4
        punpcklqdq xmm%1, xmm%4
%endmacro

; void ad_glass_blur_v_sse2(uint16_t *acc, const uint8_t *add, const uint8_t *sub,
;                           uint8_t *out, size_t bytes, uint32_t mul)
; 每次 16 字节：out = min(255, acc * mul >> 16)，然后 acc += add - sub；bytes 的余数不处理
ENTRY ad_glass_blur_v_sse2, 6
        movd    xmm4, edx
        pshuflw xmm4, xmm4, 0
        punpcklqdq xmm4, xmm4            ; 8 × mul
        pxor    xmm5, xmm5
        shr     rax, 4
        jz      .done
.loop:  movdqu  xmm0, [r10]
        movdqu  xmm1, [r10 + 16]
        movdqa  xmm2, xmm0
        movdqa  xmm3, xmm1
        pmulhuw xmm2, xmm4
        pmulhuw xmm3, xmm4
        packuswb xmm2, xmm3
        movdqu  [r9], xmm2
        movdqu  xmm2, [r11]
        movdqa  xmm3, xmm2
        punpcklbw xmm2, xmm5
        punpckhbw xmm3, xmm5
        paddw   xmm0, xmm2
        paddw   xmm1, xmm3
        movdqu  xmm2, [r8]
        movdqa  xmm3, xmm2
        punpcklbw xmm2, xmm5
        punpckhbw xmm3, xmm5
        psubw   xmm0, xmm2
        psubw   xmm1, xmm3
        movdqu  [r10], xmm0
        movdqu  [r10 + 16], xmm1
        add     r10, 32
        add     r11, 16
        add     r8, 16
        add     r9, 16
        dec     rax
        jnz     .loop
.done:  ret

; void ad_glass_blur_h_sse2(const uint32_t *src, uint32_t *dst, size_t count, size_t radius,
;                           uint16_t acc[4], uint32_t mul)
; 每像素：dst[i] = min(255, acc * mul >> 16)，然后 acc += src[i + r + 1] - src[i - r]
ENTRY ad_glass_blur_h_sse2, 6
        movd    xmm4, edx
        pshuflw xmm4, xmm4, 0            ; 低 4 字 = mul
        pxor    xmm5, xmm5
        movq    xmm0, [rax]
        lea     rcx, [r9 * 4]
        lea     rdx, [r10 + rcx + 4]     ; 进窗口的像素
        sub     r10, rcx                 ; 出窗口的像素
        test    r8, r8
        jz      .store
.loop:  movdqa  xmm1, xmm0
        pmulhuw xmm1, xmm4
        packuswb xmm1, xmm1
        movd    [r11], xmm1
        movd    xmm2, [rdx]
        movd    xmm3, [r10]
        punpcklbw xmm2, xmm5
        punpcklbw xmm3, xmm5
        paddw   xmm0, xmm2
        psubw   xmm0, xmm3
        add     rdx, 4
        add     r10, 4
        add     r11, 4
        dec     r8
        jnz     .loop
.store: movq    [rax], xmm0
        ret

; void ad_glass_down4_sse2(const uint32_t *src, ptrdiff_t stride, uint32_t *dst, size_t count)
; 每个输出像素 = 4×4 块各通道 (和 + 8) >> 4；stride 以像素计
ENTRY ad_glass_down4_sse2, 4
        shl     r11, 2
        lea     rax, [r11 + r11 * 2]
        pxor    xmm5, xmm5
        mov     ecx, 0x00080008
        movd    xmm4, ecx
        pshufd  xmm4, xmm4, 0
        test    r9, r9
        jz      .done
.loop:  movdqu  xmm0, [r10]
        movdqa  xmm1, xmm0
        punpcklbw xmm0, xmm5
        punpckhbw xmm1, xmm5
        paddw   xmm0, xmm1
        movdqu  xmm2, [r10 + r11]
        movdqa  xmm3, xmm2
        punpcklbw xmm2, xmm5
        punpckhbw xmm3, xmm5
        paddw   xmm0, xmm2
        paddw   xmm0, xmm3
        movdqu  xmm2, [r10 + r11 * 2]
        movdqa  xmm3, xmm2
        punpcklbw xmm2, xmm5
        punpckhbw xmm3, xmm5
        paddw   xmm0, xmm2
        paddw   xmm0, xmm3
        movdqu  xmm2, [r10 + rax]
        movdqa  xmm3, xmm2
        punpcklbw xmm2, xmm5
        punpckhbw xmm3, xmm5
        paddw   xmm0, xmm2
        paddw   xmm0, xmm3               ; 两组列和：像素 0+2、1+3
        movdqa  xmm1, xmm0
        psrldq  xmm1, 8
        paddw   xmm0, xmm1
        paddw   xmm0, xmm4
        psrlw   xmm0, 4
        packuswb xmm0, xmm0
        movd    [r8], xmm0
        add     r10, 16
        add     r8, 4
        dec     r9
        jnz     .loop
.done:  ret

; void ad_glass_lerp_sse2(const uint32_t *a, const uint32_t *b, int16_t *out, size_t count,
;                         uint32_t weights)
; out[4i + c] = wa * a[i].c + wb * b[i].c；每次 4 个像素，count 的余数不处理
ENTRY ad_glass_lerp_sse2, 5
        movd    xmm4, eax
        movdqa  xmm3, xmm4
        pshuflw xmm4, xmm4, 0x00
        punpcklqdq xmm4, xmm4            ; 8 × wa
        pshuflw xmm3, xmm3, 0x55
        punpcklqdq xmm3, xmm3            ; 8 × wb
        pxor    xmm5, xmm5
        shr     r9, 2
        jz      .done
.loop:  movdqu  xmm0, [r10]
        movdqa  xmm1, xmm0
        punpcklbw xmm0, xmm5
        punpckhbw xmm1, xmm5
        pmullw  xmm0, xmm4
        pmullw  xmm1, xmm4
        movdqu  xmm2, [r11]
        punpcklbw xmm2, xmm5
        pmullw  xmm2, xmm3
        paddw   xmm0, xmm2
        movdqu  xmm2, [r11]              ; 再读一次省一个寄存器
        punpckhbw xmm2, xmm5
        pmullw  xmm2, xmm3
        paddw   xmm1, xmm2
        movdqu  [r8], xmm0
        movdqu  [r8 + 16], xmm1
        add     r10, 16
        add     r11, 16
        add     r8, 32
        dec     r9
        jnz     .loop
.done:  ret

; void ad_glass_expand4_sse2(const int16_t *t, uint32_t *dst, size_t groups)
; t 指向 T[k-1]（每项 4 个 int16）。a=T[k-1] b=T[k] c=T[k+1]：
;   (3a+5b, a+7b, 7b+c, 5b+3c) 各 +32 >> 6 → 4 个像素。只读到 T[k+1]，不越界
ENTRY ad_glass_expand4_sse2, 3
        WORDS   4, 3, 1, 0               ; xmm4 = 3|1
        WORDS   3, 5, 7, 0               ; xmm3 = 5|7
        mov     ecx, 0x00200020
        movd    xmm5, ecx
        pshufd  xmm5, xmm5, 0            ; 8 × 32
        test    r8, r8
        jz      .done
.loop:  movdqu  xmm0, [r10]              ; a|b
        movdqu  xmm1, [r10 + 8]          ; b|c
        pshufd  xmm2, xmm0, 0xEE         ; b|b
        pshufd  xmm0, xmm0, 0x44         ; a|a
        pshufd  xmm1, xmm1, 0xEE         ; c|c
        pmullw  xmm2, xmm3               ; 5b|7b
        pmullw  xmm0, xmm4               ; 3a|a
        pmullw  xmm1, xmm4               ; 3c|c
        paddw   xmm0, xmm2               ; 像素 0|1
        pshufd  xmm2, xmm2, 0x4E         ; 7b|5b
        pshufd  xmm1, xmm1, 0x4E         ; c|3c
        paddw   xmm1, xmm2               ; 像素 2|3
        paddw   xmm0, xmm5
        paddw   xmm1, xmm5
        psrlw   xmm0, 6
        psrlw   xmm1, 6
        packuswb xmm0, xmm1
        movdqu  [r11], xmm0
        add     r10, 8
        add     r11, 16
        dec     r8
        jnz     .loop
.done:  ret

%ifidn __OUTPUT_FORMAT__,elf64
        section .note.GNU-stack noalloc noexec nowrite progbits
%endif
