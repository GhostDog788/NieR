#!/usr/bin/env bash
set -euo pipefail
selac=$1
export SELA_SDK_ROOT=$2
config=$3
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-publisher-XXXXXX")
clang="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
source_file="$test_root/tests/fixtures/publisher/flags.c"
include_dir="$test_root/tests/fixtures/publisher/include"

# Ordinary compiler options survive all native captures and device lowering.
for level in O0 O1 O2 O3 Os Oz; do
    "$clang" --config="$config" "-$level" -std=c17 -DPUBLISHED_VALUE=19 \
      -I "$include_dir" "$source_file" -o "$test_work/$level.sela"
    "$selac" "$test_work/$level.sela" -o "$test_work/$level"
    test "$(env -u LD_LIBRARY_PATH "$test_work/$level")" = '23 19'
done

# Explicit project system-header paths are not lost when changing SDK profiles.
"$clang" --config="$config" -O2 -std=c17 -DPUBLISHED_VALUE=19 \
  -isystem "$include_dir" "$source_file" -o "$test_work/isystem.sela"
"$selac" "$test_work/isystem.sela" -o "$test_work/isystem"
test "$(env -u LD_LIBRARY_PATH "$test_work/isystem")" = '23 19'

# Dependency outputs use the user's -MT/-MF, never the private native object.
"$clang" --config="$config" -O2 -DPUBLISHED_VALUE=19 -I "$include_dir" \
  -MMD -MP -MF "$test_work/flags.d" -MT flags.o -c "$source_file" \
  -o "$test_work/flags.o"
rg -q '^flags.o:' "$test_work/flags.d"
rg -q 'sela_options.h' "$test_work/flags.d"
rg -q 'sela_width64.h' "$test_work/flags.d"
rg -q 'sela_width32.h' "$test_work/flags.d"
if rg 'sela-private-' "$test_work/flags.d"; then
    printf 'ERROR: private native output leaked into dependency file\n' >&2; exit 1
fi

# Options invalid for any requested target fail the entire publication.
for flag in -march=haswell -mcpu=cortex-a53; do
    if "$clang" --config="$config" "$flag" -O2 -DPUBLISHED_VALUE=19 \
        -I "$include_dir" -c "$source_file" -o "$test_work/rejected.o"; then
        printf 'ERROR: silently accepted target override %s\n' "$flag" >&2; exit 1
    fi
    test ! -e "$test_work/rejected.o"
done
# A target-scoped CPU choice survives import and inverse lowering. This checks
# code generation, not the still-pending minimum-device-capability admission.
SELA_ARCHS=x86_64 "$clang" --config="$config" -march=haswell -O2 -DPUBLISHED_VALUE=19 \
  -I "$include_dir" -c "$source_file" -o "$test_work/haswell.o"
"${SELA_REFERENCE_LOWER:-$(dirname -- "$selac")/sela_reference_lower}" lower \
  "$test_work/haswell.o" --target x86_64 --output-dir "$test_work/haswell-lowered"
rg -q '"target-cpu"="haswell"' "$test_work/haswell-lowered/0.ll"

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
  -Wl,-soname,libwidth.so -o "$test_work/libwidth.sela"
"$selac" "$test_work/libwidth.sela" -o "$test_work/libwidth.so"
readelf -h "$test_work/libwidth.so" | rg -q 'DYN \(Shared object file\)'
readelf --dyn-syms "$test_work/libwidth.so" | rg -q 'native_width'
readelf -d "$test_work/libwidth.so" | rg -q 'SONAME.*\[libwidth.so\]'

# The native-build observer does not replace ordinary Clang actions. Its
# readonly provenance section survives stock archives but is absent from the
# pristine LLVM capture that is later merged into public Sela Code.
plugin_dir=$(dirname -- "$config")
llvm_bin="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin"
native_lane="$test_work/native"
mkdir -p "$native_lane/metadata"
native_flags=(--target=x86_64-unknown-linux-gnu
    --sysroot="$SELA_SDK_ROOT/sysroots/x86_64-linux-gnu"
    -fno-temp-file -fPIC -g -fstandalone-debug -frecord-command-line
    -fplugin="$plugin_dir/libsela-clang.so"
    -fpass-plugin="$plugin_dir/sela-capture.so")
export SELA_BUILD_METADATA="$native_lane/metadata"
export SELA_BUILD_LANE="$native_lane"
export SELA_BUILD_PROFILE=x86_64
"$clang" "${native_flags[@]}" -E "$test_root/tests/fixtures/build/helper.c" \
    -o "$native_lane/preprocessed.c"
"$clang" "${native_flags[@]}" -M "$test_root/tests/fixtures/build/helper.c" \
    -MF "$native_lane/native.d"
