#!/usr/bin/env bash
# Build unmodified LLVM/MLIR components for one genuine device-host ABI.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
profile=${1:?Usage: bootstrap-consumer-sdk.sh x86_64|i686 [output-directory]}
[[ $# -le 2 && $profile =~ ^(x86_64|i686)$ ]] || { echo 'Expected x86_64 or i686' >&2; exit 2; }
[[ $(uname -m) == x86_64 ]] || { echo 'Bootstrap currently requires an x86-64 Linux build host.' >&2; exit 2; }
for utility in curl sha256sum dpkg-deb flock realpath tar readelf python3; do
  command -v "$utility" >/dev/null || { echo "Missing utility: $utility" >&2; exit 1; }
done
publisher=$(realpath -- "${NIER_BUILD_SDK_ROOT:-$repo/.sdk}")
destination=$(realpath -m -- "${2:-$repo/.sdk/consumer/$profile}")
case "$destination" in /|/usr|/usr/local|"$repo"|"$publisher"|"${HOME:-/nonexistent}")
  echo 'Output must be a dedicated consumer SDK directory.' >&2; exit 2;; esac
work="$repo/.sdk/consumer"
[[ $repo != *[[:space:]]* && $publisher != *[[:space:]]* && $destination != *[[:space:]]* ]] || {
  echo 'This source-SDK bootstrap requires checkout and SDK paths without whitespace.' >&2; exit 2;
}
mkdir -p "$destination"
exec 9>"$destination/bootstrap.lock"
flock 9
mirror=${NIER_CONSUMER_UBUNTU_MIRROR:-https://snapshot.ubuntu.com/ubuntu/20260910T000000Z}
definition="$repo/sdk/consumer"
python3 "$definition/receipt.py" check-build "$destination" "$work" "$profile" "$publisher"
python3 "$definition/receipt.py" claim "$destination" "$profile"
python3 "$definition/receipt.py" invalidate "$destination"
mkdir -p "$work/downloads" "$work/source" "$destination/receipts" "$destination/host"
compile_jobs=${NIER_CONSUMER_COMPILE_JOBS:-2}
[[ $compile_jobs =~ ^[12]$ ]] || { echo 'NIER_CONSUMER_COMPILE_JOBS must be 1 or 2' >&2; exit 2; }
source_lock="$definition/source.lock"
packages_lock="$definition/packages.lock"
host_llvm="$publisher/host/usr/lib/llvm-18"
cmake="$publisher/host/usr/bin/cmake"
ninja="$publisher/host/usr/bin/ninja"
for executable in "$host_llvm/bin/clang" "$host_llvm/bin/clang++" "$host_llvm/bin/ld.lld" "$cmake" "$ninja"; do
  [[ -x $executable ]] || { echo "Bootstrap the publisher SDK first; missing $executable" >&2; exit 1; }
done
# Only build-host programs inherit these paths. Never run an ELF32 program with
# the amd64 loader search path; its probe below gets an explicit clean one.
export LD_LIBRARY_PATH="$host_llvm/lib:$publisher/host/usr/lib/x86_64-linux-gnu"
export PATH="$host_llvm/bin:$publisher/host/usr/bin:/usr/bin:/bin"
unset CMAKE_PREFIX_PATH LLVM_DIR MLIR_DIR Clang_DIR CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH LIBRARY_PATH
triple="$profile-linux-gnu"
multiarch=x86_64-linux-gnu
[[ $profile == i686 ]] && multiarch=i386-linux-gnu
sysroot="$destination/sysroots/$triple"
prefix="$destination/host/usr/lib/llvm-18"
mkdir -p "$sysroot" "$prefix/bin" "$prefix/lib" "$destination/host/usr/lib/$multiarch"
fetch() {
  local digest=$1 url=$2 output=$3
  if [[ -f $output ]] && [[ $(sha256sum "$output" | cut -d ' ' -f 1) == "$digest" ]]; then return; fi
  curl --fail --silent --show-error --location --retry 3 --connect-timeout 20 --output "$output.part" "$url"
  [[ $(sha256sum "$output.part" | cut -d ' ' -f 1) == "$digest" ]] || { echo "SHA256 mismatch: $url" >&2; exit 1; }
  mv -- "$output.part" "$output"
}
read -r source_name source_version source_digest source_url < <(awk '!/^#/ && NF {print; exit}' "$source_lock")
[[ $source_name == llvm-project && $source_version == 18.1.3 && $source_digest =~ ^[0-9a-f]{64}$ ]] || exit 1
source_archive="$work/downloads/${source_url##*/}"
exec 7>"$work/source-extraction.lock"
flock 7
fetch "$source_digest" "$source_url" "$source_archive"
source_tree="$work/source/llvm-project-$source_version.src"
if [[ ! -d $source_tree ]]; then tar -xf "$source_archive" -C "$work/source"; fi
flock -u 7
while read -r lane package arch version digest filename extra; do
  [[ -z ${lane:-} || $lane == \#* || $lane != "$profile" ]] && continue
  [[ $digest =~ ^[0-9a-f]{64}$ && $filename == pool/* && -z ${extra:-} ]] || { echo 'Malformed dependency lock' >&2; exit 1; }
  archive="$work/downloads/${filename##*/}"
  if [[ ! -f $archive && -f $publisher/downloads/${filename##*/} ]]; then
    cp -- "$publisher/downloads/${filename##*/}" "$archive"
  fi
  printf 'Dependency %s %s (%s)\n' "$package" "$version" "$arch"
  fetch "$digest" "$mirror/$filename" "$archive"
  receipt="$destination/receipts/$package-$digest"
  if [[ ! -f $receipt ]]; then
    dpkg-deb --extract "$archive" "$sysroot"
    # Keep a separate host-runtime/development prefix for existing SDK users.
    # Distribution packaging selects actual ELF dependencies, not this whole tree.
    dpkg-deb --extract "$archive" "$destination/host"
    touch "$receipt"
  fi
done < "$packages_lock"
for alias in "$sysroot/lib" "$destination/host/lib"; do
  if [[ ! -e $alias && ! -L $alias ]]; then ln -s usr/lib "$alias"; fi
done
if [[ $profile == x86_64 && ! -e $sysroot/lib64 && ! -L $sysroot/lib64 ]]; then ln -s usr/lib64 "$sysroot/lib64"; fi
# The pinned publisher compiler-rt package includes unmodified runtimes for both
# widths. Copy only this selected target's builtins/CRT, never Clang/frontends.
runtime="$prefix/lib/clang/18/lib/linux"
mkdir -p "$runtime"
runtime_arch=$profile
[[ $profile == i686 ]] && runtime_arch=i386
for file in "libclang_rt.builtins-$runtime_arch.a" "clang_rt.crtbegin-$runtime_arch.o" "clang_rt.crtend-$runtime_arch.o"; do
  cp -- "$host_llvm/lib/clang/18/lib/linux/$file" "$runtime/$file"
done
# Distribution inputs deliberately exclude this compiler-build sysroot's C++
# headers and GCC/libarchive development archives. The native C runtime remains
# exactly the already qualified publisher baseline, with no host-ABI mixing.
native_runtime="$destination/native-runtime"
mkdir -p "$native_runtime/usr" "$destination/licenses"
cp -a -- "$publisher/sysroots/$triple/usr/lib" "$native_runtime/usr/"
if [[ -d $publisher/sysroots/$triple/usr/lib64 ]]; then
  cp -a -- "$publisher/sysroots/$triple/usr/lib64" "$native_runtime/usr/"
fi
if [[ ! -e $native_runtime/lib && ! -L $native_runtime/lib ]]; then ln -s usr/lib "$native_runtime/lib"; fi
if [[ $profile == x86_64 && ! -e $native_runtime/lib64 && ! -L $native_runtime/lib64 ]]; then ln -s usr/lib64 "$native_runtime/lib64"; fi
cp -- "$source_tree/llvm/LICENSE.TXT" "$destination/licenses/llvm-LICENSE.txt"
cp -- "$source_tree/mlir/LICENSE.TXT" "$destination/licenses/mlir-LICENSE.txt"
cp -- "$source_tree/lld/LICENSE.TXT" "$destination/licenses/lld-LICENSE.txt"
cp -- "$source_tree/llvm/lib/Support/BLAKE3/LICENSE" "$destination/licenses/llvm-BLAKE3-LICENSE.txt"
cp -- "$source_tree/llvm/lib/Support/COPYRIGHT.regex" "$destination/licenses/llvm-regex-COPYRIGHT.txt"
cp -L -- "$publisher/host/usr/share/doc/llvm-18/copyright" "$destination/licenses/llvm-package-copyright.txt"
cp -L -- "$publisher/host/usr/share/doc/libclang-rt-18-dev/copyright" "$destination/licenses/compiler-rt-copyright.txt"
pointer_bytes=8
[[ $profile == i686 ]] && pointer_bytes=4
"$host_llvm/bin/clang++" --target="$profile-unknown-linux-gnu" --sysroot="$sysroot" \
  "--gcc-install-dir=$sysroot/usr/lib/gcc/$triple/13" -std=c++17 -fuse-ld="$host_llvm/bin/ld.lld" \
  "-Wl,-rpath-link,$sysroot/usr/lib/$multiarch" -DNIER_EXPECT_POINTER_BYTES="$pointer_bytes" \
  "$definition/abi-probe.cpp" -larchive -o "$destination/abi-probe"
loader="$sysroot/usr/lib/$multiarch/ld-linux-x86-64.so.2"
[[ $profile == i686 ]] && loader="$sysroot/usr/lib/$multiarch/ld-linux.so.2"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$loader" --library-path "$sysroot/usr/lib/$multiarch" "$destination/abi-probe"
# Permit dependency preparation for the other profile in parallel, but keep the
# combined heavyweight source build at two compile jobs and one link job.
exec 6>"$work/compile.lock"
flock 6
# Another bootstrap may have initialized this shared cache while dependencies
# were prepared. Recheck under the build lock before any CMake reuse.
python3 "$definition/receipt.py" check-build "$destination" "$work" "$profile" "$publisher"
native_build="$work/build-native-generators"
build="$work/build-$profile"
common=(-G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_BUILD_TYPE=Release
  '-DLLVM_ENABLE_PROJECTS=mlir;lld' -DLLVM_TARGETS_TO_BUILD=X86
  -DBUILD_SHARED_LIBS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF
  -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_LTO=OFF
  -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF
  -DLLVM_INCLUDE_BENCHMARKS=OFF -DMLIR_ENABLE_BINDINGS_PYTHON=OFF
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBXML2=OFF
  -DLLVM_ENABLE_FFI=OFF -DLLVM_ENABLE_ZLIB=FORCE_ON -DLLVM_ENABLE_ZSTD=FORCE_ON
  -DLLVM_PARALLEL_COMPILE_JOBS=2 -DLLVM_PARALLEL_LINK_JOBS=1
  -DLLVM_APPEND_VC_REV=OFF -DLLVM_ENABLE_WARNINGS=OFF)
# TableGen is a build-host tool, built from the exact same upstream source. It
# is not installed in either device SDK and does not run on the device.
(
  exec 8>"$work/native-generators.lock"
  flock 8
  "$cmake" -S "$source_tree/llvm" -B "$native_build" "${common[@]}" \
    -DCMAKE_C_COMPILER="$host_llvm/bin/clang" -DCMAKE_CXX_COMPILER="$host_llvm/bin/clang++" \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=$host_llvm/bin/ld.lld" \
    -DCMAKE_PREFIX_PATH="$publisher/host/usr" -DLLVM_HOST_TRIPLE=x86_64-unknown-linux-gnu
  "$cmake" --build "$native_build" --parallel "$compile_jobs" --target llvm-min-tblgen llvm-tblgen mlir-tblgen
)
"$cmake" -S "$source_tree/llvm" -B "$build" "${common[@]}" \
  -DCMAKE_TOOLCHAIN_FILE="$definition/toolchain.cmake" \
  -DNIER_SDK_ROOT="$destination" -DNIER_DEVICE_TARGET="$profile" -DNIER_BUILD_SDK_ROOT="$publisher" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON "-DCMAKE_INSTALL_RPATH=\$ORIGIN/../lib;\$ORIGIN/../../$multiarch" \
  -DLLVM_HOST_TRIPLE="$profile-unknown-linux-gnu" -DLLVM_DEFAULT_TARGET_TRIPLE="$profile-unknown-linux-gnu" \
  -DLLVM_NATIVE_TOOL_DIR="$native_build/bin" \
  -DLLVM_TABLEGEN="$native_build/bin/llvm-tblgen" \
  -DLLVM_HEADERS_TABLEGEN="$native_build/bin/llvm-min-tblgen" \
  -DMLIR_TABLEGEN="$native_build/bin/mlir-tblgen" \
  -DZLIB_ROOT="$sysroot/usr" -Dzstd_DIR="$sysroot/usr/lib/$multiarch/cmake/zstd"
# Prove the actual 32-bit MLIR context/parser link before the larger LLVM tools.
"$cmake" --build "$build" --parallel "$compile_jobs" --target MLIRIR MLIRParser MLIRBytecodeReader MLIRBytecodeWriter LLVMLinker LLVMIRReader
if [[ $profile == i686 ]]; then
  probe="$build/mlir32-probe"
  "$host_llvm/bin/clang++" --target=i686-unknown-linux-gnu --sysroot="$sysroot" \
    "--gcc-install-dir=$sysroot/usr/lib/gcc/$triple/13" -std=c++17 -fuse-ld="$host_llvm/bin/ld.lld" \
    -I"$source_tree/llvm/include" -I"$build/include" -I"$source_tree/mlir/include" -I"$build/tools/mlir/include" \
    "$definition/mlir32-probe.cpp" -o "$probe" -L"$build/lib" \
    -Wl,--start-group -lMLIRParser -lMLIRAsmParser -lMLIRBytecodeReader -lMLIRBytecodeOpInterface \
    -lMLIRIR -lMLIRSupport -lLLVMSupport -lLLVMDemangle -Wl,--end-group -lz -lzstd -lpthread -ldl -lm
  readelf -h "$probe" | grep -q 'Class:.*ELF32'
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$loader" --library-path "$sysroot/usr/lib/$multiarch" "$probe"
fi
python3 "$definition/receipt.py" prepare "$destination" "$build" "$profile" "$publisher"
"$cmake" --build "$build" --parallel "$compile_jobs" --target opt llc llvm-ar lld
for tool in opt llc llvm-ar lld; do
  cp -- "$build/bin/$tool" "$prefix/bin/$tool"
done
ln -sfn lld "$prefix/bin/ld.lld"
expected_class=ELF64
[[ $profile == i686 ]] && expected_class=ELF32
for tool in opt llc llvm-ar ld.lld; do
  readelf -h "$prefix/bin/$tool" | grep -q "Class:.*$expected_class"
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$loader" --library-path "$prefix/lib:$destination/host/usr/lib/$multiarch" "$prefix/bin/$tool" --version
done
python3 "$definition/receipt.py" complete "$destination" "$build" "$profile" "$publisher"
printf '\nConsumer SDK ready: %s\nUse -C %s/development.cmake and sdk/consumer/toolchain.cmake.\n' "$destination" "$destination"
