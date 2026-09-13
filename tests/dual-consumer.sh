#!/usr/bin/env bash
# Publish each portable fixture once, then consume the same bytes twice.
set -euo pipefail
prepare_only=false
if [[ ${1:-} == --prepare-only ]]; then prepare_only=true; shift; fi
if [[ $# != 4 ]]; then
    printf 'Usage: bash tests/dual-consumer.sh [--prepare-only] PUBLISHER_CONFIG SDK X86_64_BUNDLE I686_BUNDLE\n' >&2
    exit 2
fi
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
config=$(realpath -e -- "$1")
sdk=$(realpath -e -- "$2")
wide_bundle=$(realpath -m -- "$3")
narrow_bundle=$(realpath -m -- "$4")
export NIER_SDK_ROOT="$sdk"
source "$repository/sdk/env.sh"
clang="$sdk/host/usr/lib/llvm-18/bin/clang"
llvm_ar="$sdk/host/usr/lib/llvm-18/bin/llvm-ar"
artifact_fixtures="$(dirname -- "$config")/consumer_artifact_fixtures"
test -x "$artifact_fixtures"
work=$(mktemp -d "${TMPDIR:-/tmp}/nier-dual-consumer-XXXXXX")
fixtures="$work/fixtures"
source_dir="$repository/tests/fixtures/dual-consumer"
mkdir -p "$fixtures" "$work/publication"
printf 'Dual-consumer fixture evidence: %s\n' "$work"
trap 'status=$?; printf "Dual-consumer evidence retained: %s (status %s)\n" "$work" "$status"' EXIT
"$clang" --config="$config" -O2 -c "$source_dir/hello.c" -o "$fixtures/relocatable.nier"
"$clang" --config="$config" "$fixtures/relocatable.nier" -o "$fixtures/hello.nier"
"$artifact_fixtures" "$fixtures/hello.nier" "$fixtures"
"$clang" --config="$config" -O2 -fPIC -shared "$source_dir/shared.c" \
    -Wl,-soname,libdevice.so -o "$fixtures/shared.nier"
"$clang" --config="$config" -O2 "$source_dir/shared-main.c" -l:libdevice.so -o "$fixtures/shared-main.nier"
for member in first second; do
    "$clang" --config="$config" -O2 -c "$source_dir/static-$member.c" -o "$work/publication/$member.o"
done
"$clang" --config="$config" "$work/publication/first.o" "$work/publication/second.o" \
    -Wl,--nier-static,--nier-member-name=part.o,--nier-member-name=part.o -o "$fixtures/static.nier"
"$clang" --config="$config" -O2 "$source_dir/static-main.c" -l:libdevice.a -o "$fixtures/static-main.nier"
printf 'deliberately not a NieR archive\n' > "$fixtures/malformed.nier"

# Reuse the existing positive regression sources without reducing their scope.
matrix=(scalars width storage scalar-fields packed-bitfields overlap varargs va-forward nonlocal conditional aggregate)
matrix_sources() {
    case "$1" in
        scalars|width) sources=("$repository/tests/fixtures/$1.c") ;;
        storage) sources=("$repository/tests/storage-native.c") ;;
        scalar-fields) sources=("$repository/tests/fixtures/scalar-field-result.c") ;;
        packed-bitfields) sources=("$repository/tests/fixtures/packed-bitfield-storage.c") ;;
        overlap) sources=("$repository/tests/fixtures/overlap.c") ;;
        varargs) sources=("$repository/tests/abi/scalar_varargs.c") ;;
        va-forward) sources=("$repository/tests/abi/va_forward.c") ;;
        nonlocal) sources=("$repository/tests/fixtures/nonlocal.c") ;;
        conditional) sources=("$repository/tests/fixtures/conditional-switch.c") ;;
        aggregate) sources=("$repository/tests/abi/fixed_main.c" "$repository/tests/abi/boundaries.c" "$repository/tests/abi/native_bridge.c") ;;
        *) exit 2 ;;
    esac
}
for level in O0 O2; do
    for name in "${matrix[@]}"; do
        matrix_sources "$name"
        "$clang" --config="$config" -std=gnu11 "-$level" "${sources[@]}" -o "$fixtures/$name-$level.nier"
        printf '%s\n' "$name-$level" >> "$fixtures/executables.list"
    done
    "$clang" --config="$config" -std=gnu11 "-$level" -shared "$repository/tests/abi/boundaries.c" \
        -Wl,-soname,"libaggregate-$level.so" -o "$fixtures/aggregate-library-$level.nier"
    printf '%s %s\n' "aggregate-library-$level.nier" "libaggregate-$level.so" >> "$fixtures/shared-libraries.list"
    "$clang" --config="$config" -std=gnu11 "-$level" "$repository/tests/abi/fixed_main.c" \
        "$repository/tests/abi/native_bridge.c" -l:"libaggregate-$level.so" -o "$fixtures/aggregate-dso-$level.nier"
    printf '%s\n' "aggregate-dso-$level" >> "$fixtures/executables.list"
