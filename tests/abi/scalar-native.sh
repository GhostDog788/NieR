#!/usr/bin/env bash
set -euo pipefail
capture_plugin=$1
sdk_root=$2
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
scalar_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-scalar-varargs-XXXXXX")
llvm_bin="$sdk_root/host/usr/lib/llvm-18/bin"
for profile in x86_64 i686; do
    case "$profile" in
        x86_64) library_triple=x86_64-linux-gnu; loader=ld-linux-x86-64.so.2 ;;
        i686) library_triple=i386-linux-gnu; loader=ld-linux.so.2 ;;
    esac
    sysroot="$sdk_root/sysroots/$profile-linux-gnu"
    library="$sysroot/usr/lib/$library_triple"
    for level in O0 O2; do
        lane="$scalar_work/$profile-$level"
        mkdir "$lane"
        flags=(--target="$profile-unknown-linux-gnu" --sysroot="$sysroot"
            -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18"
            -std=c11 -fPIC -g -fstandalone-debug "-$level")
        for source in scalar_varargs va_forward; do
            NIER_BUILD_METADATA= NIER_CAPTURE_RECORD= NIER_CAPTURE_PATH="$lane/$source.bc" \
                "$llvm_bin/clang" "${flags[@]}" -fpass-plugin="$capture_plugin" \
                -c "$fixture_root/$source.c" -o "$lane/$source.o"
            "$llvm_bin/llvm-dis" "$lane/$source.bc" -o "$lane/$source.ll"
            "$llvm_bin/clang" "${flags[@]}" --rtlib=compiler-rt --unwindlib=none \
                --ld-path="$llvm_bin/ld.lld" "$lane/$source.o" \
                -Wl,--dynamic-linker,"$library/$loader" -Wl,-rpath,"$library" \
                -Wl,-z,nodefaultlib -o "$lane/$source"
            env -u LD_LIBRARY_PATH "$lane/$source"
        done
    done
done
printf 'Scalar va_arg/copy and native forwarding passed both widths at O0/O2; captures: %s\n' "$scalar_work"
