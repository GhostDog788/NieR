#!/usr/bin/env bash
set -euo pipefail
selac=$1
export SELA_SDK_ROOT=$2
config=$3
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-hello-XXXXXX")
clang="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
"$clang" --config="$config" -O2 "$test_root/examples/hello/hello/main.c" "$test_root/examples/hello/hello/hello.c" -o "$test_work/hello.sela"
"$selac" inspect "$test_work/hello.sela"
"$selac" "$test_work/hello.sela" -o "$test_work/hello"
test "$(env -u LD_LIBRARY_PATH "$test_work/hello")" = 'Hello world'
readelf -l "$test_work/hello" | rg -F "$SELA_SDK_ROOT/sysroots/x86_64-linux-gnu/"
"$clang" --config="$config" -O2 -c "$test_root/examples/hello/hello/main.c" -o "$test_work/main.o"
"$clang" --config="$config" -O2 -c "$test_root/examples/hello/hello/hello.c" -o "$test_work/hello.o"
"$clang" --config="$config" "$test_work/main.o" "$test_work/hello.o" -o "$test_work/separate.sela"
cmp "$test_work/hello.sela" "$test_work/separate.sela"
mv "$test_work/main.o" "$test_work/main.private"
mv "$test_work/hello.o" "$test_work/hello.private"
"$selac" "$test_work/separate.sela" -o "$test_work/separate"
test "$(env -u LD_LIBRARY_PATH "$test_work/separate")" = 'Hello world'
# Normal compiler rebuild semantics; no compatibility with previous formats.
"$clang" --config="$config" -O2 "$test_root/examples/hello/hello/main.c" "$test_root/examples/hello/hello/hello.c" -o "$test_work/hello.sela"
"$selac" "$test_work/hello.sela" -o "$test_work/hello"
"$clang" --config="$config" -O2 "$test_root/tests/fixtures/width.c" -o "$test_work/width.sela"
"$selac" "$test_work/width.sela" -o "$test_work/width"
test "$(env -u LD_LIBRARY_PATH "$test_work/width")" = 'pointer=8 word=8 fixed=4,8'
"${SELA_REFERENCE_LOWER:-$(dirname -- "$selac")/sela_reference_lower}" lower "$test_work/width.sela" --target i686 --output-dir "$test_work/width-i686"
"$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/opt" -passes=verify -disable-output "$test_work/width-i686/0.ll"
if "$clang" --config="$config" "$test_root/tests/fixtures/unsupported.c" -o "$test_work/unsupported.sela"; then
    printf 'ERROR: accepted invalid C input\n' >&2; exit 1
fi
test ! -e "$test_work/unsupported.sela"
# A syntax failure above does not test rejection by the Sela importer. Keep a
# separately valid Clang C case for the still-unimplemented TLS contract.
for profile in x86_64 i686 armv7 aarch64; do
    eval "$(python3 "$test_root/sdk/targets.py" shell "$profile")"
    tls_source="$test_root/tests/fixtures/not-yet-supported-tls.c"
    "$clang" --target="$SELA_TARGET_TRIPLE" \
        "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}" \
        -std=c11 -fPIC -c "$tls_source" -o "$test_work/tls-$profile.o"
    readelf -s "$test_work/tls-$profile.o" | rg -q 'TLS.*sela_tls_probe'
    if SELA_ARCHS="$profile" "$clang" --config="$config" -std=c11 "$tls_source" \
        -o "$test_work/tls-$profile.sela" > "$test_work/tls-$profile.log" 2>&1; then
        printf 'ERROR: unimplemented TLS storage was silently published\n' >&2; exit 1
    fi
    rg -q 'unsupported single-target global storage/linkage: sela_tls_probe' "$test_work/tls-$profile.log"
    test ! -e "$test_work/tls-$profile.sela"
done
"$clang" --config="$config" "$test_root/tests/fixtures/inline-asm-barrier.c" -o "$test_work/barrier.sela"
"$selac" "$test_work/barrier.sela" -o "$test_work/barrier"
env -u LD_LIBRARY_PATH "$test_work/barrier"
if "$selac" "$test_work/hello.sela" --sdk "$test_work/missing-sdk" -o "$test_work/invalid"; then
    printf 'ERROR: accepted missing SDK\n' >&2; exit 1
fi
mkdir "$test_work/wrong-sdk"
printf '%064d\n' 0 > "$test_work/wrong-sdk/sdk-lock.sha256"
if "$selac" "$test_work/hello.sela" --sdk "$test_work/wrong-sdk" -o "$test_work/invalid"; then
    printf 'ERROR: accepted mismatched SDK\n' >&2; exit 1
fi
test ! -e "$test_work/invalid"
test "$(tar -tf "$test_work/hello.sela" | wc -l)" -eq 3
if tar -xOf "$test_work/hello.sela" | strings | rg 'hello\.c|main\.c|DICompileUnit|DILocalVariable|sela-private-|sela-hello-'; then
    printf 'ERROR: publisher evidence leaked\n' >&2; exit 1
fi
mv "$test_work/hello.sela" "$test_work/hello.sela.offline"
test "$(env -u LD_LIBRARY_PATH "$test_work/hello")" = 'Hello world'
printf 'Stock-Clang multi-file Sela pipeline passed. Workspace: %s\n' "$test_work"
