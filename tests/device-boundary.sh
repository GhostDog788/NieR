#!/usr/bin/env bash
set -euo pipefail
aot_bin=$1
export AOT_SDK_ROOT=$2
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/aot-device-test-XXXXXX")
"$aot_bin" publish --recipe "$test_root/examples/hello/hello.build.json" -o "$test_work/hello.aotpkg"
# Audited workspace separation, not a security sandbox. The input files still
# exist on this development machine; compilation must not open them.
strace -f -e trace=openat,execve -o "$test_work/compile.trace" \
    "$aot_bin" compile "$test_work/hello.aotpkg" --output-dir "$test_work/native"
if rg 'hello\.c|hello\.build\.json|aot-capture\.so|/clang("| )|usr/include/' "$test_work/compile.trace"; then
    printf 'ERROR: device compiler accessed publisher/frontend inputs\n' >&2; exit 1
fi
rg -q 'bin/opt"' "$test_work/compile.trace"
rg -q 'bin/llc"' "$test_work/compile.trace"
rg -q 'bin/ld.lld"' "$test_work/compile.trace"
strace -f -e trace=openat,execve -o "$test_work/run.trace" \
    env -u LD_LIBRARY_PATH "$test_work/native/bin/hello"
if rg 'aotpkg|aot-capture|bin/clang|bin/opt|bin/llc|bin/ld.lld' "$test_work/run.trace"; then
    printf 'ERROR: executable needed publication/compiler inputs at runtime\n' >&2; exit 1
fi
rg -F "$AOT_SDK_ROOT/sysroots/x86_64-linux-gnu/lib/x86_64-linux-gnu/libc.so.6" "$test_work/run.trace"
printf 'Device compiler and native execution input-access checks passed.\n'
