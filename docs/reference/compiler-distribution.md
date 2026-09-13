# NieR native compiler-only distributions

The current distribution interface builds separate **x86-64** and **i686** device compilers from unmodified, pinned LLVM/MLIR 18.1.3 sources.
Each bundle contains its native `bin/nierc`, `opt`, `llc`, `ld.lld`, and `llvm-ar`, their runtime library closure, one matching native link/runtime sysroot, and compiler-rt CRT/builtins.
The x86-64 bundle contains the x86-64 sysroot; the i686 bundle contains the i686 sysroot. Neither contains the opposite target's sysroot or NieR native ABI implementation.
The standalone compiler is language-blind. Clang, frontend plugins, publication tools, headers, CMake, source files, and private captures are not included.

Both source-built device bundles have passed the fresh publish-once matrix: each compiled 28 executable outputs, three shared libraries, and one archive, with twelve rejection/preservation cases.
The i686 run used a real 32-bit Linux kernel, not the development host's compatibility mode.
The complete fresh dual-destination corpus also passed on 2026-09-12: 50 artifacts were published once and compiled on both devices, with all original selected cJSON and zlib tests passing.
An earlier retained-artifact i686 regression compiled all 50 prior artifacts and passed the original cJSON and zlib suites, but did not rerun source publication or native-reference builds.

## Build and assemble the separate products

Run from the repository root in Bash on the current x86-64 Ubuntu 24.04 development host.
Bootstrap the publisher SDK first, then build each selected consumer SDK and matching NieR compiler:

```bash
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
bash scripts/bootstrap-consumer-sdk.sh x86_64
bash scripts/bootstrap-consumer-sdk.sh i686
cmake --preset consumer-x86_64
cmake --build --preset consumer-x86_64
ctest --preset consumer-x86_64
cmake --preset consumer-i686
cmake --build --preset consumer-i686
ctest --preset consumer-i686
```

The consumer bootstrap compiles the pinned upstream sources, rather than extracting a prebuilt monolithic LLVM library.
Build-host TableGen executables are generated privately and are not device payloads.
Their compiler and LLVM sources are pinned, but their C/C++ headers and runtime currently come from the supported build host.
This build-only dependency means the SDK bootstrap is not fully hermetic; a pinned source receipt is not a claim of reproducibility across arbitrary host environments.
The build registers only LLVM's X86 backend family and links required LLVM/MLIR components statically; a monolithic `libLLVM` dependency is rejected during packaging.
The X86 backend family itself includes both x86 widths, but a particular `nierc` links exactly one native specialization and admits only its own device target for native compilation or lowering.
Stock LLD retains its upstream multi-format and relocation handling; this step does not patch LLVM or LLD to remove those internal capabilities.

This is a substantial source build, not the quick publisher package extraction.
It requires the publisher SDK and ordinary host build/download tools, including Python 3, and currently requires checkout/SDK paths without whitespace.
Source build caches are tied to their original consumer SDK and build-host SDK directories.
Changing either directory rejects before reusing cached compiler paths; reuse the original directories or start with a fresh build cache/checkout.
`NIER_CONSUMER_COMPILE_JOBS=1|2` controls source compilation parallelism; the source bootstrap serializes heavyweight target builds and bounds link parallelism.
The i686 build's host-side component tests use the development machine's 32-bit compatibility support. They do not replace the real 32-bit-kernel acceptance below.

Assemble both already built products into new directories:

```bash
mkdir -p artifacts
bash scripts/package-consumer.sh build/consumer-x86_64 \
  artifacts/nierc-x86_64 .sdk/consumer/x86_64
bash scripts/package-consumer.sh build/consumer-i686 \
  artifacts/nierc-i686 .sdk/consumer/i686
```

Each output directory must not already exist; its parent must exist.
The packager requires a completed matching consumer SDK receipt and component-linked device build. The publisher `.sdk` and `build/prealpha` are not substitutes for these inputs.
Assembly uses Python 3, Bash/coreutils, `rg`, and `readelf`; these assembly utilities are not runtime requirements of the installed compiler.
The relocation tests additionally use `strace`, `nm`, and `ar` on the development host.

## Run on the matching device

Move the entire directory anywhere before compiling:

```sh
./bin/nierc --print-target
./bin/nierc --check-sdk
./bin/nierc application.nier -o application
./application
```

