#!/usr/bin/env bash
# Boot the target's real Linux kernel; never substitute user-mode emulation.
set -euo pipefail
if [[ $# != 3 ]]; then
    printf 'Usage: bash tests/consumer-vm.sh TARGET BUNDLE FIXTURE_DIRECTORY\n' >&2
    exit 2
fi
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=$1
eval "$(python3 "$repository/sdk/targets.py" shell "$target")"
bundle=$(realpath -e -- "$2")
fixtures=$(realpath -e -- "$3")
case "$target" in
    x86_64) qemu=qemu-system-x86_64; machine=pc; cpu=max; console=ttyS0; kernel_release=6.8.0-139-generic; kernel_machine=x86_64; kernel_flag=CONFIG_X86_64; foreign=armv7 ;;
    i686) qemu=qemu-system-i386; machine=pc; cpu=max; console=ttyS0; kernel_release=6.1.0-50-686-pae; kernel_machine=i686; kernel_flag=CONFIG_X86_32; foreign=x86_64 ;;
    armv7) qemu=qemu-system-arm; machine=virt; cpu=cortex-a15; console=ttyAMA0; kernel_release=6.8.0-139-generic; kernel_machine=armv7l; kernel_flag=CONFIG_ARM; foreign=aarch64 ;;
    aarch64) qemu=qemu-system-aarch64; machine=virt; cpu=cortex-a53; console=ttyAMA0; kernel_release=6.8.0-139-generic; kernel_machine=aarch64; kernel_flag=CONFIG_ARM64; foreign=x86_64 ;;
    *) exit 2 ;;
esac
for command in "$qemu" cpio gzip curl dpkg-deb sha256sum readelf timeout; do
    command -v "$command" >/dev/null || { printf 'Required VM host tool: %s\n' "$command" >&2; exit 1; }
