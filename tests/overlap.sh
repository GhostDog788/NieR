#!/usr/bin/env bash
set -euo pipefail
selac=$(realpath "$1")
sdk=$(realpath "$2")
config=$(realpath "$3")
checker=$(realpath "$4")
project=$(cd "$(dirname "$0")/.." && pwd)
llvm="$sdk/host/usr/lib/llvm-18/bin"
export LD_LIBRARY_PATH="$sdk/host/usr/lib/llvm-18/lib:$sdk/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
work=$(mktemp -d -t sela-overlap-XXXXXX)
trap 'rm -r -- "$work"' EXIT
for level in O0 O2; do
  for target in x86_64 i686; do
    "$llvm/clang" --target="$target-unknown-linux-gnu" --sysroot="$sdk/sysroots/$target-linux-gnu" \
      -resource-dir="$sdk/host/usr/lib/llvm-18/lib/clang/18" -fPIC -g -"$level" \
      -Xclang -disable-llvm-passes -emit-llvm -c "$project/tests/fixtures/overlap.c" -o "$work/$target.bc"
  done
  "$checker" "$work/x86_64.bc" "$work/i686.bc"
  "$llvm/clang" --config="$config" -"$level" "$project/tests/fixtures/overlap.c" -o "$work/program.sela"
  "$selac" "$work/program.sela" --sdk "$sdk" -o "$work/program"
  test "$(env -u LD_LIBRARY_PATH "$work/program")" = 'Overlapping native storage passed'
done
