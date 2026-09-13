#!/usr/bin/env bash
# Exhaustive pinned corpus gate. A compiler rejection is a failure, not a skip.
set -euo pipefail
if test "$#" -lt 3; then
  echo "usage: bash corpus/qualify.sh /path/sela-build /path/selac /path/sdk [--project cjson|zlib] [--i686-bundle DIR]" >&2
  exit 2
fi
build_tool=$(realpath -- "$1")
selac=$(realpath -- "$2")
export SELA_SDK_ROOT=$(realpath -- "$3")
shift 3
selected_project=all
i686_bundle=
while test "$#" -gt 0; do
  case "$1" in
    --project)
      test "$#" -ge 2 && test "$selected_project" = all
      case "$2" in cjson|zlib) selected_project=$2 ;; *) echo 'Select the complete cjson or zlib project.' >&2; exit 2 ;; esac
      shift 2 ;;
    --i686-bundle)
      test "$#" -ge 2 && test -z "$i686_bundle"
      i686_bundle=$(realpath -e -- "$2")
      test -x "$i686_bundle/bin/selac"
      readelf -h "$i686_bundle/bin/selac" | grep 'Class:.*ELF32' >/dev/null
      readelf -h "$i686_bundle/bin/selac" | grep 'Machine:.*Intel 80386' >/dev/null
      test -f "$i686_bundle/payload.sha256"
      shift 2 ;;
    *) printf 'Unknown qualification option: %s\n' "$1" >&2; exit 2 ;;
  esac
done
corpus_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
corpus_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-corpus-XXXXXX")
tool_directory=$(dirname -- "$build_tool")
clang_configuration="$tool_directory/sela.cfg"
qualification_report="$corpus_work/qualification.txt"
i686_fixtures="$corpus_work/i686-consumer-fixtures"
if test -n "$i686_bundle"; then mkdir -p "$i686_fixtures/corpus"; fi
echo "Corpus evidence: $corpus_work"
finish_report() {
  local status=$?
  trap - EXIT
  if test "$status" -eq 0; then
    printf 'Result: PASS (%s)\n' "$selected_project" | tee -a "$qualification_report"
  else
    printf 'Result: FAIL (%s), exit status %s\n' "$selected_project" "$status" \
      | tee -a "$qualification_report"
    echo "Corpus qualification FAILED; evidence retained at $corpus_work" >&2
  fi
  printf 'Finished UTC: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" | tee -a "$qualification_report"
  exit "$status"
}
trap finish_report EXIT
{
  printf 'Sela configured upstream corpus qualification\nStarted UTC: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'Selected projects: %s\nNative profiles: x86_64, i686\nDestination: x86_64\n' "$selected_project"
  if test -n "$i686_bundle"; then printf 'Additional required destination: i686 under a real 32-bit kernel\n'; fi
  printf 'Replay:'
  printf ' %q' bash "$corpus_root/qualify.sh" "$build_tool" "$selac" "$SELA_SDK_ROOT"
  if test "$selected_project" != all; then printf ' --project %q' "$selected_project"; fi
  if test -n "$i686_bundle"; then printf ' --i686-bundle %q' "$i686_bundle"; fi
  printf '\nActual tool/configuration/recipe SHA-256:\n'
  sha256sum "$build_tool" "$selac" "$tool_directory/sela-native-ld" "$tool_directory/sela-ld" \
    "$tool_directory/libsela-clang.so" "$tool_directory/sela-capture.so" "$clang_configuration" \
    "$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" "$SELA_SDK_ROOT/sdk-lock.sha256" \
    "$corpus_root/../sdk/packages.lock" "$corpus_root/releases.lock" \
    "$corpus_root/cjson-static.cmake" "$corpus_root/cjson-shared.cmake" "$corpus_root/qualify.sh"
  if test -n "$i686_bundle"; then
    sha256sum "$i686_bundle/bin/selac" "$i686_bundle/payload.sha256" \
      "$corpus_root/../tests/consumer-vm.sh" "$corpus_root/../tests/consumer-corpus.sh" \
      "$corpus_root/../tests/vm/packages.lock" "$corpus_root/../tests/vm/test-tools.lock"
  fi
  printf 'Native configure/build/test commands and immutable input journals are retained per publication.\n'
  printf 'This report is functional regression evidence, not a security attestation or publication recipe.\n'
} | tee "$qualification_report"
export LD_LIBRARY_PATH="$SELA_SDK_ROOT/host/usr/lib/llvm-18/lib:$SELA_SDK_ROOT/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ctest="$SELA_SDK_ROOT/host/usr/bin/ctest"
test_loader="$SELA_SDK_ROOT/sysroots/x86_64-linux-gnu/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
test_tool_libraries="$SELA_SDK_ROOT/host/usr/lib/llvm-18/lib:$SELA_SDK_ROOT/host/usr/lib/x86_64-linux-gnu:$SELA_SDK_ROOT/sysroots/x86_64-linux-gnu/usr/lib/x86_64-linux-gnu"
# The extracted CTest tool needs SDK-host libraries. Supply those to its loader
# alone, not through environment inherited by destination application processes.
destination_ctest() {
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$test_loader" --library-path "$test_tool_libraries" "$ctest" "$@"
}
destination_selac() {
  # Never let the publisher's SDK receipt or loader overrides replace a thin
  # compiler's own sibling SDK. A normal development compiler retains its
  # configured default SDK when those environment overrides are absent.
  env -u SELA_SDK_ROOT -u LD_LIBRARY_PATH -u LD_PRELOAD "$selac" "$@"
}
test "$(destination_selac --print-target)" = x86_64
unset CFLAGS CPPFLAGS LDFLAGS LIBS
mkdir "$corpus_work/sources"
while read -r project version url expected; do
  case "$project" in ''|'#'*) continue ;; esac
  if test "$selected_project" = cjson && test "$project" != cJSON; then continue; fi
  if test "$selected_project" = zlib && test "$project" != zlib; then continue; fi
  archive="$corpus_work/sources/$project-$version.tar.gz"
  curl --fail --location --retry 2 --max-time 120 "$url" -o "$archive"
  actual=$(sha256sum "$archive")
  test "${actual%% *}" = "$expected"
  tar -xzf "$archive" -C "$corpus_work/sources"
