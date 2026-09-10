#!/usr/bin/env bash
set -euo pipefail
aot_bin=$1
export AOT_SDK_ROOT=$2
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/aot-test-XXXXXX")
# Retain evidence on failure. Successful runs also retain their small temp directory.
printf 'Test workspace: %s\n' "$test_work"
"$aot_bin" publish --recipe "$test_root/examples/hello/hello.build.json" -o "$test_work/hello.aotpkg"
"$aot_bin" inspect "$test_work/hello.aotpkg"
"$aot_bin" compile "$test_work/hello.aotpkg" --output-dir "$test_work/native"
test "$(env -u LD_LIBRARY_PATH "$test_work/native/bin/hello")" = 'Hello world'
readelf -l "$test_work/native/bin/hello" | rg -F "$AOT_SDK_ROOT/sysroots/x86_64-linux-gnu/"
"$aot_bin" publish --recipe "$test_root/tests/fixtures/width.build.json" -o "$test_work/width.aotpkg"
"$aot_bin" inspect "$test_work/width.aotpkg"
"$aot_bin" compile "$test_work/width.aotpkg" --output-dir "$test_work/width-native"
test "$(env -u LD_LIBRARY_PATH "$test_work/width-native/bin/width")" = 'pointer=8 word=8 fixed=4,8'
"$aot_bin" lower "$test_work/width.aotpkg" --profile i686 --output-dir "$test_work/width-i686"
"$AOT_SDK_ROOT/host/usr/lib/llvm-18/bin/opt" -passes=verify -disable-output "$test_work/width-i686/0.ll"
if "$aot_bin" compile "$test_work/hello.aotpkg" --output-dir "$test_work/native"; then
    printf 'ERROR: overwrote existing native output\n' >&2; exit 1
fi
if "$aot_bin" publish --recipe "$test_root/tests/fixtures/unsupported.build.json" -o "$test_work/unsupported.aotpkg"; then
    printf 'ERROR: accepted unqualified control flow\n' >&2; exit 1
fi
test ! -e "$test_work/unsupported.aotpkg"
if "$aot_bin" compile "$test_work/hello.aotpkg" --sdk "$test_work/missing-sdk" --output-dir "$test_work/should-not-exist"; then
    printf 'ERROR: accepted missing SDK\n' >&2; exit 1
fi
mkdir "$test_work/wrong-sdk"
printf '%064d\n' 0 > "$test_work/wrong-sdk/sdk-lock.sha256"
if "$aot_bin" compile "$test_work/hello.aotpkg" --sdk "$test_work/wrong-sdk" --output-dir "$test_work/should-not-exist"; then
    printf 'ERROR: accepted mismatched SDK contract\n' >&2; exit 1
fi
test ! -e "$test_work/should-not-exist"
# Source/captures are not payloads. Only a JSON manifest and custom MLIR bytecode.
tar -tf "$test_work/hello.aotpkg" | sort
if tar -xOf "$test_work/hello.aotpkg" | strings | rg 'hello\.c|DICompileUnit|DILocalVariable|aot-test-|aot-private-'; then
    printf 'ERROR: private publisher provenance leaked\n' >&2; exit 1
fi
mv "$test_work/hello.aotpkg" "$test_work/hello.aotpkg.offline"
test "$(env -u LD_LIBRARY_PATH "$test_work/native/bin/hello")" = 'Hello world'
printf 'Hello/shared-native-width pipeline passed. Broad C is not yet qualified.\n'