No environment setup, compiler wrapper, patched LLVM binary, `patchelf`, or native application ELF rewrite is required.
`nierc` locates its sibling `sdk` directory. An explicit `--sdk` or `NIER_SDK_ROOT` overrides that discovery.
Only its LLVM subprocesses receive the bundle's runtime library search path; the native application runs normally without that environment.
`--print-target` reports `x86_64` or `i686`. An explicit foreign `--target` rejects for both ordinary compilation and diagnostic `lower` before output staging.
Inspection checks the shared schema across all declared domains, but explicitly reports when a foreign plan has no available native validator in this device compiler.
`--check-sdk` is a read-only installation check: it compares this compiler's embedded SDK identity with the selected SDK's completion receipt and checks required tool files.
The packager runs it against the staged sibling SDK, catching a stale compiler paired with newly built SDK inputs before assembly succeeds.
If it fails after relocation, first clear `NIER_SDK_ROOT` to rule out an unintended developer override, then rebuild/reinstall the matching complete compiler bundle.
This is consistency validation, not a signature check, tamper-resistant attestation, or SENieR enforcement.

The compiler/tool host glibc and dynamic loader use the Noble glibc 2.39 ABI baseline for the matching architecture.
On i686 the device must provide the normal `/lib/ld-linux.so.2` loader and matching runtime; on x86-64 it provides the normal x86-64 loader/runtime.
Other compiler dependencies come from the bundled SDK libraries.
Native application output uses the bundle's managed glibc/runtime paths as they existed when it was compiled.
This prototype does not relocate already compiled output when its runtime directory moves.
Keep that directory in place; production runtime installation and lifecycle management remain future work.

The bundle's `sdk/consumer-sdk.json`, `sdk/source.lock`, `sdk/packages.lock`, and `sdk/sdk-lock.sha256` identify the source-built target SDK; `payload.sha256` records copied file contents.
In the repository, the source/dependency locks live under `sdk/consumer/`, separately from the publisher's `sdk/packages.lock`.
These receipts are not signatures or tamper-resistant security enforcement.
The root `LICENSE` carries NieR's Apache-2.0 terms and is included in the payload checksums.
Ubuntu package copyright notices for included components are retained under `licenses`.
Release license/source-offer review remains a distribution-release task.

This is not a minimum-footprint SDK: the native C runtime set is intentionally preserved and required optimizer/code-generator components remain substantial even with only the X86 backend family registered.
Removing publication components does not impose an additional language subset on NieR input. The compiler's existing semantic and native-target qualification limits still apply; this prototype does not claim every C ABI construct is already supported.

The local checkpoint's assembled directories have the following measured sizes, rounded in MiB:

| Distribution | Size |
|---|---:|
| Earlier monolithic x86-64 baseline | 190 MiB |
| Native-component x86-64 bundle | 244 MiB |
| Native-component i686 bundle | 265 MiB |

Static components are repeated across separate compiler tools, so the new bundles are larger than the earlier baseline.
This checkpoint establishes native implementation isolation and frontend-free distribution, not a footprint reduction or a smallest possible compiler.
These are measurements of the current assembled directories, not compressed download sizes or guarantees for future builds.

## Dual-device acceptance

The distribution now supplies an independent, genuinely 32-bit i686 compiler bundle alongside the x86-64 bundle.
This is separate from compiling an ELF32 application with a 64-bit compiler or running one under a 64-bit kernel's compatibility support.
The consumer SDK inputs live under `.sdk/consumer/x86_64` and `.sdk/consumer/i686`; the corresponding assembled bundles remain distinct device products.
Both refreshed bundles passed the complete fresh dual script, including per-target stock-native references and the i686 kernel's ELF64 `ENOEXEC` rejection.
The i686 VM used KVM and 3 GiB RAM. The complete fresh dual-destination corpus also passed with these compiler/runtime/SDK inputs.
The manually dispatched GitHub Actions workflow `Device compiler qualification`, defined in `.github/workflows/device-compilers.yml`, runs SDK source builds, publisher/device component tests, packaging, this matrix, and the complete dual-destination corpus.
It is separate from the normal publisher CI smoke workflow; a configured workflow is not a recorded passing run.

With a coherent publisher build and both already assembled bundles, run from the repository root in Bash:

```bash
bash tests/dual-consumer.sh build/prealpha/nier.cfg .sdk \
  /absolute/path/to/x86_64-bundle /absolute/path/to/i686-bundle
```

The script creates a fresh private fixture directory, publishes each portable artifact once, and checks the same artifact hashes before and after both compilations.
It compares Hello and shared/static caller behavior with stock-native references, checks duplicate archive-member ordering and lazy extraction, and exercises native callers of generated DSOs.
The existing scalar, native-width, storage, packed/bitfield, overlapping storage, variadic/forwarded `va_list`, nonlocal-jump, conditional, and aggregate fixtures run at O0 and O2.
Per device, the runner checks 28 executable outputs, three shared libraries, one static archive, and twelve rejection/preservation cases.
A foreign-only artifact may pass structural inspection only with an explicit unavailable-native-validation report; native compilation and lowering must reject it.
Malformed operations in either conditional domain must reject even when that domain is inactive on the device.
These are bounded functional regressions, not the full C/ABI corpus or a performance measurement.

