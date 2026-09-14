#!/usr/bin/env bash
set -euo pipefail
selac=$1
export SELA_SDK_ROOT=$2
clang_config=$3
build_tool="$(dirname -- "$clang_config")/sela-build"
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-source-inventory-XXXXXX")
for system in make cmake; do
  "$build_tool" --system "$system" --source "$test_root/tests/fixtures/source-inventory" \
    --build-target hello --output hello --artifact "$test_work/$system.sela" --keep-private \
    2>&1 | tee "$test_work/$system.log"
  "$selac" inspect "$test_work/$system.sela" | tee "$test_work/$system-inspection.log"
  grep -q 'x86_64: 3 native compilation units' "$test_work/$system-inspection.log"
  grep -q 'i686: 2 native compilation units' "$test_work/$system-inspection.log"
  # Same common program bodies are stored once, with different TU ownership.
  test "$(tar -tf "$test_work/$system.sela" | grep -c '^modules/')" -eq 3
  for target in x86_64 i686; do
    "${SELA_REFERENCE_LOWER:-$(dirname -- "$selac")/sela_reference_lower}" lower "$test_work/$system.sela" --target "$target" \
      --output-dir "$test_work/$system-$target"
  done
  test "$(find "$test_work/$system-x86_64" -maxdepth 1 -name '*.ll' | wc -l)" -eq 3
  test "$(find "$test_work/$system-i686" -maxdepth 1 -name '*.ll' | wc -l)" -eq 2
  grep -q 'define.*@first' "$test_work/$system-i686/1.ll"
  grep -q 'define.*@second' "$test_work/$system-i686/1.ll"
  "$selac" "$test_work/$system.sela" -o "$test_work/$system-native" --keep-work \
    2>&1 | tee "$test_work/$system-compile.log"
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$test_work/$system-native"
  private=$(sed -n 's/^Private compiler workspace: //p' "$test_work/$system-compile.log")
  test "$(find "$private" -maxdepth 1 -name '*.opt.bc' | wc -l)" -eq 3
  for variant in SELA_CROSS_FRAGMENT SELA_PRIVATE_IDENTITY; do
    "$build_tool" --system "$system" --source "$test_root/tests/fixtures/source-inventory" \
        --build-target hello --output hello --artifact "$test_work/$system-$variant.sela" --cflag "-D$variant" \
        > "$test_work/$system-$variant.log" 2>&1
    "$selac" "$test_work/$system-$variant.sela" -o "$test_work/$system-$variant"
    env -u LD_LIBRARY_PATH -u LD_PRELOAD "$test_work/$system-$variant"
    for target in x86_64 i686 armv7 aarch64; do
      "${SELA_REFERENCE_LOWER:-$(dirname -- "$selac")/sela_reference_lower}" lower \
        "$test_work/$system-$variant.sela" --target "$target" --output-dir "$test_work/$system-$variant-$target"
    done
  done
  test ! -e "$test_root/tests/fixtures/source-inventory/main.o"
done
echo 'Different native source counts share one program and preserve original TU optimization boundaries.'
