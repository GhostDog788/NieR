#!/usr/bin/env bash
set -euo pipefail
capture_plugin=$1
sdk_root=$(realpath -e -- "$2")
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
extended_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-extended-abi-XXXXXX")
llvm_bin="$sdk_root/host/usr/lib/llvm-18/bin"
for profile in x86_64 i686; do
    case "$profile" in
        x86_64) triple=x86_64-linux-gnu; loader=ld-linux-x86-64.so.2 ;;
        i686) triple=i386-linux-gnu; loader=ld-linux.so.2 ;;
    esac
    sysroot="$sdk_root/sysroots/$profile-linux-gnu"
    library="$sysroot/usr/lib/$triple"
    for level in O0 O2; do
        lane="$extended_work/$profile-$level"
        mkdir "$lane"
        flags=(--target="$profile-unknown-linux-gnu" --sysroot="$sysroot"
            -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18"
            -std=c11 -fPIC -g -fstandalone-debug "-$level")
        for unit in extended extended-bridge extended-main; do
            SELA_BUILD_METADATA= SELA_CAPTURE_RECORD= SELA_CAPTURE_PATH="$lane/$unit.bc" \
                "$llvm_bin/clang" "${flags[@]}" -fpass-plugin="$capture_plugin" \
                -c "$fixture_root/$unit.c" -o "$lane/$unit.o"
            "$llvm_bin/llvm-dis" "$lane/$unit.bc" -o "$lane/$unit.ll"
        done
        "$llvm_bin/clang" "${flags[@]}" --rtlib=compiler-rt --unwindlib=none \
            --ld-path="$llvm_bin/ld.lld" -shared "$lane/extended-bridge.o" \
            -Wl,-rpath,"$library" -o "$lane/libextended-native.so"
        "$llvm_bin/clang" "${flags[@]}" --rtlib=compiler-rt --unwindlib=none \
            --ld-path="$llvm_bin/ld.lld" "$lane/extended-main.o" "$lane/extended.o" \
            -L"$lane" -lextended-native -Wl,--dynamic-linker,"$library/$loader" \
            -Wl,-rpath,"$lane:$library" -Wl,-z,nodefaultlib -o "$lane/native"
        env -u LD_LIBRARY_PATH "$lane/native"
    done
done
printf 'Native union/bitfield/packed callbacks and pressure passed both widths O0/O2: %s\n' "$extended_work"
