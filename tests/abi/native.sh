#!/usr/bin/env bash
set -euo pipefail
capture_plugin=$1
sdk_root=$2
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
abi_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-native-abi-XXXXXX")
llvm_bin="$sdk_root/host/usr/lib/llvm-18/bin"
for profile in x86_64 i686; do
    case "$profile" in
        x86_64) library_triple=x86_64-linux-gnu; loader=ld-linux-x86-64.so.2 ;;
        i686) library_triple=i386-linux-gnu; loader=ld-linux.so.2 ;;
    esac
    sysroot="$sdk_root/sysroots/$profile-linux-gnu"
    library="$sysroot/usr/lib/$library_triple"
    for level in O0 O2; do
        lane="$abi_work/$profile-$level"
        mkdir "$lane"
        flags=(--target="$profile-unknown-linux-gnu" --sysroot="$sysroot"
            -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18"
            -std=c11 -fPIC -g -fstandalone-debug "-$level")
        for source in main boundaries native_bridge varargs; do
            SELA_BUILD_METADATA= SELA_CAPTURE_RECORD= SELA_CAPTURE_PATH="$lane/$source.bc" \
                "$llvm_bin/clang" "${flags[@]}" -fpass-plugin="$capture_plugin" \
                -c "$fixture_root/$source.c" -o "$lane/$source.o"
            "$llvm_bin/llvm-dis" "$lane/$source.bc" -o "$lane/$source.ll"
        done
        link_flags=(--target="$profile-unknown-linux-gnu" --sysroot="$sysroot"
            --rtlib=compiler-rt --unwindlib=none --ld-path="$llvm_bin/ld.lld"
            -Wl,--dynamic-linker,"$library/$loader" -Wl,-rpath,"$library"
            -Wl,-z,nodefaultlib)
        "$llvm_bin/clang" "${link_flags[@]}" -shared "$lane/native_bridge.o" \
            -Wl,-soname,libnative-abi.so -o "$lane/libnative-abi.so"
        "$llvm_bin/clang" "${link_flags[@]}" "$lane/main.o" "$lane/boundaries.o" "$lane/varargs.o" \
            -L"$lane" -lnative-abi -Wl,-rpath,"$lane" -o "$lane/native"
        test "$(env -u LD_LIBRARY_PATH "$lane/native")" = 'Native aggregate ABI matrix passed'
    done
done
printf 'Native ABI fixtures passed on both widths at O0/O2; captures: %s\n' "$abi_work"
