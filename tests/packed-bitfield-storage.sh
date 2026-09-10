#!/usr/bin/env bash
set -euo pipefail
nierc=$(realpath "$1")
sdk=$(realpath "$2")
config=$(realpath "$3")
project=$(cd "$(dirname "$0")/.." && pwd)
llvm="$sdk/host/usr/lib/llvm-18/bin"
export NIER_SDK_ROOT="$sdk"
export LD_LIBRARY_PATH="$sdk/host/usr/lib/llvm-18/lib:$sdk/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
work=$(mktemp -d -t nier-packed-bitfield-storage-XXXXXX)
fixture="$project/tests/fixtures/packed-bitfield-storage.c"

for level in O0 O2; do
  lane="$work/$level"
  mkdir "$lane"
  for target in x86_64 i686; do
    case "$target" in
      x86_64) triple=x86_64-linux-gnu; loader=ld-linux-x86-64.so.2
        expected='storage 40 16 25 270544967 0 22 -12 175060 30' ;;
      i686) triple=i386-linux-gnu; loader=ld-linux.so.2
        expected='storage 28 8 21 270544967 0 22 -12 175060 30' ;;
    esac
    sysroot="$sdk/sysroots/$target-linux-gnu"
    library="$sysroot/usr/lib/$triple"
    flags=(--target="$target-unknown-linux-gnu" --sysroot="$sysroot"
      -resource-dir="$sdk/host/usr/lib/llvm-18/lib/clang/18"
      -std=c11 -fPIC -g -fstandalone-debug -"$level")
    "$llvm/clang" "${flags[@]}" -Xclang -disable-llvm-passes -emit-llvm -c \
      "$fixture" -o "$lane/native-$target.bc"
    "$llvm/clang" "${flags[@]}" --rtlib=compiler-rt --unwindlib=none \
      --ld-path="$llvm/ld.lld" "$fixture" -Wl,--dynamic-linker,"$library/$loader" \
      -Wl,-rpath,"$library" -Wl,-z,nodefaultlib -o "$lane/native-$target"
    test "$(env -u LD_LIBRARY_PATH "$lane/native-$target")" = "$expected"
  done

  "$llvm/clang" --config="$config" -std=c11 -"$level" "$fixture" -o "$lane/program.nier"
  "$nierc" "$lane/program.nier" --sdk "$sdk" -o "$lane/program"
  test "$(env -u LD_LIBRARY_PATH "$lane/program")" = \
       "$(env -u LD_LIBRARY_PATH "$lane/native-x86_64")"

  for target in x86_64 i686; do
    "$nierc" lower "$lane/program.nier" --target "$target" --output-dir "$lane/lowered-$target"
    native_ir="$lane/lowered-$target/0.ll"
    "$llvm/opt" -passes=verify -disable-output "$native_ir"
    # Check real packed storage, unaligned native scalar accesses and signed
    # bitfield extraction survived publication, rather than a scalar substitute.
    rg -q 'type <\{' "$native_ir"
    rg -q '(load|store) i32.*align 1' "$native_ir"
    rg -q 'ashr i32' "$native_ir"
    rg -q 'llvm.memcpy' "$native_ir"
  done

  # Private qualification of the narrow lowering uses stock LLVM/LLD, without
  # extending the thin public compiler's currently x64-only native link target.
  "$llvm/opt" -passes="default<$level>" -verify-each "$lane/lowered-i686/0.ll" -o "$lane/optimized32.bc"
  "$llvm/llc" -O="${level#O}" -filetype=obj -relocation-model=pic "$lane/optimized32.bc" -o "$lane/program32.o"
  sysroot="$sdk/sysroots/i686-linux-gnu"
  library="$sysroot/usr/lib/i386-linux-gnu"
  runtime="$sdk/host/usr/lib/llvm-18/lib/clang/18/lib/linux"
  "$llvm/ld.lld" --sysroot="$sysroot" -pie --eh-frame-hdr \
    --dynamic-linker="$library/ld-linux.so.2" -rpath "$library" -z nodefaultlib \
    "$library/Scrt1.o" "$library/crti.o" "$runtime/clang_rt.crtbegin-i386.o" \
    "$lane/program32.o" -L"$library" -lc "$runtime/libclang_rt.builtins-i386.a" \
    "$runtime/clang_rt.crtend-i386.o" "$library/crtn.o" -o "$lane/program32"
  test "$(env -u LD_LIBRARY_PATH "$lane/program32")" = \
       "$(env -u LD_LIBRARY_PATH "$lane/native-i686")"
done
printf 'Nested packed/bitfield storage, initialization, copying and pointer mutation passed both widths O0/O2: %s\n' "$work"
