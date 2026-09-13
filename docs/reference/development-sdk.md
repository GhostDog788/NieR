# Development SDK

This SDK is a user-local extraction of unmodified Ubuntu 24.04 packages, not a compiler fork, system installation, container, or finished product installer.
It supplies Clang/LLVM/MLIR/LLD **18.1.3-1ubuntu1**, matching Clang frontend headers and the Clang C++ plugin library,
compiler-rt, CMake 3.28.3, Ninja 1.11.1, libarchive development files, LLVM dependencies,
and separate x86-64, i686, ARMv7-A hard-float, and AArch64 glibc **2.39-0ubuntu8.8** development/runtime sysroots.
The matching GCC runtime base package is also pinned so the extracted `libstdc++6` and `libgcc-s1` copyright notices resolve.
Distributable device compilers use separate target-specific source SDKs, not this publisher SDK; see the [compiler distribution reference](compiler-distribution.md).

The footprint work keeps this publisher SDK's archive linkage and frontend behavior unchanged.
Only the independent device `selac` selects objects from pinned static libarchive; publisher tools and fixture writers retain shared libarchive.
Ubuntu Noble's ARM32 `libarchive13t64` exposes a 64-bit `time_t` ABI.
The build enables `_TIME_BITS=64` and `_FILE_OFFSET_BITS=64` privately for the two Sela translation units that call libarchive when linking that ARM library.
This is an implementation dependency ABI setting, not a change to published C programs, target layouts, or LLVM's build flags.
Package tests check timestamp round trips and deterministic archive bytes to catch a header/library ABI mismatch.
The consumer bootstrap defaults to stock shared LLVM restricted to the target's backend family: X86 for either x86 width, ARM for ARMv7, and AArch64 for ARM64.
It also supports `SELA_CONSUMER_LLVM_LINKAGE=static-components` for comparison.
The source SDKs can be built concurrently with the opt-in [parallel source-build procedure](building-sela.md#parallel-sdk-source-builds); compilation job limits apply per target, not to the combined machine workload.
Historical x86 package sizes and qualification records are kept separately in [Building Sela](building-sela.md#historical-x86-checkpoints) and the distribution reference; they are not measurements or acceptance evidence for a new four-target build.

## Bootstrap and check

From the repository root, on an x86-64 Ubuntu 24.04 development host:

```sh
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
bash sdk/check-sdk.sh
```

No `sudo`, package installation, shell startup-file change, compiler patch, custom loader, or `patchelf` operation is used.
Packages are downloaded over HTTPS, checked against their locked SHA-256 digest, and extracted with `dpkg-deb`.
Maintainer scripts from the packages are not run.
The earlier two-target publisher SDK occupied approximately 2.1 GiB including its download cache; additional target sysroots and source-built consumer SDKs require additional storage.

The host must already provide Bash, coreutils, `curl`, `dpkg-deb`, `flock`, `rg`, Python 3, ELF inspection tools, and a working C++ development environment.
Fresh project configurations default to the pinned SDK Clang/Clang++ and LLD unless compiler settings are explicitly supplied.
Building our tools still uses the host C++ standard-library headers and startup files.
SDK publisher programs use the host Ubuntu glibc/loader, and CMake uses the host libcurl/networking dependency family.
This is **not a fully hermetic publisher image**. The SDK pins the extracted package inputs; it does not claim to pin every publisher-host dependency or prove reproducible application builds.
Optional CMake notices about missing CURL/LibEdit development files do not prevent the tested MLIR/LLVM/libarchive build.

Build-host identity and device-target identity are separate.
The registry and cross-toolchain do not require an ARM or future RISC-V development host to build a compiler for that device.
This release's supplied publisher-host package lock is still x86-64 Ubuntu 24.04; a qualified ARM publisher-host SDK is deferred.
Bootstrap diagnoses an incompatible host package lock rather than attempting to execute its foreign host tools.
This structural separation is not a claim that an ARM development-host release has passed qualification.

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
| `SELA_SYSROOT_ARMV7` | `.sdk/sysroots/armv7-linux-gnueabihf` |
| `SELA_SYSROOT_AARCH64` | `.sdk/sysroots/aarch64-linux-gnu` |
| `SELA_BUILD_HOST` | Detected build-host target ID; currently qualified as `x86_64` |

`PATH`, `LD_LIBRARY_PATH`, and `CMAKE_PREFIX_PATH` select the extracted host tools/libraries.
CMake and Ninja live in `.sdk/host/usr/bin`.
Compiler-rt link inputs for all four targets are under `$SELA_LLVM_ROOT/lib/clang/18/lib/linux`.
The stable `.sdk/host/runtime` alias selects the detected host's extracted runtime directory for editor presets.

`sdk/targets.json` is the central target registry.
It records target ID, LLVM triple/backend, sysroot and GCC multiarch names, ABI, byte order, word width, CPU/features, plain-char signedness, native loader, compiler-rt name, linker emulation, and Clang policy flags.
`sdk/targets.py` exposes that metadata to shell/CMake and generates the checked-in C++ registry used by the producer and consumer.
Do not derive the ABI from pointer width: i686 and ARMv7 are both 32-bit but have different ABI and alignment rules.
`clangArgs` contains target ISA/ABI settings used to cross-build the compiler implementation itself.
`publicationClangArgs` adds application-publication policy; callers use both for source capture and private native application builds.
For AArch64, publication explicitly disables optional outline-atomics and function-multiversioning defaults so native-driver and capture paths observe the same policy.
That application policy does not require rebuilding stock LLVM with different implementation flags.

```sh
python3 sdk/targets.py list
python3 sdk/targets.py get armv7 triple
python3 sdk/targets.py get armv7 clangArgs
python3 sdk/targets.py get aarch64 publicationClangArgs
python3 sdk/targets.py check-cpp src/targets/Targets.cpp
```

The ARM policies are little-endian ARMv7-A with VFPv3-D16/hard-float and little-endian ARMv8-A AArch64.
The ARMv7 policy does not require NEON.
Layouts, CPU features, and plain-char defaults are checked against actual pinned-Clang output by `sdk/test_targets.py`.

Example private native-profile capture (this is **not** portable publication IR and is not the product's LLVM capture plugin):

```sh
clang --target=i686-linux-gnu --sysroot="$SELA_SYSROOT_I686" \
  -std=c11 -O2 -Xclang -disable-llvm-passes -emit-llvm -c \
  sdk/smoke/stdio.c -o "$SELA_SDK_ROOT/check/stdio-i686.bc"
```

The x86-64 equivalent uses `--target=x86_64-linux-gnu` and `--sysroot="$SELA_SYSROOT_X86_64"`.
For another target, use its exact registry triple, sysroot, and both sets of publication Clang policy flags together.
No profile's raw LLVM IR is assumed to be architecture-neutral: two targets of the same width can still differ in layout, calling convention, and language defaults.

The four-target link recipes are in `sdk/check-sdk.sh`: LLD receives each target's `Scrt1.o`, `crti.o`, compiler-rt CRT/builtins, libc, and `crtn.o`, with an explicit SDK glibc interpreter and runtime library path.
The check compiles and links all four targets and inspects their ELF identities without executing foreign tools or applications.
Only the build host's matching smoke ELF is executed, with `LD_LIBRARY_PATH` unset; the stock loader's dependency report must select SDK libc.
Ubuntu's usual relative `lib`/`lib64` usr-merge aliases are created in the sysroots so the **unmodified** glibc linker scripts resolve correctly.
The SDK loader binary is not changed.

That development smoke ELF embeds its local SDK path. It is not an immutable installed product release or a relocatable distribution artifact.
Release installation and managed dependency layout belong to the product, not this bootstrap script.

## Cross-building is not target execution

Building and packaging a device SDK or `selac` on x86 does not require target execution.
Host-native Clang/LLD compile and link the target binaries; host-native TableGen supplies build-time generators.
The bootstrap and packager validate target ELF identity, pinned tool hashes, backend-family receipts, and shared-library closure without running cross-built LLVM tools.
Runtime `selac --check-sdk`, backend inventories, and compilation/execution tests belong to consuming-device qualification.

Publishing an application with Make/CMake is a different operation.
Its private native builds may execute configure probes, tests, or generated programs for every target.
On x86, that workflow requires explicitly provisioned automatic ARM executable handling through QEMU user-mode/binfmt, plus the i686 loader prerequisite where applicable.
The publisher executes real probes before beginning the build and fails if the execution environment is missing; it never fabricates target-generated output or silently installs/registers binfmt handlers.
See the [build prerequisites](building-sela.md#application-publication-emulation-prerequisites) before publishing a project.
User-mode emulation does not replace the separate real-kernel VM acceptance gate.

## Package lock provenance

`sdk/packages.lock` pins each extraction lane, package identity, architecture, version, SHA-256, and Ubuntu repository path. The initial lock was derived from local Ubuntu `noble`/`noble-updates` APT indexes.
`sdk/refresh-lock.sh` verifies their InRelease signatures with the installed Ubuntu archive keyring, checks the SHA-256 of every selected package index against its signed InRelease, and then requires an exact match of each apt-cache record in those indexes.
The verified signing key was Ubuntu Archive Automatic Signing Key (2018), fingerprint `F6ECB3762474EDA9D21B7022871920D1991BC93C`.

Maintainers can inspect a proposed refresh without modifying APT or the lock:

```sh
bash sdk/refresh-lock.sh
```

The refresh helpers expect already refreshed, uncompressed APT `Packages` indexes for every architecture requested by `packages.txt`.
Its default archive-cache prefix is `il.archive.ubuntu.com_ubuntu`; override `SDK_APT_ARCHIVE_PREFIX`, `SDK_APT_LISTS`, and/or `SDK_UBUNTU_KEYRING` for another verified cache layout.
Review emitted changes before updating the checked-in lock.
Ordinary SDK users need neither APT repository metadata nor the refresh helper; they consume the checked-in digests.
Availability of old upstream package URLs is not guaranteed indefinitely; an archival mirror can preserve the same hash-verified package bytes.

The ARM additions were selected from signed snapshot indexes using `sdk/pin-target-packages.py`.
It verifies InRelease with the Ubuntu archive keyring and verifies each compressed index's size and SHA-256 before selecting the matching pinned package versions.
It emits proposed ARM consumer rows without editing either lock or changing APT configuration:

```sh
python3 sdk/pin-target-packages.py /path/to/private-metadata-cache
```

Publisher sysroots take the corresponding C-runtime/header subset; foreign compiler-rt packages supply target link inputs, not build-host executable tools.

`sdk/check-sdk.sh` is an SDK plumbing test:
CMake/Ninja build and run a small host MLIR/LLVM/libarchive program; all four profiles compile and link hosted C; only the matching host target runs with managed libc.
It does not establish general C coverage, publication privacy, neutral-IR correctness, performance parity, or any security-enforcement requirement.

## Private native-build provenance

The multi-profile build integration uses stock Clang's `-fno-temp-file` only inside fresh private native build lanes.
Clang's after-main AST consumer runs after the native backend has closed its output stream;
this option makes the finished object available at its requested path at that point.
The observer then binds the original object hash to its pristine LLVM capture.
Native compiler errors never finalize that hash, and publication additionally requires the complete native build to succeed.

Private objects carry a readonly `.sela.capture` reference added **after** the LLVM snapshot.
Copying, moving and ordinary archiving preserve provenance; postprocessing that changes object bytes is rejected when selecting build inputs.
The marker never enters public Sela Code or the on-device executable.
These checks protect build correspondence, not against a hostile process able to rewrite the entire private build workspace; they are not security signing.