The i686 half boots a real pinned Linux 6.1 i386 kernel with Noble i386 BusyBox and the bundle's ordinary glibc 2.39 runtime.
`tests/vm/packages.lock` pins the test-only packages and their SHA-256 values; packages are extracted locally without installation or maintainer scripts.
The guest has no network device, source frontend, or shared host filesystem.
It must identify as i686, run ELF32 compiler tools, and reject a supplied ELF64 executable with the kernel's `ENOEXEC` before its compilation checks can pass.
An explicit serial PASS receipt is required; merely exiting QEMU is insufficient.

The VM host needs `qemu-system-i386`, `cpio`, `gzip`, `curl`, `dpkg-deb`, `readelf`, `timeout`, and ordinary Bash/coreutils.
Defaults are 3 GiB of guest RAM, a 900-second timeout, and KVM when an actual preflight succeeds; otherwise the runner uses TCG.
`NIER_VM_ACCEL=auto|kvm|tcg`, `NIER_VM_RAM_MIB` (512–4096), and `NIER_VM_TIMEOUT` (30–3600 seconds) make those test limits explicit.
TCG results are functional evidence only. The runner records the selected accelerator and does not compare its elapsed time with native compilation performance.

Downloaded test packages are cached under `.sdk/test-vm/downloads` by default; `NIER_VM_CACHE` selects another cache.
Every use rechecks the pinned package digest.
Private fixture, root filesystem, initramfs, and serial-log workspaces are retained in the printed temporary locations, including on failure.
To repeat only the i686 consumer stage against an existing fixture set without claiming a fresh publication, run:

```bash
bash tests/consumer-vm.sh /absolute/path/to/i686-bundle \
  /absolute/path/to/retained-fixtures
```

Keep the original fixture directory for comparison and reporting.
This repeat is consumer regression evidence, not a fresh source-to-artifact or full upstream-corpus qualification.

### Full corpus on both destination compilers

The opt-in dual-destination corpus command uses the normal publisher SDK and builder, the independent x86-64 compiler, and the independent i686 bundle:

```bash
bash corpus/qualify.sh build/prealpha/nier-build \
  /absolute/path/to/x86_64-bundle/bin/nierc .sdk \
  --i686-bundle /absolute/path/to/i686-bundle
```

Add `--project cjson` or `--project zlib` to qualify only that complete project; this does not filter individual upstream tests.
Full selection publishes 50 artifacts once and requires both destination compilers to consume those same bytes.
The existing native-profile checks and x86-64 destination checks run first.
The i686 guest then runs the original 19 cJSON CTests for each static/shared configuration and zlib's original `test test64` recipes against its own generated native outputs.
Native-reference callers, shared-library loader traces, SONAME/version checks, and archive-member ordering provide additional evidence.

The guest receives the generated test recipes and runtime data, not source files or a frontend.
Its pinned ELF32 CTest, GNU make, and readelf tools, CTest support data, and runtime dependency closure live under `/opt/test-tools`; they are not included in the distributed compiler.
`tests/vm/test-tools.lock` records those test-only inputs. Explicit loader arguments scope test-tool libraries to the runners, so application processes do not inherit an accidental test-library override.
The runner also clears publisher SDK and loader environment overrides before invoking either thin compiler.

The full-corpus guest has a default 3600-second bound, configurable through the same `NIER_VM_TIMEOUT` setting.
Publication, repeated native builds, and emulated compilation can be substantial work; an unchanged upstream test inventory is required regardless of accelerator.
`qualification.txt`, per-publication logs, the exported fixture hashes, and the VM serial evidence remain under the printed private corpus workspace.
A successful prior x86-64 run or fixture-preparation pass is not a completed dual-device qualification.

The recorded 2026-09-12 local run completed the full fresh command with `Result: PASS (all)`.
All 50 publications and their native references were rebuilt; both devices compiled the same artifact bytes.
cJSON passed all 19 original CTests in each static/shared configuration on each destination, and zlib passed its original static/shared/64-bit-offset recipes.
Native-caller, loader, SONAME/version, archive-order, and checksum checks also passed, with an explicit real-kernel i686 serial receipt.
This is a qualified functional checkpoint for the pinned configurations, not unrestricted C/ABI support, performance parity, reverse-engineering equivalence, security enforcement, or a remote Actions result.
