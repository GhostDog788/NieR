#!/usr/bin/env bash
set -euo pipefail
proof_tests=$1
sdk_root=$(realpath -e -- "$2")
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/abi" && pwd)
abi_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-aggregate-normalization-XXXXXX")
clang="$sdk_root/host/usr/lib/llvm-18/bin/clang"
for level in O0 O2; do
    for target in x86_64 i686; do
        mkdir "$abi_work/$target-$level"
        for unit in boundaries main; do
            "$clang" --target="$target-unknown-linux-gnu" \
                --sysroot="$sdk_root/sysroots/$target-linux-gnu" \
                -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18" \
                -std=c11 -fPIC -g -fstandalone-debug "-$level" \
                -Xclang -disable-llvm-passes -S -emit-llvm \
                "$fixture_root/$unit.c" -o "$abi_work/$target-$level/$unit.ll"
        done
    done
    "$proof_tests" "$abi_work/x86_64-$level/boundaries.ll" \
        "$abi_work/i686-$level/boundaries.ll" "$abi_work/x86_64-$level/main.ll" \
        "$abi_work/i686-$level/main.ll"
done
printf 'Native aggregate definition/call proofs and rejection gates passed: %s\n' "$abi_work"
