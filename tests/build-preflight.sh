#!/usr/bin/env bash
set -euo pipefail
builder=$(realpath -- "$1")
export SELA_SDK_ROOT=$(realpath -- "$2")
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
preflight_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-preflight-test-XXXXXX")
if SELA_EMULATOR_ARMV7="$preflight_work/missing-qemu" "$builder" \
    --system make --source "$project/tests/fixtures/build" --output hello \
    --build-target hello --artifact "$preflight_work/rejected.sela" \
    >"$preflight_work/rejected.log" 2>&1; then
  printf 'Missing explicitly selected emulator unexpectedly passed setup preflight\n' >&2
  exit 1
fi
rg -q 'requires provisioned qemu-arm.*SELA_EMULATOR_ARMV7' "$preflight_work/rejected.log"
test ! -e "$preflight_work/rejected.sela"
test ! -e "$project/tests/fixtures/build/hello"
printf 'Missing ARM execution prerequisites fail explicitly without publishing or altering sources.\n'
