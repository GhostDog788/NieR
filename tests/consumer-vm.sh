#!/usr/bin/env bash
# Boot a real 32-bit Linux kernel; never substitute user-mode emulation.
set -euo pipefail
if [[ $# != 2 ]]; then
    printf 'Usage: bash tests/consumer-vm.sh I686_BUNDLE FIXTURE_DIRECTORY\n' >&2
    exit 2
fi
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bundle=$(realpath -e -- "$1")
fixtures=$(realpath -e -- "$2")
for command in qemu-system-i386 cpio gzip curl dpkg-deb sha256sum readelf timeout; do
    command -v "$command" >/dev/null || { printf 'Required VM host tool: %s\n' "$command" >&2; exit 1; }
done
readelf -h "$bundle/bin/nierc" | grep -q 'Class:.*ELF32'
readelf -h "$bundle/bin/nierc" | grep -q 'Machine:.*Intel 80386'
readelf -h "$fixtures/exec-format" | grep -q 'Class:.*ELF32'
readelf -h "$fixtures/elf64-negative" | grep -q 'Class:.*ELF64'
test -f "$fixtures/fixtures.sha256"
test -d "$bundle/sdk/sysroots/i686-linux-gnu/usr/lib/i386-linux-gnu"
test ! -e "$bundle/sdk/sysroots/x86_64-linux-gnu"
vm_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-consumer-vm-XXXXXX")
printf 'VM acceptance workspace: %s\n' "$vm_work"
trap 'status=$?; printf "VM evidence retained: %s (status %s)\n" "$vm_work" "$status"' EXIT
cache=${NIER_VM_CACHE:-$repository/.sdk/test-vm/downloads}
mkdir -p "$cache"
while read -r role package architecture version digest url extra; do
    [[ -n $role && $role != \#* ]] || continue
    [[ -z ${extra:-} && $architecture == i386 && $digest =~ ^[0-9a-f]{64}$ && $url == https://* ]]
    archive="$cache/${url##*/}"
    if [[ ! -f $archive ]]; then
        download=$(mktemp "$cache/.vm-download-XXXXXX")
        curl --fail --location --retry 3 --connect-timeout 20 --max-time 300 "$url" -o "$download"
        printf '%s  %s\n' "$digest" "$download" | sha256sum -c -
        mv -T -- "$download" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | sha256sum -c -
    test "$(dpkg-deb -f "$archive" Package)" = "$package"
    test "$(dpkg-deb -f "$archive" Architecture)" = "$architecture"
    test "$(dpkg-deb -f "$archive" Version)" = "$version"
    mkdir "$vm_work/$role-package"
    dpkg-deb -x "$archive" "$vm_work/$role-package"
done < "$repository/tests/vm/packages.lock"
kernel="$vm_work/kernel-package/boot/vmlinuz-6.1.0-50-686-pae"
kernel_config="$vm_work/kernel-package/boot/config-6.1.0-50-686-pae"
test -s "$kernel"
grep -Fxq 'CONFIG_X86_32=y' "$kernel_config"
grep -Fxq 'CONFIG_HIGHMEM64G=y' "$kernel_config"
grep -Fxq 'CONFIG_DEVTMPFS=y' "$kernel_config"
root="$vm_work/root"
mkdir -p "$root"/{bin,dev,proc,sys,tmp,usr,opt,fixtures,licenses}
chmod 1777 "$root/tmp"
busybox="$vm_work/busybox-package/bin/busybox"
[[ -f $busybox ]] || busybox="$vm_work/busybox-package/usr/bin/busybox"
cp -Lp -- "$busybox" "$root/bin/busybox"
for role in kernel busybox; do
    for directory in "$vm_work/$role-package/usr/share/doc/"*; do
        if [[ -f $directory/copyright ]]; then
            cp -Lp -- "$directory/copyright" "$root/licenses/${directory##*/}.copyright"
        fi
    done
done
cp -- "$repository/tests/vm/packages.lock" "$root/licenses/vm-packages.lock"
readelf -h "$root/bin/busybox" | grep -q 'Class:.*ELF32'
# The static rescue shell supplies the base VM utilities; corpus test runners
# are staged separately below and never become compiler product dependencies.
ln -s busybox "$root/bin/sh"
cp -a -- "$bundle" "$root/opt/nier"
cp -a -- "$fixtures/." "$root/fixtures/"
cp -- "$repository/tests/vm/init.sh" "$root/init"
cp -- "$repository/tests/consumer-fixtures.sh" "$root/consumer-fixtures.sh"
cp -- "$repository/tests/consumer-corpus.sh" "$root/consumer-corpus.sh"
if [[ -f $fixtures/corpus.list ]]; then
    bash "$repository/tests/vm/stage-test-tools.sh" "$bundle" "$root/opt/test-tools" "$cache"
fi
chmod 755 "$root/init"
# Real ELF32 processes use the ordinary loader path and Noble 2.39 runtime.
# No separate host filesystem, network mount, or publisher tree is available.
ln -s opt/nier/sdk/sysroots/i686-linux-gnu/usr/lib "$root/lib"
ln -s ../lib "$root/usr/lib"
token=${vm_work##*/}
printf '%s\n' "$token" > "$root/receipt-token"
(
    cd "$root"
    find . -print0 | LC_ALL=C sort -z | cpio --null -o --format=newc --owner=0:0 --quiet | gzip -1
) > "$vm_work/initramfs.cpio.gz"
memory=${NIER_VM_RAM_MIB:-3072}
deadline=${NIER_VM_TIMEOUT:-900}
[[ $memory =~ ^[0-9]+$ && $memory -ge 512 && $memory -le 4096 ]]
[[ $deadline =~ ^[0-9]+$ && $deadline -ge 30 && $deadline -le 3600 ]]
accelerator=${NIER_VM_ACCEL:-auto}
case "$accelerator" in auto|kvm|tcg) ;; *) printf 'NIER_VM_ACCEL must be auto, kvm, or tcg\n' >&2; exit 2 ;; esac
if [[ $accelerator == auto ]]; then
    accelerator=tcg
    if [[ -r /dev/kvm && -w /dev/kvm ]]; then
        set +e
        timeout --kill-after=1 2 qemu-system-i386 -machine accel=kvm -cpu host \
            -nodefaults -display none -monitor none -serial none -S \
            > "$vm_work/kvm-probe.log" 2>&1
        probe_status=$?
        set -e
        [[ $probe_status != 124 ]] || accelerator=kvm
    fi
fi
cpu=max
[[ $accelerator != kvm ]] || cpu=host
printf 'acceleration=%s\nram_mib=%s\ntimeout_seconds=%s\n' "$accelerator" "$memory" "$deadline" > "$vm_work/host-settings.txt"
qemu-system-i386 --version >> "$vm_work/host-settings.txt"
sha256sum "$repository/tests/vm/packages.lock" "$kernel" "$vm_work/initramfs.cpio.gz" >> "$vm_work/host-settings.txt"
printf 'Booting real i686 Linux: %s, %s MiB, %ss bound (functional evidence, not performance)\n' "$accelerator" "$memory" "$deadline"
set +e
timeout --kill-after=10 "$deadline" qemu-system-i386 \
    -machine "pc,accel=$accelerator" -cpu "$cpu" -smp 2 -m "$memory" \
    -nodefaults -display none -monitor none -serial stdio -nic none -no-reboot \
    -kernel "$kernel" -initrd "$vm_work/initramfs.cpio.gz" \
    -append 'console=ttyS0 rdinit=/init panic=1 quiet' \
    </dev/null 2>&1 | tee "$vm_work/serial.log"
vm_status=${PIPESTATUS[0]}
set -e
test "$vm_status" -eq 0
tr -d '\r' < "$vm_work/serial.log" > "$vm_work/serial-normalized.log"
grep -Fxq "NIER_VM_PASS $token" "$vm_work/serial-normalized.log"
grep -Fxq 'NIER_VM_KERNEL 6.1.0-50-686-pae i686' "$vm_work/serial-normalized.log"
grep -Fxq 'NIER_VM_ELF64_ENOEXEC' "$vm_work/serial-normalized.log"
if [[ -f $fixtures/corpus.list ]]; then
    expected=$(<"$fixtures/corpus-count")
    grep -Fxq "NIER_CONSUMER_CORPUS_PASS target=i686 artifacts=$expected" "$vm_work/serial-normalized.log"
else
    grep -Fq 'NIER_CONSUMER_FIXTURES_PASS target=i686 ' "$vm_work/serial-normalized.log"
fi
if grep -Eq 'NIER_VM_FAIL|Kernel panic|Out of memory:' "$vm_work/serial-normalized.log"; then exit 1; fi
printf 'Real 32-bit kernel/compiler/native-output VM acceptance passed: %s\n' "$vm_work"
