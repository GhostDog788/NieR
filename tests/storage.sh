#!/usr/bin/env bash
set -euo pipefail
nierc=$(realpath "$1")
sdk=$(realpath "$2")
config=$(realpath "$3")
project=$(cd "$(dirname "$0")/.." && pwd)
export LD_LIBRARY_PATH="$sdk/host/usr/lib/llvm-18/lib:$sdk/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
work=$(mktemp -d -t nier-storage-XXXXXX)
trap 'rm -r -- "$work"' EXIT
for optimization in O0 O2; do
  "$sdk/host/usr/lib/llvm-18/bin/clang" --config="$config" "-$optimization" \
    "$project/tests/storage-native.c" -o "$work/storage.nier"
  "$nierc" "$work/storage.nier" --sdk "$sdk" -o "$work/storage"
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$work/storage"
  "$sdk/host/usr/lib/llvm-18/bin/clang" --config="$config" "-$optimization" \
    "$project/tests/fixtures/scalar-field-result.c" -o "$work/scalar-fields.nier"
  "$nierc" "$work/scalar-fields.nier" --sdk "$sdk" -o "$work/scalar-fields"
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$work/scalar-fields"
done
echo 'Native record/global/callback/setjmp and scalar field-result pipelines passed at O0 and O2'
