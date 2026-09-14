#!/usr/bin/env bash
set -euo pipefail
replayer=$(realpath -- "$1")
sdk=$(realpath -- "$2")
config=$(realpath -- "$3")
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_tool="$(dirname -- "$config")/sela-build"
test_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-retained-selection-XXXXXX")
export SELA_SDK_ROOT=$sdk
export LD_LIBRARY_PATH="$sdk/host/usr/lib/llvm-18/lib:$sdk/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$build_tool" --system make --source "$project/tests/fixtures/link-order" \
  --build-target libfirst.a --output libfirst.a --artifact "$test_work/library.sela" \
  --keep-private > "$test_work/build.log" 2>&1
retained=$(sed -n 's/^Private build evidence: //p' "$test_work/build.log")
test -d "$retained/build-x86_64/metadata"
snapshot() {
  while IFS= read -r path; do
    # Reads may update filesystem access times; data, inode and modification
    # identity must stay unchanged. No retained evidence is repaired or added.
    stat -c '%n:%i:%s:%Y:%Z' "$path"
    sha256sum "$path"
  done < <(rg --files --hidden "$retained" | sort)
}
snapshot > "$test_work/before-positive"
"$replayer" "$config" "$sdk" "$retained" source/libfirst.a "$test_work/library.sela"
snapshot > "$test_work/after-positive"
cmp "$test_work/before-positive" "$test_work/after-positive"
mapfile -t saved < <(rg --files "$retained/build-x86_64/metadata" | rg '\.native\.o$')
test "${#saved[@]}" -eq 1
missing=${saved[0]}
# This is a disposable native fixture, not original corpus evidence. Move the
# one saved object aside recoverably; replay must reject without recreating it.
mv -- "$missing" "$missing.retained-test-backup"
snapshot > "$test_work/before-negative"
if "$replayer" "$config" "$sdk" "$retained" source/libfirst.a "$test_work/library.sela" \
    > "$test_work/rejected.log" 2>&1; then
  echo 'Missing retained native provenance was silently reconstructed' >&2
  exit 1
fi
grep -q 'retained original native object is missing' "$test_work/rejected.log"
test ! -e "$missing"
snapshot > "$test_work/after-negative"
cmp "$test_work/before-negative" "$test_work/after-negative"
mv -- "$missing.retained-test-backup" "$missing"
echo "Retained selection validates evidence without mutating or repairing it: $test_work"
