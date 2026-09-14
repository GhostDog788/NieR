#!/usr/bin/env bash
# Pinned, unmodified xxHash: target-specific SIMD, dispatch, CLI and library ABI.
set -euo pipefail
if [[ $# != 3 && $# != 4 ]]; then
  echo 'Usage: bash corpus/qualify-xxhash.sh BUILD_TOOL SDK --prepare-only|--bundle-parent DIR' >&2
  exit 2
fi
build_tool=$(realpath -e -- "$1")
export SELA_SDK_ROOT=$(realpath -e -- "$2")
prepare_only=false
bundle_parent=
case "$3" in
  --prepare-only) [[ $# == 3 ]]; prepare_only=true ;;
  --bundle-parent) [[ $# == 4 ]]; bundle_parent=$(realpath -e -- "$4") ;;
  *) exit 2 ;;
esac
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
corpus_root="$repository/corpus"
work=$(mktemp -d "${TMPDIR:-/tmp}/sela-xxhash-XXXXXX")
fixtures="$work/consumer-fixtures"
pack="$fixtures/corpus/xxhash"
config="$(dirname -- "$build_tool")/sela.cfg"
report="$work/qualification.txt"
mkdir -p "$pack/artifacts" "$work/sources"
mapfile -t targets < <(python3 "$repository/sdk/targets.py" list)
python3 "$repository/tests/vm/stage-target-metadata.py" "$fixtures"
printf 'xxHash qualification evidence: %s\n' "$work"
finish_report() {
  local status=$?
  trap - EXIT
  local result=FAIL
  if [[ $status == 0 ]]; then
    result=PASS
    if [[ $prepare_only == true ]]; then result='PREPARED; device acceptance not run'; fi
  fi
  printf 'Result: %s\nFinished UTC: %s\n' "$result" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" | tee -a "$report"
  exit "$status"
}
trap finish_report EXIT
{
  printf 'Unmodified xxHash 0.8.3 functional qualification\nStarted UTC: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'Coverage: default dispatch/SIMD, upstream CLI sanity, upstream tests/sanity_test, static/shared ABI and streaming.\n'
  printf 'Not a benchmark or the entire upstream make check suite.\n'
  sha256sum "$corpus_root/xxhash.lock" "$corpus_root/xxhash-api.c" \
    "$corpus_root/qualify-xxhash.sh" "$repository/tests/consumer-corpus.sh" \
    "$repository/tests/consumer-vm.sh" "$repository/tests/vm/packages.lock"
} | tee "$report"
source "$repository/tests/publication-inputs.sh"
receipt="$work/publication-inputs.sha256"
sela_record_publication_inputs "$receipt" "$config" "$SELA_SDK_ROOT" \
  "$repository/sdk/targets.json" "$corpus_root/xxhash.lock" "$corpus_root/xxhash-api.c" \
  "$corpus_root/qualify-xxhash.sh" "$repository/tests/consumer-corpus.sh"
cat "$receipt" | tee -a "$report"
source "$repository/sdk/env.sh"
unset CFLAGS CPPFLAGS LDFLAGS LIBS SELA_ARCHS
clang="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
llvm_ar="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar"
read -r project version url expected < <(sed '/^#/d; /^$/d' "$corpus_root/xxhash.lock")
archive="$work/sources/$project-$version.tar.gz"
curl --fail --location --retry 2 --max-time 120 "$url" -o "$archive"
printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
tar -xzf "$archive" -C "$work/sources"
upstream="$work/sources/$project-$version"
publish() {
  local label=$1 goal=$2 output=$3
  sela_verify_publication_inputs "$receipt"
  local command=("$build_tool" --system make --source "$upstream" --build-target "$goal"
    --output "$output" --clang-config "$config" --artifact "$pack/artifacts/$label.sela" --keep-private)
  { printf 'Command:'; printf ' %q' "${command[@]}"; printf '\n'; } | tee -a "$report"
  "${command[@]}" 2>&1 | tee "$work/$label.log"
  sela_verify_publication_inputs "$receipt"
  last_private=$(sed -n 's/^Private build evidence: //p' "$work/$label.log")
  test -f "$last_private/capture-targets.json"
  printf 'Private evidence: %s\n' "$last_private" | tee -a "$report"
  sha256sum "$pack/artifacts/$label.sela" | tee -a "$report"
}
publish xxhsum all xxhsum
cli_private=$last_private
publish static-library libxxhash.a libxxhash.a
static_private=$last_private
publish library libxxhash.so.0.8.3 libxxhash.so.0.8.3
shared_private=$last_private
publish sanity test_sanity tests/sanity_test
sanity_private=$last_private
for mode in static shared; do
  library=libxxhash.a
  if [[ $mode == shared ]]; then library=libxxhash.so.0; fi
  sela_verify_publication_inputs "$receipt"
  "$clang" --config "$config" -O2 -I"$upstream" "$corpus_root/xxhash-api.c" \
    -l:"$library" -o "$pack/artifacts/$mode-api.sela"
  sela_verify_publication_inputs "$receipt"
done
for target in "${targets[@]}"; do
  eval "$(python3 "$repository/sdk/targets.py" shell "$target")"
  retained="$pack/reference/$target"
  mkdir -p "$retained" "$fixtures/reference/$target"
  cp "$cli_private/build-$target/source/xxhsum" "$retained/xxhsum"
  cp "$sanity_private/build-$target/source/tests/sanity_test" "$retained/sanity"
  cp "$shared_private/build-$target/source/libxxhash.so.0.8.3" "$retained/libxxhash.so.0"
  "$llvm_ar" t "$static_private/build-$target/source/libxxhash.a" > "$retained/archive-members.txt"
  guest_runtime="/opt/sela/sdk/sysroots/$SELA_TARGET_SYSROOT_TRIPLE/usr/lib/$SELA_TARGET_MULTIARCH"
  native=("$clang" --target="$SELA_TARGET_TRIPLE"
    --sysroot="$SELA_SDK_ROOT/sysroots/$SELA_TARGET_SYSROOT_TRIPLE"
    -resource-dir="$SELA_SDK_ROOT/host/usr/lib/llvm-18/lib/clang/18"
    "${SELA_TARGET_CLANG_ARGS[@]}" "${SELA_TARGET_PUBLICATION_CLANG_ARGS[@]}"
    --rtlib=compiler-rt --unwindlib=none --ld-path="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/ld.lld")
  link=("-Wl,--dynamic-linker,$guest_runtime/$SELA_TARGET_LOADER"
    "-Wl,-rpath,\$ORIGIN:$guest_runtime" -Wl,-z,nodefaultlib)
  "${native[@]}" -O2 -I"$upstream" "$corpus_root/xxhash-api.c" \
    -L"$retained" -l:libxxhash.so.0 "${link[@]}" -o "$retained/native-api"
  "${native[@]}" "$repository/tests/vm/exec-format.c" "${link[@]}" -o "$fixtures/reference/$target/exec-format"
  "${native[@]}" "$repository/tests/fixtures/dual-consumer/hello.c" "${link[@]}" -o "$fixtures/reference/$target/hello"
done
# Generated data, not upstream source: the guest has no publication inputs.
awk 'BEGIN { for (i=0; i<4096; ++i) printf "%08x sela xxHash test\n", i }' > "$pack/input.dat"
printf 'xxhash\n' > "$fixtures/corpus.list"
printf '6\n' > "$fixtures/corpus-count"
sela_verify_publication_inputs "$receipt"
(
  cd "$fixtures"
  find . -type f ! -name fixtures.sha256 -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
) > "$fixtures/fixtures.sha256"
cat "$fixtures/fixtures.sha256" | tee -a "$report"
if [[ $prepare_only == true ]]; then
  printf 'Prepared source-free fixtures: %s\n' "$fixtures"
  exit 0
fi
for target in "${targets[@]}"; do
  env -u SELA_SDK_ROOT -u LD_LIBRARY_PATH -u LD_PRELOAD SELA_VM_TIMEOUT="${SELA_VM_TIMEOUT:-3600}" \
    bash "$repository/tests/consumer-vm.sh" "$target" "$bundle_parent/selac-$target" "$fixtures" \
    2>&1 | tee "$work/$target-consumer-vm.log"
  printf 'Real-kernel %s: PASS (6 artifacts)\n' "$target" | tee -a "$report"
done
