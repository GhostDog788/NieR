#!/usr/bin/env bash
# Test harness only: original upstream test runners, never product payload.
set -euo pipefail
if [[ $# != 4 ]]; then
    printf 'Usage: bash tests/vm/stage-test-tools.sh TARGET BUNDLE NEW_OUTPUT CACHE\n' >&2
    exit 2
fi
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
target=$1
eval "$(python3 "$repository/sdk/targets.py" shell "$target")"
bundle=$(realpath -e -- "$2")
output=$(realpath -m -- "$3")
cache=$4
test ! -e "$output"
mkdir -p "$output"/{bin,lib,licenses} "$cache"
packages=$(mktemp -d "${TMPDIR:-/tmp}/sela-vm-test-packages-XXXXXX")
printf 'Test-runner package extraction: %s\n' "$packages"
while read -r package architecture version digest url extra; do
    [[ -n $package && $package != \#* ]] || continue
    [[ $architecture == "$SELA_TARGET_PACKAGE_ARCH" || $architecture == all ]] || continue
    [[ -z ${extra:-} && $digest =~ ^[0-9a-f]{64}$ && $url == https://* ]]
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
cp -Lp -- "$packages/usr/bin/$SELA_TARGET_GCC_TRIPLE-readelf" "$output/bin/readelf"
# CTest discovers CMAKE_ROOT next to itself even when no configure is run.
# Keep its unmodified installed support data, but not the CMake executable.
mkdir "$output/share"
cp -a -- "$packages/usr/share/cmake-3.28" "$output/share/cmake-3.28"
queue=("$output/bin/ctest" "$output/bin/make" "$output/bin/readelf")
declare -A copied=()
for (( index=0; index<${#queue[@]}; ++index )); do
    python3 "$repository/sdk/targets.py" check-elf "$target" "${queue[index]}"
    dynamic=$(readelf -d "${queue[index]}")
    needed=$(sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p' <<< "$dynamic")
    while IFS= read -r library; do
        [[ -n $library ]] || continue
        case "$library" in
            libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libresolv.so.2|libutil.so.1|ld-linux*.so.*) continue ;;
        esac
        [[ ${copied[$library]:-} ]] && continue
        original=
        # The test harness owns its complete non-glibc closure. Compiler
        # specialization must not silently remove CTest's optional libraries.
        for directory in "$packages/usr/lib/$SELA_TARGET_MULTIARCH" "$packages/lib/$SELA_TARGET_MULTIARCH"; do
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
printf 'Pinned %s CTest/make/readelf runtime staged: %s\n' "$target" "$output"
