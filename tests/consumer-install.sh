#!/usr/bin/env bash
set -euo pipefail
build_dir=$(realpath -e -- "$1")
sdk_root=$(realpath -e -- "$2")
independent_producer=$3
static_fixture=$4
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
profile=$("$build_dir/nierc" --print-target)
case "$profile" in
    x86_64) foreign=i686; multiarch=x86_64-linux-gnu; native_namespace=native64; foreign_namespace=native32 ;;
    i686) foreign=x86_64; multiarch=i386-linux-gnu; native_namespace=native32; foreign_namespace=native64 ;;
    *) exit 2 ;;
esac
install_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-consumer-test-XXXXXX")
"$independent_producer" "$install_work/independent.nier"
"$static_fixture" "$install_work/independent.nier" "$install_work/static.nier"
bash "$repository/scripts/package-consumer.sh" "$build_dir" "$install_work/original" "$sdk_root"
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
cmp -- "$sdk_root/sysroots/$profile-linux-gnu/usr/share/doc/libc6/copyright" "$bundle/licenses/libc6.copyright"
env -i PATH=/usr/bin:/bin LC_ALL=C "$bundle/bin/nierc" --check-sdk
mkdir "$install_work/incomplete-sdk"
printf '%064d\n' 0 > "$install_work/incomplete-sdk/sdk-lock.sha256"
if env -i PATH=/usr/bin:/bin LC_ALL=C NIER_SDK_ROOT="$install_work/incomplete-sdk" \
    "$bundle/bin/nierc" --check-sdk > "$install_work/mismatched-sdk.log" 2>&1; then
    printf 'ERROR: compiler accepted a mismatched SDK identity\n' >&2; exit 1
fi
rg -Fq 'SDK package lock does not match' "$install_work/mismatched-sdk.log"
cp -- "$bundle/sdk/sdk-lock.sha256" "$install_work/incomplete-sdk/sdk-lock.sha256"
if env -i PATH=/usr/bin:/bin LC_ALL=C NIER_SDK_ROOT="$install_work/incomplete-sdk" \
    "$bundle/bin/nierc" --check-sdk > "$install_work/incomplete-sdk.log" 2>&1; then
    printf 'ERROR: compiler accepted an SDK without a completion receipt\n' >&2; exit 1
fi
rg -Fq 'consumer SDK build is incomplete' "$install_work/incomplete-sdk.log"
if rg --files --hidden --no-ignore "$bundle" | rg "/include/|/cmake/|/sysroots/$foreign-linux-gnu/|/bin/(clang|clang-[0-9]+|nier-build|nier-ld|nier-native-ld)$|libclang-cpp|libnier-clang|nier-capture"; then
    printf 'ERROR: publisher/development inputs leaked into consumer bundle\n' >&2; exit 1
fi
consumer_symbols=$(nm -C --defined-only "$bundle/bin/nierc")
if ! rg -Fq "nier::detail::$native_namespace::" <<< "$consumer_symbols"; then
    printf 'ERROR: expected native implementation missing from compiler symbol inventory\n' >&2; exit 1
fi
if rg "clang::|nier::mergeProfiles|NierAction|ProfileMerger|nier::detail::$foreign_namespace::" <<< "$consumer_symbols"; then
    printf 'ERROR: frontend/producer code was linked into the independent compiler\n' >&2; exit 1
fi
for kind in independent static; do
    output="$install_work/$kind.native"
    [[ $kind != static ]] || output="$install_work/static.a"
    env -i PATH=/usr/bin:/bin LC_ALL=C TMPDIR="$install_work" \
        strace -f -e trace=openat,execve -o "$install_work/$kind.trace" \
        "$bundle/bin/nierc" "$install_work/$kind.nier" -o "$output"
    for forbidden in "$sdk_root" "$build_dir" "$install_work/original"; do
        if rg -F -- "$forbidden" "$install_work/$kind.trace"; then
            printf 'ERROR: relocated compiler accessed an original dependency path\n' >&2; exit 1
        fi
    done
    if rg 'libclang-cpp|libnier-clang|nier-capture|/bin/clang("| )|/usr/include/' "$install_work/$kind.trace"; then
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
rg -q '/bin/opt"' "$install_work/independent.trace"
rg -q '/bin/llc"' "$install_work/independent.trace"
rg -q '/bin/ld.lld"' "$install_work/independent.trace"
rg -q '/bin/llvm-ar"' "$install_work/static.trace"
test "$(ar t "$install_work/static.a")" = "$(printf 'member0.o\nmember1.o')"
env -i PATH=/usr/bin:/bin LC_ALL=C strace -f -e trace=openat,execve \
    -o "$install_work/native.trace" "$install_work/independent.native"
rg -Fq "$bundle/sdk/sysroots/$profile-linux-gnu/lib/$multiarch/libc.so.6" "$install_work/native.trace"
if rg '\.nier"|/bin/nierc|/bin/(clang|opt|llc|ld.lld|llvm-ar)"' "$install_work/native.trace"; then
    printf 'ERROR: native application required compiler/artifact inputs\n' >&2; exit 1
fi
printf 'Relocated compiler-only executable/static output and clean native execution passed: %s\n' "$install_work"
