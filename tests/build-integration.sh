#!/usr/bin/env bash
set -euo pipefail
aot_bin=$1
export AOT_SDK_ROOT=$2
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/aot-build-test-XXXXXX")
for system in make cmake; do
    "$aot_bin" publish --recipe "$test_root/tests/fixtures/build/$system.build.json" -o "$test_work/$system.aotpkg"
    "$aot_bin" compile "$test_work/$system.aotpkg" --output-dir "$test_work/$system-native"
    test "$(env -u LD_LIBRARY_PATH "$test_work/$system-native/bin/$system-hello")" = 'Hello from a normal build: 8'
done
for system in make cmake; do
    "$aot_bin" publish --recipe "$test_root/tests/fixtures/generated/$system.build.json" -o "$test_work/generated-$system.aotpkg"
    "$aot_bin" compile "$test_work/generated-$system.aotpkg" --output-dir "$test_work/generated-$system-native"
    test "$(env -u LD_LIBRARY_PATH "$test_work/generated-$system-native/bin/generated-$system")" = 'Generated width=8'
    # The native build-time generator has branches and is deliberately outside
    # the selected application graph. It must stay private, not become payload.
    test "$(tar -tf "$test_work/generated-$system.aotpkg" | wc -l)" -eq 2
done
test ! -e "$test_root/tests/fixtures/build/hello"
test ! -e "$test_root/tests/fixtures/build/hello.o"
test ! -e "$test_root/tests/fixtures/build/CMakeCache.txt"
test ! -e "$test_root/tests/fixtures/generated/generated.h"
test ! -e "$test_root/tests/fixtures/generated/generator"
printf 'Private Make/CMake direct-object build integration passed. Archive qualification remains later.\n'
