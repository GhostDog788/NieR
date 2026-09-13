#!/bin/sh
# Shared POSIX test body: real i386 guest and ordinary x86-64 host.
# Only fixtures, native reference binaries and a compiler bundle are inputs.
set -eu
if [ "$#" -ne 4 ]; then
    printf 'Usage: sh tests/consumer-fixtures.sh BUNDLE FIXTURES OUTPUT TARGET\n' >&2
    exit 2
fi
bundle=$(realpath "$1")
fixtures=$(realpath "$2")
output=$3
target=$4
case "$target" in
    i686) elf_class=1; elf_machine=3; foreign=x86_64 ;;
    x86_64) elf_class=2; elf_machine=62; foreign=i686 ;;
    *) exit 2 ;;
esac
test ! -e "$output"
mkdir -p "$output"
output=$(realpath "$output")
compiler="$bundle/bin/selac"
tools="$bundle/sdk/host/usr/lib/llvm-18/bin"
archive_tool() {
    # Match selac's child-only tool environment. LLVM tool RUNPATH need not
    # provide transitive dependency lookup for direct test-harness invocation.
    env -u LD_PRELOAD LD_LIBRARY_PATH="$bundle/sdk/host/usr/lib/llvm-18/lib" \
        "$tools/llvm-ar" "$@"
}
test "$("$compiler" --print-target)" = "$target"
"$compiler" --check-sdk
check_elf() {
    test "$(od -An -tu1 -j4 -N1 "$1" | tr -d ' \n')" = "$elf_class"
    test "$(od -An -tu2 -j18 -N2 "$1" | tr -d ' \n')" = "$elf_machine"
}
check_elf "$compiler"
for tool in opt llc ld.lld llvm-ar; do check_elf "$tools/$tool"; done
(
    cd "$bundle"
    sha256sum -c payload.sha256
)
(
    cd "$fixtures"
    sha256sum -c fixtures.sha256
)
"$compiler" inspect "$fixtures/hello.sela"
"$compiler" "$fixtures/hello.sela" -o "$output/hello"
"$compiler" "$fixtures/shared.sela" -o "$output/libdevice.so"
"$compiler" "$fixtures/static.sela" -o "$output/libdevice.a"
"$compiler" "$fixtures/shared-main.sela" --library-dir "$output" -o "$output/shared"
"$compiler" "$fixtures/static-main.sela" --library-dir "$output" -o "$output/static"
for executable in hello shared static; do
    check_elf "$output/$executable"
    env -i PATH=/usr/bin:/bin LC_ALL=C "$fixtures/reference/$target/$executable" > "$output/$executable.reference.txt"
    env -i PATH=/usr/bin:/bin LC_ALL=C "$output/$executable" > "$output/$executable.actual.txt"
    cmp "$output/$executable.reference.txt" "$output/$executable.actual.txt"
done
check_elf "$output/libdevice.so"
test "$(archive_tool t "$output/libdevice.a")" = "$(printf 'part.o\npart.o')"
# This stock-native caller now uses the independently generated Sela DSO.
env -i PATH=/usr/bin:/bin LC_ALL=C LD_LIBRARY_PATH="$output" \
    "$fixtures/reference/$target/shared" > "$output/native-caller.txt"
cmp "$output/shared.reference.txt" "$output/native-caller.txt"
env -i PATH=/usr/bin:/bin LC_ALL=C LD_DEBUG=libs \
    "$output/shared" > "$output/loader.stdout" 2> "$output/loader.log"
grep -F "calling init: $output/libdevice.so" "$output/loader.log"
cmp "$output/shared.reference.txt" "$output/loader.stdout"

while read -r artifact library; do
    test -n "$artifact" && test -n "$library"
    "$compiler" "$fixtures/$artifact" -o "$output/$library"
    check_elf "$output/$library"