done

for target in x86_64 i686; do
    triple="$target-linux-gnu"
    destination_bundle="$wide_bundle"
    loader=ld-linux-x86-64.so.2
    if [[ $target == i686 ]]; then
        destination_bundle=/opt/nier
        loader=ld-linux.so.2
        destination_lib="$destination_bundle/sdk/sysroots/i686-linux-gnu/usr/lib/i386-linux-gnu"
    else
        destination_lib="$destination_bundle/sdk/sysroots/x86_64-linux-gnu/usr/lib/x86_64-linux-gnu"
    fi
    reference="$fixtures/reference/$target"
    mkdir -p "$reference" "$work/native-$target/first" "$work/native-$target/second"
    native=("$clang" --target="$triple" --sysroot="$sdk/sysroots/$triple" \
        -resource-dir="$sdk/host/usr/lib/llvm-18/lib/clang/18" -O2)
    link=(--rtlib=compiler-rt --unwindlib=none --ld-path="$sdk/host/usr/lib/llvm-18/bin/ld.lld" \
        "-Wl,--dynamic-linker,$destination_lib/$loader" "-Wl,-rpath,\$ORIGIN:$destination_lib" -Wl,-z,nodefaultlib)
    "${native[@]}" "$source_dir/hello.c" "${link[@]}" -o "$reference/hello"
    "${native[@]}" -fPIC -shared "$source_dir/shared.c" --rtlib=compiler-rt --unwindlib=none \
        --ld-path="$sdk/host/usr/lib/llvm-18/bin/ld.lld" -Wl,-soname,libdevice.so -o "$reference/libdevice.so"
    "${native[@]}" "$source_dir/shared-main.c" -L"$reference" -ldevice "${link[@]}" -o "$reference/shared"
    for member in first second; do
        "${native[@]}" -c "$source_dir/static-$member.c" -o "$work/native-$target/$member/part.o"
        "$llvm_ar" qcD "$work/native-$target/libdevice.a" "$work/native-$target/$member/part.o"
    done
    "$llvm_ar" sD "$work/native-$target/libdevice.a"
    "${native[@]}" "$source_dir/static-main.c" "$work/native-$target/libdevice.a" "${link[@]}" -o "$reference/static"
    if [[ $target == i686 ]]; then
        "${native[@]}" "$repository/tests/vm/exec-format.c" "${link[@]}" -o "$fixtures/exec-format"
    fi
    for level in O0 O2; do
        for name in "${matrix[@]}"; do
            matrix_sources "$name"
            "${native[@]}" -std=gnu11 "-$level" "${sources[@]}" "${link[@]}" -o "$reference/$name-$level"
        done
        "${native[@]}" -std=gnu11 "-$level" -fPIC -shared "$repository/tests/abi/boundaries.c" \
            --rtlib=compiler-rt --unwindlib=none --ld-path="$sdk/host/usr/lib/llvm-18/bin/ld.lld" \
            -Wl,-soname,"libaggregate-$level.so" -o "$reference/libaggregate-$level.so"
        "${native[@]}" -std=gnu11 "-$level" "$repository/tests/abi/fixed_main.c" \
            "$repository/tests/abi/native_bridge.c" -L"$reference" -l:"libaggregate-$level.so" \
            "${link[@]}" -o "$reference/aggregate-dso-$level"
    done
done
# This ELF64 probe is supplied only to test the 32-bit kernel's execve boundary.
cp -- "$fixtures/reference/x86_64/hello" "$fixtures/elf64-negative"
(
    cd "$fixtures"
    find . -type f ! -name fixtures.sha256 -print0 | LC_ALL=C sort -z | \
        xargs -0 sha256sum
) > "$fixtures/fixtures.sha256"
if [[ $prepare_only == true ]]; then
    printf 'Fixture preparation completed; no device acceptance was run: %s\n' "$fixtures"
    exit 0
fi
env -i PATH=/usr/bin:/bin LC_ALL=C TMPDIR="${TMPDIR:-/tmp}" \
    /bin/sh "$repository/tests/consumer-fixtures.sh" "$wide_bundle" "$fixtures" "$work/x86_64-output" x86_64
env -u LD_LIBRARY_PATH -u LD_PRELOAD -u NIER_SDK_ROOT \
    bash "$repository/tests/consumer-vm.sh" "$narrow_bundle" "$fixtures"
printf 'Dual-consumer functional smoke passed using one portable fixture set: %s\n' "$work"
