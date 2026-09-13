#!/usr/bin/env bash
set -euo pipefail
proof_tests=$1
sdk_root=$(realpath -e -- "$2")
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/abi" && pwd)
abi_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-aggregate-regenerated-XXXXXX")
llvm_bin="$sdk_root/host/usr/lib/llvm-18/bin"
registry="$fixture_root/../../sdk/targets.py"
mapfile -t targets < <(python3 -B "$registry" list)
for profile in "${targets[@]}"; do
    eval "$(python3 -B "$registry" shell "$profile")"
    sysroot="$sdk_root/sysroots/$SELA_TARGET_SYSROOT_TRIPLE"
    library="$sysroot/usr/lib/$SELA_TARGET_MULTIARCH"
    for level in O0 O2; do
        lane="$abi_work/$profile-$level"
        mkdir "$lane"
        flags=(--target="$SELA_TARGET_TRIPLE" "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}" --sysroot="$sysroot"
            -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18" -std=c11 -fPIC "-$level")
        for source in main boundaries; do
            "$llvm_bin/clang" "${flags[@]}" -g -fstandalone-debug -Xclang -disable-llvm-passes \
                -S -emit-llvm "$fixture_root/$source.c" -o "$lane/$source.native.ll"
            "$proof_tests" "$lane/$source.native.ll" "$profile" "$lane/$source.regenerated.ll"
            "$llvm_bin/clang" "${flags[@]}" -c "$lane/$source.regenerated.ll" -o "$lane/$source.o"
        done
        # These remain ordinary native dependencies. Aggregate variadic-tail
        # extraction is not being advertised as a Sela qualification by this
        # fixed-boundary helper test.
        for source in native_bridge varargs; do
            "$llvm_bin/clang" "${flags[@]}" -c "$fixture_root/$source.c" -o "$lane/$source.o"
        done
        link_flags=(--target="$SELA_TARGET_TRIPLE" "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}" --sysroot="$sysroot"
            --rtlib=compiler-rt --unwindlib=none --ld-path="$llvm_bin/ld.lld"
            -Wl,--dynamic-linker,"$library/$SELA_TARGET_LOADER" -Wl,-rpath,"$library" -Wl,-z,nodefaultlib)
        "$llvm_bin/clang" "${link_flags[@]}" -shared "$lane/native_bridge.o" \
            -Wl,-soname,libnative-abi.so -o "$lane/libnative-abi.so"
        "$llvm_bin/clang" "${link_flags[@]}" "$lane/main.o" "$lane/boundaries.o" "$lane/varargs.o" \
            -L"$lane" -lnative-abi -Wl,-rpath,"$lane" -o "$lane/regenerated"
        python3 -B "$registry" check-elf "$profile" "$lane/regenerated"
        test "$(env -u LD_LIBRARY_PATH timeout 30s "$lane/regenerated")" = 'Native aggregate ABI matrix passed'
    done
done
printf 'Regenerated native ABI and unchanged native callbacks passed: %s\n' "$abi_work"
