#!/usr/bin/env bash
set -euo pipefail
nierc=$1
export NIER_SDK_ROOT=$2
clang_config=$3
build_tool="$(dirname -- "$clang_config")/nier-build"
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-source-inventory-XXXXXX")
for system in make cmake; do
  "$build_tool" --system "$system" --source "$test_root/tests/fixtures/source-inventory" \
    --target hello --output hello --artifact "$test_work/$system.nier" --keep-private \
    2>&1 | tee "$test_work/$system.log"
  "$nierc" inspect "$test_work/$system.nier" | tee "$test_work/$system-inspection.log"
  grep -q 'x86_64: 3 native compilation units' "$test_work/$system-inspection.log"
  grep -q 'i686: 2 native compilation units' "$test_work/$system-inspection.log"
  # Same common program bodies are stored once, with different TU ownership.
  test "$(tar -tf "$test_work/$system.nier" | grep -c '^modules/')" -eq 3
  for target in x86_64 i686; do
    "$nierc" lower "$test_work/$system.nier" --target "$target" \
      --output-dir "$test_work/$system-$target"
  done
  test "$(find "$test_work/$system-x86_64" -maxdepth 1 -name '*.ll' | wc -l)" -eq 3
  test "$(find "$test_work/$system-i686" -maxdepth 1 -name '*.ll' | wc -l)" -eq 2
  grep -q 'define.*@first' "$test_work/$system-i686/1.ll"
  grep -q 'define.*@second' "$test_work/$system-i686/1.ll"
  "$nierc" "$test_work/$system.nier" -o "$test_work/$system-native" --keep-work \
    2>&1 | tee "$test_work/$system-compile.log"
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$test_work/$system-native"
  private=$(sed -n 's/^Private compiler workspace: //p' "$test_work/$system-compile.log")
  test "$(find "$private" -maxdepth 1 -name '*.opt.bc' | wc -l)" -eq 3
  before=$(sha256sum "$test_work/$system.nier")
  for variant in NIER_CROSS_FRAGMENT NIER_PRIVATE_IDENTITY; do
    if "$build_tool" --system "$system" --source "$test_root/tests/fixtures/source-inventory" \
        --target hello --output hello --artifact "$test_work/$system.nier" --cflag "-D$variant" \
        > "$test_work/$system-$variant.log" 2>&1; then
      echo 'An unqualified native identity boundary unexpectedly published' >&2
      exit 1
    fi
    if test "$variant" = NIER_CROSS_FRAGMENT; then
      grep -q 'cross-fragment native definition reference needs an explicit shared identity' \
        "$test_work/$system-$variant.log"
    else
      grep -q 'regrouped native units currently require self-contained external scalar definitions' \
        "$test_work/$system-$variant.log"
    fi
    test "$(sha256sum "$test_work/$system.nier")" = "$before"
  done
  test ! -e "$test_root/tests/fixtures/source-inventory/main.o"
done
echo 'Different native source counts share one program and preserve original TU optimization boundaries.'
