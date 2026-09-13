#!/usr/bin/env bash
# Maintainer operation: emit a lock from already downloaded, authenticated APT
# indexes. Does not refresh APT, install packages, or write the lock itself.
set -euo pipefail
sdk_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
apt_lists=${SDK_APT_LISTS:-/var/lib/apt/lists}
archive_prefix=${SDK_APT_ARCHIVE_PREFIX:-il.archive.ubuntu.com_ubuntu}
keyring=${SDK_UBUNTU_KEYRING:-/usr/share/keyrings/ubuntu-archive-keyring.gpg}
verified_indexes=()
mapfile -t architectures < <(awk '!/^#/ && NF {if (index($2, ":")) {split($2, parts, ":"); print parts[2]}}' "$sdk_dir/packages.txt" | sort -u)
for suite in noble noble-updates; do
  release="$apt_lists/${archive_prefix}_dists_${suite}_InRelease"
  gpgv --keyring "$keyring" "$release" >&2
  for component in main universe; do
    for arch in "${architectures[@]}"; do
      index="$apt_lists/${archive_prefix}_dists_${suite}_${component}_binary-${arch}_Packages"
      relative="$component/binary-$arch/Packages"
      expected=$(awk -v path="$relative" '$1 == "SHA256:" { hashes=1; next } hashes && $3 == path {print $1; exit}' "$release")
      actual=$(sha256sum "$index")
      [[ ${actual%% *} == "$expected" ]] || { echo "Unauthenticated index: $index" >&2; exit 1; }
      verified_indexes+=("$index")
    done
  done
done
printf '# SDK package lock v1: lane package architecture version sha256 repository-path\n'
printf '# Ubuntu noble/noble-updates; InRelease signatures and index SHA256 verified.\n'
while read -r lane requested rest; do
  [[ -z ${lane:-} || $lane == \#* ]] && continue
  metadata=$(apt-cache show --no-all-versions "$requested")
  package=$(awk '/^Package:/ {print $2; exit}' <<< "$metadata")
  arch=$(awk '/^Architecture:/ {print $2; exit}' <<< "$metadata")
  version=$(awk '/^Version:/ {print $2; exit}' <<< "$metadata")
  digest=$(awk '/^SHA256:/ {print $2; exit}' <<< "$metadata")
  filename=$(awk '/^Filename:/ {print $2; exit}' <<< "$metadata")
  [[ $digest =~ ^[0-9a-f]{64}$ && $filename == pool/* ]] || { echo "Missing repository metadata: $requested" >&2; exit 1; }
  # Match the complete package identity in the authenticated index, not just an
  # arbitrary apt-cache candidate from another configured repository.
  awk -v p="$package" -v a="$arch" -v v="$version" -v h="$digest" -v f="$filename" '
    BEGIN { RS=""; FS="\n" }
    { delete field; for (i=1; i<=NF; i++) { n=index($i,": "); if(n) field[substr($i,1,n-1)]=substr($i,n+2) }
      if(field["Package"]==p && field["Architecture"]==a && field["Version"]==v && field["SHA256"]==h && field["Filename"]==f) found=1 }
    END { exit !found }
  ' "${verified_indexes[@]}" || { echo "Candidate absent from authenticated Ubuntu indexes: $requested" >&2; exit 1; }
  printf '%s %s %s %s %s %s\n' "$lane" "$package" "$arch" "$version" "$digest" "$filename"
done < "$sdk_dir/packages.txt"
