#!/usr/bin/env bash
set -euo pipefail
nierc=$(realpath "$1")
sdk=$(realpath "$2")
config=$(realpath "$3")
project=$(cd "$(dirname "$0")/.." && pwd)
fixture="$project/tests/fixtures/linking"
llvm="$sdk/host/usr/lib/llvm-18/bin"
export LD_LIBRARY_PATH="$sdk/host/usr/lib/llvm-18/lib:$sdk/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
work=$(mktemp -d -t nier-linking-XXXXXX)
trap 'rm -r -- "$work"' EXIT
mkdir "$work/libraries"

"$llvm/clang" --config="$config" -O2 -shared "$fixture/api.c" \
  -Wl,-soname,libexample.so.1,--undefined-version,--version-script,"$fixture/api.map" \
  -o "$work/library.nier"
"$nierc" "$work/library.nier" --sdk "$sdk" -o "$work/libraries/libexample.so.1"
readelf --dyn-syms --wide "$work/libraries/libexample.so.1" > "$work/symbols"
grep -q 'public_answer@@API_1' "$work/symbols"
if grep -q 'hidden_answer' "$work/symbols"; then
  echo 'Symbol version script lost local visibility' >&2
  exit 1
fi
readelf -d "$work/libraries/libexample.so.1" | grep -q 'SONAME.*libexample.so.1'
tar -xOf "$work/library.nier" link/version.script > "$work/version.script"
if grep -q 'private publication\|/home/builder' "$work/version.script"; then
  echo 'Version script leaked publication comments' >&2
  exit 1
fi

# An explicit native dependency used only for constructors must survive
# destination linking. This fixture is a conventional native DSO, not NieR IR.
"$llvm/clang" --target=x86_64-linux-gnu --sysroot="$sdk/sysroots/x86_64-linux-gnu" \
  -resource-dir="$sdk/host/usr/lib/llvm-18/lib/clang/18" --ld-path="$llvm/ld.lld" \
  -O2 -shared -fPIC -nostdlib "$fixture/constructor.c" \
  -Wl,-soname,libinitializer.so.1 -o "$work/libraries/libinitializer.so.1"
"$llvm/clang" --config="$config" -O2 "$fixture/main.c" \
  -l:libexample.so.1 -l:libinitializer.so.1 -Wl,--export-dynamic,--hash-style=both \
  -o "$work/main.nier"
"$nierc" "$work/main.nier" --sdk "$sdk" --library-dir "$work/libraries" -o "$work/main"
test "$(env -u LD_LIBRARY_PATH "$work/main")" = $'constructor\n42'
readelf -d "$work/main" > "$work/dynamic"
grep -q 'NEEDED.*libinitializer.so.1' "$work/dynamic"
grep -q '(HASH)' "$work/dynamic"
grep -q '(GNU_HASH)' "$work/dynamic"
readelf --dyn-syms --wide "$work/main" | grep -q 'executable_export'

before=$(sha256sum "$work/main")
if "$nierc" "$work/main.nier" --sdk "$sdk" -o "$work/main" > "$work/missing.log" 2>&1; then
  echo 'Missing native dependency unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'missing declared managed native dependency' "$work/missing.log"
test "$before" = "$(sha256sum "$work/main")"
# Linker options use their final effective value, including overrides of
# Clang's own defaults. Publication must not quietly discard a custom loader.
"$llvm/clang" --config="$config" -O2 "$fixture/main.c" \
  -l:libexample.so.1 -l:libinitializer.so.1 -Wl,--hash-style=both,--hash-style=gnu \
  -o "$work/gnu.nier"
"$nierc" "$work/gnu.nier" --sdk "$sdk" --library-dir "$work/libraries" -o "$work/gnu"
readelf -d "$work/gnu" > "$work/gnu-dynamic"
grep -q '(GNU_HASH)' "$work/gnu-dynamic"
if grep -q '(HASH)' "$work/gnu-dynamic"; then
  echo 'Overridden hash style survived publication' >&2
  exit 1
fi
for loader in '-Wl,--dynamic-linker,/custom/loader' '-Wl,--dynamic-linker=/custom/loader'; do
  if "$llvm/clang" --config="$config" -O2 "$fixture/dynamic.c" "$loader" \
      -o "$work/custom-loader.nier" > "$work/loader.log" 2>&1; then
    echo 'Custom interpreter was silently dropped' >&2
    exit 1
  fi
  grep -q 'unqualified native dynamic interpreter' "$work/loader.log"
done
"$llvm/clang" --config="$config" -O2 "$fixture/dynamic.c" -ldl -o "$work/dynamic.nier"
"$nierc" "$work/dynamic.nier" --sdk "$sdk" -o "$work/dynamic"
test "$(env -u LD_LIBRARY_PATH "$work/dynamic")" = 'Ordinary dlopen passed'
echo 'SONAME, versioned exports, dynamic visibility and constructor-only native imports passed'
