#!/usr/bin/env bash
# Bounded CI coverage, not the full qualification corpus or security acceptance.
set -euo pipefail
export LC_ALL=C

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if (( $# > 1 )); then
  printf 'Usage: bash scripts/ci-smoke.sh [configured-build-directory]\n' >&2
  exit 2
fi
build_dir=${1:-$repo_root/build/prealpha}
source "$repo_root/sdk/env.sh"
ctest="$NIER_SDK_ROOT/host/usr/bin/ctest"
if [[ ! -x $ctest || ! -f $build_dir/CTestTestfile.cmake ]]; then
  printf 'Bootstrap the SDK and configure/build NieR with publisher tests enabled first.\n' >&2
  exit 1
fi
build_dir=$(cd -- "$build_dir" && pwd -P)

tests=(
  ir_validation
  package_validation
  read_validation
  producer_validation
  independent_producer
  capture_hook
  hello_pipeline
  hello_project_walkthrough
  build_integration
)
test_regex="^($(IFS='|'; printf '%s' "${tests[*]}"))$"

# A consumer-only or stale build must not silently pass a smaller selection.
inventory=$("$ctest" --test-dir "$build_dir" --show-only -R "$test_regex")
printf '%s\n' "$inventory"
selected=$(printf '%s\n' "$inventory" | awk '/^Total Tests: / { print $3 }')
if [[ $selected != ${#tests[@]} ]]; then
  printf 'Expected all %s CI smoke tests; found %s. Reconfigure the publisher build.\n' \
    "${#tests[@]}" "${selected:-none}" >&2
  exit 1
fi

"$ctest" --test-dir "$build_dir" --parallel 2 --timeout 180 \
  --no-tests=error --output-on-failure --output-junit "$build_dir/ci-smoke.xml" \
  -R "$test_regex"
