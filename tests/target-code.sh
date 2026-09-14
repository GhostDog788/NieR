#!/usr/bin/env bash
# Bounded source/native/Sela comparisons. Foreign execution here uses binfmt;
# matching-kernel acceptance is separately provided by multi-consumer.sh.
set -euo pipefail
selac=$(realpath "$1")
export SELA_SDK_ROOT=$(realpath "$2")
config=$(realpath "$3")
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$repository/sdk/env.sh"
clang="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
reference_lower="$(dirname -- "$selac")/sela_reference_lower"
target_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-target-code-XXXXXX")
printf 'Target code evidence: %s\n' "$target_work"
mapfile -t targets < <(python3 "$repository/sdk/targets.py" list)
for level in O0 O2; do
  for fixture in vector asm init; do
    source_file="$repository/tests/fixtures/target-$fixture.c"
    artifact="$target_work/$fixture-$level.sela"
    env -u SELA_ARCHS "$clang" --config="$config" "-$level" "$source_file" -o "$artifact"
    for target in "${targets[@]}"; do
      eval "$(python3 "$repository/sdk/targets.py" shell "$target")"
      lane="$target_work/$fixture-$level-$target"
      mkdir "$lane"
      "$reference_lower" lower "$artifact" --target "$target" --output-dir "$lane/lowered"
      runtime="$SELA_SDK_ROOT/sysroots/$SELA_TARGET_SYSROOT_TRIPLE/usr/lib/$SELA_TARGET_MULTIARCH"
      command=("$clang" --target="$SELA_TARGET_TRIPLE"
        --sysroot="$SELA_SDK_ROOT/sysroots/$SELA_TARGET_SYSROOT_TRIPLE"
        -resource-dir="$SELA_SDK_ROOT/host/usr/lib/llvm-18/lib/clang/18"
        "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}"
        --rtlib=compiler-rt --unwindlib=none --ld-path="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/ld.lld"
        "-$level" -fPIC "-Wl,--dynamic-linker,$runtime/$SELA_TARGET_LOADER"
        "-Wl,-rpath,$runtime" -Wl,-z,nodefaultlib)
      "${command[@]}" "$source_file" -o "$lane/reference"
      "${command[@]}" "$lane/lowered/0.ll" -o "$lane/reconstructed"
      env -u LD_LIBRARY_PATH "$lane/reference" > "$lane/reference.stdout"
      env -u LD_LIBRARY_PATH "$lane/reconstructed" > "$lane/reconstructed.stdout"
      cmp "$lane/reference.stdout" "$lane/reconstructed.stdout"
    done
    "$selac" "$artifact" -o "$target_work/$fixture-$level"
    env -u LD_LIBRARY_PATH "$target_work/$fixture-$level" > "$target_work/$fixture-$level.stdout"
  done
done
printf 'Vectors, wide integers, intrinsics, inline assembly/asm goto, and initialization passed four targets at O0/O2.\n'
