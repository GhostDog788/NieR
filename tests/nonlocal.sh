#!/usr/bin/env bash
set -euo pipefail
nierc=$(realpath "$1")
sdk=$(realpath "$2")
config=$(realpath "$3")
project=$(cd "$(dirname "$0")/.." && pwd)
llvm="$sdk/host/usr/lib/llvm-18/bin"
export LD_LIBRARY_PATH="$sdk/host/usr/lib/llvm-18/lib:$sdk/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
work=$(mktemp -d -t nier-nonlocal-XXXXXX)
expected='Native nonlocal jumps passed'
for level in O0 O2; do
  lane="$work/$level"
  mkdir "$lane"
  for target in x86_64 i686; do
    case "$target" in
      x86_64) triple=x86_64-linux-gnu; loader=ld-linux-x86-64.so.2 ;;
      i686) triple=i386-linux-gnu; loader=ld-linux.so.2 ;;
    esac
    sysroot="$sdk/sysroots/$target-linux-gnu"
    library="$sysroot/usr/lib/$triple"
    "$llvm/clang" --target="$target-unknown-linux-gnu" --sysroot="$sysroot" \
      -resource-dir="$sdk/host/usr/lib/llvm-18/lib/clang/18" -std=gnu11 -fPIC -"$level" \
      --rtlib=compiler-rt --unwindlib=none --ld-path="$llvm/ld.lld" \
      "$project/tests/fixtures/nonlocal.c" -Wl,--dynamic-linker,"$library/$loader" \
      -Wl,-rpath,"$library" -Wl,-z,nodefaultlib -o "$lane/native-$target"
    test "$(env -u LD_LIBRARY_PATH "$lane/native-$target")" = "$expected"
  done
  "$llvm/clang" --config="$config" -std=gnu11 -"$level" \
    "$project/tests/fixtures/nonlocal.c" -o "$lane/program.nier"
  "$nierc" "$lane/program.nier" --sdk "$sdk" -o "$lane/program"
  test "$(env -u LD_LIBRARY_PATH "$lane/program")" = "$expected"
  "$nierc" lower "$lane/program.nier" --target i686 --output-dir "$lane/lowered32"
  "$llvm/opt" -passes="default<$level>" -verify-each "$lane/lowered32/0.ll" -o "$lane/optimized32.bc"
  "$llvm/llc" -O="${level#O}" -filetype=obj -relocation-model=pic "$lane/optimized32.bc" -o "$lane/program32.o"
  sysroot="$sdk/sysroots/i686-linux-gnu"
  library="$sysroot/usr/lib/i386-linux-gnu"
  runtime="$sdk/host/usr/lib/llvm-18/lib/clang/18/lib/linux"
  "$llvm/ld.lld" --sysroot="$sysroot" -pie --eh-frame-hdr \
    --dynamic-linker="$library/ld-linux.so.2" -rpath "$library" -z nodefaultlib \
    "$library/Scrt1.o" "$library/crti.o" "$runtime/clang_rt.crtbegin-i386.o" \
    "$lane/program32.o" -L"$library" -lc "$runtime/libclang_rt.builtins-i386.a" \
    "$runtime/clang_rt.crtend-i386.o" "$library/crtn.o" -o "$lane/program32"
  test "$(env -u LD_LIBRARY_PATH "$lane/program32")" = "$expected"
done
printf 'Native cross-frame/nested jumps, volatile state and zero-to-one conversion passed both widths O0/O2: %s\n' "$work"
