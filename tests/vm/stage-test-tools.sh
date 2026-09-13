#!/usr/bin/env bash
# Test harness only: original upstream test runners, never product payload.
set -euo pipefail
if [[ $# != 3 ]]; then
    printf 'Usage: bash tests/vm/stage-test-tools.sh I686_BUNDLE NEW_OUTPUT CACHE\n' >&2
    exit 2
fi
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
bundle=$(realpath -e -- "$1")
output=$(realpath -m -- "$2")
cache=$3
test ! -e "$output"
mkdir -p "$output"/{bin,lib,licenses} "$cache"
packages=$(mktemp -d "${TMPDIR:-/tmp}/nier-vm-test-packages-XXXXXX")
printf 'Test-runner package extraction: %s\n' "$packages"
while read -r package architecture version digest url extra; do
    [[ -n $package && $package != \#* ]] || continue
    [[ -z ${extra:-} && ( $architecture == i386 || $architecture == all ) && $digest =~ ^[0-9a-f]{64}$ && $url == https://* ]]
    archive="$cache/${url##*/}"
    if [[ ! -f $archive ]]; then
        download=$(mktemp "$cache/.test-runner-download-XXXXXX")
        curl --fail --location --retry 3 --connect-timeout 20 --max-time 300 "$url" -o "$download"
        printf '%s  %s\n' "$digest" "$download" | sha256sum -c -
        mv -T -- "$download" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | sha256sum -c -
    test "$(dpkg-deb -f "$archive" Package)" = "$package"
    test "$(dpkg-deb -f "$archive" Architecture)" = "$architecture"
    test "$(dpkg-deb -f "$archive" Version)" = "$version"
    dpkg-deb -x "$archive" "$packages"
done < "$repository/tests/vm/test-tools.lock"
cp -Lp -- "$packages/usr/bin/ctest" "$output/bin/ctest"
cp -Lp -- "$packages/usr/bin/make" "$output/bin/make"
cp -Lp -- "$packages/usr/bin/i686-linux-gnu-readelf" "$output/bin/readelf"
# CTest discovers CMAKE_ROOT next to itself even when no configure is run.
# Keep its unmodified installed support data, but not the CMake executable.
mkdir "$output/share"
cp -a -- "$packages/usr/share/cmake-3.28" "$output/share/cmake-3.28"
queue=("$output/bin/ctest" "$output/bin/make" "$output/bin/readelf")
declare -A copied=()
for (( index=0; index<${#queue[@]}; ++index )); do
    readelf -h "${queue[index]}" | grep -q 'Class:.*ELF32'
    dynamic=$(readelf -d "${queue[index]}")
    needed=$(sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p' <<< "$dynamic")
    while IFS= read -r library; do
        [[ -n $library ]] || continue
        case "$library" in
            libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libresolv.so.2|libutil.so.1|ld-linux.so.2) continue ;;
        esac
        [[ ${copied[$library]:-} ]] && continue
        original=
        for directory in "$packages/usr/lib/i386-linux-gnu" "$packages/lib/i386-linux-gnu" \
            "$bundle/sdk/host/usr/lib/llvm-18/lib" "$bundle/sdk/host/usr/lib/i386-linux-gnu"; do
            if [[ -f $directory/$library ]]; then original="$directory/$library"; break; fi
        done
        if [[ -z $original ]]; then printf 'Missing pinned VM test-runner dependency: %s\n' "$library" >&2; exit 1; fi
        cp -Lp -- "$original" "$output/lib/$library"
        copied[$library]=1
        queue+=("$output/lib/$library")
    done <<< "$needed"
done
for directory in "$packages/usr/share/doc/"*; do
    if [[ -f $directory/copyright ]]; then cp -Lp -- "$directory/copyright" "$output/licenses/${directory##*/}.copyright"; fi
done
cp -a -- "$bundle/licenses" "$output/licenses/compiler-bundle"
cp -- "$repository/tests/vm/test-tools.lock" "$output/packages.lock"
(
    cd "$output"
    find . -type f ! -name payload.sha256 -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
) > "$output/payload.sha256"
printf 'Pinned ELF32 CTest/make/readelf runtime staged: %s\n' "$output"
