#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 ]]; then
    printf 'Usage: bash scripts/package-consumer.sh BUILD_DIR OUTPUT_DIR [SDK_ROOT]\n' >&2
    exit 2
fi
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=$(realpath -e -- "$1")
output_dir=$(realpath -m -- "$2")
sdk_root=$(realpath -e -- "${3:-${NIER_SDK_ROOT:-$repository/.sdk}}")
if [[ -e $output_dir || -L $output_dir ]]; then
    printf 'Consumer bundle output must not already exist: %s\n' "$output_dir" >&2
    exit 1
fi
output_parent=$(dirname -- "$output_dir")
test -d "$output_parent"
lock_hash=$(sha256sum "$repository/sdk/packages.lock")
lock_hash=${lock_hash%% *}
test "$(<"$sdk_root/sdk-lock.sha256")" = "$lock_hash"
stage=$(mktemp -d "$output_parent/.nier-consumer-stage-XXXXXX")
trap 'status=$?; if (( status )); then printf "Incomplete private bundle retained at: %s\n" "$stage" >&2; fi' EXIT
env LD_LIBRARY_PATH="$sdk_root/host/usr/lib/llvm-18/lib:$sdk_root/host/usr/lib/x86_64-linux-gnu" \
    "$sdk_root/host/usr/bin/cmake" --install "$build_dir" --prefix "$stage" --component Consumer
test -x "$stage/bin/nierc"
readelf -d "$stage/bin/nierc" | rg -Fq '(RPATH)'
readelf -d "$stage/bin/nierc" | rg -Fq '$ORIGIN/../sdk/host/usr/lib/llvm-18/lib'

llvm_relative=host/usr/lib/llvm-18
bundle_bin="$stage/sdk/$llvm_relative/bin"
bundle_lib="$stage/sdk/$llvm_relative/lib"
mkdir -p "$bundle_bin" "$bundle_lib" "$stage/licenses"
queue=("$stage/bin/nierc")
for tool in opt llc ld.lld llvm-ar; do
    original="$sdk_root/$llvm_relative/bin/$tool"
    cp -Lp -- "$original" "$bundle_bin/$tool"
    cmp -- "$original" "$bundle_bin/$tool"
    queue+=("$bundle_bin/$tool")
done

# The host loader/glibc family is the explicit Ubuntu 24.04 baseline. Every
# other ELF dependency must come from the pinned extracted SDK, never ldd's
# opportunistic host resolution. Resolve library symlinks while copying so the
# flattened runtime directory has no links escaping the bundle.
declare -A copied=() notices=()
for (( index=0; index<${#queue[@]}; ++index )); do
    dynamic=$(readelf -d -- "${queue[index]}")
    needed=$(sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p' <<< "$dynamic")
    while IFS= read -r library; do
        [[ -n $library ]] || continue
        case "$library" in
            libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libresolv.so.2|libutil.so.1|ld-linux-x86-64.so.2) continue ;;
        esac
        [[ $library =~ ^[a-zA-Z0-9_.+-]+$ ]]
        [[ ${copied[$library]:-} ]] && continue
        original=
        for directory in "$sdk_root/$llvm_relative/lib" "$sdk_root/host/usr/lib/x86_64-linux-gnu"; do
            if [[ -f $directory/$library ]]; then original=$(realpath -e -- "$directory/$library"); break; fi
        done
        if [[ -z $original || $original != "$sdk_root/host/"* ]]; then
            printf 'Missing pinned consumer dependency: %s\n' "$library" >&2; exit 1
        fi
        cp -Lp -- "$original" "$bundle_lib/$library"
        cmp -- "$original" "$bundle_lib/$library"
        copied[$library]=1
        queue+=("$bundle_lib/$library")
        case "$library" in
            libLLVM*) package=libllvm18 ;;
            libarchive*) package=libarchive13t64 ;;
            libstdc++*) package=libstdc++6 ;;
            libgcc_s*) package=libgcc-s1 ;;
            libffi*) package=libffi8 ;;
            libedit*) package=libedit2 ;;
            libz.so*) package=zlib1g ;;
            libzstd*) package=libzstd1 ;;
            libtinfo*) package=libtinfo6 ;;
            libxml2*) package=libxml2 ;;
            libbsd*) package=libbsd0 ;;
            libmd*) package=libmd0 ;;
            libicu*) package=libicu74 ;;
            liblzma*) package=liblzma5 ;;
            liblz4*) package=liblz4-1 ;;
            libbz2*) package=libbz2-1.0 ;;
            libacl*) package=libacl1 ;;
            libnettle*) package=libnettle8t64 ;;
            *) printf 'Add the license mapping for dependency: %s\n' "$library" >&2; exit 1 ;;
        esac
        notices[$package]=1
    done <<< "$needed"
done

# Retain the complete current x86-64 native link/runtime library set (not its
# source headers); this includes glibc scripts, NSS modules, archives and CRTs.
target="$stage/sdk/sysroots/x86_64-linux-gnu"
mkdir -p "$target/usr"
cp -a -- "$sdk_root/sysroots/x86_64-linux-gnu/usr/lib" "$target/usr/lib"
cp -a -- "$sdk_root/sysroots/x86_64-linux-gnu/usr/lib64" "$target/usr/lib64"
ln -s usr/lib "$target/lib"
ln -s usr/lib64 "$target/lib64"
runtime="$bundle_lib/clang/18/lib/linux"
mkdir -p "$runtime"
for name in libclang_rt.builtins-x86_64.a clang_rt.crtbegin-x86_64.o clang_rt.crtend-x86_64.o; do
    cp -Lp -- "$sdk_root/$llvm_relative/lib/clang/18/lib/linux/$name" "$runtime/$name"
done
for package in llvm-18 lld-18 libclang-rt-18-dev "${!notices[@]}"; do
    cp -Lp -- "$sdk_root/host/usr/share/doc/$package/copyright" "$stage/licenses/$package.copyright"
done
for package in libc6 libc6-dev libcrypt1 libcrypt-dev; do
    cp -Lp -- "$sdk_root/sysroots/x86_64-linux-gnu/usr/share/doc/$package/copyright" "$stage/licenses/$package.copyright"
done
cp -- "$sdk_root/sdk-lock.sha256" "$stage/sdk/sdk-lock.sha256"
cp -- "$repository/sdk/packages.lock" "$stage/sdk/packages.lock"
cp -- "$repository/LICENSE" "$stage/LICENSE"
cp -- "$repository/docs/reference/compiler-distribution.md" "$stage/README.md"
(
    cd -- "$stage"
    while IFS= read -r -d '' file; do sha256sum -- "$file"; done \
        < <(rg --files --hidden --no-ignore -g '!payload.sha256' -0 | sort -z)
) > "$stage/payload.sha256"
mv -T -- "$stage" "$output_dir"
printf 'Consumer-only Ubuntu 24.04 x86-64 bundle: %s\n' "$output_dir"
du -sh -- "$output_dir"
