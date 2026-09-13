#!/usr/bin/env bash
set -euo pipefail
selac=$(realpath -e -- "$1")
sdk_root=$(realpath -e -- "$2")
config=$(realpath -e -- "$3")
fixture_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/abi" && pwd)
abi_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-aggregate-pipeline-XXXXXX")
llvm_bin="$sdk_root/host/usr/lib/llvm-18/bin"
registry="$fixture_root/../../sdk/targets.py"
host_target=$(python3 -B "$registry" host)
eval "$(python3 -B "$registry" shell "$host_target")"
library="$sdk_root/sysroots/$SELA_TARGET_SYSROOT_TRIPLE/usr/lib/$SELA_TARGET_MULTIARCH"
native_flags=(--target="$SELA_TARGET_TRIPLE" "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}" --sysroot="$sdk_root/sysroots/$SELA_TARGET_SYSROOT_TRIPLE"
    -resource-dir="$sdk_root/host/usr/lib/llvm-18/lib/clang/18" -fPIC)
link_flags=(--rtlib=compiler-rt --unwindlib=none --ld-path="$llvm_bin/ld.lld"
    -Wl,--dynamic-linker,"$library/$SELA_TARGET_LOADER" -Wl,-rpath,"$library" -Wl,-z,nodefaultlib)
for level in O0 O2; do
    lane="$abi_work/$level"
    mkdir "$lane"
    "$llvm_bin/clang" --config="$config" "-$level" "$fixture_root/fixed_main.c" \
        "$fixture_root/boundaries.c" "$fixture_root/native_bridge.c" -o "$lane/fixed.sela"
    "$selac" "$lane/fixed.sela" --sdk "$sdk_root" -o "$lane/fixed"
    test "$(env -u LD_LIBRARY_PATH "$lane/fixed")" = 'Sela fixed aggregate ABI matrix passed'
    "$llvm_bin/clang" --config="$config" "-$level" -shared "$fixture_root/boundaries.c" \
        -Wl,-soname,libsela-abi.so -o "$lane/shared.sela"
    "$selac" "$lane/shared.sela" --sdk "$sdk_root" -o "$lane/libsela-abi.so"
    "$llvm_bin/clang" "${native_flags[@]}" "${link_flags[@]}" "-$level" -shared \
        "$fixture_root/native_bridge.c" -Wl,-soname,libnative-abi.so -o "$lane/libnative-abi.so"
    "$llvm_bin/clang" "${native_flags[@]}" "${link_flags[@]}" "-$level" \
        "$fixture_root/main.c" "$fixture_root/varargs.c" -L"$lane" -lsela-abi -lnative-abi \
        -Wl,-rpath,"$lane" -o "$lane/native-client"
    test "$(env -u LD_LIBRARY_PATH "$lane/native-client")" = 'Native aggregate ABI matrix passed'
done
printf 'Published Sela aggregate application/DSO and stock native caller/callback gates passed: %s\n' "$abi_work"
