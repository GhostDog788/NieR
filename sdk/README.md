# Development SDK

This SDK is a user-local extraction of unmodified Ubuntu 24.04 packages, not a
compiler fork, system installation, container, or finished product installer.
It supplies Clang/LLVM/MLIR/LLD **18.1.3-1ubuntu1**, compiler-rt, CMake 3.28.3,
Ninja 1.11.1, libarchive development files, LLVM dependencies, and separate
x86-64/i686 glibc **2.39-0ubuntu8.8** development/runtime sysroots.

## Bootstrap and check

From the repository root, on an x86-64 Ubuntu 24.04 development host:

```sh
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
bash sdk/check-sdk.sh
```

No `sudo`, package installation, shell startup-file change, compiler patch,
custom loader, or `patchelf` operation is used. Packages are downloaded over
HTTPS, checked against their locked SHA-256 digest, and extracted with
`dpkg-deb`. Maintainer scripts from the packages are not run. The initial SDK
occupies approximately 1.8 GiB including the retained download cache.

The host must already provide Bash, coreutils, `curl`, `dpkg-deb`, `flock`,
`rg`, and a working C++ development environment. In particular, building our
tools still uses the host C++ standard-library headers and startup files. SDK
publisher programs use the host Ubuntu glibc/loader, and CMake uses the host
libcurl/networking dependency family. This is **not a fully hermetic publisher
image**. The SDK pins the extracted package inputs; it does not claim to pin
every publisher-host dependency or prove reproducible application builds.
Optional CMake notices about missing CURL/LibEdit development files do not
prevent the tested MLIR/LLVM/libarchive build.

`AOT_SDK_ROOT` can select another dedicated directory. Set it before both the
bootstrap and `source sdk/env.sh`. A completed SDK rejects a different package
lock; use a new directory for a new SDK version. The download mirror can be
changed with `SDK_UBUNTU_MIRROR`, but hashes remain mandatory. Cached packages
allow subsequent bootstraps without downloading them again. Receipts provide
restartability, not runtime tamper protection for a developer-writable SDK.

## Paths and target compilation

After sourcing `sdk/env.sh`:

| Variable | Default location |
| --- | --- |
| `AOT_SDK_ROOT` | `<repository>/.sdk` |
| `AOT_LLVM_ROOT` | `.sdk/host/usr/lib/llvm-18` |
| `LLVM_DIR` | `$AOT_LLVM_ROOT/lib/cmake/llvm` |
| `MLIR_DIR` | `$AOT_LLVM_ROOT/lib/cmake/mlir` |
| `AOT_SYSROOT_X86_64` | `.sdk/sysroots/x86_64-linux-gnu` |
| `AOT_SYSROOT_I686` | `.sdk/sysroots/i686-linux-gnu` |

`PATH`, `LD_LIBRARY_PATH`, and `CMAKE_PREFIX_PATH` select the extracted host
tools/libraries. CMake and Ninja live in `.sdk/host/usr/bin`. Compiler-rt
libraries for both x86 widths are under
`$AOT_LLVM_ROOT/lib/clang/18/lib/linux`.

Example private native-profile capture (this is **not** portable publication
IR and is not the product's LLVM capture plugin):

```sh
clang --target=i686-linux-gnu --sysroot="$AOT_SYSROOT_I686" \
  -std=c11 -O2 -Xclang -disable-llvm-passes -emit-llvm -c \
  sdk/smoke/stdio.c -o "$AOT_SDK_ROOT/check/stdio-i686.bc"
```

The x86-64 equivalent uses `--target=x86_64-linux-gnu` and
`--sysroot="$AOT_SYSROOT_X86_64"`. Neither profile's raw LLVM IR is assumed to
be architecture-neutral. The smoke check verifies distinct native pointer
widths (8 and 4 bytes).

The exact native x86-64 link recipe is in `sdk/check-sdk.sh`: LLD receives the
SDK's `Scrt1.o`, `crti.o`, compiler-rt CRT/builtins, libc, and `crtn.o`, with an
explicit SDK glibc interpreter and runtime library path. The ordinary native
ELF is executed directly, with `LD_LIBRARY_PATH` unset; the stock loader's
dependency report must select SDK libc. Ubuntu's usual relative `lib`/`lib64`
usr-merge aliases are created in the sysroots so the **unmodified** glibc
linker scripts resolve correctly. The SDK loader binary is not changed.

That development smoke ELF embeds its local SDK path. It is not an immutable
installed product release or a relocatable distribution artifact. Release
installation and managed dependency layout belong to the product, not this
bootstrap script. The check compiles **but never executes i686 code**.

## Package lock provenance

`packages.lock` pins each extraction lane, package identity, architecture,
version, SHA-256, and Ubuntu repository path. The initial lock was derived
from local Ubuntu `noble`/`noble-updates` APT indexes. `refresh-lock.sh` verifies
their InRelease signatures with the installed Ubuntu archive keyring, checks
the SHA-256 of every selected package index against its signed InRelease,
and then requires an exact match of each apt-cache record in those indexes.
The verified signing key was Ubuntu Archive Automatic Signing Key (2018),
fingerprint `F6ECB3762474EDA9D21B7022871920D1991BC93C`.

Maintainers can inspect a proposed refresh without modifying APT or the lock:

```sh
bash sdk/refresh-lock.sh
```

The refresh helper expects already refreshed, uncompressed APT `Packages`
indexes. Its default archive-cache prefix is
`il.archive.ubuntu.com_ubuntu`; override `SDK_APT_ARCHIVE_PREFIX`,
`SDK_APT_LISTS`, and/or `SDK_UBUNTU_KEYRING` for another verified cache layout.
Review emitted changes before updating the checked-in lock. Ordinary SDK
users need neither APT repository metadata nor the refresh helper; they
consume the checked-in digests. Availability of old upstream package URLs
is not guaranteed indefinitely; an archival mirror can preserve the same
hash-verified package bytes.

`sdk/check-sdk.sh` is an SDK plumbing test: CMake/Ninja build and run a small
MLIR/LLVM/libarchive program; both target profiles compile hosted C; a native
x86-64 object links and runs with managed libc. It does not establish general
C coverage, publication privacy, neutral-IR correctness, performance parity,
or any security-enforcement requirement.
