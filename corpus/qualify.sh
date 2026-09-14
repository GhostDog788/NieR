#!/usr/bin/env bash
# Exhaustive pinned corpus: publish once, replay identical bytes on all devices.
# A compiler rejection is a failure, never a skip or an upstream-test reduction.
set -euo pipefail
if [[ $# -lt 3 ]]; then
  echo 'Usage: bash corpus/qualify.sh BUILD_TOOL SDK [--project cjson|zlib] --prepare-only|--bundle-parent DIR' >&2
  exit 2
fi
build_tool=$(realpath -e -- "$1")
export SELA_SDK_ROOT=$(realpath -e -- "$2")
shift 2
selected_project=all
prepare_only=false
bundle_parent=
while [[ $# -gt 0 ]]; do
  case "$1" in
    --project)
      [[ $# -ge 2 && $selected_project == all ]]
      case "$2" in cjson|zlib) selected_project=$2 ;; *) exit 2 ;; esac
      shift 2 ;;
    --prepare-only) [[ $prepare_only == false && -z $bundle_parent ]]; prepare_only=true; shift ;;
    --bundle-parent) [[ $# -ge 2 && $prepare_only == false && -z $bundle_parent ]]; bundle_parent=$(realpath -e -- "$2"); shift 2 ;;
    *) printf 'Unknown qualification option: %s\n' "$1" >&2; exit 2 ;;
  esac
done
[[ $prepare_only == true || -n $bundle_parent ]]
corpus_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repository=$(realpath "$corpus_root/..")
corpus_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-corpus-XXXXXX")
tool_directory=$(dirname -- "$build_tool")
clang_configuration="$tool_directory/sela.cfg"
qualification_report="$corpus_work/qualification.txt"
fixtures="$corpus_work/consumer-fixtures"
mkdir -p "$fixtures/corpus" "$corpus_work/sources"
mapfile -t targets < <(python3 "$repository/sdk/targets.py" list)
python3 "$repository/tests/vm/stage-target-metadata.py" "$fixtures"
echo "Corpus evidence: $corpus_work"
finish_report() {
  local status=$?
  trap - EXIT
  if [[ $status -eq 0 && $prepare_only == true ]]; then
    printf 'Result: PREPARED (%s); destination acceptance not run\n' "$selected_project" | tee -a "$qualification_report"
  elif [[ $status -eq 0 ]]; then
    printf 'Result: PASS (%s)\n' "$selected_project" | tee -a "$qualification_report"
  else
    printf 'Result: FAIL (%s), exit status %s\n' "$selected_project" "$status" | tee -a "$qualification_report"
    echo "Corpus qualification FAILED; evidence retained at $corpus_work" >&2
  fi
  printf 'Finished UTC: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" | tee -a "$qualification_report"
  exit "$status"
}
trap finish_report EXIT
{
  printf 'Sela configured upstream corpus qualification\nStarted UTC: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'Selected projects: %s\nNative profiles:' "$selected_project"
  printf ' %s' "${targets[@]}"
  printf '\nReplay:'
  printf ' %q' bash "$corpus_root/qualify.sh" "$build_tool" "$SELA_SDK_ROOT"
  if [[ $selected_project != all ]]; then printf ' --project %q' "$selected_project"; fi
  if [[ $prepare_only == true ]]; then printf ' --prepare-only'; else printf ' --bundle-parent %q' "$bundle_parent"; fi
  printf '\nActual tool/configuration/recipe SHA-256:\n'
  sha256sum "$build_tool" "$tool_directory/sela-native-ld" "$tool_directory/sela-ld" \
    "$tool_directory/libsela-clang.so" "$tool_directory/sela-capture.so" "$clang_configuration" \
    "$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" "$SELA_SDK_ROOT/sdk-lock.sha256" \
    "$repository/sdk/packages.lock" "$repository/sdk/targets.json" "$corpus_root/releases.lock" \
    "$corpus_root/cjson-static.cmake" "$corpus_root/cjson-shared.cmake" "$corpus_root/qualify.sh" \
    "$repository/tests/consumer-vm.sh" "$repository/tests/consumer-corpus.sh" \
    "$repository/tests/vm/packages.lock" "$repository/tests/vm/test-tools.lock"
  printf 'Native configure/build/test commands and immutable input journals are retained per publication.\n'
  printf 'This report is functional evidence, not a security attestation or performance threshold.\n'
} | tee "$qualification_report"
source "$repository/tests/publication-inputs.sh"
publication_receipt="$corpus_work/publication-inputs.sha256"
sela_record_publication_inputs "$publication_receipt" "$clang_configuration" "$SELA_SDK_ROOT" \
  "$repository/sdk/targets.json" "$repository/sdk/targets.py" \
  "$corpus_root/cjson-static.cmake" "$corpus_root/cjson-shared.cmake"
printf 'Publisher identity guard receipt: %s\n' "$publication_receipt" | tee -a "$qualification_report"
cat "$publication_receipt" | tee -a "$qualification_report"
source "$repository/sdk/env.sh"
clang="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
llvm_ar="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar"
unset CFLAGS CPPFLAGS LDFLAGS LIBS
while read -r project version url expected; do
  case "$project" in ''|'#'*) continue ;; esac
  if [[ $selected_project == cjson && $project != cJSON ]]; then continue; fi
  if [[ $selected_project == zlib && $project != zlib ]]; then continue; fi
  archive="$corpus_work/sources/$project-$version.tar.gz"
  curl --fail --location --retry 2 --max-time 120 "$url" -o "$archive"
  printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
  tar -xzf "$archive" -C "$corpus_work/sources"
done < "$corpus_root/releases.lock"

publish() {
  local label=$1 artifact=$2
  shift 2
  sela_verify_publication_inputs "$publication_receipt"
  {
    printf '\nPublication log: %s\nCommand:' "$corpus_work/$label.log"
    printf ' %q' "$build_tool" "$@" --clang-config "$clang_configuration" --artifact "$artifact" --keep-private
    printf '\n'
  } | tee -a "$qualification_report"
  "$build_tool" "$@" --clang-config "$clang_configuration" --artifact "$artifact" --keep-private \
    2>&1 | tee "$corpus_work/$label.log"
  sela_verify_publication_inputs "$publication_receipt"
  last_private=$(sed -n 's/^Private build evidence: //p' "$corpus_work/$label.log")
  {
    printf 'Private build evidence: %s\nPublished artifact and native configuration SHA-256:\n' "$last_private"
    sha256sum "$artifact"
    for target in "${targets[@]}"; do
      test -d "$last_private/build-$target"
      sha256sum "$last_private/build-$target/native.cfg"
    done
  } | tee -a "$qualification_report"
}

native_command() {
  eval "$(python3 "$repository/sdk/targets.py" shell "$1")"
  guest_runtime="/opt/sela/sdk/sysroots/$SELA_TARGET_SYSROOT_TRIPLE/usr/lib/$SELA_TARGET_MULTIARCH"
  native=("$clang" --target="$SELA_TARGET_TRIPLE" \
    --sysroot="$SELA_SDK_ROOT/sysroots/$SELA_TARGET_SYSROOT_TRIPLE" \
    -resource-dir="$SELA_SDK_ROOT/host/usr/lib/llvm-18/lib/clang/18" "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}" \
    --rtlib=compiler-rt --unwindlib=none --ld-path="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/ld.lld")
  link=("-Wl,--dynamic-linker,$guest_runtime/$SELA_TARGET_LOADER" \
    "-Wl,-rpath,\$ORIGIN:$guest_runtime" -Wl,-z,nodefaultlib)
}

if [[ $selected_project != zlib ]]; then
  cjson_source="$corpus_work/sources/cJSON-1.7.19"
  cjson_programs=(cJSON_test tests/parse_examples tests/parse_number tests/parse_hex4
    tests/parse_string tests/parse_array tests/parse_object tests/parse_value
    tests/print_string tests/print_number tests/print_array tests/print_object
    tests/print_value tests/misc_tests tests/parse_with_opts tests/compare_tests
    tests/cjson_add tests/readme_examples tests/minify_tests fuzzing/fuzz_main)
  for mode in static shared; do
    pack="$fixtures/corpus/cjson-$mode"
    mkdir -p "$pack/artifacts"
    printf '%s\n' "${cjson_programs[@]}" > "$pack/programs.list"
    cjson_args=(--system cmake --source "$cjson_source" --configure-arg -C
      --configure-arg "$corpus_root/cjson-$mode.cmake" --build-target all --build-target check)
    library=libcjson.a
    if [[ $mode == shared ]]; then library=libcjson.so.1.7.19; fi
    publish "cjson-$mode-library" "$pack/artifacts/library.sela" "${cjson_args[@]}" --output "$library"
    for program in "${cjson_programs[@]}"; do
      label=${program//\//-}
      publish "cjson-$mode-$label" "$pack/artifacts/$label.sela" "${cjson_args[@]}" --output "$program"
    done
    for target in "${targets[@]}"; do
      reference="$last_private/build-$target/build"
      retained="$pack/reference/$target"
      mkdir -p "$retained/data/tests" "$retained/data/fuzzing"
      # Preserve original commands/properties; only execution routing changes.
      python3 "$repository/tests/vm/relocate-ctest.py" "$reference" "$retained/data"
      cp -R "$reference/tests/inputs" "$retained/data/tests/inputs"
      env -u LD_LIBRARY_PATH -u LD_PRELOAD "$reference/cJSON_test" > "$retained/reference.stdout"
      if [[ $mode == static ]]; then
        "$llvm_ar" t "$reference/libcjson.a" > "$retained/archive-members.txt"
      else
        native_command "$target"
        "${native[@]}" "$reference/CMakeFiles/cJSON_test.dir/test.c.o" \
          -L"$reference" -l:libcjson.so.1 -lm "${link[@]}" -o "$retained/native-caller"
      fi
    done
    printf '%s\n' "cjson-$mode" >> "$fixtures/corpus.list"
  done
fi

if [[ $selected_project != cjson ]]; then
  zlib_source="$corpus_work/sources/zlib-1.3.2"
  pack="$fixtures/corpus/zlib"
  mkdir -p "$pack/artifacts"
  # Upstream's documented input is shared by all untouched native references.
  export CFLAGS=-O3
  zlib_args=(--system make --source "$zlib_source" --configure-arg --shared
    --build-target all --build-target test --build-target test64)
  publish zlib-static-library "$pack/artifacts/static-library.sela" "${zlib_args[@]}" --output libz.a
  publish zlib-library "$pack/artifacts/library.sela" "${zlib_args[@]}" --output libz.so.1.3.2
  for program in example minigzip examplesh minigzipsh example64 minigzip64; do
    publish "zlib-$program" "$pack/artifacts/$program.sela" "${zlib_args[@]}" --output "$program"
  done
  for target in "${targets[@]}"; do
    reference="$last_private/build-$target/source"
    retained="$pack/reference/$target"
    mkdir -p "$retained/data"
    cp "$reference/Makefile" "$retained/data/Makefile"
    "$llvm_ar" t "$reference/libz.a" > "$retained/archive-members.txt"
  done
  printf 'zlib\n' >> "$fixtures/corpus.list"
fi

for target in "${targets[@]}"; do
  native_command "$target"
  mkdir -p "$fixtures/reference/$target"
  "${native[@]}" "$repository/tests/vm/exec-format.c" "${link[@]}" -o "$fixtures/reference/$target/exec-format"
  "${native[@]}" "$repository/tests/fixtures/dual-consumer/hello.c" "${link[@]}" -o "$fixtures/reference/$target/hello"
done
sela_verify_publication_inputs "$publication_receipt"
case "$selected_project" in all) count=50 ;; cjson) count=42 ;; zlib) count=8 ;; esac
printf '%s\n' "$count" > "$fixtures/corpus-count"
(
  cd "$fixtures"
  find . -type f ! -name fixtures.sha256 -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
) > "$fixtures/fixtures.sha256"
printf 'Source-free four-device fixture hashes:\n' | tee -a "$qualification_report"
cat "$fixtures/fixtures.sha256" | tee -a "$qualification_report"
if [[ $prepare_only == true ]]; then
  printf 'Fixture preparation completed; no device acceptance was run: %s\n' "$fixtures"
  exit 0
fi
for target in "${targets[@]}"; do
  env -u SELA_SDK_ROOT -u LD_LIBRARY_PATH -u LD_PRELOAD SELA_VM_TIMEOUT="${SELA_VM_TIMEOUT:-3600}" \
    bash "$repository/tests/consumer-vm.sh" "$target" "$bundle_parent/selac-$target" "$fixtures" \
    2>&1 | tee "$corpus_work/$target-consumer-vm.log"
  printf 'Real-kernel %s destination qualification: PASS (%s artifacts)\n' "$target" "$count" | tee -a "$qualification_report"
done
