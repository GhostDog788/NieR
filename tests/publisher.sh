#!/usr/bin/env bash
set -euo pipefail
nierc=$1
export NIER_SDK_ROOT=$2
config=$3
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-publisher-XXXXXX")
clang="$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
source_file="$test_root/tests/fixtures/publisher/flags.c"
include_dir="$test_root/tests/fixtures/publisher/include"

# Ordinary compiler options survive both native captures and device lowering.
for level in O0 O1 O2 O3 Os Oz; do
    "$clang" --config="$config" "-$level" -std=c17 -DPUBLISHED_VALUE=19 \
      -I "$include_dir" "$source_file" -o "$test_work/$level.nier"
    "$nierc" "$test_work/$level.nier" -o "$test_work/$level"
    test "$(env -u LD_LIBRARY_PATH "$test_work/$level")" = '23 19'
done

# Explicit project system-header paths are not lost when changing SDK profiles.
"$clang" --config="$config" -O2 -std=c17 -DPUBLISHED_VALUE=19 \
  -isystem "$include_dir" "$source_file" -o "$test_work/isystem.nier"
"$nierc" "$test_work/isystem.nier" -o "$test_work/isystem"
test "$(env -u LD_LIBRARY_PATH "$test_work/isystem")" = '23 19'

# Dependency outputs use the user's -MT/-MF, never the private native object.
"$clang" --config="$config" -O2 -DPUBLISHED_VALUE=19 -I "$include_dir" \
  -MMD -MP -MF "$test_work/flags.d" -MT flags.o -c "$source_file" \
  -o "$test_work/flags.o"
rg -q '^flags.o:' "$test_work/flags.d"
rg -q 'nier_options.h' "$test_work/flags.d"
rg -q 'nier_width64.h' "$test_work/flags.d"
rg -q 'nier_width32.h' "$test_work/flags.d"
if rg 'nier-private-' "$test_work/flags.d"; then
    printf 'ERROR: private native output leaked into dependency file\n' >&2; exit 1
fi

# Unsupported custom targets must fail, not silently lose their requested flags.
for flag in -march=haswell -mavx -mcpu=cortex-a53; do
    if "$clang" --config="$config" "$flag" -O2 -DPUBLISHED_VALUE=19 \
        -I "$include_dir" -c "$source_file" -o "$test_work/rejected.o"; then
        printf 'ERROR: silently accepted target override %s\n' "$flag" >&2; exit 1
    fi
    test ! -e "$test_work/rejected.o"
done

# The plugin never publishes partial bytes. Stock Clang itself may unlink a
# failed job's -o, including an older artifact; that driver policy is retained.
cp "$test_work/flags.o" "$test_work/before.o"
if "$clang" --config="$config" -O2 -c "$test_root/tests/fixtures/unsupported.c" \
    -o "$test_work/flags.o"; then
    printf 'ERROR: unsupported rebuild succeeded\n' >&2; exit 1
fi
if [[ -e $test_work/flags.o ]]; then
    cmp "$test_work/flags.o" "$test_work/before.o"
fi

# Input aliases are diagnosed before publication. These are disposable copies:
# Clang's failed-output cleanup can unlink the requested path after diagnosis.
for alias in direct hardlink; do
    input="$test_work/$alias.c"
    output="$input"
    cp "$source_file" "$input"
    if [[ $alias == hardlink ]]; then
        output="$test_work/$alias.o"
        ln "$input" "$output"
    fi
    if "$clang" --config="$config" -O2 -DPUBLISHED_VALUE=19 -I "$include_dir" \
        -c "$input" -o "$output"; then
        printf 'ERROR: publication overwrote an input\n' >&2; exit 1
    fi
    if [[ -e $input ]]; then cmp "$input" "$source_file"; fi
done

# -shared is still a stock-Clang producer invocation; the device emits a DSO.
"$clang" --config="$config" -O2 -shared "$test_root/tests/fixtures/build/helper.c" \
  -Wl,-soname,libwidth.so -o "$test_work/libwidth.nier"
"$nierc" "$test_work/libwidth.nier" -o "$test_work/libwidth.so"
readelf -h "$test_work/libwidth.so" | rg -q 'DYN \(Shared object file\)'
readelf --dyn-syms "$test_work/libwidth.so" | rg -q 'native_width'
readelf -d "$test_work/libwidth.so" | rg -q 'SONAME.*\[libwidth.so\]'

