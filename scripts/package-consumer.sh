#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
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
test -f "$sdk_root/consumer-sdk.json" || {
    printf 'Build a target-specific SDK with scripts/bootstrap-consumer-sdk.sh first.\n' >&2; exit 1;
}
sdk_description=$(python3 - "$sdk_root" "$repository" <<'PY'
import hashlib, json, pathlib, re, subprocess, sys
root = pathlib.Path(sys.argv[1])
receipt = json.loads((root / 'consumer-sdk.json').read_text())
identity_fields = {key: value for key, value in receipt.items() if key not in ('sdk_identity', 'tools', 'runtime_libraries')}
identity = hashlib.sha256(json.dumps(identity_fields, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
if identity != (root / 'sdk-lock.sha256').read_text().strip() or receipt.get('sdk_identity') != identity:
    raise SystemExit('Consumer SDK receipt mismatch')
linkage = receipt.get('linkage')
if receipt.get('targets') != ['X86'] or linkage not in ('static-components', 'shared'):
    raise SystemExit('Expected a pinned X86-only consumer SDK')
for name in ('source.lock', 'packages.lock'):
    copied_input = pathlib.Path(sys.argv[2]) / 'sdk/consumer' / name
    if hashlib.sha256(copied_input.read_bytes()).hexdigest() != receipt['inputs'][name]:
        raise SystemExit('Consumer SDK input lock changed; rebuild the SDK: ' + name)
if set(receipt['tools']) != {'opt', 'llc', 'llvm-ar', 'lld'}:
    raise SystemExit('Consumer SDK tool inventory mismatch')
for name, expected in receipt['tools'].items():
    if hashlib.sha256((root / 'host/usr/lib/llvm-18/bin' / name).read_bytes()).hexdigest() != expected:
        raise SystemExit('Consumer SDK tool changed: ' + name)
if hashlib.sha256((root / 'host/usr/lib/llvm-18/bin/ld.lld').read_bytes()).hexdigest() != receipt['tools']['lld']:
    raise SystemExit('Consumer SDK ld.lld alias does not match the verified lld')
libraries = receipt.get('runtime_libraries')
if not isinstance(libraries, dict) or len(libraries) != (1 if linkage == 'shared' else 0):
    raise SystemExit('Consumer SDK LLVM runtime inventory mismatch')
soname = '-'
for soname, record in libraries.items():
    if not re.fullmatch(r'libLLVM[A-Za-z0-9_.+-]*', soname):
        raise SystemExit('Invalid LLVM runtime SONAME')
    filename = pathlib.PurePosixPath(record['file'])
    if filename.parent != pathlib.PurePosixPath('host/usr/lib/llvm-18/lib') or not re.fullmatch(r'libLLVM[A-Za-z0-9_.+-]*', filename.name):
        raise SystemExit('Invalid LLVM runtime filename')
    directory = root / 'host/usr/lib/llvm-18/lib'
    library = (root / filename).resolve(strict=True)
    if library.parent != directory.resolve() or (directory / soname).resolve(strict=True) != library:
        raise SystemExit('LLVM runtime alias escaped or does not match its receipt')
    if hashlib.sha256(library.read_bytes()).hexdigest() != record['sha256']:
        raise SystemExit('Consumer SDK LLVM runtime changed: ' + str(filename))
    dynamic = subprocess.check_output(['readelf', '-d', str(library)], text=True)
    if not re.search(r'\(SONAME\).*\[' + re.escape(soname) + r'\]', dynamic):
        raise SystemExit('LLVM runtime SONAME does not match its receipt')
print(receipt['profile'], linkage, soname)
PY
)
read -r profile linkage llvm_soname <<< "$sdk_description"
case "$profile" in
    x86_64) multiarch=x86_64-linux-gnu; foreign_multiarch=i386-linux-gnu; runtime_arch=x86_64; elf_class=ELF64; elf_machine='Advanced Micro Devices X86-64' ;;
    i686) multiarch=i386-linux-gnu; foreign_multiarch=x86_64-linux-gnu; runtime_arch=i386; elf_class=ELF32; elf_machine='Intel 80386' ;;
    *) printf 'Unsupported consumer SDK target: %s\n' "$profile" >&2; exit 1 ;;
esac
built_target=$(sed -n 's/^NIER_DEVICE_TARGET:STRING=//p' "$build_dir/CMakeCache.txt")
if [[ $built_target != "$profile" ]]; then
    printf 'Compiler target %s does not match SDK target %s\n' "$built_target" "$profile" >&2; exit 1
