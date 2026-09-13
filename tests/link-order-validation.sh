#!/usr/bin/env bash
set -euo pipefail
nierc=$(realpath "$1")
sdk=$(realpath "$2")
config=$(realpath "$3")
project=$(cd "$(dirname "$0")/.." && pwd)
llvm="$sdk/host/usr/lib/llvm-18/bin"
linker="$(dirname "$config")/nier-ld"
export NIER_SDK_ROOT="$sdk"
export LD_LIBRARY_PATH="$sdk/host/usr/lib/llvm-18/lib:$sdk/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
work=$(mktemp -d -t nier-link-order-validation-XXXXXX)
inputs=()
for unit in main first second; do
  level=O2
  if [[ $unit == main ]]; then level=O0; fi
  "$llvm/clang" --config="$config" -"$level" -c \
    "$project/tests/fixtures/link-order/$unit.c" -o "$work/$unit.o"
  inputs+=("$work/$unit.o")
done

# Valid publication still enters through actual stock Clang. The permutation
# changes only the narrow native extraction order; settings stay with each TU.
"$llvm/clang" --config="$config" "${inputs[@]}" \
  -Xlinker --nier-unit-order-i686=0,2,1 -o "$work/valid.nier"
for target in x86_64 i686; do
  "${NIER_REFERENCE_LOWER:-$(dirname -- "$nierc")/nier_reference_lower}" lower "$work/valid.nier" --target "$target" --output-dir "$work/$target"
  rg -q 'define.*@main' "$work/$target/0.ll"
  rg -q 'optnone' "$work/$target/0.ll"
done
rg -q 'define.*@first' "$work/x86_64/1.ll"
rg -q 'define.*@second' "$work/x86_64/2.ll"
rg -q 'define.*@second' "$work/i686/1.ll"
rg -q 'define.*@first' "$work/i686/2.ll"
if rg -q 'optnone' "$work/x86_64/1.ll" "$work/x86_64/2.ll" \
    "$work/i686/1.ll" "$work/i686/2.ll"; then
  echo 'Permutation moved or replaced per-TU optimization settings' >&2
  exit 1
fi
"$nierc" "$work/valid.nier" -o "$work/program"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$work/program"

# Test the internal linker's own failure-atomicity contract directly. Stock
# Clang's separate failed-job cleanup is allowed to unlink its requested -o;
# the SDK coordinator stages Clang output before replacing a published artifact.
cp "$work/valid.nier" "$work/retained.nier"
retained_identity=$(stat -c '%d:%i:%s:%y:%z' "$work/retained.nier")
failures=0
reject() {
  local diagnostic=$1
  shift
  if "$linker" "${inputs[@]}" "$@" -o "$work/retained.nier" \
      >"$work/rejection.log" 2>&1; then
    printf 'ERROR: malformed native-unit order was accepted: %s\n' "$*" >&2
    exit 1
  fi
  rg -q "$diagnostic" "$work/rejection.log"
  cmp "$work/retained.nier" "$work/valid.nier"
  test "$(stat -c '%d:%i:%s:%y:%z' "$work/retained.nier")" = "$retained_identity"
  failures=$((failures + 1))
}

# Empty whole value/segments, wrong separators, nondecimal tokens, signs,
# whitespace, partial parses, integer overflow, missing and excessive entries.
invalid=(
  '' ',' ',0,1,2' '0,1,2,' '0,,2' '0;1;2' '0:1:2'
  '0,one,2' '0,0x1,2' '0,0b1,2' '0,1e0,2' '0,1.0,2'
  '0,+1,2' '0,-1,2' '-0,1,2' '0, 1,2' '0,1 ,2' $'0,1\t,2'
  '0,1suffix,2' '0,4294967296,2' '0,18446744073709551616,2'
  '0,0,2' '0,1,1' '0,1' '0' '0,1,2,3' '0,1,3' '0,1,4294967295'
)
for order in "${invalid[@]}"; do
  reject 'i686 native-unit permutation' "--nier-unit-order-i686=$order"
done
reject 'multiple i686 native-unit permutations' \
  --nier-unit-order-i686=0,1,2 --nier-unit-order-i686=0,2,1
reject 'unqualified publication link option' --nier-unit-order-i686

# Bound parsing work before allocation/publication, including oversized token
# lists whose byte length is still below the independent 4096-byte limit.
many=0
for ((index = 1; index < 513; ++index)); do many+=,0; done
reject 'oversized i686 native-unit permutation' "--nier-unit-order-i686=$many"
printf -v oversized '%04097d' 0
reject 'invalid i686 native-unit permutation' "--nier-unit-order-i686=$oversized"

# Leading zeroes are still decimal, and an explicit identity order is valid.
# Neither changes the x64 sequence or drops any narrow unit.
"$linker" "${inputs[@]}" --nier-unit-order-i686=00,01,02 -o "$work/identity.nier"
"${NIER_REFERENCE_LOWER:-$(dirname -- "$nierc")/nier_reference_lower}" lower "$work/identity.nier" --target i686 --output-dir "$work/identity"
rg -q 'define.*@first' "$work/identity/1.ll"
rg -q 'define.*@second' "$work/identity/2.ll"

# Call the internal linker directly so its own non-destructive diagnostics are
# tested independently of stock Clang's failed-job output cleanup policy.
for alias in direct input-link output-link input-parent output-parent hardlink parent-dotdot; do
  lane="$work/alias-$alias"
  mkdir -p "$lane/real/child"
  cp "$work/main.o" "$lane/real/input.o"
  input="$lane/real/input.o"
  output="$input"
  case "$alias" in
    input-link) ln -s "$input" "$lane/source.o"; input="$lane/source.o" ;;
    output-link) ln -s "$input" "$lane/output.o"; output="$lane/output.o" ;;
    input-parent) ln -s "$lane/real" "$lane/source"; input="$lane/source/input.o" ;;
    output-parent) ln -s "$lane/real" "$lane/output"; output="$lane/output/input.o" ;;
    hardlink) ln "$input" "$lane/output.o"; output="$lane/output.o" ;;
    parent-dotdot) ln -s "$lane/real/child" "$lane/output"; output="$lane/output/../input.o" ;;
  esac
  identity=$(stat -Lc '%d:%i:%s:%y:%z' "$input")
  if "$linker" "$work/first.o" "$input" "$work/second.o" -o "$output" >"$lane/rejection.log" 2>&1; then
    printf 'ERROR: publication linker accepted %s input/output alias\n' "$alias" >&2
    exit 1
  fi
  rg -q 'publication output must not overwrite an input' "$lane/rejection.log"
  cmp "$lane/real/input.o" "$work/main.o"
  test "$(stat -Lc '%d:%i:%s:%y:%z' "$input")" = "$identity"
  test "$(stat -Lc '%d:%i:%s:%y:%z' "$output")" = "$identity"
done
printf '%s invalid permutations rejected without replacing the existing artifact; valid native order preserved: %s\n' "$failures" "$work"
