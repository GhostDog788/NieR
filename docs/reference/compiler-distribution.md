# Sela native compiler-only distributions

The current distribution interface builds separate **x86-64**, **i686**, **ARMv7 hard-float**, and **AArch64** Linux/glibc device compilers from unmodified, pinned LLVM/MLIR 18.1.3 sources.
Each bundle contains its native `bin/selac`, `opt`, `llc`, `ld.lld`, and `llvm-ar`, their runtime library closure, one matching native link/runtime sysroot, and compiler-rt CRT/builtins.
Every bundle contains exactly its own target's sysroot and Sela native ABI implementation.
The [native-target reference](adding-native-targets.md) explains the registry, baseline ABIs, and requirements for a new target.
The standalone compiler is language-blind. Clang, frontend plugins, publication tools, headers, CMake, source files, and private captures are not included.

The recorded pre-rename static-component device bundles at commit `63592ab` passed the fresh publish-once matrix: each compiled 28 executable outputs, three shared libraries, and one archive, with twelve rejection/preservation cases.
The i686 run used a real 32-bit Linux kernel, not the development host's compatibility mode.
The complete fresh dual-destination corpus also passed on 2026-09-12: 50 artifacts were published once and compiled on both devices, with all original selected cJSON and zlib tests passing.
An earlier retained-artifact i686 regression compiled all 50 prior artifacts and passed the original cJSON and zlib suites, but did not rerun source publication or native-reference builds.

The pre-rename footprint update, recorded in commit `be63890`, has a separate qualification status.
The earlier compact static-component checkpoint passed 10 component tests for each device and all 40 publisher tests.
The source recipe defaults to shared LLVM; both historical measured packages passed all 11 consumer CTests for their architecture, all 40 publisher tests, and the fresh same-fixture two-device matrix.
The new i686 matrix ran on a real 32-bit Linux kernel and rejected the ELF64 probe before passing its native compilation checks.
On 2026-09-13 the final shared packages also passed the full 50-artifact dual-device corpus through complete fresh cJSON and zlib project runs.
The earlier full qualification does not automatically cover newly linked or stripped compilers.
These historical results are not retroactively renamed Sela executions.
The earlier two-target Sela packages separately passed all 40 publisher tests, all 11 consumer CTests for each architecture, and their fresh same-artifact two-device matrix.
That earlier dual-device zlib corpus passed.
cJSON's fresh publication, native-reference, and x86-64 stages passed; its i686 stage subsequently passed using the unchanged published fixtures after a failed first VM boot.
That covers all required stages for all 50 artifacts, but the original cJSON command remains recorded as failed, not retroactively changed to a single-command pass.

## Build and assemble the separate products

Run from the repository root in Bash on the current x86-64 Ubuntu 24.04 development host.
Bootstrap the publisher SDK first, then build each selected consumer SDK and matching Sela compiler:

```bash
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
device_target=armv7 # choose x86_64, i686, armv7, or aarch64
bash scripts/bootstrap-consumer-sdk.sh "$device_target"
cmake --preset "consumer-$device_target"
cmake --build --preset "consumer-$device_target"
ctest --preset "consumer-$device_target"
```

The consumer bootstrap compiles the pinned upstream sources, rather than extracting a prebuilt monolithic LLVM library.
Build-host TableGen executables are generated privately and are not device payloads.
Their compiler and LLVM sources are pinned, but their C/C++ headers and runtime currently come from the supported build host.
This build-only dependency means the SDK bootstrap is not fully hermetic; a pinned source receipt is not a claim of reproducibility across arbitrary host environments.
The build registers only the selected target's LLVM family: X86, ARM, or AArch64.
`SELA_CONSUMER_LLVM_LINKAGE=shared` is the default layout: stock LLVM builds one native shared library used by the tools and `selac`, while the required MLIR components remain statically linked.
`SELA_CONSUMER_LLVM_LINKAGE=static-components` retains the component-linked comparison path.
The completed SDK receipt determines the matching Sela link configuration; `SELA_USE_CONSUMER_SDK=ON` identifies a device SDK independently of that linkage choice.
For example, prefix a bootstrap command with `SELA_CONSUMER_LLVM_LINKAGE=static-components` to request that comparison build.
The X86 backend family itself includes both x86 widths, but a particular `selac` links exactly one native specialization and admits only its own device target for native compilation or lowering.
Stock LLD retains its upstream multi-format and relocation handling; this step does not patch LLVM or LLD to remove those internal capabilities.

Sharing LLVM is an ordinary native-library packaging choice, not a JIT execution stage.
The pipeline still writes native objects and links a normal executable before application execution; the resulting application does not need LLVM or `selac`.
The stock shared-library configuration includes LLVM's available components, so this is not a claim that all unused JIT-related code has been removed from the library.
SESela execution-policy enforcement remains a separate, unimplemented axis.

