#!/usr/bin/env bash
# Source this helper; identity checks do not lock or mutate publisher inputs.
sela_record_publication_inputs() {
    local receipt=$1 configuration=$2 sdk=$3
    shift 3
    local tools
    tools=$(dirname -- "$configuration")
    sha256sum -- "$configuration" "$tools/sela-build" "$tools/sela-native-ld" \
        "$tools/sela-ld" "$tools/libsela-clang.so" "$tools/sela-capture.so" \
        "$sdk/host/usr/lib/llvm-18/bin/clang" "$sdk/host/usr/lib/llvm-18/bin/ld.lld" \
        "$sdk/sdk-lock.sha256" "$@" > "$receipt"
}

sela_verify_publication_inputs() {
    if ! sha256sum --check --quiet -- "$1"; then
        printf 'Publisher inputs changed during fixture preparation; this mixed checkpoint cannot be qualified. Start a fresh preparation after builds finish. Receipt: %s\n' "$1" >&2
        return 1
    fi
}
