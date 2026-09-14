#!/usr/bin/env bash
set -euo pipefail
selac=$(realpath "$1")
export SELA_SDK_ROOT=$(realpath "$2")
config=$(realpath "$3")
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$repository/sdk/env.sh"
build_tool="$(dirname -- "$selac")/sela-build"
replay="$(dirname -- "$selac")/corpus_replay_tests"
target_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-target-build-XXXXXX")
for output in program libparts.a; do
  env -u SELA_ARCHS "$build_tool" --system cmake --source "$repository/tests/fixtures/target-build" \
    --build-target all --output "$output" --artifact "$target_work/$output.sela" \
    --clang-config "$config" --keep-private > "$target_work/$output.log" 2>&1
  retained=$(sed -n 's/^Private build evidence: //p' "$target_work/$output.log")
  "$replay" "$config" "$SELA_SDK_ROOT" "$retained" "build/$output" "$target_work/$output.sela"
  tar -xOf "$target_work/$output.sela" manifest.json | python3 -c '
import json,sys
m=json.load(sys.stdin)
assert m["targets"]==["x86_64","i686","armv7","aarch64"]
p=m["compilation_units"]
assert len(p["x86_64"])==len(p["aarch64"])==1
assert len(p["i686"])==len(p["armv7"])==2
assert any(len(module["targets"])<4 for module in m["modules"])
if m["kind"]=="executable":
    links=m["target_links"]
    assert links["x86_64"]["libraries"]==links["aarch64"]["libraries"]==[]
    assert links["i686"]["libraries"]==links["armv7"]["libraries"]==["m"]
'
done
"$selac" "$target_work/program.sela" -o "$target_work/program"
test "$(env -u LD_LIBRARY_PATH "$target_work/program")" = 'target build passed'
"$selac" "$target_work/libparts.a.sela" -o "$target_work/libparts.a"
"$build_tool" --system cmake --source "$repository/tests/fixtures/target-build" \
  --arch x86_64 --build-target program --output program --artifact "$target_work/singleton.sela" \
  --clang-config "$config" --keep-private > "$target_work/singleton.log" 2>&1
retained=$(sed -n 's/^Private build evidence: //p' "$target_work/singleton.log")
test -d "$retained/build-x86_64"
test ! -e "$retained/build-i686"
test ! -e "$retained/build-armv7"
test ! -e "$retained/build-aarch64"
"$replay" "$config" "$SELA_SDK_ROOT" "$retained" build/program "$target_work/singleton.sela"
printf 'Target-dependent flags, files, libraries, archives, singleton capture and replay passed: %s\n' "$target_work"