done < "$corpus_root/releases.lock"

# The helper runs the unchanged default native build and its original tests in
# both private profiles before it emits the selected output through stock Clang.
# Keep the journals and pristine captures even on the first failing IR contract.
publish() {
  local label=$1 artifact=$2
  shift 2
  {
    printf '\nPublication log: %s\nCommand:' "$corpus_work/$label.log"
    printf ' %q' "$build_tool" "$@" --clang-config "$clang_configuration" --artifact "$artifact" --keep-private
    printf '\n'
  } | tee -a "$qualification_report"
  "$build_tool" "$@" --clang-config "$clang_configuration" --artifact "$artifact" --keep-private \
    2>&1 | tee "$corpus_work/$label.log"
  last_private=$(sed -n 's/^Private build evidence: //p' "$corpus_work/$label.log")
  test -d "$last_private/build-x86_64"
  test -d "$last_private/build-i686"
  {
    printf 'Private build evidence: %s\nPublished artifact and native configuration SHA-256:\n' "$last_private"
    sha256sum "$artifact" "$last_private/build-x86_64/native.cfg" "$last_private/build-i686/native.cfg"
  } | tee -a "$qualification_report"
}

record_destination() {
  printf 'Destination output SHA-256: ' | tee -a "$qualification_report"
  sha256sum "$1" | tee -a "$qualification_report"
}

if test "$selected_project" != zlib; then
cjson_source="$corpus_work/sources/cJSON-1.7.19"
cjson_programs=(cJSON_test tests/parse_examples tests/parse_number tests/parse_hex4
  tests/parse_string tests/parse_array tests/parse_object tests/parse_value
  tests/print_string tests/print_number tests/print_array tests/print_object
  tests/print_value tests/misc_tests tests/parse_with_opts tests/compare_tests
  tests/cjson_add tests/readme_examples tests/minify_tests fuzzing/fuzz_main)
