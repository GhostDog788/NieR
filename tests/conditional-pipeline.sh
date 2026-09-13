#!/usr/bin/env bash
set -euo pipefail
nierc=$(realpath "$1")
sdk=$(realpath "$2")
config=$(realpath "$3")
checker=$4
mode=${5:-pipeline}
project=$(cd "$(dirname "$0")/.." && pwd)
llvm="$sdk/host/usr/lib/llvm-18/bin"
export LD_LIBRARY_PATH="$sdk/host/usr/lib/llvm-18/lib:$sdk/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
work=$(mktemp -d -t nier-conditional-pipeline-XXXXXX)
fixture="$project/tests/fixtures/conditional-switch.c"
if [[ $mode != native-only && $mode != pipeline ]]; then exit 2; fi

for level in O0 O2; do
  lane="$work/$level"
  mkdir "$lane"
  for target in x86_64 i686; do
    case "$target" in
      x86_64) triple=x86_64-linux-gnu; loader=ld-linux-x86-64.so.2; expected='switch 336 374 6730' ;;
      i686) triple=i386-linux-gnu; loader=ld-linux.so.2; expected='switch 166 187 5340' ;;
    esac
    sysroot="$sdk/sysroots/$target-linux-gnu"
    library="$sysroot/usr/lib/$triple"
    flags=(--target="$target-unknown-linux-gnu" --sysroot="$sysroot"
      -resource-dir="$sdk/host/usr/lib/llvm-18/lib/clang/18"
      -std=gnu11 -fPIC -g -fstandalone-debug -"$level")
    "$llvm/clang" "${flags[@]}" -Xclang -disable-llvm-passes -emit-llvm -c \
      "$fixture" -o "$lane/native-$target.bc"
    "$llvm/clang" "${flags[@]}" --rtlib=compiler-rt --unwindlib=none \
      --ld-path="$llvm/ld.lld" "$fixture" -Wl,--dynamic-linker,"$library/$loader" \
      -Wl,-rpath,"$library" -Wl,-z,nodefaultlib -o "$lane/native-$target"
    test "$(env -u LD_LIBRARY_PATH "$lane/native-$target")" = "$expected"
  done
  if [[ $mode == native-only ]]; then continue; fi

  "$llvm/clang" --config="$config" -std=gnu11 -"$level" "$fixture" -o "$lane/program.nier"
  "$checker" "$lane/program.nier" "$lane/shared.mlir"
  "$nierc" "$lane/program.nier" --sdk "$sdk" -o "$lane/program"
  test "$(env -u LD_LIBRARY_PATH "$lane/program")" = \
       "$(env -u LD_LIBRARY_PATH "$lane/native-x86_64")"

  # The developer-only reference lowerer checks the other publication profile.
  # device-linking product. This private test uses only stock LLVM and LLD.
  "${NIER_REFERENCE_LOWER:-$(dirname -- "$nierc")/nier_reference_lower}" lower "$lane/program.nier" --target i686 --output-dir "$lane/lowered32"
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
  test "$(env -u LD_LIBRARY_PATH "$lane/program32")" = \
       "$(env -u LD_LIBRARY_PATH "$lane/native-i686")"

  for negative in branch escape; do
    if "$llvm/clang" --config="$config" -std=gnu11 -"$level" -c \
        "$project/tests/fixtures/conditional-$negative-rejected.c" \
        -o "$lane/rejected-$negative.o" >"$lane/$negative.log" 2>&1; then
      printf 'ERROR: unqualified conditional %s unexpectedly published\n' "$negative" >&2
      exit 1
    fi
    test ! -e "$lane/rejected-$negative.o"
  done
done
if [[ $mode == native-only ]]; then
  printf 'Conditional switch native baselines passed both widths O0/O2: %s\n' "$work"
else
  printf 'One-body conditional switch artifacts and native execution passed both widths O0/O2: %s\n' "$work"
fi
