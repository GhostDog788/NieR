#!/usr/bin/env bash
set -euo pipefail
build_dir=$(realpath -e -- "$1")
sdk_root=$(realpath -e -- "$2")
independent_producer=$3
static_fixture=$4
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
profile=$(sed -n 's/^SELA_DEVICE_TARGET:STRING=//p' "$build_dir/CMakeCache.txt")
eval "$(python3 "$repository/sdk/targets.py" shell "$profile")"
[[ ${SELA_TARGET_ID:-} == "$profile" ]] || exit 2
multiarch=$SELA_TARGET_MULTIARCH
sysroot_triple=$SELA_TARGET_SYSROOT_TRIPLE
native_namespace=native_$profile
host_target=$(python3 "$repository/sdk/targets.py" host)
native_trace=false
if [[ $host_target == "$profile" || ( $host_target == x86_64 && $profile == i686 ) ]]; then native_trace=true; fi
build_environment=(env -i PATH=/usr/bin:/bin LC_ALL=C
    "LD_LIBRARY_PATH=$sdk_root/host/usr/lib/llvm-18/lib:$sdk_root/host/usr/lib/$multiarch"
    "QEMU_LD_PREFIX=$sdk_root/sysroots/$sysroot_triple")
test "$("${build_environment[@]}" "$build_dir/selac" --print-target)" = "$profile"
install_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-consumer-test-XXXXXX")
"${build_environment[@]}" "$independent_producer" "$install_work/independent.sela"
"${build_environment[@]}" "$static_fixture" "$install_work/independent.sela" "$install_work/static.sela"
source_products=("$build_dir/selac")
for tool in opt llc ld.lld llvm-ar; do
    source_products+=("$sdk_root/host/usr/lib/llvm-18/bin/$tool")
