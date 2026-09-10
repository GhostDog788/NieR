#!/usr/bin/env bash
set -euo pipefail
capture_plugin=$1
sdk_root=$2
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-capture-test-XXXXXX")
llvm_bin="$sdk_root/host/usr/lib/llvm-18/bin"
for level in 0 1 2 3 s z; do
    for profile in x86_64 i686; do
        case "$profile" in
            x86_64) triple=x86_64-unknown-linux-gnu; sysroot=x86_64-linux-gnu ;;
            i686) triple=i686-unknown-linux-gnu; sysroot=i686-linux-gnu ;;
        esac
        capture="$test_work/$profile-O$level.bc"
        NIER_CAPTURE_PATH="$capture" "$llvm_bin/clang" --target="$triple" \
            --sysroot="$sdk_root/sysroots/$sysroot" -O"$level" -fPIC -g \
            -fstandalone-debug -fpass-plugin="$capture_plugin" \
            -c "$test_root/examples/hello/hello.c" -o "$test_work/$profile-O$level.o"
        "$llvm_bin/llvm-dis" "$capture" -o "$test_work/capture.ll"
        rg -q 'call i32 \(ptr, \.\.\.\) @printf' "$test_work/capture.ll"
        if [[ $level == 0 ]]; then
            rg -q 'optnone' "$test_work/capture.ll"
        elif rg -q 'optnone' "$test_work/capture.ll"; then
            printf 'Unexpected optnone in release capture\n' >&2; exit 1
        fi
        test -s "$test_work/$profile-O$level.o"
    done
done
printf 'Preoptimization capture verified at O0/O1/O2/O3/Os/Oz for both profiles.\n'
