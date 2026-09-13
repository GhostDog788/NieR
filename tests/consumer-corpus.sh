#!/bin/sh
# Original upstream destination recipes inside the real i686 guest.
set -eu
test "$#" -eq 4
bundle=$1
fixtures=$2
output=$3
target=$4
test "$target" = i686
test ! -e "$output"
mkdir -p "$output"
compiler="$bundle/bin/selac"
test "$("$compiler" --print-target)" = i686
"$compiler" --check-sdk
(
    cd "$bundle"
    sha256sum -c payload.sha256
)
runtime="$bundle/sdk/sysroots/i686-linux-gnu/usr/lib/i386-linux-gnu"
loader="$runtime/ld-linux.so.2"
test_tools=/opt/test-tools
archive_tool() {
    env -i PATH=/bin:/usr/bin LC_ALL=C \
        LD_LIBRARY_PATH="$bundle/sdk/host/usr/lib/llvm-18/lib" \
        "$bundle/sdk/host/usr/lib/llvm-18/bin/llvm-ar" "$@"
}
test_tool() {
    program=$1
    shift
    env -i PATH=/bin:/usr/bin LC_ALL=C "$loader" \
        --library-path "$test_tools/lib:$runtime" "$test_tools/bin/$program" "$@"
}
test_tool ctest --version
test_tool make --version
test_tool readelf --version
record_output() {
    test "$(od -An -tu1 -j4 -N1 "$1" | tr -d ' \n')" = 1
    test "$(od -An -tu2 -j18 -N2 "$1" | tr -d ' \n')" = 3
    printf 'SELA_CORPUS_NATIVE_OUTPUT_SHA256 '
    sha256sum "$1"
}
(
    cd "$fixtures"
    sha256sum -c fixtures.sha256
)
(
    cd "$test_tools"
    sha256sum -c payload.sha256
)
count=0
while IFS= read -r project; do
    pack="$fixtures/corpus/$project"
    destination="$output/$project"
    mkdir -p "$destination"
    case "$project" in
        cjson-static|cjson-shared)
            mkdir "$destination/tests" "$destination/fuzzing"
            if [ "$project" = cjson-shared ]; then
                "$compiler" "$pack/artifacts/library.sela" -o "$destination/libcjson.so.1"
                record_output "$destination/libcjson.so.1"
                test_tool readelf -d "$destination/libcjson.so.1" > "$destination/dynamic.txt"
                grep 'SONAME.*libcjson.so.1' "$destination/dynamic.txt"
            else
                "$compiler" "$pack/artifacts/library.sela" -o "$destination/libcjson.a"
                sha256sum "$destination/libcjson.a"
                archive_tool t "$destination/libcjson.a" > "$destination/archive-members.txt"
                cmp "$pack/archive-members.txt" "$destination/archive-members.txt"
            fi
            programs=0
            while IFS= read -r program; do
                label=$(printf '%s' "$program" | tr / -)
                "$compiler" "$pack/artifacts/$label.sela" --library-dir "$destination" -o "$destination/$program"
                record_output "$destination/$program"
                programs=$((programs + 1))
            done < "$pack/programs.list"
            test "$programs" -eq 20
            for directory in . tests fuzzing; do
                sed "s|@SELA_CORPUS_DESTINATION@|$destination|g" \
                    "$pack/data/$directory/CTestTestfile.cmake" > "$destination/$directory/CTestTestfile.cmake"
            done
            cp -R "$pack/data/tests/inputs" "$destination/tests/inputs"
            test_tool ctest --test-dir "$destination" --show-only > "$destination/inventory.log"
            grep '^Total Tests: 19$' "$destination/inventory.log"
            test_tool ctest --test-dir "$destination" --timeout 180 --output-on-failure
            env -i PATH=/bin:/usr/bin LC_ALL=C LD_DEBUG=libs "$destination/cJSON_test" \
                > "$destination/demo.stdout" 2> "$destination/loader.log"
            cmp "$pack/reference.stdout" "$destination/demo.stdout"
            if [ "$project" = cjson-shared ]; then
                grep -F "calling init: $destination/libcjson.so.1" "$destination/loader.log"
                cp "$pack/native-caller" "$destination/native-caller"
                env -i PATH=/bin:/usr/bin LC_ALL=C LD_DEBUG=libs "$destination/native-caller" \
                    > "$destination/native-caller.stdout" 2> "$destination/native-caller-loader.log"
                cmp "$pack/reference.stdout" "$destination/native-caller.stdout"
                grep -F "calling init: $destination/libcjson.so.1" "$destination/native-caller-loader.log"
            fi
            count=$((count + 21))
            ;;
        zlib)
            "$compiler" "$pack/artifacts/static-library.sela" -o "$destination/libz.a"
            sha256sum "$destination/libz.a"
            archive_tool t "$destination/libz.a" > "$destination/archive-members.txt"
            cmp "$pack/archive-members.txt" "$destination/archive-members.txt"
            "$compiler" "$pack/artifacts/library.sela" -o "$destination/libz.so.1"
            record_output "$destination/libz.so.1"
            test_tool readelf -d "$destination/libz.so.1" > "$destination/dynamic.txt"
            grep 'SONAME.*libz.so.1' "$destination/dynamic.txt"
            test_tool readelf --version-info "$destination/libz.so.1" > "$destination/versions.txt"
            grep ZLIB_ "$destination/versions.txt"
            for program in example minigzip examplesh minigzipsh example64 minigzip64; do
                "$compiler" "$pack/artifacts/$program.sela" --library-dir "$destination" -o "$destination/$program"
                record_output "$destination/$program"
            done
            cp "$pack/data/Makefile" "$destination/Makefile"
            test_tool make -C "$destination" -o all -o static -o shared -o all64 \
                CC=/bin/false AR=/bin/false RANLIB=/bin/false LD=/bin/false QEMU_RUN= test test64
            env -i PATH=/bin:/usr/bin LC_ALL=C LD_DEBUG=libs "$destination/examplesh" \
                > "$destination/shared.stdout" 2> "$destination/loader.log"
            grep -F "calling init: $destination/libz.so.1" "$destination/loader.log"
            count=$((count + 8))
            ;;
        *) printf 'Unexpected corpus selection: %s\n' "$project" >&2; exit 1 ;;
    esac
    printf 'SELA_CORPUS_PROJECT_PASS %s\n' "$project"
done < "$fixtures/corpus.list"
expected=$(cat "$fixtures/corpus-count")
test "$count" -eq "$expected"
(
    cd "$bundle"
    sha256sum -c payload.sha256
)
(
    cd "$fixtures"
    sha256sum -c fixtures.sha256
)
printf 'SELA_CONSUMER_CORPUS_PASS target=i686 artifacts=%s\n' "$count"
