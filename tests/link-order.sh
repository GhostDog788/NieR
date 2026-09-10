#!/usr/bin/env bash
set -euo pipefail
nierc=$1
export NIER_SDK_ROOT=$2
clang_config=$3
build_tool="$(dirname -- "$clang_config")/nier-build"
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-link-order-XXXXXX")
for system in make cmake; do
  "$build_tool" --system "$system" --source "$test_root/tests/fixtures/link-order" \
    --target hello --output hello --artifact "$test_work/$system.nier"
  for target in x86_64 i686; do
    "$nierc" lower "$test_work/$system.nier" --target "$target" \
      --output-dir "$test_work/$system-$target"
  done
  grep -q 'define.*@first' "$test_work/$system-x86_64/1.ll"
  grep -q 'define.*@second' "$test_work/$system-x86_64/2.ll"
  grep -q 'define.*@second' "$test_work/$system-i686/1.ll"
  grep -q 'define.*@first' "$test_work/$system-i686/2.ll"
  # Native TU settings differ between the caller and libraries and stay with
  # each unit when the selected native archive order is permuted.
  grep -q 'optnone' "$test_work/$system-x86_64/0.ll"
  grep -q 'optnone' "$test_work/$system-i686/0.ll"
  if grep -q 'optnone' "$test_work/$system-x86_64/1.ll" "$test_work/$system-i686/1.ll"; then
    echo 'Native TU settings were silently made uniform' >&2
    exit 1
  fi
  "$nierc" "$test_work/$system.nier" -o "$test_work/$system-native"
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$test_work/$system-native"
done
echo 'Per-profile archive selection order and independent per-TU settings preserved.'
