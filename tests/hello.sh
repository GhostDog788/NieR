#!/usr/bin/env bash
set -euo pipefail
nierc=$1
export NIER_SDK_ROOT=$2
config=$3
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-hello-XXXXXX")
clang="$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
"$clang" --config="$config" -O2 "$test_root/examples/hello/hello/main.c" "$test_root/examples/hello/hello/hello.c" -o "$test_work/hello.nier"
"$nierc" inspect "$test_work/hello.nier"
"$nierc" "$test_work/hello.nier" -o "$test_work/hello"
test "$(env -u LD_LIBRARY_PATH "$test_work/hello")" = 'Hello world'
readelf -l "$test_work/hello" | rg -F "$NIER_SDK_ROOT/sysroots/x86_64-linux-gnu/"
"$clang" --config="$config" -O2 -c "$test_root/examples/hello/hello/main.c" -o "$test_work/main.o"
"$clang" --config="$config" -O2 -c "$test_root/examples/hello/hello/hello.c" -o "$test_work/hello.o"
"$clang" --config="$config" "$test_work/main.o" "$test_work/hello.o" -o "$test_work/separate.nier"
cmp "$test_work/hello.nier" "$test_work/separate.nier"
mv "$test_work/main.o" "$test_work/main.private"
mv "$test_work/hello.o" "$test_work/hello.private"
"$nierc" "$test_work/separate.nier" -o "$test_work/separate"
test "$(env -u LD_LIBRARY_PATH "$test_work/separate")" = 'Hello world'
# Normal compiler rebuild semantics; no compatibility with previous formats.
"$clang" --config="$config" -O2 "$test_root/examples/hello/hello/main.c" "$test_root/examples/hello/hello/hello.c" -o "$test_work/hello.nier"
"$nierc" "$test_work/hello.nier" -o "$test_work/hello"
"$clang" --config="$config" -O2 "$test_root/tests/fixtures/width.c" -o "$test_work/width.nier"
"$nierc" "$test_work/width.nier" -o "$test_work/width"
test "$(env -u LD_LIBRARY_PATH "$test_work/width")" = 'pointer=8 word=8 fixed=4,8'
"$nierc" lower "$test_work/width.nier" --target i686 --output-dir "$test_work/width-i686"
"$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/opt" -passes=verify -disable-output "$test_work/width-i686/0.ll"
if "$clang" --config="$config" "$test_root/tests/fixtures/unsupported.c" -o "$test_work/unsupported.nier"; then
    printf 'ERROR: accepted unqualified inline assembly\n' >&2; exit 1
fi
test ! -e "$test_work/unsupported.nier"
if "$nierc" "$test_work/hello.nier" --sdk "$test_work/missing-sdk" -o "$test_work/invalid"; then
    printf 'ERROR: accepted missing SDK\n' >&2; exit 1
fi
mkdir "$test_work/wrong-sdk"
printf '%064d\n' 0 > "$test_work/wrong-sdk/sdk-lock.sha256"
if "$nierc" "$test_work/hello.nier" --sdk "$test_work/wrong-sdk" -o "$test_work/invalid"; then
    printf 'ERROR: accepted mismatched SDK\n' >&2; exit 1
fi
test ! -e "$test_work/invalid"
test "$(tar -tf "$test_work/hello.nier" | wc -l)" -eq 3
if tar -xOf "$test_work/hello.nier" | strings | rg 'hello\.c|main\.c|DICompileUnit|DILocalVariable|nier-private-|nier-hello-'; then
    printf 'ERROR: publisher evidence leaked\n' >&2; exit 1
fi
mv "$test_work/hello.nier" "$test_work/hello.nier.offline"
test "$(env -u LD_LIBRARY_PATH "$test_work/hello")" = 'Hello world'
printf 'Stock-Clang multi-file NieR pipeline passed. Workspace: %s\n' "$test_work"
