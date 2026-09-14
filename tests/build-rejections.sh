#!/usr/bin/env bash
set -euo pipefail
build_tool=$1
export SELA_SDK_ROOT=$2
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-build-rejections-XXXXXX")
for selected in missing-capture mutated-object transplanted-marker repeated-journal unsupported-link unsupported-interpreter; do
  if make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
      SELA_SOURCE_DIR="$test_root/tests/fixtures/build-rejections" \
      SELA_BUILD_TARGETS="$selected" SELA_NATIVE_OUTPUT="$selected" \
      SELA_ARTIFACT="$test_work/$selected.sela" > "$test_work/$selected.log" 2>&1; then
    echo "An unqualified selected link unexpectedly published: $selected" >&2
    exit 1
  fi
  grep -q 'selected link is not yet qualified:' "$test_work/$selected.log"
  if test "$selected" = mutated-object || test "$selected" = transplanted-marker; then
    grep -q 'native object changed after successful Clang code generation:' "$test_work/$selected.log"
  fi
  if test "$selected" = repeated-journal; then
    grep -q 'ambiguous repeated-journal archive member:' "$test_work/$selected.log"
  fi
  if test "$selected" = unsupported-interpreter; then
    grep -q 'unqualified native dynamic interpreter /unqualified/loader.so' "$test_work/$selected.log"
  fi
  test ! -e "$test_work/$selected.sela"
done
test ! -e "$test_root/tests/fixtures/build-rejections/main.o"
test ! -e "$test_root/tests/fixtures/build-rejections/libhelper.a"
printf 'Missing/mutated capture and unqualified link semantics fail publication after real native builds.\n'
