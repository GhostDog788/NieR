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
while IFS= read -r profile; do
  eval "$(python3 "$sdk_dir/targets.py" shell "$profile")"
  sysroot="$SELA_SDK_ROOT/sysroots/$SELA_TARGET_SYSROOT_TRIPLE"
  clang --target="$SELA_TARGET_TRIPLE" "${SELA_TARGET_CLANG_ARGS[@]}" --sysroot="$sysroot" -std=c11 -O2 \
    -Xclang -disable-llvm-passes -emit-llvm -c "$sdk_dir/smoke/stdio.c" \
    -o "$work/stdio-$profile.bc"
  llvm-dis "$work/stdio-$profile.bc" -o "$work/stdio-$profile.ll"
  rg -Fq "target triple = \"$SELA_TARGET_TRIPLE\"" "$work/stdio-$profile.ll"
  rg -q "ret i$SELA_TARGET_WORD_BITS $(( SELA_TARGET_WORD_BITS / 8 ))" "$work/stdio-$profile.ll"
  # Cross-link with explicit target CRTs/runtime, never execute a foreign tool.
  target_lib="$sysroot/usr/lib/$SELA_TARGET_MULTIARCH"
  runtime_lib="$SELA_LLVM_ROOT/lib/clang/18/lib/linux"
  clang --target="$SELA_TARGET_TRIPLE" "${SELA_TARGET_CLANG_ARGS[@]}" --sysroot="$sysroot" \
    -std=c11 -O2 -fPIE -c "$sdk_dir/smoke/stdio.c" -o "$work/stdio-$profile.o"
  ld.lld -m "$SELA_TARGET_LLD_EMULATION" --sysroot="$sysroot" -pie \
  --dynamic-linker "$target_lib/$SELA_TARGET_LOADER" \
  -rpath "$target_lib" -o "$work/stdio-$profile" \
  "$target_lib/Scrt1.o" "$target_lib/crti.o" \
  "$runtime_lib/clang_rt.crtbegin-$SELA_TARGET_COMPILER_RT_ARCH.o" "$work/stdio-$profile.o" \
  -L "$target_lib" -lc "$runtime_lib/libclang_rt.builtins-$SELA_TARGET_COMPILER_RT_ARCH.a" \
  "$runtime_lib/clang_rt.crtend-$SELA_TARGET_COMPILER_RT_ARCH.o" "$target_lib/crtn.o"
  python3 "$sdk_dir/targets.py" check-elf "$profile" "$work/stdio-$profile"
  if [[ $profile == "$SELA_BUILD_HOST" ]]; then
    env -u LD_LIBRARY_PATH "$work/stdio-$profile"
    resolution=$(env -u LD_LIBRARY_PATH "$target_lib/$SELA_TARGET_LOADER" --list "$work/stdio-$profile")
    [[ $resolution == *"libc.so.6 => $target_lib/libc.so.6"* ]] || { echo 'SDK libc was not selected' >&2; exit 1; }
    printf '%s\n' "$resolution"
  fi
done < <(python3 "$sdk_dir/targets.py" list)
printf '\nSDK compile/link checks passed for every target; foreign execution is deferred to device qualification.\n'
