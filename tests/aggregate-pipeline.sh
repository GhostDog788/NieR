#!/usr/bin/env bash
set -euo pipefail
nierc=$(realpath -e -- "$1")
sdk_root=$(realpath -e -- "$2")
config=$(realpath -e -- "$3")
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/abi" && pwd)
abi_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-aggregate-pipeline-XXXXXX")
llvm_bin="$sdk_root/host/usr/lib/llvm-18/bin"
library="$sdk_root/sysroots/x86_64-linux-gnu/usr/lib/x86_64-linux-gnu"
native_flags=(--target=x86_64-unknown-linux-gnu --sysroot="$sdk_root/sysroots/x86_64-linux-gnu"
    -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18" -fPIC)
link_flags=(--rtlib=compiler-rt --unwindlib=none --ld-path="$llvm_bin/ld.lld"
    -Wl,--dynamic-linker,"$library/ld-linux-x86-64.so.2" -Wl,-rpath,"$library" -Wl,-z,nodefaultlib)
for level in O0 O2; do
    lane="$abi_work/$level"
    mkdir "$lane"
    "$llvm_bin/clang" --config="$config" "-$level" "$fixture_root/fixed_main.c" \
        "$fixture_root/boundaries.c" "$fixture_root/native_bridge.c" -o "$lane/fixed.nier"
    "$nierc" "$lane/fixed.nier" --sdk "$sdk_root" -o "$lane/fixed"
    test "$(env -u LD_LIBRARY_PATH "$lane/fixed")" = 'NieR fixed aggregate ABI matrix passed'
    "$llvm_bin/clang" --config="$config" "-$level" -shared "$fixture_root/boundaries.c" \
        -Wl,-soname,libnier-abi.so -o "$lane/shared.nier"
    "$nierc" "$lane/shared.nier" --sdk "$sdk_root" -o "$lane/libnier-abi.so"
    "$llvm_bin/clang" "${native_flags[@]}" "${link_flags[@]}" "-$level" -shared \
        "$fixture_root/native_bridge.c" -Wl,-soname,libnative-abi.so -o "$lane/libnative-abi.so"
    "$llvm_bin/clang" "${native_flags[@]}" "${link_flags[@]}" "-$level" \
        "$fixture_root/main.c" "$fixture_root/varargs.c" -L"$lane" -lnier-abi -lnative-abi \
        -Wl,-rpath,"$lane" -o "$lane/native-client"
    test "$(env -u LD_LIBRARY_PATH "$lane/native-client")" = 'Native aggregate ABI matrix passed'
done
printf 'Published NieR aggregate application/DSO and stock native caller/callback gates passed: %s\n' "$abi_work"
