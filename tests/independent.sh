#!/usr/bin/env bash
set -euo pipefail
producer=$1
nierc=$2
export NIER_SDK_ROOT=$3
test_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-independent-XXXXXX")
"$producer" "$test_work/independent.nier"
"$nierc" inspect "$test_work/independent.nier"
"$nierc" "$test_work/independent.nier" -o "$test_work/independent"
env -u LD_LIBRARY_PATH "$test_work/independent"
target=$("$nierc" --print-target)
case "$target" in
    x86_64) foreign=i686; width=8; word=i64 ;;
    i686) foreign=x86_64; width=4; word=i32 ;;
    *) exit 1 ;;
esac
"$nierc" lower "$test_work/independent.nier" --target "$target" --output-dir "$test_work/native-ir"
rg -q "ret $word $width" "$test_work/native-ir/1.ll"
rg -q 'ret i32 8' "$test_work/native-ir/1.ll"
if "$nierc" lower "$test_work/independent.nier" --target "$foreign" --output-dir "$test_work/foreign-ir" > "$test_work/foreign.log" 2>&1; then
    printf 'ERROR: target-specific compiler accepted a foreign native target\n' >&2; exit 1
fi
rg -q 'native target unavailable' "$test_work/foreign.log"
test ! -e "$test_work/foreign-ir"
if tar -xOf "$test_work/independent.nier" manifest.json | rg 'profiles|clang|capture'; then
    printf 'ERROR: independent artifact requires producer provenance\n' >&2; exit 1
fi
# Both canonical names and filesystem identity matter. In particular, a
# symlinked output parent can replace the original input despite different
# lexical paths; hard-link aliases are conservatively rejected as well.
for alias in direct input-link output-link input-parent output-parent hardlink parent-dotdot; do
    lane="$test_work/alias-$alias"
    mkdir -p "$lane/real/child"
    cp "$test_work/independent.nier" "$lane/real/input.nier"
    input="$lane/real/input.nier"
    output="$input"
    case "$alias" in
      input-link) ln -s "$input" "$lane/source.nier"; input="$lane/source.nier" ;;
      output-link) ln -s "$input" "$lane/output.nier"; output="$lane/output.nier" ;;
      input-parent) ln -s "$lane/real" "$lane/source"; input="$lane/source/input.nier" ;;
      output-parent) ln -s "$lane/real" "$lane/output"; output="$lane/output/input.nier" ;;
      hardlink) ln "$input" "$lane/output.nier"; output="$lane/output.nier" ;;
      parent-dotdot) ln -s "$lane/real/child" "$lane/output"; output="$lane/output/../input.nier" ;;
    esac
    identity=$(stat -Lc '%d:%i:%s:%y:%z' "$input")
    for action in compile lower; do
        if [[ $action == compile ]]; then
            command=("$nierc" "$input" -o "$output")
        else
            command=("$nierc" lower "$input" --target "$target" --output-dir "$output")
        fi
        if "${command[@]}" >"$lane/$action.log" 2>&1; then
            printf 'ERROR: consumer accepted %s input/output alias for %s\n' "$alias" "$action" >&2
            exit 1
        fi
        rg -q 'input and output must be different files' "$lane/$action.log"
        cmp "$lane/real/input.nier" "$test_work/independent.nier"
        test "$(stat -Lc '%d:%i:%s:%y:%z' "$input")" = "$identity"
        test "$(stat -Lc '%d:%i:%s:%y:%z' "$output")" = "$identity"
    done
done
# Read-only use of an ordinary input symlink is still supported.
"$nierc" inspect "$test_work/alias-input-link/source.nier" >"$test_work/symlink-inspection.log"
printf 'Independent producer and consumer passed.\n'
