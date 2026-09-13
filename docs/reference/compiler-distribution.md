# Sela native compiler-only distributions

The current distribution interface builds separate **x86-64** and **i686** device compilers from unmodified, pinned LLVM/MLIR 18.1.3 sources.
Each bundle contains its native `bin/selac`, `opt`, `llc`, `ld.lld`, and `llvm-ar`, their runtime library closure, one matching native link/runtime sysroot, and compiler-rt CRT/builtins.
The x86-64 bundle contains the x86-64 sysroot; the i686 bundle contains the i686 sysroot. Neither contains the opposite target's sysroot or Sela native ABI implementation.
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
The newly built Sela packages have separately passed all 40 publisher tests, all 11 consumer CTests for each architecture, and their fresh same-artifact two-device matrix.
The fresh dual-device zlib corpus has passed.
cJSON's fresh publication, native-reference, and x86-64 stages passed; its i686 stage subsequently passed using the unchanged published fixtures after a failed first VM boot.
That covers all required stages for all 50 artifacts, but the original cJSON command remains recorded as failed, not retroactively changed to a single-command pass.

## Build and assemble the separate products

Run from the repository root in Bash on the current x86-64 Ubuntu 24.04 development host.
Bootstrap the publisher SDK first, then build each selected consumer SDK and matching Sela compiler:

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
The build registers only LLVM's X86 backend family.
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
Set it to 1 in both bootstrap processes to build x86-64 and i686 concurrently in their separate SDK/build directories.
For example, 3 jobs for each architecture permit up to 6 concurrent target compilation jobs; choose the combined limit from available CPU and memory headroom.
Parallel target builds share the compile lock, but each profile retains an exclusive build lock, native-generator builds remain serialized, and a shared link lock serializes memory-heavy target C/C++ links.
One architecture does not have to finish before the other starts.
See [parallel SDK source builds](building-sela.md#parallel-sdk-source-builds) for an example which waits for and checks both processes.
The i686 build's host-side component tests use the development machine's 32-bit compatibility support. They do not replace the real 32-bit-kernel acceptance below.

Assemble both already built products into new directories:

```bash
mkdir -p artifacts
bash scripts/package-consumer.sh build/consumer-x86_64 \
  artifacts/selac-x86_64 .sdk/consumer/x86_64
bash scripts/package-consumer.sh build/consumer-i686 \
  artifacts/selac-i686 .sdk/consumer/i686
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
`--print-target` reports `x86_64` or `i686`. An explicit foreign `--target` rejects for both ordinary compilation and diagnostic `lower` before output staging.
Inspection checks the shared schema across all declared domains, but explicitly reports when a foreign plan has no available native validator in this device compiler.
`--check-sdk` is a read-only installation check: it compares this compiler's embedded SDK identity with the selected SDK's completion receipt and checks required tool files.
The packager runs it against the staged sibling SDK, catching a stale compiler paired with newly built SDK inputs before assembly succeeds.
If it fails after relocation, first clear `SELA_SDK_ROOT` to rule out an unintended developer override, then rebuild/reinstall the matching complete compiler bundle.
This is consistency validation, not a signature check, tamper-resistant attestation, or SESela enforcement.

The compiler/tool host glibc and dynamic loader use the Noble glibc 2.39 ABI baseline for the matching architecture.
On i686 the device must provide the normal `/lib/ld-linux.so.2` loader and matching runtime; on x86-64 it provides the normal x86-64 loader/runtime.
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

This is not a minimum-footprint SDK: the native C runtime set is intentionally preserved and required optimizer/code-generator components remain substantial even with only the X86 backend family registered.
Removing publication components does not impose an additional language subset on Sela input. The compiler's existing semantic and native-target qualification limits still apply; this prototype does not claim every C ABI construct is already supported.

## Footprint measurements and current checkpoint

Compare total bundle size as well as the individual `selac` file.
Moving a dependency into a binary can enlarge that binary while shrinking its complete installed dependency closure; shared LLVM addresses a different duplication cost across the separate tools.
Neither linkage choice alone proves that a package is smaller.

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

### Fresh Sela rename checkpoint

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
python3 -B scripts/bundle-size.py artifacts/selac-x86_64 \
  --compare /path/to/another-sela-bundle
python3 -B scripts/bundle-size.py artifacts/selac-i686 --json
python3 -B scripts/bundle-size.py artifacts/selac-x86_64 --compressed
```

The reporter groups `selac`, each LLVM tool, shared LLVM, other compiler libraries, the managed runtime, compiler-rt, and metadata.
It reports exact regular-file bytes separately from allocated filesystem bytes and ELF code, ordinary symbol, and unwind sections.
Hardlinked data is counted once and symlinks are not followed.
The archive-exclusive dependency subset is already included in the component totals; it is not an extra saving to add a second time.
`--compressed` measures a normalized tar/gzip-1 stream without writing an archive and is optional to keep ordinary reporting fast.
Compare compressed values produced by the same reporter; different tar/compression recipes are not interchangeable measurements.

## Dual-device acceptance

The distribution now supplies an independent, genuinely 32-bit i686 compiler bundle alongside the x86-64 bundle.
This is separate from compiling an ELF32 application with a 64-bit compiler or running one under a 64-bit kernel's compatibility support.
The consumer SDK inputs live under `.sdk/consumer/x86_64` and `.sdk/consumer/i686`; the corresponding assembled bundles remain distinct device products.
The original 2026-09-12 static-component bundles passed the complete fresh dual script, including per-target stock-native references and the i686 kernel's ELF64 `ENOEXEC` rejection.
The i686 VM used KVM and 3 GiB RAM. The complete fresh dual-destination corpus also passed with these compiler/runtime/SDK inputs.
Both pre-rename shared-LLVM packages passed the fresh fixture matrix using the same published artifacts, including the source-free real-kernel i686 stage.
The fixture and i686 VM locations are preserved in the original evidence record at commit `be63890`.
Those shared packages also passed the full fresh dual-device corpus in the separate complete cJSON and zlib runs recorded below; this was separate evidence for those final packages, not inherited static-component qualification or qualification of later Sela builds.
The manually dispatched GitHub Actions workflow `Device compiler qualification`, defined in `.github/workflows/device-compilers.yml`, runs SDK source builds, publisher/device component tests, packaging, this matrix, and the complete dual-destination corpus.
It is separate from the normal publisher CI smoke workflow; a configured workflow is not a recorded passing run.

With a coherent publisher build and both already assembled bundles, run from the repository root in Bash:

```bash
bash tests/dual-consumer.sh build/prealpha/sela.cfg .sdk \
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
`SELA_VM_ACCEL=auto|kvm|tcg`, `SELA_VM_RAM_MIB` (512–4096), and `SELA_VM_TIMEOUT` (30–3600 seconds) make those test limits explicit.
TCG results are functional evidence only. The runner records the selected accelerator and does not compare its elapsed time with native compilation performance.

Downloaded test packages are cached under `.sdk/test-vm/downloads` by default; `SELA_VM_CACHE` selects another cache.
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
bash corpus/qualify.sh build/prealpha/sela-build \
  /absolute/path/to/x86_64-bundle/bin/selac .sdk \
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
The test-tool dependency closure is staged independently from its own pinned packages, without falling back to compiler-bundle host libraries.
Slimming the product must not silently remove a test runner's dependencies or require putting those dependencies back into the distributed compiler.

The full-corpus guest has a default 3600-second bound, configurable through the same `SELA_VM_TIMEOUT` setting.
Publication, repeated native builds, and emulated compilation can be substantial work; an unchanged upstream test inventory is required regardless of accelerator.
`qualification.txt`, per-publication logs, the exported fixture hashes, and the VM serial evidence remain under the printed private corpus workspace.
A successful prior x86-64 run or fixture-preparation pass is not a completed dual-device qualification.

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