fi
component_linking=$(sed -n 's/^NIER_LLVM_COMPONENT_LINKING:BOOL=//p' "$build_dir/CMakeCache.txt")
consumer_sdk=$(sed -n 's/^NIER_USE_CONSUMER_SDK:BOOL=//p' "$build_dir/CMakeCache.txt")
case "${consumer_sdk^^}" in
    ON|TRUE|YES|Y|1) ;;
    *) printf 'Compiler must be configured with NIER_USE_CONSUMER_SDK enabled\n' >&2; exit 1 ;;
esac
case "$linkage:${component_linking^^}" in
    static-components:ON|shared:OFF) ;;
    *) printf 'Compiler LLVM linkage does not match its consumer SDK\n' >&2; exit 1 ;;
esac
cmake_program=$(sed -n 's/^CMAKE_COMMAND:INTERNAL=//p' "$build_dir/CMakeCache.txt")
test -x "$cmake_program"
cmake_prefix=$(dirname -- "$(dirname -- "$cmake_program")")
build_sdk=$(sed -n 's/^NIER_BUILD_SDK_ROOT:PATH=//p' "$build_dir/CMakeCache.txt")
strip_tool="$build_sdk/host/usr/lib/llvm-18/bin/llvm-strip"
test -x "$strip_tool" || { printf 'Missing pinned build-host llvm-strip: %s\n' "$strip_tool" >&2; exit 1; }
stage=$(mktemp -d "$output_parent/.nier-consumer-stage-XXXXXX")
trap 'status=$?; if (( status )); then printf "Incomplete private bundle retained at: %s\n" "$stage" >&2; fi' EXIT
env LD_LIBRARY_PATH="$cmake_prefix/lib/llvm-18/lib:$cmake_prefix/lib/x86_64-linux-gnu" \
    "$cmake_program" --install "$build_dir" --prefix "$stage" --component Consumer
test -x "$stage/bin/nierc"
readelf -d "$stage/bin/nierc" | rg -Fq '(RPATH)'
readelf -d "$stage/bin/nierc" | rg -Fq '$ORIGIN/../sdk/host/usr/lib/llvm-18/lib'

llvm_relative=host/usr/lib/llvm-18
bundle_bin="$stage/sdk/$llvm_relative/bin"
bundle_lib="$stage/sdk/$llvm_relative/lib"
mkdir -p "$bundle_bin" "$bundle_lib" "$stage/licenses"
queue=("$stage/bin/nierc")
strip_files=("$stage/bin/nierc")
for tool in opt llc ld.lld llvm-ar; do
    original="$sdk_root/$llvm_relative/bin/$tool"
    cp -Lp -- "$original" "$bundle_bin/$tool"
    cmp -- "$original" "$bundle_bin/$tool"
    queue+=("$bundle_bin/$tool")
    strip_files+=("$bundle_bin/$tool")
done

# The host loader/glibc family is the explicit Ubuntu 24.04 baseline. Every
# other ELF dependency must come from the pinned extracted SDK, never ldd's
# opportunistic host resolution. Resolve library symlinks while copying so the
# flattened runtime directory has no links escaping the bundle.
declare -A copied=() notices=([libarchive13t64]=1)
for (( index=0; index<${#queue[@]}; ++index )); do
    header=$(readelf -h -- "${queue[index]}")
    if ! rg -q "Class:.*$elf_class" <<< "$header"; then
        printf 'Wrong ELF class in %s: expected %s\n' "${queue[index]}" "$elf_class" >&2; exit 1
    fi
    if ! rg -q "Machine:.*$elf_machine" <<< "$header"; then
        printf 'Wrong ELF machine in %s: expected %s\n' "${queue[index]}" "$elf_machine" >&2; exit 1
    fi
    dynamic=$(readelf -d -- "${queue[index]}")
    needed=$(sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p' <<< "$dynamic")
    while IFS= read -r library; do
        [[ -n $library ]] || continue
        case "$library" in
            libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libresolv.so.2|libutil.so.1|ld-linux-x86-64.so.2|ld-linux.so.2) continue ;;
            libLLVM*)
                if [[ $linkage != shared || $library != "$llvm_soname" ]]; then
                    printf 'Unexpected LLVM runtime dependency: %s\n' "$library" >&2; exit 1
                fi ;;
        esac
        [[ $library =~ ^[a-zA-Z0-9_.+-]+$ ]]
        [[ ${copied[$library]:-} ]] && continue
        original=
        for directory in "$sdk_root/$llvm_relative/lib" "$sdk_root/host/usr/lib/$multiarch"; do
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
            libLLVM*) strip_files+=("$bundle_lib/$library"); continue ;;
            libarchive*) package=libarchive13t64 ;;
            libstdc++*) package=libstdc++6 ;;
            libgcc_s*) package=libgcc-s1 ;;
            libatomic*) package=libatomic1 ;;
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
if [[ $linkage == shared && ! ${copied[$llvm_soname]:-} ]]; then
    printf 'Shared LLVM SDK was not used by the compiler package\n' >&2; exit 1
