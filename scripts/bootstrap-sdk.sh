#!/usr/bin/env bash
# Extract unmodified, SHA256-pinned Ubuntu packages into a user-owned SDK.
set -euo pipefail
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk_root=${SELA_SDK_ROOT:-$repo_root/.sdk}
# The rolling archive removes superseded packages. This official snapshot
# retains every exact package in sdk/packages.lock; hashes remain authoritative.
mirror=${SDK_UBUNTU_MIRROR:-https://snapshot.ubuntu.com/ubuntu/20260910T000000Z}
lock="$repo_root/sdk/packages.lock"
for command in curl sha256sum dpkg-deb flock realpath python3; do
  command -v "$command" >/dev/null || { echo "Required host utility missing: $command" >&2; exit 1; }
done
build_host=$(python3 "$repo_root/sdk/targets.py" host)
build_arch=$(python3 "$repo_root/sdk/targets.py" get "$build_host" packageArch)
[[ -f $lock ]] || { echo "Missing checked-in lock: $lock" >&2; exit 1; }
sdk_root=$(realpath -m -- "$sdk_root")
case "$sdk_root" in
  /|/usr|/usr/local|"$repo_root"|"${HOME:-/nonexistent}")
    echo 'SELA_SDK_ROOT must identify a dedicated SDK directory.' >&2; exit 1 ;;
esac
mkdir -p -- "$sdk_root/downloads" "$sdk_root/receipts" "$sdk_root/host" "$sdk_root/sysroots"
sdk_root=$(cd -- "$sdk_root" && pwd -P)
exec 9>"$sdk_root/bootstrap.lock"
flock 9
lock_digest=$(sha256sum "$lock" | cut -d ' ' -f 1)
if [[ -f $sdk_root/sdk-lock.sha256 ]] && [[ $(< "$sdk_root/sdk-lock.sha256") != "$lock_digest" ]]; then
  echo 'Pre-alpha SDK lock changed: extracting current pinned packages; rebuild Sela tools afterward.' >&2
fi
while read -r lane package arch version digest filename extra; do
  [[ -z ${lane:-} || $lane == \#* ]] && continue
  [[ $digest =~ ^[0-9a-f]{64}$ && $filename == pool/* && -z ${extra:-} ]] || { echo 'Malformed SDK lock row' >&2; exit 1; }
  case "$lane" in
    host)
      [[ $arch == "$build_arch" || $arch == all ]] || {
        echo "No qualified $build_host publisher SDK lock is supplied (found $arch host packages). Device cross-builds do not require a target-host SDK." >&2; exit 1;
      }
      destination="$sdk_root/host" ;;
    *)
      matched=false
      while IFS= read -r profile; do
        if [[ $lane == "$(python3 "$repo_root/sdk/targets.py" get "$profile" sysrootTriple)" ]]; then matched=true; break; fi
      done < <(python3 "$repo_root/sdk/targets.py" list)
      [[ $matched == true ]] || { echo "Unknown SDK lane: $lane" >&2; exit 1; }
      destination="$sdk_root/sysroots/$lane" ;;
  esac
  archive="$sdk_root/downloads/${filename##*/}"
  receipt="$sdk_root/receipts/$lane-$package-$arch-$digest"
  if [[ ! -f $archive ]] || [[ $(sha256sum "$archive" | cut -d ' ' -f 1) != "$digest" ]]; then
    printf 'Downloading %s %s (%s)\n' "$package" "$version" "$arch"
    curl --fail --silent --show-error --location --retry 3 --connect-timeout 20 --output "$archive.part" "$mirror/$filename"
    [[ $(sha256sum "$archive.part" | cut -d ' ' -f 1) == "$digest" ]] || { echo "SHA256 mismatch: $filename" >&2; exit 1; }
    mv -- "$archive.part" "$archive"
  fi
  if [[ ! -f $receipt ]]; then
    printf 'Extracting %s into %s\n' "$package" "$lane"
    mkdir -p -- "$destination"
    dpkg-deb --extract "$archive" "$destination"
    touch -- "$receipt"
  fi
done < "$lock"
# Verify the frontend extension development surface in the extracted SDK. The
# stock compiler and libraries remain unmodified; headers come from its exact
# matching Ubuntu development packages.
[[ -f $sdk_root/host/usr/lib/llvm-18/include/clang/Frontend/FrontendAction.h ]] || { echo 'Missing pinned Clang plugin headers' >&2; exit 1; }
[[ -f $sdk_root/host/usr/lib/llvm-18/lib/libclang-cpp.so ]] || { echo 'Missing pinned Clang plugin library' >&2; exit 1; }
# Ubuntu's usr-merged filesystem aliases are normally provided by the installed
# root filesystem, not these development packages. Recreate only the aliases;
# no compiler, loader, ELF binary, header, or linker script is patched.
ensure_alias() {
  local link=$1 target=$2
  if [[ -e $link || -L $link ]]; then
    [[ -L $link && $(readlink "$link") == "$target" ]] || { echo "Unexpected SDK path: $link" >&2; exit 1; }
  else
    ln -s -- "$target" "$link"
  fi
}
# Stable build-host runtime path for presets; its architecture is selected from
# actual host metadata instead of embedding x86 paths in every build consumer.
host_multiarch=$(python3 "$repo_root/sdk/targets.py" get "$build_host" multiarch)
ensure_alias "$sdk_root/host/runtime" "usr/lib/$host_multiarch"
while IFS= read -r profile; do
  eval "$(python3 "$repo_root/sdk/targets.py" shell "$profile")"
  target_root="$sdk_root/sysroots/$SELA_TARGET_SYSROOT_TRIPLE"
  ensure_alias "$target_root/lib" usr/lib
  if [[ -d $target_root/usr/lib64 ]]; then ensure_alias "$target_root/lib64" usr/lib64; fi
  # Foreign compiler-rt packages contain data/link inputs, never build-host
  # executables. Add just each admitted target's C runtime to host Clang.
  target_runtime="$target_root/usr/lib/llvm-18/lib/clang/18/lib/linux"
  if [[ -d $target_runtime ]]; then
    host_runtime="$sdk_root/host/usr/lib/llvm-18/lib/clang/18/lib/linux"
    for file in "libclang_rt.builtins-$SELA_TARGET_COMPILER_RT_ARCH.a" "clang_rt.crtbegin-$SELA_TARGET_COMPILER_RT_ARCH.o" "clang_rt.crtend-$SELA_TARGET_COMPILER_RT_ARCH.o"; do
      cp -- "$target_runtime/$file" "$host_runtime/$file"
    done
  fi
done < <(python3 "$repo_root/sdk/targets.py" list)
printf '%s\n' "$lock_digest" > "$sdk_root/sdk-lock.sha256"
printf '\nSDK ready at %s\nSource %s/sdk/env.sh to select it.\n' "$sdk_root" "$repo_root"