This is a substantial source build, not the quick publisher package extraction.
It requires the publisher SDK and ordinary host build/download tools, including Python 3, and currently requires checkout/SDK paths without whitespace.
Source build caches are tied to their original consumer SDK and build-host SDK directories.
Changing either directory rejects before reusing cached compiler paths; reuse the original directories or start with a fresh build cache/checkout.
Changing shared/static linkage can change generated LLVM headers and trigger compilation, not just relinking.
The bootstrap prints its actual planned compile, link, and generation work before the target build.
Do not run competing linkage variants against the same profile's source-build cache or SDK directory.
Retain an already assembled checkpoint before changing its SDK recipe, and rebuild the matching Sela compiler after the new SDK completes.
`SELA_CONSUMER_COMPILE_JOBS` accepts 1–8 and defaults to 2 compile jobs per target, not across the complete build machine.
`SELA_CONSUMER_PARALLEL_TARGETS=0|1` defaults to 0, which serializes the heavyweight target builds.
Set it to 1 in each bootstrap process to build independent targets concurrently in their separate SDK/build directories.
For example, 3 jobs for each architecture permit up to 6 concurrent target compilation jobs; choose the combined limit from available CPU and memory headroom.
Parallel target builds share the compile lock, but each profile retains an exclusive build lock, native-generator builds remain serialized, and a shared link lock serializes memory-heavy target C/C++ links.
No architecture must finish before another starts.
See [parallel SDK source builds](building-sela.md#parallel-sdk-source-builds) for an example which waits for and checks both processes.
The i686 build's host-side component tests use the development machine's 32-bit compatibility support. They do not replace the real 32-bit-kernel acceptance below.

Assemble the selected already built product into a new directory:

```bash
mkdir -p artifacts
bash scripts/package-consumer.sh "build/consumer-$device_target" \
  "artifacts/selac-$device_target" ".sdk/consumer/$device_target"
```

Each output directory must not already exist; its parent must exist.
The packager requires a completed matching consumer SDK receipt and device build with matching shared/static linkage. The publisher `.sdk` and `build/prealpha` are not substitutes for these inputs.
Assembly uses Python 3, Bash/coreutils, `rg`, `readelf`, and the pinned build-host `llvm-strip`; these assembly utilities are not runtime requirements of the installed compiler.
The relocation tests additionally use `strace`, `nm`, and `ar` on the development host.

### Device-only archive dependency and release stripping

Only the device artifact reader links selected objects from the pinned `libarchive.a`.
The existing tar reader, format limits, and rejection rules are retained; no new archive parser or public artifact format is introduced.
Publisher tools and test-fixture writers continue using shared libarchive, including the support needed to produce rejected compressed-input fixtures.
Selecting the device reader's archive objects removes its shared libarchive/XML/ICU dependency chain without changing the managed application runtime.

The packager first verifies the original SDK tool/library receipts and stages copies.
It then strips ordinary symbol tables from the staged `selac`, four LLVM tools, and shared LLVM library when present.
The build products and SDK originals remain unstripped for debugging and backend-identity inspection.
Required dynamic symbols and unwind data are retained.
Application runtime files, static link archives, CRT objects, and compiler-rt are copied without this stripping pass; locale, NSS, and charset modules are not removed to obtain a smaller number.

## Run on the matching device

Move the entire directory anywhere before compiling:

```sh
./bin/selac --print-target
./bin/selac --check-sdk
./bin/selac application.sela -o application
./application
```

No environment setup, compiler wrapper, LLVM source patch, `patchelf`, or native application ELF rewrite is required.
`selac` locates its sibling `sdk` directory. An explicit `--sdk` or `SELA_SDK_ROOT` overrides that discovery.
Only its LLVM subprocesses receive the bundle's runtime library search path; the native application runs normally without that environment.
`--print-target` reports exactly one registered target ID. An explicit foreign `--target` rejects for both ordinary compilation and diagnostic `lower` before output staging.
Inspection checks the shared schema across all declared domains, but explicitly reports when a foreign plan has no available native validator in this device compiler.
`--check-sdk` is a read-only installation check: it compares this compiler's embedded SDK identity with the selected SDK's completion receipt and checks required tool files.
Packaging validates the matching build/SDK identity and inspects the staged ELF/dependency closure without executing a cross-built compiler.
Runtime `--check-sdk` and real compilation belong to device/component qualification, including the relocated-installation test.
If it fails after relocation, first clear `SELA_SDK_ROOT` to rule out an unintended developer override, then rebuild/reinstall the matching complete compiler bundle.
This is consistency validation, not a signature check, tamper-resistant attestation, or SESela enforcement.

The compiler/tool host glibc and dynamic loader use the Noble glibc 2.39 ABI baseline for the matching architecture.
The device must provide its normal native loader and matching host runtime: for example `/lib/ld-linux.so.2` on i686 and `/lib/ld-linux-armhf.so.3` on ARMv7.
The registry defines the precise loader and multiarch paths; changing CPU does not change the glibc platform contract.
Other compiler dependencies come from the bundled SDK libraries.
Native application output uses the bundle's managed glibc/runtime paths as they existed when it was compiled.
This prototype does not relocate already compiled output when its runtime directory moves.
Keep that directory in place; production runtime installation and lifecycle management remain future work.

The bundle's `sdk/consumer-sdk.json`, `sdk/source.lock`, `sdk/packages.lock`, and `sdk/sdk-lock.sha256` identify the source-built target SDK.
The completion receipt records original unstripped tool hashes, LLVM linkage/configuration, and the native shared LLVM library's SONAME and hash when present.
Those source-product hashes are checked before staging; they are not hashes of the stripped delivery copies.
`payload.sha256` records the final delivered regular-file contents, including stripped binaries and copied receipts.
In the repository, the source/dependency locks live under `sdk/consumer/`, separately from the publisher's `sdk/packages.lock`.
These receipts are not signatures or tamper-resistant security enforcement.
The root `LICENSE` carries Sela's Apache-2.0 terms and is included in the payload checksums.
Ubuntu package copyright notices for included components are retained under `licenses`.
Release license/source-offer review remains a distribution-release task.

This is not a minimum-footprint SDK: the native C runtime set is intentionally preserved and required optimizer/code-generator components remain substantial even with only one LLVM backend family registered.
Removing publication components does not impose an additional language subset on Sela input. The compiler's existing semantic and native-target qualification limits still apply; this prototype does not claim every C ABI construct is already supported.

## Footprint measurements and current checkpoint

Compare total bundle size as well as the individual `selac` file.
Moving a dependency into a binary can enlarge that binary while shrinking its complete installed dependency closure; shared LLVM addresses a different duplication cost across the separate tools.
Neither linkage choice alone proves that a package is smaller.

### Four-target component-qualified packages

The current four-target candidates are retained at `artifacts/four-target-final/selac-TARGET`.
Their exact measured regular-file inventory is:

| Target | Complete bundle bytes | MiB | Stripped `selac` bytes | Shared LLVM bytes | Native runtime bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| x86-64 | 96,429,504 | 91.96 | 1,873,952 | 59,985,784 | 24,375,736 |
| i686 | 103,153,655 | 98.37 | 2,333,200 | 70,300,420 | 19,815,135 |
| ARMv7 | 70,724,546 | 67.45 | 1,603,668 | 47,956,892 | 13,094,743 |
| AArch64 | 97,589,462 | 93.07 | 1,851,440 | 54,805,600 | 30,993,113 |

The component columns are already included in the complete bundle, not extra costs.
Other included bytes are the four LLVM executables, their remaining runtime libraries, compiler-rt, licenses, and metadata.
These are installed regular-file totals, not compressed download sizes, memory usage, or filesystem-rounded `du` values.

Each bundle has one matching sysroot, one Sela native ABI implementation, and one registered stock LLVM backend family.
The two x86 widths intentionally retain the shared X86 family; ARMv7 registers ARM and AArch64 registers AArch64.
No shared libarchive, XML, or ICU dependency is shipped, and the dependency inventory has no unreferenced compiler libraries.
The shipped compiler, LLVM tools, and shared LLVM have no ordinary symbol tables; dynamic symbols and required unwind information remain.
The managed runtime and link inputs are preserved without that stripping pass, so whole-bundle symbol/debug totals can still be nonzero, particularly in the ARMv7 runtime.

All four final consumer builds passed their complete **11/11 component tests**.
This includes package/IR validation, malformed inputs, target ABI checks, independent production, relocated installation, and package-input rejection tests.
The x86-64 tests ran natively, i686 used the host's compatibility mode, and ARM tests used explicitly provisioned user-mode emulation.
These results do not claim real target-kernel VM acceptance or complete corpus qualification; those are separate gates below.
Host `strace` evidence is used only for native/compatibility execution; foreign emulation cannot substitute for source-free device evidence.

The initial failures are not hidden: the conditional test incorrectly requested unavailable native backends in thin builds, and ARM32 fixture writing initially used a 32-bit header `time_t` against Noble's 64-bit-time libarchive ABI.
The conditional test now validates foreign public structure without requesting an unlinked backend.
Only the two libarchive-calling translation units receive the matching private ARM time64 definitions; direct timestamp-ABI and deterministic-archive regressions pass.
No public application ABI, archive validation limit, or native comparison proof was weakened for these fixes.

Local component logs are `.work/four-targets/final-consumer-tests/TARGET.log`.
The local `.work/four-targets/final-consumers.json` snapshot records exact package groups, compiler and payload-index hashes, SDK identities, and execution scope.
Each delivered bundle also carries its own `payload.sha256` and matching SDK receipt.
Earlier packages, including `artifacts/four-target/` and the pre-ARM `artifacts/selac-*` products, remain unchanged.
The final packages retain their assembly-time README snapshots; updating this live reference does not rewrite those measured payloads.

For this local checkpoint, ARM LLVM/MLIR SDKs were genuinely cross-built from the pinned sources on x86.
The already installed x86 LLVM/MLIR payloads were reused after an explicit relocation audit: the upstream source lock, selected dependency rows, compiler/linkage configuration, and installed tool/shared-library hashes matched their earlier receipts, and native ABI/loader/tool probes passed.
Their new receipts bind a `reuse_provenance` record describing exactly that local audit and the previous SDK identity.
This is not a claim that x86 LLVM was rebuilt, a general bypass for stale receipts, or permission to rewrite Ninja dependency history.
The Sela compilers themselves were rebuilt and separately requalified; future LLVM source builds retain their ordinary cache checks.

### Historical pre-rename measurements

The pre-rename checkpoints recorded in commits `63592ab` and `be63890` have these regular-file payload sizes, rounded to two decimal places in MiB.
They are historical package measurements, not measurements of the current renamed Sela deliverables:

| Distribution checkpoint | x86-64 | i686 |
|---|---:|---:|
| Earlier monolithic LLVM baseline | 188.96 MiB | — |
| Original unstripped static-component packages | 242.70 MiB | 263.15 MiB |
| Compact static-component packages | 179.43 MiB | 201.58 MiB |
| Shared-LLVM checkpoint | 91.89 MiB | 98.28 MiB |

The compact static checkpoint used separate locally retained x86-64 and i686 packages before the rename.
Its respective exact totals are 188,145,251 and 211,373,966 regular-file bytes; its stripped compiler executables are 4,897,048 and 5,468,444 bytes.
These figures describe those retained packages, not an estimate of shared-library savings.
The earlier compact static checkpoint passed 10 component tests per device and all 40 publisher tests.

The pre-rename shared-LLVM packages were retained separately from that static checkpoint.
Their exact historical measurements are:

| Measurement | x86-64 bytes | i686 bytes |
|---|---:|---:|
| Total regular-file payload | 96,352,022 | 103,058,855 |
| Stripped compiler executable | 1,816,856 | 2,257,912 |
| Shared LLVM library | 59,985,784 | 70,300,420 |
| Normalized tar/gzip-1 stream | 38,388,230 | 41,890,028 |

The historical compiler executables are 1.73 MiB and 2.15 MiB; the shared LLVM libraries are 57.21 MiB and 67.04 MiB.
Those component rows are already part of the total payload, not additional bytes to add to it.
The compressed row is the reporter's normalized stream measurement, not installed size or an actual shipping archive.

Both historical shared packages passed their complete 11-test consumer suites; all 40 publisher tests also passed.
The fresh matrix passed 28 executable outputs, three shared libraries, one archive, and twelve rejection/preservation cases on each device using the same fixtures.
The i686 stage used Linux `6.1.0-50-686-pae`, KVM, and 3 GiB RAM, with the required ELF64 `ENOEXEC` rejection.
Both architectures passed the total-package size-reduction gate and the full fresh dual-device corpus qualification at that checkpoint, establishing shared LLVM as the adopted default layout.
The native runtime and compiler-rt payloads remain byte-identical to the original packages: 309 regular files on x86-64 and 305 on i686, with symlink targets also preserved.

Those historical packages remain unchanged after assembly and measurement; the rename does not rewrite their executable names, manifests, hashes, or receipts.
Each contains the README snapshot captured when it was assembled, which may still describe a qualification step as pending.
This reference records the later qualification of those same historical package bytes; it does not require editing or repackaging a measured checkpoint just to update its README.
The original local package paths and temporary evidence locations are preserved in `git show be63890:docs/reference/compiler-distribution.md`, rather than replaced here with invented renamed paths.

These regular-file totals exclude symlink/directory entries and filesystem block rounding, so they differ from the earlier `du -sh` figures of 190/244/265 MiB.
They are not compressed download sizes, runtime memory use, or guarantees for future builds.
The [device-compiler retrospective](device-compiler-retrospective.md) preserves the original regression analysis and timing evidence.

### Historical Sela rename checkpoint

Fresh Sela compiler builds and assembly produced `artifacts/selac-x86_64` and `artifacts/selac-i686` on 2026-09-13.
The x86-64 package contains approximately 91.9 MiB of regular-file data; the i686 package contains approximately 98.3 MiB.
Their stripped `selac` executables are 1,816,856 and 2,257,912 bytes respectively.
These are fresh measurements of renamed packages, not relabeled historical totals; metadata and receipt changes account for the small total-size differences.
Use the read-only reporter below for exact bytes of a particular assembled package; refreshing its README and payload checksum changes metadata sizes without changing the compiler or runtime.

The Sela publisher passed all 40 tests, and each separate device compiler passed all 11 consumer tests.
The fresh matrix published fixtures once in `/tmp/sela-dual-consumer-J2vB8m` and passed 28 executable outputs, three shared libraries, one static archive, and twelve rejection/preservation cases on both devices.
The real i686 run in `/tmp/sela-consumer-vm-SrRjQE` identified the pinned Linux `6.1.0-50-686-pae` kernel and rejected the ELF64 probe with `ENOEXEC` before passing.
Its first VM attempt produced no serial output and was stopped with its logs retained; an unchanged-fixture retry passed without a compiler or test-code change, and the first stall's root cause was not established.

The fresh zlib report in `/tmp/sela-corpus-Wd1aMk/qualification.txt` passed all eight artifacts on both devices from 10:46:35 to 10:48:35 UTC, with i686 evidence in `/tmp/sela-consumer-vm-pvIIjp`.
The cJSON run in `/tmp/sela-corpus-psJfLM/qualification.txt` completed all 42 publications and both x86-64 destination 19-test suites, but ended with `Result: FAIL (cjson)` at 11:00:15 UTC.
Its i686 guest in `/tmp/sela-consumer-vm-VPUftY` reported `Initramfs unpacking failed: broken padding` before it could run the compiler or corpus tests.
That failed report is preserved unchanged.
The retained image later failed gzip CRC/length and archive-alignment checks, and its observed hash differed from the recorded pre-boot hash; the origin and timing of the corruption were not established.

A separate source-free i686 continuation in `/tmp/sela-consumer-vm-UsDTHC` consumed the same 42 published fixtures and passed both original 19-test suites, all artifact checksum checks, and explicit `SELA_CONSUMER_CORPUS_PASS target=i686 artifacts=42` and `SELA_VM_PASS` receipts.
Its log is `.work/sela-rename/cjson-vm-retry.log`; no application sources, compiler packages, or publication inputs were changed for that continuation.
Together with the completed fresh publication/native-reference/x86-64 stages, this completes the required cJSON coverage across both devices.
It is staged qualification evidence, not a new fresh publication run or a claim that the original `Result: FAIL (cjson)` became `PASS`.
These Sela results are separate from the pre-rename corpus passes above.

The final package documentation refresh is planned after this reference snapshot is frozen.
It must change only the copied README and its payload checksum, preserving every compiler, SDK, and runtime byte and every symlink; a file-by-file comparison verifies that boundary.
A separate fresh two-device matrix will check that final packaged snapshot, including the test harness's added pre-boot gzip-integrity check.
That later matrix result and exact final package sizes belong in the separate local validation record, `.work/sela-rename/validation.md`, without recursively changing the measured package README.
This snapshot records the completed stages above, not an unobserved pass for that later packaging check.

### Read-only bundle size report

Packaging prints a component report automatically.
To inspect an existing package or compare two checkpoints, run:

```bash
python3 -B scripts/bundle-size.py artifacts/four-target-final/selac-x86_64 \
  --compare /path/to/another-sela-bundle
python3 -B scripts/bundle-size.py artifacts/four-target-final/selac-armv7 --json
python3 -B scripts/bundle-size.py artifacts/four-target-final/selac-aarch64 --compressed
```

The reporter groups `selac`, each LLVM tool, shared LLVM, other compiler libraries, the managed runtime, compiler-rt, and metadata.
It reports exact regular-file bytes separately from allocated filesystem bytes and ELF code, ordinary symbol, and unwind sections.
Hardlinked data is counted once and symlinks are not followed.
The archive-exclusive dependency subset is already included in the component totals; it is not an extra saving to add a second time.
`--compressed` measures a normalized tar/gzip-1 stream without writing an archive and is optional to keep ordinary reporting fast.
Compare compressed values produced by the same reporter; different tar/compression recipes are not interchangeable measurements.

## Four-target acceptance

The fresh core matrix passed on all four final bundles on 2026-09-13.
Its final-publisher refresh used one fixture set, `/tmp/sela-multi-consumer-SerRew/fixtures`, with identical checked artifact bytes for every device.
Each guest reported 28 executable checks, three shared outputs, one static output, and 24 negative cases, then its unique `SELA_VM_PASS` receipt.

| Device | Real Linux kernel | Execution | Elapsed seconds | Retained VM evidence |
| --- | --- | --- | ---: | --- |
| x86-64 | `6.8.0-139-generic`, x86-64 | KVM, unchanged-image retry | 6 | `/tmp/sela-consumer-vm-mDnCKF/retry-kvm-t0bO2d.log` |
| i686 | `6.1.0-50-686-pae`, i686 | KVM | 9 | `/tmp/sela-consumer-vm-S2aKN7` |
| ARMv7 | `6.8.0-139-generic`, armv7l | TCG | 153 | `/tmp/sela-consumer-vm-Ivc4sD` |
| AArch64 | `6.8.0-139-generic`, aarch64 | TCG | 192 | `/tmp/sela-consumer-vm-KC9bNO` |

The four guests ran concurrently with 1536 MiB RAM each and a 900-second bound.
Their initramfs contained only the device bundle, fixtures, native references, and test harness—not publisher/source trees, frontends, shared host filesystems, or networking.
Both 32-bit products used genuine 32-bit kernels; every guest also required foreign-architecture ELF execution to fail with `ENOEXEC`.
Logs are retained at `.work/four-targets/core-vm-final-TARGET.log` and the separate x86-64 retry log above; native-reference output equality and unchanged payload/fixture hashes were checked inside each passing guest.
The first x86-64 attempt failed with `Initramfs unpacking failed: broken padding` before executing the compiler.
Its original log remains a failure; the separate retry passed with the exact same kernel, image, bundle, and fixture bytes.
Gzip integrity and a strict scan of all 585 newc records, alignment padding, and the archive trailer passed, and the image SHA-256 remained `676062ff7d1940618fbdef108ee09dfca2f38c84ec61f28c5b445abc2ff8144b` before and after retry.
The first boot failure's cause is not established; this is not evidence of a compiler fix or a new publication run.
The preceding four-device core checkpoint is also retained in `.work/four-targets/core-vm-TARGET.log` with fixtures `/tmp/sela-multi-consumer-xIiDSW/fixtures`.
Elapsed times are functional diagnostics, not native-performance measurements.
The complete cJSON/zlib corpus is a separate acceptance gate, recorded in the [four-target corpus checkpoint](#four-target-corpus-checkpoint-on-2026-09-13); core success alone does not establish it.

The current generic matrix is `tests/multi-consumer.sh`.
Prepare portable fixtures once without any device compiler dependency:

```bash
bash tests/multi-consumer.sh --prepare-only build/prealpha/sela.cfg .sdk
```

The command prints its retained workspace; the source-free fixtures are its `fixtures` subdirectory.
Each target job receives exactly those bytes, checked against `fixtures.sha256`:

```bash
bash tests/consumer-vm.sh armv7 artifacts/selac-armv7 \
  /absolute/path/to/retained-fixtures
```

Repeat for every target, with the matching `selac-TARGET` bundle.
To prepare and run all four consecutively with existing bundles:

```bash
bash tests/multi-consumer.sh build/prealpha/sela.cfg .sdk artifacts
```

The runner preserves the existing 28 executable outputs, three DSOs, and one archive per target, including native references, shared-library callers, duplicate archive-member order, and lazy extraction.
It also checks 24 rejection/preservation cases per target, covering all three foreign targets and malformed inactive domains.
These counts are obligations of the current harness, not inherited proof that a newly built candidate passed.
The same fixtures include scalar/native-width, storage, packed/bitfield, overlap, variadic/forwarded-cursor, nonlocal-jump, conditional, and ordinary aggregate-call cases at O0 and O2.
A foreign-only artifact can pass structural inspection only with an explicit unavailable-native-validation report; compilation and lowering reject it.

All four destination stages boot their own real pinned Linux kernel and static BusyBox from `tests/vm/packages.lock`.
Both 32-bit targets must run a 32-bit kernel, not user-mode emulation or a 64-bit compatibility kernel.
Guests have no network device, frontend, application sources, or shared host filesystem.
The bundle is staged at `/opt/sela`, so independently prepared native-reference loader paths remain identical across jobs.
Checks establish actual kernel identity, compiler-tool ELF class/machine, foreign executable `ENOEXEC`, and an explicit fresh serial PASS receipt.
Merely exiting QEMU is insufficient.

The host needs the matching `qemu-system-*` tools plus `cpio`, `gzip`, `curl`, `dpkg-deb`, `readelf`, `timeout`, and ordinary Bash/coreutils.
KVM is used when applicable and its preflight succeeds; foreign ARM guests on x86 use TCG.
Guest RAM, accelerator and finite time limits are recorded in retained evidence.
User-mode QEMU/binfmt support for publisher probes is provisioned explicitly and is separate from these full-system guests.
No guest test-only tools or kernel inputs become compiler-bundle dependencies.

The layered `Device compiler qualification` workflow runs the four-target core matrix on PRs and adds the complete corpus on main/manual runs.
Each job captures build stderr from the start and collects diagnostics before its always-run artifact upload, including CTest logs and publication/VM receipts even when packaging never finishes.
The collector uses a dedicated job temporary directory, excludes source and SDK trees, and does not follow symlinks or hardlinks.
Failed-job boot images and selected private capture evidence are retained within explicit size limits; `evidence-manifest.json` records omissions and any truncated text-log tails, while binary evidence is never truncated.
The publisher uploads one hash-checked fixture archive and device jobs run independently in parallel.
Configured CI is not a claim of a completed remote run.
VM elapsed time is diagnostic, not a native-performance threshold.

### Full corpus on every destination compiler

The publisher can prepare all 50 artifacts and their per-target native references before any device bundle exists:

```bash
bash corpus/qualify.sh build/prealpha/sela-build .sdk --prepare-only
```

Use the printed `consumer-fixtures` directory with `tests/consumer-vm.sh TARGET BUNDLE FIXTURES` on each target.
That direct command selects a 3600-second default when `FIXTURES/corpus.list` exists, while core fixtures default to 900 seconds.
An explicit `SELA_VM_TIMEOUT` overrides either default within the supported 30–3600-second range; ARM corpus runs under software emulation can exceed the core limit.
Alternatively, with all four bundles under one parent:

```bash
bash corpus/qualify.sh build/prealpha/sela-build .sdk --bundle-parent artifacts
```

`--project cjson` or `--project zlib` selects one complete project, never individual upstream tests.
Both project runs together must cover all 42 cJSON and eight zlib artifacts, with identical hashes consumed by all four compilers.
Each guest runs the original 19 cJSON CTests for both static/shared configurations or zlib's original `test test64` recipes.
Native-reference callers, loader traces, SONAME/version checks, and archive ordering remain mandatory.

Generated recipes and runtime data are source-free.
Pinned native CTest, GNU make, readelf, and their separate dependency closure are staged under `/opt/test-tools`, not shipped in the compiler.
Their provenance is recorded by `tests/vm/test-tools.lock`.
Loader overrides are scoped to tools and cleared for application execution.

A preparation report is marked PREPARED, not PASS.
Fresh qualification requires every destination receipt against the exact compiler packages and published artifacts.
Logs and failure evidence remain in the printed private workspaces; replaying retained bytes is explicitly a consumer regression, not fresh publication.

### Four-target corpus checkpoint on 2026-09-13

Two complete fresh project preparations published all 50 artifacts with the final publisher: cJSON's 42 outputs from 13:46:02 to 14:14:29 UTC, and zlib's eight outputs from 13:46:40 to 13:49:55 UTC.
Every publication rebuilt and tested its original native references on all four targets; upstream sources, required tests, and configurations were not reduced for ARM.
Their separate `qualification.txt` reports remain correctly marked PREPARED because destination testing runs in separate jobs.
Both preparations have the identical publisher-input receipt SHA-256 `2f062e7758da8734459dac4f3dc07b25f46804c5dfe5128377935e54d51618ae`; the receipt was checked throughout preparation and after completion.

| Project | Source-free fixture directory | SHA-256 of `fixtures.sha256` |
| --- | --- | --- |
| cJSON, 42 artifacts | `/tmp/sela-corpus-344D5N/consumer-fixtures` | `f0022a113cec067fce9435cb4f2a7ae32d7d1d0235cbc94933076ecc25ba53dc` |
| zlib, eight artifacts | `/tmp/sela-corpus-tJX6sC/consumer-fixtures` | `6a8a97db079caa3e8039d33bef5a5e5fa51a8457b7442eda6ac122618e83c6d9` |

All 50 artifacts passed destination qualification on all four final packages, with the same checked fixture bytes supplied to every device.
This combines two complete fresh project preparations with separate source-free destination receipts; it is not a single combined `PASS (all)` command or a relabeling of the PREPARED reports.

| Target | cJSON: 42 artifacts, both 19-test suites | zlib: eight artifacts, original `test test64` |
| --- | --- | --- |
| x86-64 | PASS, 141 seconds | PASS, 49 seconds |
| i686 | PASS, 162 seconds | PASS, 55 seconds |
| ARMv7 | PASS, 2705 seconds | PASS, 884 seconds |
| AArch64 | PASS, 2730 seconds | PASS, approximately 938 guest seconds; unchanged-image continuation |

Device logs are `.work/four-targets/cjson-vm-final-TARGET.log` and `.work/four-targets/zlib-vm-final-TARGET.log`, with the separate AArch64 zlib continuations described below.
Times describe these functional VM runs, not native-performance comparisons.
The cJSON guests retained their full acceptance receipts in `/tmp/sela-consumer-vm-uE98ge` (x86-64), `/tmp/sela-consumer-vm-G8n69P` (i686), `/tmp/sela-consumer-vm-LAbhDe` (ARMv7), and `/tmp/sela-consumer-vm-LZRxXP` (AArch64).
The first three zlib guests are `/tmp/sela-consumer-vm-EYNAq4`, `/tmp/sela-consumer-vm-ptQ2De`, and `/tmp/sela-consumer-vm-CIAxNS`, respectively; AArch64's continuation is described below.
Each cJSON guest passed both original 19-test suites, its native caller and loader checks, and the final checksum/nonce receipts; every zlib guest passed the original `test test64` recipes and its library/archive checks.
No upstream source or required test was weakened, and every compiler-bundle payload remained unchanged from the component-qualified package inventory above.

The original AArch64 zlib attempt in `/tmp/sela-consumer-vm-60fRKv` failed when packaged LLVM `llc` received SIGSEGV while reading optimized bitcode for `deflate.o`.
The unchanged artifact and bundle subsequently compiled the complete static library under user-mode emulation; explicit `cortex-a53` and `max` runs of the failing stage produced identical objects, and the generated bitcode passed the host LLVM verifier.
Those controls use regenerated intermediate bitcode: the original guest's failing temporary bitcode was not recovered from its RAM filesystem after shutdown, so byte equality with that intermediate is not established.
An unchanged-image real-kernel retry then completed seven of eight artifacts without repeating the crash, but hit its 900-second diagnostic limit before the last executable and upstream tests finished.
That timeout remains a failure, not a full-corpus result.
The next unchanged-image continuation passed all eight artifacts and the original upstream recipes within its 3600-second bound; its log is `/tmp/sela-consumer-vm-60fRKv/retry-tcg-3600-PhI87z.log`.
It required matching-kernel identity, foreign ELF `ENOEXEC`, unchanged bundle/fixture checksums, and the exact `SELA_CONSUMER_CORPUS_PASS target=aarch64 artifacts=8` and `SELA_VM_PASS sela-consumer-vm-60fRKv` receipts.
The original crash's cause is not established, and no compiler, fixture, kernel, or boot-image changes were made for these diagnostic retries.
The image SHA-256 remained `4ca065c0289435e60f3f93e8e8d2fec6bdcc343b166026f7c70923f5c7064d22`; detailed controls are retained in `.work/four-targets/zlib-aarch64-retry-evidence.txt`.
After these VMs finished, a host-harness-only change gave manual corpus jobs a 3600-second default while retaining 900 seconds for core fixtures and honoring explicit overrides.
The passing corpus jobs already used explicit limits; this policy change does not alter their compiler, artifact, guest recipe, or image bytes.

### Historical dual-device acceptance

The recorded 2026-09-12 local run completed the full fresh command with `Result: PASS (all)`.
All 50 publications and their native references were rebuilt; both devices compiled the same artifact bytes.
cJSON passed all 19 original CTests in each static/shared configuration on each destination, and zlib passed its original static/shared/64-bit-offset recipes.
Native-caller, loader, SONAME/version, archive-order, and checksum checks also passed, with an explicit real-kernel i686 serial receipt.
This is a qualified functional checkpoint for the pinned configurations, not unrestricted C/ABI support, performance parity, reverse-engineering equivalence, security enforcement, or a remote Actions result.

### Pre-rename shared-LLVM qualification on 2026-09-13

At the historical checkpoint recorded in commit `be63890`, both final shared packages passed the complete selected 50-artifact corpus through two fresh, full-project runs: `--project cjson` covered 42 publications and `--project zlib` covered eight.
Both devices consumed the same checked artifacts and passed the original cJSON 19-test static/shared inventories and zlib `test`/`test64` recipes, including explicit source-free real-kernel i686 receipts.
These two complete project reports cover the full inventory; they are not one combined `PASS (all)` report.

The successful cJSON report covers 08:31:56 to 08:46:53 UTC, and the successful zlib report covers 08:36:31 to 08:38:29 UTC.
Both retained separate i686 VM evidence; their original temporary paths are recorded in `git show be63890:docs/reference/compiler-distribution.md`.
The final compiler packages and payload checksums remained unchanged; retained temporary evidence is not a permanent storage guarantee.

The failed first zlib attempt remains part of that historical evidence: guest CTest lacked `libarchive.so.13` during test-tool staging, not during compiler or application execution.
Completing the separate pinned test-tool dependency closure and removing compiler-library fallback fixed the harness; the complete zlib rerun passed without changing either compiler package, publisher binary, application source, or publication recipe.

cJSON's initial report recorded test-tools lock SHA-256 `2e2d102bbe612fe453693427cb18244ecc123ede33f5cce52bde60146b0b179f` before that test-only correction.
Both actual final guests staged `/opt/test-tools/packages.lock` with SHA-256 `675adcafd22ff645ac427dd353ee0f99e51a775635c56b394cfae2ebeb326a0f`; the corrected staging helper's hash is `d9728772d3a7ba4b87f80b5cddd136330079918f7038eb4440917ef106c284aa`.
The cJSON publications continued unchanged and its full guest tests passed with the corrected isolated tools; the initial report is not claimed to describe that later test-tool closure unchanged.
