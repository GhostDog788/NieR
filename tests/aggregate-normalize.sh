#!/usr/bin/env bash
set -euo pipefail
proof_tests=$1
sdk_root=$(realpath -e -- "$2")
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/abi" && pwd)
abi_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-aggregate-normalization-XXXXXX")
clang="$sdk_root/host/usr/lib/llvm-18/bin/clang"
registry="$fixture_root/../../sdk/targets.py"
mapfile -t targets < <(python3 -B "$registry" list)
for level in O0 O2; do
    for target in "${targets[@]}"; do
        eval "$(python3 -B "$registry" shell "$target")"
        mkdir "$abi_work/$target-$level"
        for unit in boundaries main; do
            "$clang" --target="$SELA_TARGET_TRIPLE" "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}" \
                --sysroot="$sdk_root/sysroots/$SELA_TARGET_SYSROOT_TRIPLE" \
                -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18" \
                -std=c11 -fPIC -g -fstandalone-debug "-$level" \
                -Xclang -disable-llvm-passes -S -emit-llvm \
                "$fixture_root/$unit.c" -o "$abi_work/$target-$level/$unit.ll"
        done
        "$proof_tests" --target "$target" "$abi_work/$target-$level/boundaries.ll" \
            "$abi_work/$target-$level/main.ll"
    done
    "$proof_tests" "$abi_work/x86_64-$level/boundaries.ll" \
        "$abi_work/i686-$level/boundaries.ll" "$abi_work/x86_64-$level/main.ll" \
        "$abi_work/i686-$level/main.ll"
done
printf 'Native aggregate definition/call proofs and rejection gates passed: %s\n' "$abi_work"
