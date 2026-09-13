#!/bin/sh
# This is PID 1 inside the disposable, offline, real i386-kernel guest.
/bin/busybox mount -t devtmpfs devtmpfs /dev
exec </dev/console >/dev/console 2>&1
/bin/busybox --install -s /bin
export PATH=/bin:/usr/bin LC_ALL=C TMPDIR=/tmp
set -eu
finish() {
    status=$?
    trap - EXIT
    if [ "$status" -eq 0 ]; then
        printf 'NIER_VM_PASS %s\n' "$(cat /receipt-token)"
    else
        printf 'NIER_VM_FAIL status=%s\n' "$status"
    fi
    sync
    /bin/busybox poweroff -f
    # Do not let a failed shutdown turn PID 1 exit into a successful test.
    while :; do /bin/busybox sleep 1; done
}
trap finish EXIT
mount -t proc proc /proc
mount -t sysfs sysfs /sys
test "$(uname -m)" = i686
case "$(uname -r)" in 6.1.0-50-686-pae) ;; *) exit 1 ;; esac
printf 'NIER_VM_KERNEL %s %s\n' "$(uname -r)" "$(uname -m)"
test ! -e /usr/bin/clang
test ! -e /opt/nier/sdk/sysroots/x86_64-linux-gnu
/fixtures/exec-format /fixtures/elf64-negative
for tool in /opt/nier/bin/nierc /opt/nier/sdk/host/usr/lib/llvm-18/bin/opt \
    /opt/nier/sdk/host/usr/lib/llvm-18/bin/llc /opt/nier/sdk/host/usr/lib/llvm-18/bin/ld.lld \
    /opt/nier/sdk/host/usr/lib/llvm-18/bin/llvm-ar; do
    test "$(od -An -tu1 -j4 -N1 "$tool" | tr -d ' \n')" = 1
    test "$(od -An -tu2 -j18 -N2 "$tool" | tr -d ' \n')" = 3
done
if [ -f /fixtures/corpus.list ]; then
    /bin/sh /consumer-corpus.sh /opt/nier /fixtures /work i686
else
    /bin/sh /consumer-fixtures.sh /opt/nier /fixtures /work i686
fi
