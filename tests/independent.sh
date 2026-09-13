#!/usr/bin/env bash
set -euo pipefail
producer=$1
selac=$2
export SELA_SDK_ROOT=$3
test_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-independent-XXXXXX")
"$producer" "$test_work/independent.sela"
"$selac" inspect "$test_work/independent.sela"
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python3 -B "$repository/tests/consumer-package-boundary.py" "$selac" "$test_work/independent.sela"
"$selac" "$test_work/independent.sela" -o "$test_work/independent"
env -u LD_LIBRARY_PATH "$test_work/independent"
target=$("$selac" --print-target)
case "$target" in
    x86_64) foreign=i686; width=8; word=i64 ;;
    i686) foreign=x86_64; width=4; word=i32 ;;
    *) exit 1 ;;
esac
"$selac" lower "$test_work/independent.sela" --target "$target" --output-dir "$test_work/native-ir"
rg -q "ret $word $width" "$test_work/native-ir/1.ll"
rg -q 'ret i32 8' "$test_work/native-ir/1.ll"
if "$selac" lower "$test_work/independent.sela" --target "$foreign" --output-dir "$test_work/foreign-ir" > "$test_work/foreign.log" 2>&1; then
    printf 'ERROR: target-specific compiler accepted a foreign native target\n' >&2; exit 1
fi
rg -q 'native target unavailable' "$test_work/foreign.log"
test ! -e "$test_work/foreign-ir"
if tar -xOf "$test_work/independent.sela" manifest.json | rg 'profiles|clang|capture'; then
    printf 'ERROR: independent artifact requires producer provenance\n' >&2; exit 1
fi
# Both canonical names and filesystem identity matter. In particular, a
# symlinked output parent can replace the original input despite different
# lexical paths; hard-link aliases are conservatively rejected as well.
for alias in direct input-link output-link input-parent output-parent hardlink parent-dotdot; do
    lane="$test_work/alias-$alias"
    mkdir -p "$lane/real/child"
    cp "$test_work/independent.sela" "$lane/real/input.sela"
    input="$lane/real/input.sela"
    output="$input"
    case "$alias" in
      input-link) ln -s "$input" "$lane/source.sela"; input="$lane/source.sela" ;;
      output-link) ln -s "$input" "$lane/output.sela"; output="$lane/output.sela" ;;
      input-parent) ln -s "$lane/real" "$lane/source"; input="$lane/source/input.sela" ;;
      output-parent) ln -s "$lane/real" "$lane/output"; output="$lane/output/input.sela" ;;
      hardlink) ln "$input" "$lane/output.sela"; output="$lane/output.sela" ;;
      parent-dotdot) ln -s "$lane/real/child" "$lane/output"; output="$lane/output/../input.sela" ;;
    esac
    identity=$(stat -Lc '%d:%i:%s:%y:%z' "$input")
    for action in compile lower; do
        if [[ $action == compile ]]; then
            command=("$selac" "$input" -o "$output")
        else
            command=("$selac" lower "$input" --target "$target" --output-dir "$output")
        fi
        if "${command[@]}" >"$lane/$action.log" 2>&1; then
            printf 'ERROR: consumer accepted %s input/output alias for %s\n' "$alias" "$action" >&2
            exit 1
        fi
        rg -q 'input and output must be different files' "$lane/$action.log"
        cmp "$lane/real/input.sela" "$test_work/independent.sela"
        test "$(stat -Lc '%d:%i:%s:%y:%z' "$input")" = "$identity"
        test "$(stat -Lc '%d:%i:%s:%y:%z' "$output")" = "$identity"
    done
done
# Read-only use of an ordinary input symlink is still supported.
"$selac" inspect "$test_work/alias-input-link/source.sela" >"$test_work/symlink-inspection.log"
printf 'Independent producer and consumer passed.\n'