done
mapfile -t llvm_runtime < <(python3 - "$sdk_root/consumer-sdk.json" <<'PY'
import json, sys
for soname, record in json.load(open(sys.argv[1])).get('runtime_libraries', {}).items():
    print(record['file'])
    print(soname)
PY
)
if (( ${#llvm_runtime[@]} )); then source_products+=("$sdk_root/${llvm_runtime[0]}"); fi
source_hashes=$(sha256sum -- "${source_products[@]}")
bash "$repository/scripts/package-consumer.sh" "$build_dir" "$install_work/original" "$sdk_root"
test "$source_hashes" = "$(sha256sum -- "${source_products[@]}")"
bundle="$install_work/relocated bundle"
mv -- "$install_work/original" "$bundle"
(
    cd -- "$bundle"
    sha256sum --quiet -c payload.sha256
)
cmp -- "$repository/LICENSE" "$bundle/LICENSE"
license_hash=$(sha256sum "$repository/LICENSE")
license_hash=${license_hash%% *}
rg -Fxq -- "$license_hash  LICENSE" "$bundle/payload.sha256"
cmp -- "$sdk_root/consumer-sdk.json" "$bundle/sdk/consumer-sdk.json"
cmp -- "$sdk_root/sysroots/$sysroot_triple/usr/share/doc/libc6/copyright" "$bundle/licenses/libc6.copyright"
cmp -- "$sdk_root/host/usr/share/doc/libarchive13t64/copyright" "$bundle/licenses/libarchive13t64.copyright"
consumer_environment=(env -i PATH=/usr/bin:/bin LC_ALL=C)
if [[ $native_trace == false ]]; then
    consumer_environment+=("QEMU_LD_PREFIX=$bundle/sdk/sysroots/$sysroot_triple")
    printf 'Foreign user-mode execution: structural/relocation/runtime checks run, but host strace cannot prove target-library absence. Matching source-free device VM acceptance is required.\n' \
        | tee "$install_work/trace-policy.txt"
else
    printf 'Native or x86 compatibility execution: full host-system-call dependency trace applies.\n' > "$install_work/trace-policy.txt"
fi
"${consumer_environment[@]}" "$bundle/bin/selac" --check-sdk
"${consumer_environment[@]}" python3 -B \
    "$repository/tests/consumer-package-boundary.py" "$bundle/bin/selac" "$install_work/independent.sela"
mkdir "$install_work/incomplete-sdk"
printf '%064d\n' 0 > "$install_work/incomplete-sdk/sdk-lock.sha256"
if "${consumer_environment[@]}" SELA_SDK_ROOT="$install_work/incomplete-sdk" \
    "$bundle/bin/selac" --check-sdk > "$install_work/mismatched-sdk.log" 2>&1; then
    printf 'ERROR: compiler accepted a mismatched SDK identity\n' >&2; exit 1
fi
rg -Fq 'SDK package lock does not match' "$install_work/mismatched-sdk.log"
cp -- "$bundle/sdk/sdk-lock.sha256" "$install_work/incomplete-sdk/sdk-lock.sha256"
if "${consumer_environment[@]}" SELA_SDK_ROOT="$install_work/incomplete-sdk" \
    "$bundle/bin/selac" --check-sdk > "$install_work/incomplete-sdk.log" 2>&1; then
    printf 'ERROR: compiler accepted an SDK without a completion receipt\n' >&2; exit 1
fi
rg -Fq 'consumer SDK build is incomplete' "$install_work/incomplete-sdk.log"
if rg --files --hidden --no-ignore "$bundle" | rg '/include/|/cmake/|/bin/(clang|clang-[0-9]+|sela-build|sela-ld|sela-native-ld)$|libclang-cpp|libsela-clang|sela-capture'; then
    printf 'ERROR: publisher/development inputs leaked into consumer bundle\n' >&2; exit 1
fi
# Static backend identity is audited before release stripping. Successful
# execution, architecture and dependency checks below cover the shipped copy.
consumer_symbols=$(nm -C --defined-only "$build_dir/selac")
if ! rg -Fq "sela::detail::$native_namespace::" <<< "$consumer_symbols"; then
    printf 'ERROR: expected native implementation missing from compiler symbol inventory\n' >&2; exit 1
fi
if rg 'clang::|sela::mergeProfiles|SelaAction|ProfileMerger' <<< "$consumer_symbols"; then
    printf 'ERROR: frontend/producer code was linked into the independent compiler\n' >&2; exit 1
fi
test "$(find "$bundle/sdk/sysroots" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 1
test -d "$bundle/sdk/sysroots/$sysroot_triple"
while IFS= read -r foreign; do
    [[ $foreign != "$profile" ]] || continue
    foreign_triple=$(python3 "$repository/sdk/targets.py" get "$foreign" sysrootTriple)
    foreign_multiarch=$(python3 "$repository/sdk/targets.py" get "$foreign" multiarch)
    test ! -e "$bundle/sdk/sysroots/$foreign_triple"
    test ! -e "$bundle/sdk/host/usr/lib/$foreign_multiarch"
    if rg -Fq "sela::detail::native_$foreign::" <<< "$consumer_symbols"; then
        printf 'ERROR: foreign native ABI was linked into the independent compiler: %s\n' "$foreign" >&2; exit 1
    fi
done < <(python3 "$repository/sdk/targets.py" list)
shipped_binaries=("$bundle/bin/selac" "$bundle/sdk/host/usr/lib/llvm-18/bin/"{opt,llc,ld.lld,llvm-ar})
if (( ${#llvm_runtime[@]} )); then shipped_binaries+=("$bundle/sdk/host/usr/lib/llvm-18/lib/${llvm_runtime[1]}"); fi
for binary in "${shipped_binaries[@]}"; do
    sections=$(readelf -SW "$binary")
    if rg '\.(symtab|strtab)\s' <<< "$sections"; then
        printf 'ERROR: ordinary symbol tables leaked into release binary: %s\n' "$binary" >&2; exit 1
    fi
    if [[ $SELA_TARGET_ABI == aapcs32-vfp ]]; then
        rg -q '\.ARM\.exidx\s' <<< "$sections"
    else
        rg -q '\.eh_frame\s' <<< "$sections"
    fi
    dependencies=$(readelf -d "$binary")
    if rg '\(NEEDED\).*\[(libarchive|libxml2|libicu)' <<< "$dependencies"; then
        printf 'ERROR: archive/XML/ICU dependency leaked into compiler package\n' >&2; exit 1
    fi
done
for kind in independent static; do
    output="$install_work/$kind.native"
    [[ $kind != static ]] || output="$install_work/static.a"
    if [[ $native_trace == false ]]; then
        "${consumer_environment[@]}" TMPDIR="$install_work" \
            "$bundle/bin/selac" "$install_work/$kind.sela" -o "$output"
        continue
    fi
    "${consumer_environment[@]}" TMPDIR="$install_work" \
        strace -f -e trace=openat,execve -o "$install_work/$kind.trace" \
        "$bundle/bin/selac" "$install_work/$kind.sela" -o "$output"
    for forbidden in "$sdk_root" "$build_dir" "$install_work/original"; do
        if rg -F -- "$forbidden" "$install_work/$kind.trace"; then
            printf 'ERROR: relocated compiler accessed an original dependency path\n' >&2; exit 1
        fi
    done
    if rg 'libclang-cpp|libsela-clang|sela-capture|/bin/clang("| )|/usr/include/' "$install_work/$kind.trace"; then
        printf 'ERROR: compiler used a frontend or source header\n' >&2; exit 1
    fi
    # Failed loader probes are harmless; successful non-glibc host loads are
    # not. The Ubuntu baseline supplies only its ordinary glibc/loader family.
    host_loads=$(rg 'openat.*"/(usr/)?lib[^" ]*/[^/"]*\.so[^/"]*".* = [0-9]+$' \
        "$install_work/$kind.trace" || true)
    if [[ -n $host_loads ]] && printf '%s\n' "$host_loads" | \
        rg -v '/(libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1|libresolv\.so\.2|libutil\.so\.1|ld-linux-x86-64\.so\.2|ld-linux\.so\.2)"'; then
        printf 'ERROR: LLVM subprocess fell back to a non-baseline host library\n' >&2; exit 1
    fi
done
if [[ $native_trace == true ]]; then
    rg -q '/bin/opt"' "$install_work/independent.trace"
    rg -q '/bin/llc"' "$install_work/independent.trace"
    rg -q '/bin/ld.lld"' "$install_work/independent.trace"
    rg -q '/bin/llvm-ar"' "$install_work/static.trace"
fi
test "$(ar t "$install_work/static.a")" = "$(printf 'member0.o\nmember1.o')"
python3 "$repository/sdk/targets.py" check-elf "$profile" "$install_work/independent.native"
if [[ $native_trace == true ]]; then
"${consumer_environment[@]}" strace -f -e trace=openat,execve \
    -o "$install_work/native.trace" "$install_work/independent.native"
rg -Fq "$bundle/sdk/sysroots/$sysroot_triple/lib/$multiarch/libc.so.6" "$install_work/native.trace"
if rg '\.sela"|/bin/selac|libLLVM|/bin/(clang|opt|llc|ld.lld|llvm-ar)"' "$install_work/native.trace"; then
    printf 'ERROR: native application required compiler/artifact inputs\n' >&2; exit 1
fi
else
    "${consumer_environment[@]}" "$install_work/independent.native"
fi
printf 'Relocated %s compiler executable/static output and application execution passed: %s\n' "$profile" "$install_work"