fi

# Keep SDK/build products unmodified. Their receipt hashes establish input
# provenance; payload.sha256 below describes the stripped distribution bytes.
# Do not remove unwind data, dynamic symbols, or runtime/archive link inputs.
for file in "${strip_files[@]}"; do
    env LD_LIBRARY_PATH="$build_sdk/host/usr/lib/llvm-18/lib:$build_sdk/host/usr/lib/x86_64-linux-gnu" \
        "$strip_tool" --strip-all "$file"
done

# Native runtime staging excludes GCC/C++ development libraries used only to
# build the compiler. Retain the qualified C runtime set, including CRTs/NSS.
native_source="$sdk_root/native-runtime"
test -d "$native_source/usr/lib"
if [[ -e $native_source/usr/lib/$foreign_multiarch || -L $native_source/usr/lib/$foreign_multiarch ]]; then
    printf 'Opposite-architecture libraries leaked into the native runtime staging tree\n' >&2; exit 1
fi
target="$stage/sdk/sysroots/$profile-linux-gnu"
mkdir -p "$target/usr"
cp -a -- "$native_source/usr/lib" "$target/usr/lib"
ln -s usr/lib "$target/lib"
if [[ $profile == x86_64 ]]; then
    cp -a -- "$native_source/usr/lib64" "$target/usr/lib64"
    ln -s usr/lib64 "$target/lib64"
fi
runtime="$bundle_lib/clang/18/lib/linux"
mkdir -p "$runtime"
for name in "libclang_rt.builtins-$runtime_arch.a" "clang_rt.crtbegin-$runtime_arch.o" "clang_rt.crtend-$runtime_arch.o"; do
    cp -Lp -- "$sdk_root/$llvm_relative/lib/clang/18/lib/linux/$name" "$runtime/$name"
done
cp -a -- "$sdk_root/licenses/." "$stage/licenses/"
for package in "${!notices[@]}"; do
    cp -Lp -- "$sdk_root/host/usr/share/doc/$package/copyright" "$stage/licenses/$package.copyright"
done
for package in libc6 libc6-dev libcrypt1 libcrypt-dev; do
    cp -Lp -- "$sdk_root/sysroots/$profile-linux-gnu/usr/share/doc/$package/copyright" "$stage/licenses/$package.copyright"
done
cp -- "$sdk_root/sdk-lock.sha256" "$stage/sdk/sdk-lock.sha256"
cp -- "$sdk_root/consumer-sdk.json" "$stage/sdk/consumer-sdk.json"
cp -- "$repository/sdk/consumer/packages.lock" "$stage/sdk/packages.lock"
cp -- "$repository/sdk/consumer/source.lock" "$stage/sdk/source.lock"
cp -- "$repository/LICENSE" "$stage/LICENSE"
cp -- "$repository/docs/reference/compiler-distribution.md" "$stage/README.md"
test "$(env -u LD_PRELOAD LD_LIBRARY_PATH="$bundle_lib" "$stage/bin/nierc" --print-target)" = "$profile"
env -u NIER_SDK_ROOT -u LD_PRELOAD LD_LIBRARY_PATH="$bundle_lib" "$stage/bin/nierc" --check-sdk
registered=$(env -u LD_PRELOAD LD_LIBRARY_PATH="$bundle_lib" "$bundle_bin/llc" --version)
if ! rg -q 'Registered Targets:' <<< "$registered" || \
    sed -n '/Registered Targets:/,$p' <<< "$registered" | sed '1d' | \
      rg -v '^\s*(x86|x86-64)\s+-|^\s*$'; then
    printf 'Unexpected LLVM backend inventory in device package\n' >&2; exit 1
fi
if rg --files --hidden --no-ignore "$stage" | rg '/include/|/cmake/|/bin/(clang|clang-[0-9]+|nier-build|nier-ld|nier-native-ld)$|libclang-cpp|libnier-clang|nier-capture'; then
    printf 'Publisher/development files leaked into device package\n' >&2; exit 1
fi
du -sb -- "$stage/bin" "$stage/sdk/host" "$stage/sdk/sysroots" "$stage/licenses"
(
    cd -- "$stage"
    while IFS= read -r -d '' file; do sha256sum -- "$file"; done \
        < <(rg --files --hidden --no-ignore -g '!payload.sha256' -0 | sort -z)
) > "$stage/payload.sha256"
mv -T -- "$stage" "$output_dir"
printf 'Target-specific %s compiler bundle: %s\n' "$profile" "$output_dir"
du -sh -- "$output_dir"
python3 "$repository/scripts/bundle-size.py" "$output_dir"
