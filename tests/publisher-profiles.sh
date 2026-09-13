#!/usr/bin/env bash
set -euo pipefail
selac=$(realpath -- "$1")
export SELA_SDK_ROOT=$(realpath -- "$2")
config=$(realpath -- "$3")
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
llvm_bin="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin"
profile_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-profile-test-XXXXXX")
for choice in default signed unsigned; do
  flags=()
  if [[ $choice == signed ]]; then flags=(-fsigned-char); fi
  if [[ $choice == unsigned ]]; then flags=(-funsigned-char); fi
  "$llvm_bin/clang" --config="$config" -Werror -O2 "${flags[@]}" \
    -Xclang -plugin-arg-sela -Xclang keep-work \
    "$project/tests/fixtures/publisher/plain-char.c" \
    -o "$profile_work/$choice.sela" >"$profile_work/$choice.log" 2>&1
  private=$(sed -n 's/^Private Sela producer workspace: //p' "$profile_work/$choice.log")
  test -d "$private"
  for target in x86_64 i686 armv7 aarch64; do
    expected=1
    if [[ $choice == unsigned || ($choice == default && ($target == armv7 || $target == aarch64)) ]]; then expected=0; fi
    "$llvm_bin/llvm-dis" "$private/$target.bc" -o "$profile_work/$choice-$target.ll"
    observed=$(awk '/define .*@plain_char_signed\(/ { inside = 1 } inside && /ret i32/ { gsub(/,/, "", $3); print $3; exit }' "$profile_work/$choice-$target.ll")
    test "$observed" = "$expected"
  done
  tar -xOf "$profile_work/$choice.sela" manifest.json | python3 -c \
    'import json,sys; assert json.load(sys.stdin)["targets"] == ["x86_64","i686","armv7","aarch64"]'
  "$selac" "$profile_work/$choice.sela" -o "$profile_work/$choice"
  status=0
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$profile_work/$choice" || status=$?
  if [[ $choice == unsigned ]]; then test "$status" -eq 0; else test "$status" -eq 1; fi
done
if "$llvm_bin/clang" --config="$config" -Werror \
    "$project/tests/fixtures/publisher/source-warning.c" \
    -o "$profile_work/source-warning.sela" >"$profile_work/source-warning.log" 2>&1; then
  printf 'Source warnings must still fail publication under -Werror\n' >&2
  exit 1
fi
grep -q 'publication source diagnostics remain enabled' "$profile_work/source-warning.log"
test ! -e "$profile_work/source-warning.sela"
printf 'Four genuine Clang target captures preserve default and explicit plain-char semantics: %s\n' "$profile_work"
