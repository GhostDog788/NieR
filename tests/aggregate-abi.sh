#!/usr/bin/env bash
set -euo pipefail
classifier_tests=$1
sdk_root=$(realpath -e -- "$2")
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/abi" && pwd)
abi_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-aggregate-classifier-XXXXXX")
clang="$sdk_root/host/usr/lib/llvm-18/bin/clang"
for level in O0 O2; do
    for target in x86_64 i686; do
        mkdir "$abi_work/$target-$level"
        for unit in boundaries classifier; do
            "$clang" --target="$target-unknown-linux-gnu" \
                --sysroot="$sdk_root/sysroots/$target-linux-gnu" \
                -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18" \
                -std=c11 -fPIC -g -fstandalone-debug "-$level" \
                -Xclang -disable-llvm-passes -S -emit-llvm \
                "$fixture_root/$unit.c" -o "$abi_work/$target-$level/$unit.ll"
        done
    done
    "$classifier_tests" "$abi_work/x86_64-$level/boundaries.ll" \
        "$abi_work/i686-$level/boundaries.ll" "$abi_work/x86_64-$level/classifier.ll" \
        "$abi_work/i686-$level/classifier.ll"
done
printf 'Aggregate ABI classifier matches pinned stock Clang x64/i686 O0/O2 signatures: %s\n' "$abi_work"