# The native-build observer does not replace ordinary Clang actions. Its
# readonly provenance section survives stock archives but is absent from the
# pristine LLVM capture that is later merged into public Nier code.
plugin_dir=$(dirname -- "$config")
llvm_bin="$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin"
native_lane="$test_work/native"
mkdir -p "$native_lane/metadata"
native_flags=(--target=x86_64-unknown-linux-gnu
    --sysroot="$NIER_SDK_ROOT/sysroots/x86_64-linux-gnu"
    -fno-temp-file -fPIC -g -fstandalone-debug
    -fplugin="$plugin_dir/libnier-clang.so"
    -fpass-plugin="$plugin_dir/nier-capture.so")
export NIER_BUILD_METADATA="$native_lane/metadata"
export NIER_BUILD_LANE="$native_lane"
export NIER_BUILD_PROFILE=x86_64
"$clang" "${native_flags[@]}" -E "$test_root/tests/fixtures/build/helper.c" \
    -o "$native_lane/preprocessed.c"
"$clang" "${native_flags[@]}" -M "$test_root/tests/fixtures/build/helper.c" \
    -MF "$native_lane/native.d"
test -z "$(rg --files "$native_lane/metadata")"
for profile in x86_64 i686; do
  export NIER_BUILD_PROFILE="$profile"
  for level in O0 O1 O2 O3 Os Oz; do
    native_object="$native_lane/$profile-$level.o"
    "$clang" "${native_flags[@]}" --target="$profile-unknown-linux-gnu" \
        --sysroot="$NIER_SDK_ROOT/sysroots/$profile-linux-gnu" \
        "-$level" -c "$test_root/tests/fixtures/build/helper.c" -o "$native_object"
    record=$(readelf -p .nier.capture "$native_object" | sed -n 's/^  \[ *0\]  //p')
    capture=${record%.compile.json}
    capture_hash=$(sha256sum "$capture")
    capture_hash=${capture_hash%% *}
    native_hash=$(sha256sum "$native_object")
    native_hash=${native_hash%% *}
    rg -Fq "\"capture_sha256\": \"$capture_hash\"" "$record"
    rg -Fq "\"native_sha256\": \"$native_hash\"" "$record"
    readelf -SW "$native_object" | rg -q '\.nier\.capture +PROGBITS .* A +0 +0 +1$'
    "$llvm_bin/llvm-dis" "$capture" -o "$native_lane/capture.ll"
    if rg '__nier_private_capture|\.nier\.capture' "$native_lane/capture.ll"; then
        printf 'ERROR: private provenance contaminated the portable input\n' >&2; exit 1
    fi
    "$llvm_bin/llvm-ar" rc "$native_lane/libhelper-$profile-$level.a" "$native_object"
    readelf -p .nier.capture "$native_lane/libhelper-$profile-$level.a" | rg -Fq "$record"
  done
done
export NIER_BUILD_PROFILE=x86_64
# Discarded configure output remains an ordinary successful native compile.
"$clang" "${native_flags[@]}" -O2 -c "$test_root/tests/fixtures/build/helper.c" -o /dev/null

# Neither frontend errors nor errors in the native assembler may finalize an
# original-object hash. The latter happens after a pristine capture exists.
for failure in syntax backend; do
    failure_metadata="$native_lane/$failure-metadata"
    mkdir "$failure_metadata"
    if NIER_BUILD_METADATA="$failure_metadata" "$clang" "${native_flags[@]}" \
        -O2 -c "$test_root/tests/fixtures/publisher/native-$failure.c" \
        -o "$native_lane/failed-$failure.o"; then
        printf 'ERROR: native %s failure unexpectedly succeeded\n' "$failure" >&2; exit 1
    fi
    if rg '"native_sha256"' "$failure_metadata"; then
        printf 'ERROR: failed native codegen finalized an object hash\n' >&2; exit 1
    fi
done
unset NIER_BUILD_METADATA NIER_BUILD_LANE NIER_BUILD_PROFILE
printf 'Publisher options, dependencies, failure cleanup, shared output, and native provenance passed.\n'