done < "$fixtures/shared-libraries.list"
matrix_count=0
while IFS= read -r executable; do
    test -n "$executable"
    "$compiler" "$fixtures/$executable.sela" --library-dir "$output" -o "$output/$executable"
    check_elf "$output/$executable"
    env -i PATH=/usr/bin:/bin LC_ALL=C "$fixtures/reference/$target/$executable" > "$output/$executable.reference.txt"
    env -i PATH=/usr/bin:/bin LC_ALL=C "$output/$executable" > "$output/$executable.actual.txt"
    cmp "$output/$executable.reference.txt" "$output/$executable.actual.txt"
    case "$executable" in
        aggregate-dso-*)
            env -i PATH=/usr/bin:/bin LC_ALL=C LD_LIBRARY_PATH="$output" \
                "$fixtures/reference/$target/$executable" > "$output/$executable.native-caller.txt"
            cmp "$output/$executable.reference.txt" "$output/$executable.native-caller.txt"
            ;;
    esac
    matrix_count=$((matrix_count + 1))
done < "$fixtures/executables.list"
test "$matrix_count" -eq 24

cp "$output/hello" "$output/protected"
protected_before=$(sha256sum "$output/protected")
identity_before=$(stat -c '%i:%s:%Y:%a' "$output/protected")
reject() {
    name=$1
    shift
    if "$compiler" "$@" > "$output/$name.log" 2>&1; then
        printf 'Unexpected acceptance: %s\n' "$name" >&2
        exit 1
    fi
    test "$protected_before" = "$(sha256sum "$output/protected")"
    test "$identity_before" = "$(stat -c '%i:%s:%Y:%a' "$output/protected")"
}
reject malformed "$fixtures/malformed.sela" -o "$output/protected"
reject relocatable "$fixtures/relocatable.sela" -o "$output/protected"
mkdir "$output/missing-library"
reject missing-library "$fixtures/shared-main.sela" --library-dir "$output/missing-library" -o "$output/protected"
reject foreign-compile "$fixtures/hello.sela" --target "$foreign" -o "$output/protected"
reject foreign-lower lower "$fixtures/hello.sela" --target "$foreign" --output-dir "$output/foreign-lower"
test ! -e "$output/foreign-lower"
reject alias "$fixtures/hello.sela" -o "$fixtures/hello.sela"
"$compiler" inspect "$fixtures/foreign-only-$foreign.sela" > "$output/foreign-inspect.log"
grep -F "$foreign: 1 native compilation units; not validated (native backend unavailable)" "$output/foreign-inspect.log"
if grep -F 'native validation passed' "$output/foreign-inspect.log"; then exit 1; fi
reject foreign-only-compile "$fixtures/foreign-only-$foreign.sela" -o "$output/protected"
reject foreign-only-lower lower "$fixtures/foreign-only-$foreign.sela" --output-dir "$output/foreign-only-lower"
test ! -e "$output/foreign-only-lower"
"$compiler" inspect "$fixtures/foreign-only-$target.sela" > "$output/native-only-inspect.log"
grep -F "$target: 1 native compilation units; native validation passed" "$output/native-only-inspect.log"
"$compiler" "$fixtures/foreign-only-$target.sela" -o "$output/native-only"
check_elf "$output/native-only"
env -i PATH=/usr/bin:/bin LC_ALL=C "$output/native-only" > "$output/native-only.txt"
cmp "$output/hello.reference.txt" "$output/native-only.txt"
for domain in word32 word64; do
    reject "malformed-$domain-inspect" inspect "$fixtures/malformed-inactive-$domain.sela"
    reject "malformed-$domain-compile" "$fixtures/malformed-inactive-$domain.sela" -o "$output/protected"
    grep -F 'invalid arithmetic flags in a public word domain' "$output/malformed-$domain-inspect.log"
    grep -F 'invalid arithmetic flags in a public word domain' "$output/malformed-$domain-compile.log"
done
printf 'SELA_CONSUMER_NATIVE_OUTPUT_HASHES target=%s\n' "$target"
sha256sum "$output/hello" "$output/shared" "$output/static" "$output/native-only" \
    "$output/libdevice.so" "$output/libdevice.a"
while read -r artifact library; do sha256sum "$output/$library"; done < "$fixtures/shared-libraries.list"
while IFS= read -r executable; do sha256sum "$output/$executable"; done < "$fixtures/executables.list"
(
    cd "$bundle"
    sha256sum -c payload.sha256
)
(
    cd "$fixtures"
    sha256sum -c fixtures.sha256
)
printf 'SELA_CONSUMER_FIXTURES_PASS target=%s executables=28 shared=3 static=1 negatives=12\n' "$target"
