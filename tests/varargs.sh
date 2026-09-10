#!/usr/bin/env bash
set -euo pipefail
nierc=$(realpath "$1")
sdk=$(realpath "$2")
config=$(realpath "$3")
project=$(cd "$(dirname "$0")/.." && pwd)
case "${4:-scalar}" in
  scalar) fixture=scalar_varargs; expected='Scalar varargs passed' ;;
  forward) fixture=va_forward; expected='Native va_list forwarding passed' ;;
  *) echo 'Unknown variadic fixture' >&2; exit 2 ;;
esac
export LD_LIBRARY_PATH="$sdk/host/usr/lib/llvm-18/lib:$sdk/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
work=$(mktemp -d -t nier-varargs-XXXXXX)
trap 'rm -r -- "$work"' EXIT
for optimization in O0 O2; do
  "$sdk/host/usr/lib/llvm-18/bin/clang" --config="$config" "-$optimization" \
    "$project/tests/abi/$fixture.c" -o "$work/scalars.nier"
  "$nierc" "$work/scalars.nier" --sdk "$sdk" -o "$work/scalars"
  test "$(env -u LD_LIBRARY_PATH "$work/scalars")" = "$expected"
done
echo "$expected at O0 and O2 through the Nier pipeline"