for mode in static shared; do
  destination="$corpus_work/cjson-$mode-destination"
  artifacts="$corpus_work/cjson-$mode-artifacts"
  mkdir -p "$destination/tests" "$destination/fuzzing" "$artifacts"
  cjson_args=(--system cmake --source "$cjson_source" --configure-arg -C
    --configure-arg "$corpus_root/cjson-$mode.cmake" --target all --target check)
  if test "$mode" = shared; then
    publish "cjson-$mode-library" "$artifacts/library.sela" "${cjson_args[@]}" --output libcjson.so.1.7.19
    destination_selac "$artifacts/library.sela" -o "$destination/libcjson.so.1"
    record_destination "$destination/libcjson.so.1"
    readelf -d "$destination/libcjson.so.1" | grep -q 'SONAME.*libcjson.so.1'
  else
    publish "cjson-$mode-library" "$artifacts/library.sela" "${cjson_args[@]}" --output libcjson.a
    destination_selac "$artifacts/library.sela" -o "$destination/libcjson.a"
    record_destination "$destination/libcjson.a"
    test "$("$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar" t "$destination/libcjson.a")" = \
      "$("$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar" t "$last_private/build-x86_64/build/libcjson.a")"
  fi
  for program in "${cjson_programs[@]}"; do
    label=${program//\//-}
    publish "cjson-$mode-$label" "$artifacts/$label.sela" "${cjson_args[@]}" --output "$program"
    destination_selac "$artifacts/$label.sela" --library-dir "$destination" -o "$destination/$program"
    record_destination "$destination/$program"
    if test "$program" = cJSON_test; then
      env -u LD_LIBRARY_PATH -u LD_PRELOAD "$last_private/build-x86_64/build/cJSON_test" \
        > "$corpus_work/cjson-$mode-reference-output.log"
      env -u LD_LIBRARY_PATH -u LD_PRELOAD LD_DEBUG=libs "$destination/cJSON_test" \
        > "$corpus_work/cjson-$mode-sela-output.log" 2> "$corpus_work/cjson-$mode-loader.log"
      cmp "$corpus_work/cjson-$mode-reference-output.log" "$corpus_work/cjson-$mode-sela-output.log"
      if test "$mode" = shared; then
        grep -F "calling init: $destination/libcjson.so.1" "$corpus_work/cjson-$mode-loader.log"
        # Reuse the actual stock-Clang reference object to independently verify
        # the destination DSO's native C ABI, without publishing the caller.
        runtime="$SELA_SDK_ROOT/sysroots/x86_64-linux-gnu/usr/lib/x86_64-linux-gnu"
        "$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" --target=x86_64-unknown-linux-gnu \
          --sysroot="$SELA_SDK_ROOT/sysroots/x86_64-linux-gnu" \
          -resource-dir="$SELA_SDK_ROOT/host/usr/lib/llvm-18/lib/clang/18" \
          --rtlib=compiler-rt --unwindlib=none --ld-path="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/ld.lld" \
          "$last_private/build-x86_64/build/CMakeFiles/cJSON_test.dir/test.c.o" \
          -L"$destination" -l:libcjson.so.1 -lm -Wl,--dynamic-linker,"$runtime/ld-linux-x86-64.so.2" \
          -Wl,-rpath,"$destination:$runtime" -Wl,-z,nodefaultlib -o "$destination/native-caller"
        env -u LD_LIBRARY_PATH -u LD_PRELOAD LD_DEBUG=libs "$destination/native-caller" \
          > "$corpus_work/cjson-$mode-native-caller-output.log" \
          2> "$corpus_work/cjson-$mode-native-caller-loader.log"
        cmp "$corpus_work/cjson-$mode-reference-output.log" "$corpus_work/cjson-$mode-native-caller-output.log"
        grep -F "calling init: $destination/libcjson.so.1" "$corpus_work/cjson-$mode-native-caller-loader.log"
      fi
    fi
  done
  # Preserve all original generated CTest commands/properties. Only relocate
  # the generated absolute build-directory references into the destination.
  # This is not an upstream source edit or a replacement/filtered test suite.
  reference="$last_private/build-x86_64/build"
  reference_pattern=$(printf '%s' "$reference" | sed 's/[][\\.^$*|]/\\&/g')
  destination_replacement=$(printf '%s' "$destination" | sed 's/[\\&|]/\\&/g')
  for directory in . tests fuzzing; do
    test -f "$reference/$directory/CTestTestfile.cmake"
    sed "s|$reference_pattern|$destination_replacement|g" \
      "$reference/$directory/CTestTestfile.cmake" > "$destination/$directory/CTestTestfile.cmake"
  done
  cp -R "$reference/tests/inputs" "$destination/tests/inputs"
  destination_ctest --test-dir "$destination" --show-only | tee "$corpus_work/cjson-$mode-inventory.log"
  grep -q '^Total Tests: 19$' "$corpus_work/cjson-$mode-inventory.log"
  destination_ctest --test-dir "$destination" --output-on-failure | tee "$corpus_work/cjson-$mode-destination.log"
  if test -n "$i686_bundle"; then
    pack="$i686_fixtures/corpus/cjson-$mode"
    mkdir -p "$pack/data/tests" "$pack/data/fuzzing"
    cp -a "$artifacts" "$pack/artifacts"
    printf '%s\n' "${cjson_programs[@]}" > "$pack/programs.list"
    reference="$last_private/build-i686/build"
    reference_pattern=$(printf '%s' "$reference" | sed 's/[][\\.^$*|]/\\&/g')
    for directory in . tests fuzzing; do
      sed "s|$reference_pattern|@SELA_CORPUS_DESTINATION@|g" \
        "$reference/$directory/CTestTestfile.cmake" > "$pack/data/$directory/CTestTestfile.cmake"
    done
    cp -R "$reference/tests/inputs" "$pack/data/tests/inputs"
    env -u LD_LIBRARY_PATH -u LD_PRELOAD "$reference/cJSON_test" > "$pack/reference.stdout"
    if test "$mode" = static; then
      "$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar" t "$reference/libcjson.a" > "$pack/archive-members.txt"
    else
      guest_runtime=/opt/sela/sdk/sysroots/i686-linux-gnu/usr/lib/i386-linux-gnu
      "$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" --target=i686-unknown-linux-gnu \
        --sysroot="$SELA_SDK_ROOT/sysroots/i686-linux-gnu" \
        -resource-dir="$SELA_SDK_ROOT/host/usr/lib/llvm-18/lib/clang/18" \
        --rtlib=compiler-rt --unwindlib=none --ld-path="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/ld.lld" \
        "$reference/CMakeFiles/cJSON_test.dir/test.c.o" -L"$reference" -l:libcjson.so.1 -lm \
        -Wl,--dynamic-linker,"$guest_runtime/ld-linux.so.2" \
        "-Wl,-rpath,\$ORIGIN:$guest_runtime" -Wl,-z,nodefaultlib -o "$pack/native-caller"
    fi
    printf '%s\n' "cjson-$mode" >> "$i686_fixtures/corpus.list"
  fi
done
fi

if test "$selected_project" != cjson; then
zlib_source="$corpus_work/sources/zlib-1.3.2"
destination="$corpus_work/zlib-destination"
artifacts="$corpus_work/zlib-artifacts"
mkdir "$destination" "$artifacts"
# CFLAGS here is upstream's documented configure input. Native references and
# publication share it; do not override architecture/CRC/varargs feature macros.
export CFLAGS=-O3
zlib_args=(--system make --source "$zlib_source" --configure-arg --shared
  --target all --target test --target test64)
publish zlib-static-library "$artifacts/static-library.sela" "${zlib_args[@]}" --output libz.a
destination_selac "$artifacts/static-library.sela" -o "$destination/libz.a"
record_destination "$destination/libz.a"
test "$("$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar" t "$destination/libz.a")" = \
  "$("$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar" t "$last_private/build-x86_64/source/libz.a")"
publish zlib-library "$artifacts/library.sela" "${zlib_args[@]}" --output libz.so.1.3.2
destination_selac "$artifacts/library.sela" -o "$destination/libz.so.1"
record_destination "$destination/libz.so.1"
readelf -d "$destination/libz.so.1" | grep -q 'SONAME.*libz.so.1'
readelf --version-info "$destination/libz.so.1" | grep -q ZLIB_
for program in example minigzip examplesh minigzipsh example64 minigzip64; do
  publish "zlib-$program" "$artifacts/$program.sela" "${zlib_args[@]}" --output "$program"
  destination_selac "$artifacts/$program.sela" --library-dir "$destination" -o "$destination/$program"
  record_destination "$destination/$program"
done
cp "$last_private/build-x86_64/source/Makefile" "$destination/Makefile"
# Execute the original upstream recipes using only the destination executables.
# Their already-completed native build prerequisite targets are marked old; any
# accidental compilation/link attempt fails instead of silently rebuilding code.
env -u LD_LIBRARY_PATH -u LD_PRELOAD make -C "$destination" -o all -o static -o shared -o all64 \
  CC=/bin/false AR=/bin/false RANLIB=/bin/false LD=/bin/false QEMU_RUN= test test64 \
  | tee "$corpus_work/zlib-destination.log"
if test -n "$i686_bundle"; then
  pack="$i686_fixtures/corpus/zlib"
  mkdir -p "$pack/data"
  cp -a "$artifacts" "$pack/artifacts"
  cp "$last_private/build-i686/source/Makefile" "$pack/data/Makefile"
  "$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar" t \
    "$last_private/build-i686/source/libz.a" > "$pack/archive-members.txt"
  printf 'zlib\n' >> "$i686_fixtures/corpus.list"
fi
fi
if test -n "$i686_bundle"; then
  guest_runtime=/opt/sela/sdk/sysroots/i686-linux-gnu/usr/lib/i386-linux-gnu
  "$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" --target=i686-unknown-linux-gnu \
    --sysroot="$SELA_SDK_ROOT/sysroots/i686-linux-gnu" \
    -resource-dir="$SELA_SDK_ROOT/host/usr/lib/llvm-18/lib/clang/18" \
    --rtlib=compiler-rt --unwindlib=none --ld-path="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/ld.lld" \
    "$corpus_root/../tests/vm/exec-format.c" -Wl,--dynamic-linker,"$guest_runtime/ld-linux.so.2" \
    -Wl,-rpath,"$guest_runtime" -Wl,-z,nodefaultlib -o "$i686_fixtures/exec-format"
  cp /bin/true "$i686_fixtures/elf64-negative"
  case "$selected_project" in all) count=50 ;; cjson) count=42 ;; zlib) count=8 ;; esac
  printf '%s\n' "$count" > "$i686_fixtures/corpus-count"
  (
    cd "$i686_fixtures"
    find . -type f ! -name fixtures.sha256 -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
  ) > "$i686_fixtures/fixtures.sha256"
  printf 'Source-free i686 consumer fixture hashes:\n' | tee -a "$qualification_report"
  cat "$i686_fixtures/fixtures.sha256" | tee -a "$qualification_report"
  env -u SELA_SDK_ROOT -u LD_LIBRARY_PATH -u LD_PRELOAD \
    SELA_VM_TIMEOUT="${SELA_VM_TIMEOUT:-3600}" \
    bash "$corpus_root/../tests/consumer-vm.sh" "$i686_bundle" "$i686_fixtures" \
    2>&1 | tee "$corpus_work/i686-consumer-vm.log"
  printf 'Real-kernel i686 destination qualification: PASS (%s artifacts)\n' "$count" | tee -a "$qualification_report"
fi
if test "$selected_project" = all; then
  echo 'All pinned native references and destination Sela corpus tests passed.'
else
  echo "Pinned $selected_project native references and destination tests passed; the other project was not run."
fi
