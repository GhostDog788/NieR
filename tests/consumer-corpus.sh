#!/bin/sh
# Original upstream destination recipes inside a real native-kernel guest.
set -eu
test "$#" -eq 4
bundle=$1
fixtures=$2
output=$3
target=$4
grep -Fx "$target" "$fixtures/targets.list" >/dev/null
. "$fixtures/target-metadata/$target.env"
test "$SELA_TARGET_ID" = "$target"
elf_class=$SELA_TARGET_ELF_CLASS
elf_machine=$SELA_TARGET_ELF_MACHINE
triple=$SELA_TARGET_SYSROOT_TRIPLE
multiarch=$SELA_TARGET_MULTIARCH
loader_name=$SELA_TARGET_LOADER
test ! -e "$output"
mkdir -p "$output"
compiler="$bundle/bin/selac"
test "$("$compiler" --print-target)" = "$target"
"$compiler" --check-sdk
(
    cd "$bundle"
    sha256sum -c payload.sha256
)
runtime="$bundle/sdk/sysroots/$triple/usr/lib/$multiarch"
loader="$runtime/$loader_name"
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
    test "$(od -An -tu1 -j4 -N1 "$1" | tr -d ' \n')" = "$elf_class"
    test "$(od -An -tu2 -j18 -N2 "$1" | tr -d ' \n')" = "$elf_machine"
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
    native="$pack/reference/$target"
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
                cmp "$native/archive-members.txt" "$destination/archive-members.txt"
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
                    "$native/data/$directory/CTestTestfile.cmake" > "$destination/$directory/CTestTestfile.cmake"
            done
            cp -R "$native/data/tests/inputs" "$destination/tests/inputs"
            test_tool ctest --test-dir "$destination" --show-only > "$destination/inventory.log"
            grep '^Total Tests: 19$' "$destination/inventory.log"
            test_tool ctest --test-dir "$destination" --timeout 180 --output-on-failure
            env -i PATH=/bin:/usr/bin LC_ALL=C LD_DEBUG=libs "$destination/cJSON_test" \
                > "$destination/demo.stdout" 2> "$destination/loader.log"
            cmp "$native/reference.stdout" "$destination/demo.stdout"
            if [ "$project" = cjson-shared ]; then
                grep -F "calling init: $destination/libcjson.so.1" "$destination/loader.log"
                cp "$native/native-caller" "$destination/native-caller"
                env -i PATH=/bin:/usr/bin LC_ALL=C LD_DEBUG=libs "$destination/native-caller" \
                    > "$destination/native-caller.stdout" 2> "$destination/native-caller-loader.log"
                cmp "$native/reference.stdout" "$destination/native-caller.stdout"
                grep -F "calling init: $destination/libcjson.so.1" "$destination/native-caller-loader.log"
            fi
            count=$((count + 21))
            ;;
        zlib)
            "$compiler" "$pack/artifacts/static-library.sela" -o "$destination/libz.a"
            sha256sum "$destination/libz.a"
            archive_tool t "$destination/libz.a" > "$destination/archive-members.txt"
            cmp "$native/archive-members.txt" "$destination/archive-members.txt"
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
            cp "$native/data/Makefile" "$destination/Makefile"
            test_tool make -C "$destination" -o all -o static -o shared -o all64 \
                CC=/bin/false AR=/bin/false RANLIB=/bin/false LD=/bin/false QEMU_RUN= test test64
            env -i PATH=/bin:/usr/bin LC_ALL=C LD_DEBUG=libs "$destination/examplesh" \
                > "$destination/shared.stdout" 2> "$destination/loader.log"
            grep -F "calling init: $destination/libz.so.1" "$destination/loader.log"
            count=$((count + 8))
            ;;
        xxhash)
            "$compiler" "$pack/artifacts/static-library.sela" -o "$destination/libxxhash.a"
            archive_tool t "$destination/libxxhash.a" > "$destination/archive-members.txt"
            cmp "$native/archive-members.txt" "$destination/archive-members.txt"
            "$compiler" "$pack/artifacts/library.sela" -o "$destination/libxxhash.so.0"
            record_output "$destination/libxxhash.so.0"
            test_tool readelf -d "$destination/libxxhash.so.0" > "$destination/dynamic.txt"
            grep 'SONAME.*libxxhash.so.0' "$destination/dynamic.txt"
            for program in xxhsum sanity static-api shared-api; do
                "$compiler" "$pack/artifacts/$program.sela" --library-dir "$destination" -o "$destination/$program"
                record_output "$destination/$program"
            done
            # --version also executes upstream XSUM_sanityCheck(), including dispatch.
            env -i PATH=/bin:/usr/bin LC_ALL=C "$loader" --library-path "$native:$runtime" \
                "$native/xxhsum" --version > "$destination/native-version.txt" 2>&1
            env -i PATH=/bin:/usr/bin LC_ALL=C "$destination/xxhsum" --version > "$destination/version.txt" 2>&1
            cmp "$destination/native-version.txt" "$destination/version.txt"
            for variant in 0 1 2 3; do
                env -i PATH=/bin:/usr/bin LC_ALL=C "$loader" --library-path "$native:$runtime" \
                    "$native/xxhsum" "-H$variant" "$pack/input.dat" > "$destination/native-H$variant.txt"
                env -i PATH=/bin:/usr/bin LC_ALL=C "$destination/xxhsum" "-H$variant" \
                    "$pack/input.dat" > "$destination/H$variant.txt"
                cmp "$destination/native-H$variant.txt" "$destination/H$variant.txt"
            done
            env -i PATH=/bin:/usr/bin LC_ALL=C "$loader" --library-path "$native:$runtime" \
                "$native/sanity" > "$destination/native-sanity.txt" 2>&1
            env -i PATH=/bin:/usr/bin LC_ALL=C "$destination/sanity" > "$destination/sanity.txt" 2>&1
            cmp "$destination/native-sanity.txt" "$destination/sanity.txt"
            env -i PATH=/bin:/usr/bin LC_ALL=C "$loader" --library-path "$native:$runtime" \
                "$native/native-api" > "$destination/native-api.txt"
            for program in static-api shared-api; do
                env -i PATH=/bin:/usr/bin LC_ALL=C "$destination/$program" > "$destination/$program.txt"
                cmp "$destination/native-api.txt" "$destination/$program.txt"
            done
            # Unmodified native caller ABI against the device-produced DSO.
            env -i PATH=/bin:/usr/bin LC_ALL=C LD_DEBUG=libs "$loader" \
                --library-path "$destination:$runtime" "$native/native-api" \
                > "$destination/native-caller.txt" 2> "$destination/loader.log"
            cmp "$destination/native-api.txt" "$destination/native-caller.txt"
            grep -F "calling init: $destination/libxxhash.so.0" "$destination/loader.log"
            count=$((count + 6))
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
printf 'SELA_CONSUMER_CORPUS_PASS target=%s artifacts=%s\n' "$target" "$count"