test -z "$(rg --files "$native_lane/metadata")"
for profile in x86_64 i686; do
  export SELA_BUILD_PROFILE="$profile"
  for level in O0 O1 O2 O3 Os Oz; do
    native_object="$native_lane/$profile-$level.o"
    "$clang" "${native_flags[@]}" --target="$profile-unknown-linux-gnu" \
        --sysroot="$SELA_SDK_ROOT/sysroots/$profile-linux-gnu" \
        "-$level" -c "$test_root/tests/fixtures/build/helper.c" -o "$native_object"
    record=$(readelf -p .sela.capture "$native_object" | sed -n 's/^  \[ *0\]  //p')
    capture=${record%.compile.json}
    capture_hash=$(sha256sum "$capture")
    capture_hash=${capture_hash%% *}
    native_hash=$(sha256sum "$native_object")
    native_hash=${native_hash%% *}
    rg -Fq "\"capture_sha256\": \"$capture_hash\"" "$record"
    rg -Fq "\"native_sha256\": \"$native_hash\"" "$record"
    readelf -SW "$native_object" | rg -q '\.sela\.capture +PROGBITS .* A +0 +0 +1$'
    "$llvm_bin/llvm-dis" "$capture" -o "$native_lane/capture.ll"
    if rg '__sela_private_capture|\.sela\.capture' "$native_lane/capture.ll"; then
        printf 'ERROR: private provenance contaminated the portable input\n' >&2; exit 1
    fi
    "$llvm_bin/llvm-ar" rc "$native_lane/libhelper-$profile-$level.a" "$native_object"
    readelf -p .sela.capture "$native_lane/libhelper-$profile-$level.a" | rg -Fq "$record"
  done
done
# Dependency provenance comes from the consumed file buffers, including system
# headers, forced includes, and repeated include aliases. Every architecture
# records its own conditional header; canonical aliases appear only once.
ln -s "$include_dir/sela_options.h" "$native_lane/options-alias.h"
for profile in x86_64 i686 armv7 aarch64; do
  eval "$(python3 "$test_root/sdk/targets.py" shell "$profile")"
  export SELA_BUILD_PROFILE="$profile"
  native_object="$native_lane/$profile-dependencies.o"
  "$clang" "${native_flags[@]}" --target="$SELA_TARGET_TRIPLE" \
      --sysroot="$SELA_SDK_ROOT/sysroots/$SELA_TARGET_SYSROOT_TRIPLE" \
      "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}" \
      -O2 -DPUBLISHED_VALUE=19 -I "$include_dir" \
      -include "$include_dir/sela_options.h" -include "$native_lane/options-alias.h" \
      -c "$source_file" -o "$native_object"
  record=$(readelf -p .sela.capture "$native_object" | sed -n 's/^  \[ *0\]  //p')
  python3 - "$record" "$profile" "$source_file" "$include_dir" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

record, target, source, headers = sys.argv[1:]
dependencies = json.loads(Path(record).read_text())["dependencies"]
paths = [entry["path"] for entry in dependencies]
assert len(paths) == len(set(paths)), "duplicate canonical dependency"
for entry in dependencies:
    path = Path(entry["path"])
    assert str(path.resolve()) == entry["path"]
    assert hashlib.sha256(path.read_bytes()).hexdigest() == entry["sha256"], path
bits = "64" if target in ("x86_64", "aarch64") else "32"
for required in (Path(source), Path(headers) / "sela_options.h",
                 Path(headers) / f"sela_width{bits}.h"):
    assert str(required.resolve()) in paths, required
other = "32" if bits == "64" else "64"
assert str((Path(headers) / f"sela_width{other}.h").resolve()) not in paths
assert any(path.endswith("/stdio.h") for path in paths)
PY
done
export SELA_BUILD_PROFILE=x86_64
# Discarded configure output remains an ordinary successful native compile.
"$clang" "${native_flags[@]}" -O2 -c "$test_root/tests/fixtures/build/helper.c" -o /dev/null

# Neither frontend errors nor errors in the native assembler may finalize an
# original-object hash. The latter happens after a pristine capture exists.
for failure in syntax backend; do
    failure_metadata="$native_lane/$failure-metadata"
    mkdir "$failure_metadata"
    if SELA_BUILD_METADATA="$failure_metadata" "$clang" "${native_flags[@]}" \
        -O2 -c "$test_root/tests/fixtures/publisher/native-$failure.c" \
        -o "$native_lane/failed-$failure.o"; then
        printf 'ERROR: native %s failure unexpectedly succeeded\n' "$failure" >&2; exit 1
    fi
    if rg '"native_sha256"' "$failure_metadata"; then
        printf 'ERROR: failed native codegen finalized an object hash\n' >&2; exit 1
    fi
done
unset SELA_BUILD_METADATA SELA_BUILD_LANE SELA_BUILD_PROFILE
printf 'Publisher options, dependencies, failure cleanup, shared output, and native provenance passed.\n'
