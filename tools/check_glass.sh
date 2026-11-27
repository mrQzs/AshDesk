#!/usr/bin/env bash
# Linux / WSL: real C/assembly kernels, strict optimized builds and ASan/UBSan.
# Optional NEON emulation uses the existing cross sysroot and qemu; timings under
# emulation are not performance evidence.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/glass-check}"
mkdir -p "$out"
warnings=(-std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror -Icore/kernels)
san=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
echo "== GCC x64 SysV SSE2 + ASan/UBSan"
gcc --version | head -1
nasm -f elf64 -o "$out/glass_x64.o" core/kernels/ad_glass_x64.asm
gcc "${warnings[@]}" "${san[@]}" -DAD_GLASS_ASM_X64 -o "$out/test-sse2" \
    tests/test_glass.c core/kernels/ad_glass.c "$out/glass_x64.o" -lm
"$out/test-sse2"
echo "== Clang scalar C + ASan/UBSan"
clang --version | head -1
clang "${warnings[@]}" "${san[@]}" -o "$out/test-scalar" tests/test_glass.c core/kernels/ad_glass.c -lm
"$out/test-scalar"
if [[ -n "${AARCH64_SYSROOT:-}" && -n "${QEMU_AARCH64:-}" ]]; then
    echo "== Clang AArch64 NEON + qemu (correctness only, NOT sanitizer-instrumented)"
    gccdir="$AARCH64_SYSROOT/usr/lib/gcc-cross/aarch64-linux-gnu/13"
    lib="$AARCH64_SYSROOT/usr/aarch64-linux-gnu"
    clang --target=aarch64-linux-gnu --sysroot="$lib" -isystem "$lib/include" \
        "${warnings[@]}" -DAD_GLASS_ASM_A64 -static -fuse-ld=lld -B"$gccdir" -L"$gccdir" -L"$lib/lib" \
        -o "$out/test-neon" tests/test_glass.c core/kernels/ad_glass.c core/kernels/ad_glass_a64.S "$lib/lib/libm-2.39.a"
    "$QEMU_AARCH64" "$out/test-neon"
else
    echo "SKIP NEON: set AARCH64_SYSROOT and QEMU_AARCH64 to an existing cross toolchain"
fi
