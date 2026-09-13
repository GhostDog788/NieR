#!/bin/sh
# This is PID 1 inside a disposable, offline, native-kernel guest.
/bin/busybox mount -t devtmpfs devtmpfs /dev
exec </dev/console >/dev/console 2>&1
/bin/busybox --install -s /bin
export PATH=/bin:/usr/bin LC_ALL=C TMPDIR=/tmp
set -eu
finish() {
    status=$?
    trap - EXIT
    if [ "$status" -eq 0 ]; then
        printf 'SELA_VM_PASS %s\n' "$(cat /receipt-token)"
    else
        printf 'SELA_VM_FAIL status=%s\n' "$status"
    fi
    sync
    /bin/busybox poweroff -f
    # Do not let a failed shutdown turn PID 1 exit into a successful test.
    while :; do /bin/busybox sleep 1; done
}
trap finish EXIT
mount -t proc proc /proc
mount -t sysfs sysfs /sys
. /vm.env
test "$(uname -m)" = "$kernel_machine"
test "$(uname -r)" = "$kernel_release"
printf 'SELA_VM_KERNEL %s %s\n' "$(uname -r)" "$(uname -m)"
test ! -e /usr/bin/clang
test "$(find /opt/sela/sdk/sysroots -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 1
/fixtures/reference/"$target"/exec-format /fixtures/reference/"$foreign"/hello
for tool in /opt/sela/bin/selac /opt/sela/sdk/host/usr/lib/llvm-18/bin/opt \
    /opt/sela/sdk/host/usr/lib/llvm-18/bin/llc /opt/sela/sdk/host/usr/lib/llvm-18/bin/ld.lld \
    /opt/sela/sdk/host/usr/lib/llvm-18/bin/llvm-ar; do
    test "$(od -An -tu1 -j4 -N1 "$tool" | tr -d ' \n')" = "$elf_class"
    test "$(od -An -tu2 -j18 -N2 "$tool" | tr -d ' \n')" = "$elf_machine"
done
if [ -f /fixtures/corpus.list ]; then
    /bin/sh /consumer-corpus.sh /opt/sela /fixtures /work "$target"
else
    /bin/sh /consumer-fixtures.sh /opt/sela /fixtures /work "$target"
fi
