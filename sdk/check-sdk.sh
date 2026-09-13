#!/usr/bin/env bash
# Toolchain plumbing check only, not a product-portability acceptance test.
set -euo pipefail
sdk_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$sdk_dir/env.sh"
work="$SELA_SDK_ROOT/check"
mkdir -p -- "$work"
clang --version
mlir-opt --version
ld.lld --version
[[ -f $SELA_LLVM_ROOT/include/clang/Frontend/FrontendAction.h ]]
[[ -f $SELA_LLVM_ROOT/lib/libclang-cpp.so ]]
cmake --fresh -S "$sdk_dir/smoke" -B "$work/cmake" -G Ninja \
  -DCMAKE_C_COMPILER="$SELA_LLVM_ROOT/bin/clang" \
  -DCMAKE_CXX_COMPILER="$SELA_LLVM_ROOT/bin/clang++" \
  -DLLVM_DIR="$LLVM_DIR" -DMLIR_DIR="$MLIR_DIR" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$work/cmake" --parallel 2
"$work/cmake/sdk-mlir-smoke"
for profile in x86_64 i686; do
  case "$profile" in
    x86_64) sysroot="$SELA_SYSROOT_X86_64"; triple=x86_64-linux-gnu ;;
    i686) sysroot="$SELA_SYSROOT_I686"; triple=i686-linux-gnu ;;
  esac
  clang --target="$triple" --sysroot="$sysroot" -std=c11 -O2 \
    -Xclang -disable-llvm-passes -emit-llvm -c "$sdk_dir/smoke/stdio.c" \
    -o "$work/stdio-$profile.bc"
  llvm-dis "$work/stdio-$profile.bc" -o "$work/stdio-$profile.ll"
done
rg -q '^target triple = "x86_64-unknown-linux-gnu"' "$work/stdio-x86_64.ll"
rg -q 'ret i64 8' "$work/stdio-x86_64.ll"
rg -q '^target triple = "i686-unknown-linux-gnu"' "$work/stdio-i686.ll"
rg -q 'ret i32 4' "$work/stdio-i686.ll"
# Link a genuine native x86-64 object to the SDK's glibc and loader using LLD.
# Explicit startup/runtime paths prevent accidental GCC/host-libc link inputs.
target_lib="$SELA_SYSROOT_X86_64/usr/lib/x86_64-linux-gnu"
runtime_lib="$SELA_LLVM_ROOT/lib/clang/18/lib/linux"
clang --target=x86_64-linux-gnu --sysroot="$SELA_SYSROOT_X86_64" \
  -std=c11 -O2 -fPIE -c "$sdk_dir/smoke/stdio.c" -o "$work/stdio-x86_64.o"
ld.lld --sysroot="$SELA_SYSROOT_X86_64" -pie \
  --dynamic-linker "$target_lib/ld-linux-x86-64.so.2" \
  -rpath "$target_lib" -o "$work/stdio-x86_64" \
  "$target_lib/Scrt1.o" "$target_lib/crti.o" \
  "$runtime_lib/clang_rt.crtbegin-x86_64.o" "$work/stdio-x86_64.o" \
  -L "$target_lib" -lc "$runtime_lib/libclang_rt.builtins-x86_64.a" \
  "$runtime_lib/clang_rt.crtend-x86_64.o" "$target_lib/crtn.o"
env -u LD_LIBRARY_PATH "$work/stdio-x86_64"
resolution=$(env -u LD_LIBRARY_PATH "$target_lib/ld-linux-x86-64.so.2" --list "$work/stdio-x86_64")
[[ $resolution == *"libc.so.6 => $target_lib/libc.so.6"* ]] || { echo 'SDK libc was not selected' >&2; exit 1; }
printf '%s\n' "$resolution"
llvm-readelf --program-headers "$work/stdio-x86_64" | rg 'interpreter|LOAD'
printf '\nSDK smoke checks passed; i686 IR was compiled but never executed.\n'
