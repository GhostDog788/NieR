#!/usr/bin/env bash
# Publish each portable fixture once, then consume identical bytes on every target.
set -euo pipefail
prepare_only=false
if [[ ${1:-} == --prepare-only ]]; then prepare_only=true; shift; fi
if [[ ( $prepare_only == true && $# != 2 ) || ( $prepare_only == false && $# != 3 ) ]]; then
    printf 'Usage: bash tests/multi-consumer.sh --prepare-only PUBLISHER_CONFIG SDK\n       bash tests/multi-consumer.sh PUBLISHER_CONFIG SDK BUNDLE_PARENT\n' >&2
    exit 2
fi
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
config=$(realpath -e -- "$1")
sdk=$(realpath -e -- "$2")
bundle_parent=${3:-}
export SELA_SDK_ROOT="$sdk"
source "$repository/sdk/env.sh"
clang="$sdk/host/usr/lib/llvm-18/bin/clang"
llvm_ar="$sdk/host/usr/lib/llvm-18/bin/llvm-ar"
artifact_fixtures="$(dirname -- "$config")/consumer_artifact_fixtures"
test -x "$artifact_fixtures"
work=$(mktemp -d "${TMPDIR:-/tmp}/sela-multi-consumer-XXXXXX")
fixtures="$work/fixtures"
source_dir="$repository/tests/fixtures/dual-consumer"
mkdir -p "$fixtures" "$work/publication"
printf 'Multi-consumer fixture evidence: %s\n' "$work"
trap 'status=$?; printf "Multi-consumer evidence retained: %s (status %s)\n" "$work" "$status"' EXIT
source "$repository/tests/publication-inputs.sh"
publication_receipt="$work/publication-inputs.sha256"
sela_record_publication_inputs "$publication_receipt" "$config" "$sdk" \
    "$artifact_fixtures" "$repository/sdk/targets.json" "$repository/sdk/targets.py"
printf 'Publisher identity guard receipt: %s\n' "$publication_receipt"
cat "$publication_receipt"
publish() {
    sela_verify_publication_inputs "$publication_receipt"
    "$@"
    sela_verify_publication_inputs "$publication_receipt"
}
publish "$clang" --config="$config" -O2 -c "$source_dir/hello.c" -o "$fixtures/relocatable.sela"
publish "$clang" --config="$config" "$fixtures/relocatable.sela" -o "$fixtures/hello.sela"
publish "$artifact_fixtures" "$fixtures/hello.sela" "$fixtures"
publish "$clang" --config="$config" -O2 -fPIC -shared "$source_dir/shared.c" \
    -Wl,-soname,libdevice.so -o "$fixtures/shared.sela"
publish "$clang" --config="$config" -O2 "$source_dir/shared-main.c" -l:libdevice.so -o "$fixtures/shared-main.sela"
for member in first second; do
    publish "$clang" --config="$config" -O2 -c "$source_dir/static-$member.c" -o "$work/publication/$member.o"
done
publish "$clang" --config="$config" "$work/publication/first.o" "$work/publication/second.o" \
    -Wl,--sela-static,--sela-member-name=part.o,--sela-member-name=part.o -o "$fixtures/static.sela"
publish "$clang" --config="$config" -O2 "$source_dir/static-main.c" -l:libdevice.a -o "$fixtures/static-main.sela"
printf 'deliberately not a Sela archive\n' > "$fixtures/malformed.sela"

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
        publish "$clang" --config="$config" -std=gnu11 "-$level" "${sources[@]}" -o "$fixtures/$name-$level.sela"
        printf '%s\n' "$name-$level" >> "$fixtures/executables.list"
    done
    publish "$clang" --config="$config" -std=gnu11 "-$level" -shared "$repository/tests/abi/boundaries.c" \
        -Wl,-soname,"libaggregate-$level.so" -o "$fixtures/aggregate-library-$level.sela"
    printf '%s %s\n' "aggregate-library-$level.sela" "libaggregate-$level.so" >> "$fixtures/shared-libraries.list"
    publish "$clang" --config="$config" -std=gnu11 "-$level" "$repository/tests/abi/fixed_main.c" \
        "$repository/tests/abi/native_bridge.c" -l:"libaggregate-$level.so" -o "$fixtures/aggregate-dso-$level.sela"
    printf '%s\n' "aggregate-dso-$level" >> "$fixtures/executables.list"
done

mapfile -t targets < <(python3 "$repository/sdk/targets.py" list)
python3 "$repository/tests/vm/stage-target-metadata.py" "$fixtures"
for target in "${targets[@]}"; do
    eval "$(python3 "$repository/sdk/targets.py" shell "$target")"
    destination_lib="/opt/sela/sdk/sysroots/$SELA_TARGET_SYSROOT_TRIPLE/usr/lib/$SELA_TARGET_MULTIARCH"
    reference="$fixtures/reference/$target"
    mkdir -p "$reference" "$work/native-$target/first" "$work/native-$target/second"
    native=("$clang" --target="$SELA_TARGET_TRIPLE" --sysroot="$sdk/sysroots/$SELA_TARGET_SYSROOT_TRIPLE" \
        -resource-dir="$sdk/host/usr/lib/llvm-18/lib/clang/18" "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}" -O2)
    link=(--rtlib=compiler-rt --unwindlib=none --ld-path="$sdk/host/usr/lib/llvm-18/bin/ld.lld" \
        "-Wl,--dynamic-linker,$destination_lib/$SELA_TARGET_LOADER" "-Wl,-rpath,\$ORIGIN:$destination_lib" -Wl,-z,nodefaultlib)
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
    "${native[@]}" "$repository/tests/vm/exec-format.c" "${link[@]}" -o "$reference/exec-format"
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
sela_verify_publication_inputs "$publication_receipt"
(
    cd "$fixtures"
    find . -type f ! -name fixtures.sha256 -print0 | LC_ALL=C sort -z | \
        xargs -0 sha256sum
) > "$fixtures/fixtures.sha256"
if [[ $prepare_only == true ]]; then
    printf 'Fixture preparation completed; no device acceptance was run: %s\n' "$fixtures"
    exit 0
fi
for target in "${targets[@]}"; do
    env -u LD_LIBRARY_PATH -u LD_PRELOAD -u SELA_SDK_ROOT \
        bash "$repository/tests/consumer-vm.sh" "$target" "$bundle_parent/selac-$target" "$fixtures"
done
printf 'Multi-consumer functional smoke passed using one portable fixture set: %s\n' "$work"
