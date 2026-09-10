#!/usr/bin/env bash
set -euo pipefail
nierc=$1
export NIER_SDK_ROOT=$2
config=$3
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-device-XXXXXX")
"$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" --config="$config" -O2 "$test_root/examples/hello/main.c" "$test_root/examples/hello/hello.c" -o "$test_work/hello.nier"
strace -f -e trace=openat,execve -o "$test_work/compile.trace" "$nierc" "$test_work/hello.nier" -o "$test_work/hello"
if rg 'hello\.c|main\.c|nier-capture\.so|libnier-clang|/clang("| )|nier-build|nier-ld"|usr/include/' "$test_work/compile.trace"; then
    printf 'ERROR: consumer accessed publisher/frontend inputs\n' >&2; exit 1
fi
rg -q 'bin/opt"' "$test_work/compile.trace"
rg -q 'bin/llc"' "$test_work/compile.trace"
rg -q 'bin/ld.lld"' "$test_work/compile.trace"
strace -f -e trace=openat,execve -o "$test_work/run.trace" env -u LD_LIBRARY_PATH "$test_work/hello"
if rg '\.nier"|nier-capture|bin/clang|bin/opt|bin/llc|bin/ld.lld' "$test_work/run.trace"; then
    printf 'ERROR: native executable needed artifact/compiler inputs\n' >&2; exit 1
fi
rg -F "$NIER_SDK_ROOT/sysroots/x86_64-linux-gnu/lib/x86_64-linux-gnu/libc.so.6" "$test_work/run.trace"
printf 'Independent Nier compiler and native execution access checks passed.\n'
