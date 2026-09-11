#!/usr/bin/env bash
set -euo pipefail
proof_tests=$1
sdk_root=$(realpath -e -- "$2")
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/abi" && pwd)
abi_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-aggregate-regenerated-XXXXXX")
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
            -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18" -std=c11 -fPIC "-$level")
        for source in main boundaries; do
            "$llvm_bin/clang" "${flags[@]}" -g -fstandalone-debug -Xclang -disable-llvm-passes \
                -S -emit-llvm "$fixture_root/$source.c" -o "$lane/$source.native.ll"
            "$proof_tests" "$lane/$source.native.ll" "$profile" "$lane/$source.regenerated.ll"
            "$llvm_bin/clang" "${flags[@]}" -c "$lane/$source.regenerated.ll" -o "$lane/$source.o"
        done
        # These remain ordinary native dependencies. Aggregate variadic-tail
        # extraction is not being advertised as a NieR qualification by this
        # fixed-boundary helper test.
        for source in native_bridge varargs; do
            "$llvm_bin/clang" "${flags[@]}" -c "$fixture_root/$source.c" -o "$lane/$source.o"
        done
        link_flags=(--target="$profile-unknown-linux-gnu" --sysroot="$sysroot"
            --rtlib=compiler-rt --unwindlib=none --ld-path="$llvm_bin/ld.lld"
            -Wl,--dynamic-linker,"$library/$loader" -Wl,-rpath,"$library" -Wl,-z,nodefaultlib)
        "$llvm_bin/clang" "${link_flags[@]}" -shared "$lane/native_bridge.o" \
            -Wl,-soname,libnative-abi.so -o "$lane/libnative-abi.so"
        "$llvm_bin/clang" "${link_flags[@]}" "$lane/main.o" "$lane/boundaries.o" "$lane/varargs.o" \
            -L"$lane" -lnative-abi -Wl,-rpath,"$lane" -o "$lane/regenerated"
        test "$(env -u LD_LIBRARY_PATH "$lane/regenerated")" = 'Native aggregate ABI matrix passed'
    done
done
printf 'Regenerated native ABI and unchanged native callbacks passed: %s\n' "$abi_work"
