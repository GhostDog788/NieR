# Development SDK

This SDK is a user-local extraction of unmodified Ubuntu 24.04 packages, not a compiler fork, system installation, container, or finished product installer.
It supplies Clang/LLVM/MLIR/LLD **18.1.3-1ubuntu1**, matching Clang frontend headers and the Clang C++ plugin library,
compiler-rt, CMake 3.28.3, Ninja 1.11.1, libarchive development files, LLVM dependencies,
and separate x86-64/i686 glibc **2.39-0ubuntu8.8** development/runtime sysroots.
The matching GCC runtime base package is also pinned so the extracted `libstdc++6` and `libgcc-s1` copyright notices resolve.
Distributable device compilers use separate target-specific source SDKs, not this publisher SDK; see the [compiler distribution reference](compiler-distribution.md).

The footprint work keeps this publisher SDK's archive linkage and frontend behavior unchanged.
Only the independent device `selac` selects objects from pinned static libarchive; publisher tools and fixture writers retain shared libarchive.
The consumer bootstrap defaults to stock X86-only shared LLVM and also supports `SELA_CONSUMER_LLVM_LINKAGE=static-components` for comparison.
The pre-rename shared checkpoint recorded in commit `be63890` measured 91.89 MiB for x86-64 and 98.28 MiB for i686 and passed the full 50-artifact dual-device corpus on 2026-09-13, establishing the shared default.
Fresh Sela packages separately measure approximately 91.9 MiB and 98.3 MiB and have passed all 11 consumer CTests per device, all 40 publisher tests, the fresh two-device matrix, and the complete dual-device zlib corpus.
cJSON's fresh publication/native/x86-64 stages and its separate unchanged-fixture real-i686 continuation cover all 42 artifacts; the first command's failed VM boot remains recorded as a failure, not a single-command pass.
The distribution reference separates historical evidence from these renamed builds and records the earlier test-only harness provenance correction; the historical publisher SDK and compiler packages were not changed for that correction.
Its two architecture SDKs can be built concurrently with the opt-in [parallel source-build procedure](building-sela.md#parallel-sdk-source-builds); compilation job limits apply per target, not to the combined machine workload.

## Bootstrap and check

From the repository root, on an x86-64 Ubuntu 24.04 development host:

```sh
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
bash sdk/check-sdk.sh
```

No `sudo`, package installation, shell startup-file change, compiler patch, custom loader, or `patchelf` operation is used.
Packages are downloaded over HTTPS, checked against their locked SHA-256 digest, and extracted with `dpkg-deb`.
Maintainer scripts from the packages are not run. The initial SDK occupies approximately 2.1 GiB including the retained download cache.

The host must already provide Bash, coreutils, `curl`, `dpkg-deb`, `flock`, `rg`, and a working C++ development environment.
Fresh project configurations default to the pinned SDK Clang/Clang++ and LLD unless compiler settings are explicitly supplied.
Building our tools still uses the host C++ standard-library headers and startup files.
SDK publisher programs use the host Ubuntu glibc/loader, and CMake uses the host libcurl/networking dependency family.
This is **not a fully hermetic publisher image**. The SDK pins the extracted package inputs; it does not claim to pin every publisher-host dependency or prove reproducible application builds.
Optional CMake notices about missing CURL/LibEdit development files do not prevent the tested MLIR/LLVM/libarchive build.

`SELA_SDK_ROOT` can select another dedicated directory. Set it before both the bootstrap and `source sdk/env.sh`.
During pre-alpha the package lock may change without compatibility support: bootstrap extracts the newly pinned packages and developer tools must be rebuilt.
Use a fresh directory when removing SDK packages or changing toolchain families; bootstrap does not delete unrelated files.
The default download source is Ubuntu's dated snapshot at `https://snapshot.ubuntu.com/ubuntu/20260910T000000Z`, which retains the locked package versions after the live archive rotates them out.
The mirror can be changed with `SDK_UBUNTU_MIRROR`, but hashes remain mandatory.
Cached packages allow subsequent bootstraps without downloading them again.
Receipts provide restartability, not runtime tamper protection for a developer-writable SDK.

Consumer release packaging preserves the original SDK/build binaries and their completion hashes.
It strips only staged delivery copies of the compiler, LLVM tools, and shared LLVM library when present; the final bundle's `payload.sha256` records those delivered bytes.
Do not compare a stripped delivery binary directly with an original-tool completion hash and treat their intentional difference as SDK corruption.
The managed C runtime, static link inputs, CRTs, and compiler-rt are unchanged by this packaging step.
Measured bundles retain the README snapshot copied at assembly and remain unchanged afterward.
The live distribution reference records subsequent qualification without changing those measured payloads.

Sharing LLVM does not change the ahead-of-time compilation model or add LLVM to generated applications' runtime dependencies.
Removing unused stock-library capabilities and enforcing SESela execution policy are separate work; shared linkage is not a claim that those goals have been completed.

## Paths and target compilation

After sourcing `sdk/env.sh`:

| Variable | Default location |
| --- | --- |
| `SELA_SDK_ROOT` | `<repository>/.sdk` |
| `SELA_LLVM_ROOT` | `.sdk/host/usr/lib/llvm-18` |
| `LLVM_DIR` | `$SELA_LLVM_ROOT/lib/cmake/llvm` |
| `MLIR_DIR` | `$SELA_LLVM_ROOT/lib/cmake/mlir` |
| `Clang_DIR` | `$SELA_LLVM_ROOT/lib/cmake/clang` |
| `SELA_SYSROOT_X86_64` | `.sdk/sysroots/x86_64-linux-gnu` |
| `SELA_SYSROOT_I686` | `.sdk/sysroots/i686-linux-gnu` |

`PATH`, `LD_LIBRARY_PATH`, and `CMAKE_PREFIX_PATH` select the extracted host tools/libraries.
CMake and Ninja live in `.sdk/host/usr/bin`.
Compiler-rt libraries for both x86 widths are under `$SELA_LLVM_ROOT/lib/clang/18/lib/linux`.

Example private native-profile capture (this is **not** portable publication IR and is not the product's LLVM capture plugin):

```sh
clang --target=i686-linux-gnu --sysroot="$SELA_SYSROOT_I686" \
  -std=c11 -O2 -Xclang -disable-llvm-passes -emit-llvm -c \
  sdk/smoke/stdio.c -o "$SELA_SDK_ROOT/check/stdio-i686.bc"
```

The x86-64 equivalent uses `--target=x86_64-linux-gnu` and `--sysroot="$SELA_SYSROOT_X86_64"`.
Neither profile's raw LLVM IR is assumed to be architecture-neutral. The smoke check verifies distinct native pointer widths (8 and 4 bytes).

The exact native x86-64 link recipe is in `sdk/check-sdk.sh`: LLD receives the SDK's `Scrt1.o`, `crti.o`, compiler-rt CRT/builtins, libc, and `crtn.o`, with an explicit SDK glibc interpreter and runtime library path.
The ordinary native ELF is executed directly, with `LD_LIBRARY_PATH` unset; the stock loader's dependency report must select SDK libc.
Ubuntu's usual relative `lib`/`lib64` usr-merge aliases are created in the sysroots so the **unmodified** glibc linker scripts resolve correctly.
The SDK loader binary is not changed.

That development smoke ELF embeds its local SDK path. It is not an immutable installed product release or a relocatable distribution artifact.
Release installation and managed dependency layout belong to the product, not this bootstrap script. The check compiles **but never executes i686 code**.

## Package lock provenance

`sdk/packages.lock` pins each extraction lane, package identity, architecture, version, SHA-256, and Ubuntu repository path. The initial lock was derived from local Ubuntu `noble`/`noble-updates` APT indexes.
`sdk/refresh-lock.sh` verifies their InRelease signatures with the installed Ubuntu archive keyring, checks the SHA-256 of every selected package index against its signed InRelease, and then requires an exact match of each apt-cache record in those indexes.
The verified signing key was Ubuntu Archive Automatic Signing Key (2018), fingerprint `F6ECB3762474EDA9D21B7022871920D1991BC93C`.

Maintainers can inspect a proposed refresh without modifying APT or the lock:

```sh
bash sdk/refresh-lock.sh
```

The refresh helper expects already refreshed, uncompressed APT `Packages` indexes.
Its default archive-cache prefix is `il.archive.ubuntu.com_ubuntu`; override `SDK_APT_ARCHIVE_PREFIX`, `SDK_APT_LISTS`, and/or `SDK_UBUNTU_KEYRING` for another verified cache layout.
Review emitted changes before updating the checked-in lock.
Ordinary SDK users need neither APT repository metadata nor the refresh helper; they consume the checked-in digests.
Availability of old upstream package URLs is not guaranteed indefinitely; an archival mirror can preserve the same hash-verified package bytes.

`sdk/check-sdk.sh` is an SDK plumbing test:
CMake/Ninja build and run a small MLIR/LLVM/libarchive program; both target profiles compile hosted C; a native x86-64 object links and runs with managed libc.
It does not establish general C coverage, publication privacy, neutral-IR correctness, performance parity, or any security-enforcement requirement.

## Private native-build provenance

The paired-build integration uses stock Clang's `-fno-temp-file` only inside fresh private native build lanes.
Clang's after-main AST consumer runs after the native backend has closed its output stream;
this option makes the finished object available at its requested path at that point.
The observer then binds the original object hash to its pristine LLVM capture.
Native compiler errors never finalize that hash, and publication additionally requires the complete native build to succeed.

Private objects carry a readonly `.sela.capture` reference added **after** the LLVM snapshot.
Copying, moving and ordinary archiving preserve provenance; postprocessing that changes object bytes is rejected when selecting build inputs.
The marker never enters public Sela Code or the on-device executable.
These checks protect build correspondence, not against a hostile process able to rewrite the entire private build workspace; they are not security signing.
