#!/usr/bin/env bash
set -euo pipefail
classifier_tests=$1
sdk_root=$(realpath -e -- "$2")
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/abi" && pwd)
layout_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-aggregate-layout-XXXXXX")
for level in O0 O2; do
    for target in x86_64 i686; do
        mkdir "$layout_work/$target-$level"
        for unit in extended extended-bridge; do
            "$sdk_root/host/usr/lib/llvm-18/bin/clang" --target="$target-unknown-linux-gnu" \
                --sysroot="$sdk_root/sysroots/$target-linux-gnu" \
                -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18" \
                -std=c11 -fPIC -g -fstandalone-debug "-$level" \
                -Xclang -disable-llvm-passes -S -emit-llvm "$fixture_root/$unit.c" \
                -o "$layout_work/$target-$level/$unit.ll"
        done
    done
    "$classifier_tests" "$layout_work/x86_64-$level/extended.ll" \
        "$layout_work/i686-$level/extended.ll" "$layout_work/x86_64-$level/extended-bridge.ll" \
        "$layout_work/i686-$level/extended-bridge.ll"
done
printf 'Union/bitfield/packed ABI matches stock Clang x64/i686 O0/O2: %s\n' "$layout_work"
