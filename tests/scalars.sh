#!/usr/bin/env bash
set -euo pipefail
aot_bin=$1
export AOT_SDK_ROOT=$2
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/aot-scalars-test-XXXXXX")
"$aot_bin" publish --recipe "$test_root/tests/fixtures/scalars.build.json" -o "$test_work/scalars.aotpkg"
"$aot_bin" compile "$test_work/scalars.aotpkg" --output-dir "$test_work/native"
test "$(env -u LD_LIBRARY_PATH "$test_work/native/bin/scalars")" = 'pointer=8 literals=4,8 next=9 fixed64=4294967304'
"$aot_bin" lower "$test_work/scalars.aotpkg" --profile x86_64 --output-dir "$test_work/lowered"
rg -q 'noinline' "$test_work/lowered/0.ll"
"$aot_bin" lower "$test_work/scalars.aotpkg" --profile i686 --output-dir "$test_work/lowered32"
"$AOT_SDK_ROOT/host/usr/lib/llvm-18/bin/opt" -passes=verify -disable-output "$test_work/lowered32/0.ll"
"$aot_bin" publish --recipe "$test_root/tests/fixtures/build/sources.build.json" -o "$test_work/multi.aotpkg"
"$aot_bin" compile "$test_work/multi.aotpkg" --output-dir "$test_work/multi-native"
test "$(env -u LD_LIBRARY_PATH "$test_work/multi-native/bin/multi")" = 'Hello from a normal build: 8'
printf 'Scalar/native-width/fixed64/noinline/cross-TU fixtures passed.\n'
