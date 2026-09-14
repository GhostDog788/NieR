#!/usr/bin/env bash
set -euo pipefail
selac=$1
export SELA_SDK_ROOT=$2
config=$3
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
selection_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-selection-XXXXXX")
clang="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
for selection in x86_64 i686 armv7 aarch64 x86_64,aarch64 i686,armv7 x86_64,i686,armv7 x86_64,i686,armv7,aarch64; do
  SELA_ARCHS="$selection" "$clang" --config="$config" -O2 \
    "$project/examples/hello/hello/main.c" "$project/examples/hello/hello/hello.c" \
    -o "$selection_work/$selection.sela"
  tar -xOf "$selection_work/$selection.sela" manifest.json | \
    python3 -c 'import json,sys; m=json.load(sys.stdin); assert m["targets"]==sys.argv[1].split(","); assert set(m["compilation_units"])==set(m["targets"])' "$selection"
done
"$selac" "$selection_work/x86_64.sela" -o "$selection_work/hello"
test "$(env -u LD_LIBRARY_PATH "$selection_work/hello")" = 'Hello world'
SELA_ARCHS=x86_64 "$clang" --config="$config" -O2 -c \
  "$project/examples/hello/hello/main.c" -o "$selection_work/main.o"
if env -u SELA_ARCHS "$clang" --config="$config" "$selection_work/main.o" \
    -o "$selection_work/rejected.sela" >"$selection_work/rejected.log" 2>&1; then
  printf 'ERROR: linker silently narrowed its requested targets\n' >&2
  exit 1
fi
rg -q 'requested target i686 is missing' "$selection_work/rejected.log"
test ! -e "$selection_work/rejected.sela"
for invalid in '' 'x86_64,' 'x86_64,x86_64' unknown; do
  if SELA_ARCHS="$invalid" "$clang" --config="$config" -c \
      "$project/examples/hello/hello/main.c" -o "$selection_work/invalid.o" \
      >"$selection_work/invalid.log" 2>&1; then
    printf 'ERROR: accepted invalid architecture selection\n' >&2
    exit 1
  fi
  test ! -e "$selection_work/invalid.o"
done
printf 'Target-selection publication checks passed: %s\n' "$selection_work"
