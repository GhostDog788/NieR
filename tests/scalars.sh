#!/usr/bin/env bash
set -euo pipefail
selac=$1
export SELA_SDK_ROOT=$2
config=$3
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-scalars-XXXXXX")
clang="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
"$clang" --config="$config" -O2 "$test_root/tests/fixtures/scalars.c" -o "$test_work/scalars.sela"
"$selac" "$test_work/scalars.sela" -o "$test_work/scalars"
test "$(env -u LD_LIBRARY_PATH "$test_work/scalars")" = 'pointer=8 literals=4,8 next=9 fixed64=4294967304'
"${SELA_REFERENCE_LOWER:-$(dirname -- "$selac")/sela_reference_lower}" lower "$test_work/scalars.sela" --target x86_64 --output-dir "$test_work/lowered"
rg -q 'noinline' "$test_work/lowered/0.ll"
"${SELA_REFERENCE_LOWER:-$(dirname -- "$selac")/sela_reference_lower}" lower "$test_work/scalars.sela" --target i686 --output-dir "$test_work/lowered32"
"$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/opt" -passes=verify -disable-output "$test_work/lowered32/0.ll"
"$clang" --config="$config" -O2 "$test_root/tests/fixtures/build/hello.c" "$test_root/tests/fixtures/build/helper.c" -o "$test_work/multi.sela"
"$selac" "$test_work/multi.sela" -o "$test_work/multi"
test "$(env -u LD_LIBRARY_PATH "$test_work/multi")" = 'Hello from a normal build: 8'
printf 'Scalar/native-width/fixed64/noinline/cross-TU fixtures passed.\n'