done
python3 "$repository/sdk/targets.py" check-elf "$target" "$bundle/bin/selac" "$fixtures/reference/$target/exec-format"
python3 "$repository/sdk/targets.py" check-elf "$foreign" "$fixtures/reference/$foreign/hello"
test -f "$fixtures/fixtures.sha256"
cmp <(python3 "$repository/sdk/targets.py" list) "$fixtures/targets.list"
test -d "$bundle/sdk/sysroots/$SELA_TARGET_SYSROOT_TRIPLE/usr/lib/$SELA_TARGET_MULTIARCH"
test "$(find "$bundle/sdk/sysroots" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 1
vm_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-consumer-vm-XXXXXX")
printf 'VM acceptance workspace: %s\n' "$vm_work"
trap 'status=$?; printf "VM evidence retained: %s (status %s)\n" "$vm_work" "$status"' EXIT
cache=${SELA_VM_CACHE:-$repository/.sdk/test-vm/downloads}
mkdir -p "$cache"
while read -r profile role package architecture version digest url extra; do
    [[ -n $profile && $profile != \#* ]] || continue
    [[ $profile == "$target" ]] || continue
    [[ -z ${extra:-} && $architecture == "$SELA_TARGET_PACKAGE_ARCH" && $digest =~ ^[0-9a-f]{64}$ && $url == https://* ]]
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
kernel="$vm_work/kernel-package/boot/vmlinuz-$kernel_release"
kernel_config="$vm_work/kernel-package/boot/config-$kernel_release"
if [[ $target != i686 ]]; then
    kernel_config="$vm_work/headers-package/usr/src/linux-headers-$kernel_release/.config"
fi
test -s "$kernel"
grep -Fxq "$kernel_flag=y" "$kernel_config"
if [[ $target == i686 ]]; then grep -Fxq 'CONFIG_HIGHMEM64G=y' "$kernel_config"; fi
if [[ $target == armv7 ]]; then grep -Fxq 'CONFIG_ARM_LPAE=y' "$kernel_config"; fi
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
python3 "$repository/sdk/targets.py" check-elf "$target" "$root/bin/busybox"
# The static rescue shell supplies the base VM utilities; corpus test runners
# are staged separately below and never become compiler product dependencies.
ln -s busybox "$root/bin/sh"
cp -a -- "$bundle" "$root/opt/sela"
cp -a -- "$fixtures/." "$root/fixtures/"
cp -- "$repository/tests/vm/init.sh" "$root/init"
cp -- "$repository/tests/consumer-fixtures.sh" "$root/consumer-fixtures.sh"
cp -- "$repository/tests/consumer-corpus.sh" "$root/consumer-corpus.sh"
if [[ -f $fixtures/corpus.list ]]; then
    bash "$repository/tests/vm/stage-test-tools.sh" "$target" "$bundle" "$root/opt/test-tools" "$cache"
fi
chmod 755 "$root/init"
# Native processes use the ordinary loader path and Noble 2.39 runtime.
# No separate host filesystem, network mount, or publisher tree is available.
ln -s "opt/sela/sdk/sysroots/$SELA_TARGET_SYSROOT_TRIPLE/usr/lib" "$root/lib"
ln -s ../lib "$root/usr/lib"
if [[ $target == x86_64 ]]; then
    mkdir "$root/lib64"
    ln -s "../lib/$SELA_TARGET_MULTIARCH/$SELA_TARGET_LOADER" "$root/lib64/$SELA_TARGET_LOADER"
fi
printf 'target=%s\nkernel_release=%s\nkernel_machine=%s\nelf_class=%s\nelf_machine=%s\nforeign=%s\n' \
    "$target" "$kernel_release" "$kernel_machine" "$SELA_TARGET_ELF_CLASS" "$SELA_TARGET_ELF_MACHINE" "$foreign" > "$root/vm.env"
token=${vm_work##*/}
printf '%s\n' "$token" > "$root/receipt-token"
(
    cd "$root"
    find . -print0 | LC_ALL=C sort -z | cpio --null -o --format=newc --owner=0:0 --quiet | gzip -1
) > "$vm_work/initramfs.cpio.gz"
# Reject a damaged boot image before launching a guest; retain it as evidence.
gzip -t "$vm_work/initramfs.cpio.gz"
memory=${SELA_VM_RAM_MIB:-3072}
# Full upstream corpora take substantially longer than core fixtures under
# software CPU emulation. Keep explicit caller limits authoritative.
default_deadline=900
if [[ -f $fixtures/corpus.list ]]; then default_deadline=3600; fi
deadline=${SELA_VM_TIMEOUT:-$default_deadline}
[[ $memory =~ ^[0-9]+$ && $memory -ge 512 && $memory -le 4096 ]]
[[ $deadline =~ ^[0-9]+$ && $deadline -ge 30 && $deadline -le 3600 ]]
accelerator=${SELA_VM_ACCEL:-auto}
case "$accelerator" in auto|kvm|tcg) ;; *) printf 'SELA_VM_ACCEL must be auto, kvm, or tcg\n' >&2; exit 2 ;; esac
if [[ $accelerator == auto ]]; then
    accelerator=tcg
    if [[ ( $target == x86_64 || $target == i686 ) && $(uname -m) == x86_64 && -r /dev/kvm && -w /dev/kvm ]]; then
        set +e
        timeout --kill-after=1 2 "$qemu" -machine "$machine,accel=kvm" -cpu host \
            -nodefaults -display none -monitor none -serial none -S \
            > "$vm_work/kvm-probe.log" 2>&1
        probe_status=$?
        set -e
        [[ $probe_status != 124 ]] || accelerator=kvm
    fi
fi
[[ $accelerator != kvm ]] || cpu=host
printf 'target=%s\nacceleration=%s\nram_mib=%s\ntimeout_seconds=%s\n' "$target" "$accelerator" "$memory" "$deadline" > "$vm_work/host-settings.txt"
"$qemu" --version >> "$vm_work/host-settings.txt"
sha256sum "$repository/tests/vm/packages.lock" "$kernel" "$kernel_config" "$vm_work/initramfs.cpio.gz" >> "$vm_work/host-settings.txt"
cat "$vm_work/host-settings.txt"
printf 'Booting real %s Linux: %s, %s MiB, %ss bound (functional evidence, not performance)\n' "$target" "$accelerator" "$memory" "$deadline"
started=$SECONDS
set +e
timeout --kill-after=10 "$deadline" "$qemu" \
    -machine "$machine,accel=$accelerator" -cpu "$cpu" -smp 2 -m "$memory" \
    -nodefaults -display none -monitor none -serial stdio -nic none -no-reboot \
    -kernel "$kernel" -initrd "$vm_work/initramfs.cpio.gz" \
    -append "console=$console rdinit=/init panic=1 quiet" \
    </dev/null 2>&1 | tee "$vm_work/serial.log"
vm_status=${PIPESTATUS[0]}
set -e
test "$vm_status" -eq 0
tr -d '\r' < "$vm_work/serial.log" > "$vm_work/serial-normalized.log"
grep -Fxq "SELA_VM_PASS $token" "$vm_work/serial-normalized.log"
grep -Fxq "SELA_VM_KERNEL $kernel_release $kernel_machine" "$vm_work/serial-normalized.log"
grep -Fxq 'SELA_VM_FOREIGN_ELF_ENOEXEC' "$vm_work/serial-normalized.log"
if [[ -f $fixtures/corpus.list ]]; then
    expected=$(<"$fixtures/corpus-count")
    grep -Fxq "SELA_CONSUMER_CORPUS_PASS target=$target artifacts=$expected" "$vm_work/serial-normalized.log"
else
    grep -Fq "SELA_CONSUMER_FIXTURES_PASS target=$target " "$vm_work/serial-normalized.log"
fi
if grep -Eq 'SELA_VM_FAIL|Kernel panic|Out of memory:' "$vm_work/serial-normalized.log"; then exit 1; fi
printf 'Real-kernel compiler/native-output acceptance passed: target=%s elapsed_seconds=%s evidence=%s\n' "$target" "$((SECONDS-started))" "$vm_work"
