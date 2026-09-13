#!/usr/bin/env bash
set -euo pipefail
classifier_tests=$1
sdk_root=$(realpath -e -- "$2")
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/abi" && pwd)
layout_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-aggregate-layout-XXXXXX")
registry="$fixture_root/../../sdk/targets.py"
mapfile -t targets < <(python3 -B "$registry" list)
for level in O0 O2; do
    for target in "${targets[@]}"; do
        eval "$(python3 -B "$registry" shell "$target")"
        mkdir "$layout_work/$target-$level"
        for unit in extended extended-bridge; do
            "$sdk_root/host/usr/lib/llvm-18/bin/clang" --target="$SELA_TARGET_TRIPLE" "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}" \
                --sysroot="$sdk_root/sysroots/$SELA_TARGET_SYSROOT_TRIPLE" \
                -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18" \
                -std=c11 -fPIC -g -fstandalone-debug "-$level" \
                -Xclang -disable-llvm-passes -S -emit-llvm "$fixture_root/$unit.c" \
                -o "$layout_work/$target-$level/$unit.ll"
        done
        "$classifier_tests" "$target" "$layout_work/$target-$level/extended.ll" \
            "$layout_work/$target-$level/extended-bridge.ll"
    done
done
printf 'Union/bitfield/packed ABI matches stock Clang on every target at O0/O2: %s\n' "$layout_work"
