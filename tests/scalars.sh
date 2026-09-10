#!/usr/bin/env bash
set -euo pipefail
nierc=$1
export NIER_SDK_ROOT=$2
config=$3
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-scalars-XXXXXX")
clang="$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
"$clang" --config="$config" -O2 "$test_root/tests/fixtures/scalars.c" -o "$test_work/scalars.nier"
"$nierc" "$test_work/scalars.nier" -o "$test_work/scalars"
test "$(env -u LD_LIBRARY_PATH "$test_work/scalars")" = 'pointer=8 literals=4,8 next=9 fixed64=4294967304'
"$nierc" lower "$test_work/scalars.nier" --target x86_64 --output-dir "$test_work/lowered"
rg -q 'noinline' "$test_work/lowered/0.ll"
"$nierc" lower "$test_work/scalars.nier" --target i686 --output-dir "$test_work/lowered32"
"$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/opt" -passes=verify -disable-output "$test_work/lowered32/0.ll"
"$clang" --config="$config" -O2 "$test_root/tests/fixtures/build/hello.c" "$test_root/tests/fixtures/build/helper.c" -o "$test_work/multi.nier"
"$nierc" "$test_work/multi.nier" -o "$test_work/multi"
test "$(env -u LD_LIBRARY_PATH "$test_work/multi")" = 'Hello from a normal build: 8'
printf 'Scalar/native-width/fixed64/noinline/cross-TU fixtures passed.\n'
