# 23. SDK and compiler distribution

[Series](../README.md) · [Previous: Artifact validation and robustness](22-artifact-validation-and-robustness.md) · [Next: Maintaining and evolving Sela](24-maintaining-and-evolving-sela.md)

## Objective and prerequisites

This chapter explains which dependencies belong to the developer's SDK, which belong to an on-device compiler, and which the resulting native application still needs.
You should understand ELF interpreters and shared libraries, Clang/LLVM's frontend and backend roles, and the independent Sela producer and consumer programs.

The maintainer objective is to follow dependencies through three different executions: building the publisher, running `selac`, and running its output.
Successfully moving the compiler directory does not automatically make every already-compiled application relocatable.
Likewise, downloading pinned tools does not make the publisher a fully hermetic build environment.

## Three dependency sets, not one toolchain blob

The development SDK contains the stock Clang frontend, matching Clang plugin development interfaces, LLVM, MLIR, LLD, compiler-rt, build utilities, and native development/runtime sysroots for the two private profiles.
These are inputs to building Sela and compiling normal C source.
Headers and configure tools make sense here because this side is allowed to know that it is compiling C.

The compiler-only distribution needs a different set:
`selac`, the core Sela semantics, ordinary native optimization/code generation/linking tools,
their host runtime libraries, and the selected target's native link/runtime files.
It does not need Clang, the producer merger, C source headers or the publication capture plugins.
The absence of those components is a functional boundary, not just a reduction in archive size.
There are separate x86-64 and i686 products, each with native compiler executables, one matching sysroot, and only that device's Sela native lowering/ABI implementation.

The application needs a third set. It is an ordinary native executable or library.
A dynamically linked executable still needs its ELF interpreter, libc, declared native dependencies and any application resources.
It must not need the Sela artifact, `selac`, `opt`, `llc`, LLVM's compiler libraries, Clang, or a publication-time source tree merely to execute.

One directory may temporarily contain components from several sets during development.
That convenience must not erase the distinctions.
A clean-shell execution test is valuable precisely because a developer's `PATH` and `LD_LIBRARY_PATH` can otherwise hide a missing distribution dependency.

## What bootstrap actually pins

`scripts/bootstrap-sdk.sh` downloads unmodified Ubuntu packages named in `sdk/packages.lock`, verifies their SHA-256 hashes, and extracts them into a dedicated user-owned SDK directory.
It uses `dpkg-deb --extract`; it does not run package maintainer scripts or install packages into the host system.
A lock prevents concurrent extraction from trampling the same SDK, and per-package receipts let interrupted work resume.

The current development baseline is x86-64 Ubuntu 24.04, with pinned LLVM/Clang 18 packages and matching glibc sysroots.
[development SDK reference](../../reference/development-sdk.md) records the exact package versions and host requirements.
The package lock is the version source to inspect when those details change; a guide paragraph is not a replacement for it.

The bootstrap creates the expected `lib`/`lib64` aliases in the sysroots so unmodified glibc linker scripts resolve within Ubuntu's directory layout.
It does not patch the loader, rewrite LLVM executables, or edit those linker scripts to make the smoke test pass.
This is why distinguishing a filesystem layout adjustment from a compiler fork matters during review.

The SDK is nevertheless not a hermetic publisher image.
Building Sela still uses host C++ standard-library headers and startup files.
Publisher executables use the host glibc/loader, and CMake depends on the host networking-library family.
Pinning the extracted package inputs narrows variability; it does not pin every transitive host input or prove bit-reproducible application builds.

Receipts also have a limited meaning.
A package digest binds the downloaded bytes to the checked-in lock.
An extraction receipt records completion.
Neither prevents a user-writable SDK file from being changed afterward.
Lock validation is a consistency check between the compiler's expected SDK contract and the selected SDK, not runtime tamper protection.

The device SDK has a separate bootstrap: `scripts/bootstrap-consumer-sdk.sh x86_64` or `i686`.
It verifies the upstream LLVM/MLIR 18.1.3 source archive in `sdk/consumer/source.lock`, extracts matching dependencies from `sdk/consumer/packages.lock`, and builds unmodified sources for the chosen host ABI.
TableGen runs on the build host; generated device tools do not acquire a frontend dependency because a host compiler built them.
Only the X86 backend family is registered.
The bootstrap defaults to shared LLVM: the device compiler and LLVM tools share one native `libLLVM`, while the required MLIR components remain statically linked.
`SELA_CONSUMER_LLVM_LINKAGE=static-components` selects the comparison configuration, which embeds LLVM components separately in the executables.
The completed consumer receipt records that choice; the matching Sela build follows it without changing its one-device native specialization.
Both shared packages are measured, smaller, and have passed their complete 11-test consumer suites, fresh two-device matrix, and full 50-artifact dual-device corpus through complete fresh cJSON and zlib project runs.
The distribution reference separates those current results from earlier static-component checkpoints.
The source build is substantial and remains dependent on the documented build host; pinned inputs are not a proof of fully hermetic or reproducible output.
The `consumer-x86_64` and `consumer-i686` CMake presets select the corresponding source SDK and native Sela implementation.

Changing linkage is not necessarily a relink-only operation.
Generated LLVM configuration headers can invalidate existing objects; the bootstrap prints the planned compilation/linking work before building the target.
Do not run competing variants against the same profile's SDK or source-build cache, and retain an assembled checkpoint before changing its inputs.

Independent architecture builds may run concurrently; one architecture's result is not a prerequisite for starting another.
`SELA_CONSUMER_COMPILE_JOBS` accepts 1–8 and defaults to 2 per target, while `SELA_CONSUMER_PARALLEL_TARGETS=1` opts participating bootstrap processes into parallel target builds.
The default `SELA_CONSUMER_PARALLEL_TARGETS=0` keeps heavyweight target builds serialized.
With 3 jobs per architecture the combined target compilation limit is 6, so budget CPU and memory for both processes rather than reading the per-target value as a machine-wide limit.
Per-profile cache locks still exclude competing writers, native generators remain serialized, and a shared target C/C++ link lock prevents overlapping memory-heavy links.
The [parallel build example](../../reference/building-sela.md#parallel-sdk-source-builds) waits for both processes and preserves failure reporting.

This choice does not introduce a JIT stage.
Shared libraries are ordinary native files loaded by compiler processes; Sela still compiles and links the application before running it.
The stock LLVM library may retain unused JIT-related components, so choosing shared linkage is not proof of their removal or of SESela policy enforcement.

## Worked case: constructing the compiler-only closure

Follow `scripts/package-consumer.sh` as a dependency closure algorithm rather than a long copy command.

First, it installs the project's Consumer component into a new staging directory.
The requested final bundle must not already exist, and its parent must be a directory.
The script requires a complete target-specific SDK receipt and matching device build, checks SDK/tool identities and shared/static linkage, and verifies the installed `selac` has the expected origin-relative runtime search path.
Its read-only `selac --check-sdk` step also compares the staged SDK with the identity embedded in that actual compiler; matching target names alone would not detect a stale binary beside a newer same-architecture SDK.
It is assembling an already-built compiler, not rebuilding one with a smaller language subset.

Second, it copies `opt`, `llc`, `ld.lld`, and `llvm-ar` and checks the staged bytes against the verified originals before release stripping.
These tools do different jobs: optimize native LLVM, generate native objects, link ELF outputs, and construct static archives.
Removing `llvm-ar` merely because Hello World is an executable would break a qualified output kind.

Third, the script walks `DT_NEEDED` dependencies recursively.
For each required library outside the explicitly allowed host glibc/loader family,
it resolves the library from the pinned extracted SDK, copies its bytes, and adds it to the work queue.
It does not use whatever an opportunistic host `ldd` resolution happens to find.
Symlinks are resolved while copying so a flattened library directory cannot accidentally point outside the bundle.
For shared LLVM, the library's actual native ABI, SONAME, and original hash must match the completed consumer receipt.

There is an important dependency decision before this packaging step.
Only device `selac` links selected objects from the pinned static `libarchive.a` for its existing uncompressed-tar reader.
That avoids loading the distribution's general-purpose shared libarchive and its XML/ICU dependency chain.
Publisher tools and fixture writers keep shared libarchive; the artifact format and rejection rules do not change.
This is why statically linking one small dependency can reduce a bundle, while duplicating LLVM across several tools can increase it.

Fourth, it release-strips ordinary symbol tables from the staged `selac`, four LLVM tools, and shared LLVM library when present.
It does not strip the original SDK or build binaries, which remain available for debugging and backend-identity inspection.
Required dynamic symbols and unwind information remain in the delivered binaries.

Fifth, it retains the selected device's native link/runtime library set, including glibc linker scripts, startup objects, archives and runtime modules, plus the matching compiler-rt builtins and CRT files.
Source headers and the opposite target's sysroot are excluded.
An i686 product includes its i686 runtime; an x86-64 product includes its x86-64 runtime. Neither contains the full two-profile publisher SDK.
This runtime is copied unchanged, including locale/NSS and charset modules; release stripping does not touch its static link inputs or compiler-rt files.

Finally, dependency identities are associated with package copyright notices.
An unknown library-to-package mapping rejects assembly instead of quietly shipping an unaccounted component.
The statically included libarchive also retains its notice despite no longer appearing as a shared dependency.
The bundle records its SDK lock and final delivery digests, retains notices, and is moved into the requested output path only after successful assembly.
Release license/source-offer review is still a release obligation; copying notice files is not a blanket legal conclusion.

Two kinds of hashes are intentionally different here.
The consumer SDK completion receipt describes the original unstripped tools and, when used, the original shared LLVM library.
`payload.sha256` describes the final files users receive after stripping, including the unchanged copy of that source receipt.
Comparing a delivered executable directly with its original-tool hash is not the correct installation check.
These are consistency/provenance records, not signatures or runtime security enforcement.

The README in a measured bundle is also an assembly-time snapshot.
Later qualification results belong in the live repository reference; the retained bundle is not edited or repackaged simply to update its status text.
This preserves the exact payload whose size and behavior were measured.

The closure is intentionally conservative.
It rejects unexpected backend families or LLVM libraries which do not match the selected SDK, but still includes substantial LLVM components and a broad native library set.
“Thin” here means separated from publication/frontends and foreign Sela native implementations, not a demonstrated minimum-size binary.
Removing more components requires dependency evidence and output-kind tests, not just observing that one executable happened to compile.

## Compiler relocation and application runtime paths

At execution, `selac` can locate a sibling `sdk` directory relative to its own executable.
An explicit `--sdk` or `SELA_SDK_ROOT` overrides that discovery.
This is helpful during development, but it creates a testing pitfall:
leaving the developer's `SELA_SDK_ROOT` set while testing a moved bundle can make the test use the original SDK accidentally.

The installed compiler uses an origin-relative RPATH for its bundled nonbaseline dependencies.
Its LLVM subprocesses receive an SDK-specific library search environment from `Sdk::toolEnvironment`.
That environment is for compiler processes, not a wrapper which must accompany the final application's execution.
The compiler's own host loader/glibc remains the documented Noble 2.39 baseline for its matching architecture.
`selac --print-target` identifies that one device target. Explicit foreign targets reject for both compilation and diagnostic `lower`.
Shared structural inspection can still read a foreign-only artifact, but must report that its native plan was not validated by this device compiler.

Native application linking is separate.
`Sdk::linkCommand` supplies startup objects, compiler-rt support, native libraries, an explicit managed glibc interpreter, and the runtime search paths used by the generated ELF.
Those paths describe the chosen runtime at the moment of compilation.
In this prototype, moving that runtime directory later does not rewrite an application's existing interpreter or library path.

For example, move a complete bundle from `original` to `relocated`, then compile an artifact using `relocated/bin/selac`.
The compiler can find its new sibling SDK, and the new application uses that relocated runtime path.
If you then move the runtime again and execute the already-built application, compiler relocation success does not imply application success.
Its ELF still refers to the earlier path.
Production runtime installation, stable locations, upgrades and lifecycle management belong to future product work.

This distinction also explains why the initial result is not “one static binary with no dependencies.”
The public artifact is independent of the source language frontend; the installed output can still be an ordinary dynamically linked native program.
Native dependencies are explicit requirements, not an accidental return of the publication system at execution time.

## Verify absence as well as successful execution

`tests/consumer-install.sh` starts with an artifact made by an independent producer, assembles a bundle, and moves it to a path containing a space.
It checks the payload digests and rejects forbidden bundle contents such as Clang, publisher plugins, source headers or the opposite target's sysroot.
It inspects the original unstripped compiler's symbols to catch producer/frontend code or the foreign native implementation linked into `selac`.
It separately verifies that release copies lack ordinary symbol tables, preserve unwind information, omit shared archive/XML/ICU dependencies, and leave SDK/build originals unchanged.

The test runs the moved compiler with a clean environment and traces file opens and subprocess execution.
It checks that the compiler does not access the original SDK, build directory or pre-relocation path.
Successful nonbaseline host library loads are distinguished from harmless failed loader probes.
Both executable and static-archive outputs are exercised.

Finally, the produced application is executed independently.
Its trace must show the intended managed libc and must not show the artifact or compiler programs being used.
A successful `Hello World` under a richly configured developer shell would not establish these absence properties.

The `tests/multi-consumer.sh` matrix publishes each artifact once and sends identical bytes to all four Linux/glibc products, comparing native references at O0 and O2.
Each target uses `tests/consumer-vm.sh TARGET BUNDLE FIXTURES` with a matching real kernel, including genuine 32-bit kernels for i686 and ARMv7.
The guest checks every compiler tool's ELF identity and requires `ENOEXEC` for a foreign executable before native compilation can pass.
The corpus preparation mode publishes once without a device compiler; its source-free fixtures then run the original cJSON and zlib recipes on each target.
The [distribution reference](../../reference/compiler-distribution.md#four-target-acceptance) gives the current commands, while [adding a native target](../../reference/adding-native-targets.md) explains the generic registry and ABI boundary.
The following qualification and footprint history predates the Sela rename; commits `63592ab` and `be63890` preserve the original evidence without renaming its tools, artifacts, or receipts.
The original static-component bundles passed the fresh matrix and the complete fresh dual-destination corpus at the recorded 2026-09-12 checkpoint.
The earlier compact static checkpoint passed 10 component tests for each device and all 40 publisher tests.
The historical shared-LLVM packages passed all 11 consumer CTests per device, all 40 publisher tests, and the fresh same-artifact matrix on both architectures.
Its i686 stage used a real 32-bit kernel and verified the ELF64 `ENOEXEC` rejection; this is not just a compatibility-mode component result.
Both architectures passed the total-size reduction gate and the full fresh dual-device corpus qualification, establishing shared LLVM as the adopted default layout.
The final corpus evidence comprises two complete project runs, with 42 cJSON publications and eight zlib publications, each consumed as the same artifact bytes by both devices.
All original cJSON static/shared CTest inventories and zlib `test`/`test64` recipes passed; separate project runs did not filter individual tests.
A host compatibility-mode component test or bootstrap-only VM probe is not a substitute for either gate.
The manually dispatched `Device compiler qualification` workflow assembles these checks separately from the ordinary publisher CI smoke job.
Recorded local results are not a claim that the remote workflow has passed.

Qualification also exposed an independence problem in the test harness: guest CTest needed shared libarchive after the compiler package stopped shipping it.
The fix gave test runners their own complete pinned library closure and removed fallback to compiler-bundle host libraries, rather than expanding the product again.
The first zlib attempt remains recorded as a staging failure; its complete rerun passed.
cJSON's initial report recorded the old test-tools lock, but its actual final guest used the corrected staged lock while the publisher, source inputs, and compiler packages stayed unchanged.
The distribution reference records both exact hashes and the retained evidence; it does not pretend one unchanged test-tools lock covered the whole campaign.

Target isolation does not automatically reduce installed size.
The original unstripped static-component packages contained 242.70 MiB and 263.15 MiB of regular-file data, compared with the earlier 188.96 MiB monolithic x86-64 baseline.
Release stripping and the device-only archive dependency change reduced the retained compact static checkpoints to 179.43 MiB and 201.58 MiB without pruning the managed runtime.
Sharing LLVM then reduced the retained x86-64 and i686 packages to 91.89 MiB and 98.28 MiB, with historical compiler executables of 1.73 MiB and 2.15 MiB respectively.
These are installed file bytes, not allocated disk blocks, compressed download sizes, or a claim of minimum footprint.
The [compiler distribution reference](../../reference/compiler-distribution.md) records the exact checkpoint bytes, completed matrix/full-corpus results, and the test-harness provenance transition.

The separate fresh Sela packages measure approximately 91.9 MiB and 98.3 MiB and have passed all 11 consumer tests per device, all 40 publisher tests, and the new two-device matrix.
Their complete dual-device zlib run has passed.
cJSON's fresh publication/native/x86-64 stages and a separate real-i686 continuation cover the same 42 artifacts after a failed first VM boot.
The original failed command remains recorded unchanged; this is staged qualification, not an invented single-command pass or evidence inherited from the pre-rename history above.

Use `python3 -B scripts/bundle-size.py /path/to/bundle --compare /path/to/baseline` to inspect the accounting rather than judging `selac` alone.
It separates the compiler, each helper, shared libraries, and runtime; section and archive-exclusive dependency totals are subsets, not additional bytes.
`--json` supplies a machine-readable report, and optional `--compressed` counts a normalized tar/gzip-1 stream without writing an archive.

## Optional independent lab

This lab copies a substantial runtime bundle but does not rebuild Sela.
Run it from the repository root after preparing the publisher SDK/build, the `.sdk/consumer/x86_64` source SDK, and the Release `consumer-x86_64` preset build described in the [compiler distribution reference](../../reference/compiler-distribution.md).
Do not pass the ordinary publisher build/SDK to the target-specific packager:

```bash
source sdk/env.sh
distribution_lab=$(mktemp -d "${TMPDIR:-/tmp}/sela-guide-distribution-XXXXXX")
clang --config="$PWD/build/prealpha/sela.cfg" -O2 \
  examples/hello/hello/main.c examples/hello/hello/hello.c \
  -o "$distribution_lab/hello.sela"
bash scripts/package-consumer.sh build/consumer-x86_64 \
  "$distribution_lab/original" "$PWD/.sdk/consumer/x86_64"
mv -- "$distribution_lab/original" "$distribution_lab/relocated bundle"
env -u SELA_SDK_ROOT -u LD_LIBRARY_PATH -u LD_PRELOAD \
  "$distribution_lab/relocated bundle/bin/selac" \
  "$distribution_lab/hello.sela" -o "$distribution_lab/hello"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$distribution_lab/hello"
readelf -l "$distribution_lab/hello"
printf 'Keep the runtime in place; lab files: %s\n' "$distribution_lab"
```

Find the interpreter path in `readelf`'s output.
The relocation happened before compilation, so the new native output names the new runtime location.
Keep that directory in place when running the executable again.
This lab is not a claim that arbitrary hosts or post-compilation runtime moves are supported.

## Recap and questions

The developer SDK, compiler runtime closure and native application dependencies are distinct deliverables.
Their tests should prove those distinctions even when development happens in one shared directory.

> [!faq]- Why is the bootstrap not hermetic?
>
> It pins extracted packages but still depends on documented host development files and runtime libraries.

> [!faq]- Why recursively inspect `DT_NEEDED`?
>
> A copied LLVM tool depends on libraries which can themselves require further libraries; copying only its first-level dependencies is incomplete.

> [!faq]- Why does a stripped tool differ from the SDK receipt's original hash?
>
> The SDK receipt identifies the verified build input. The delivered copy is intentionally stripped, and its final bytes are covered by `payload.sha256` instead.

> [!faq]- Does shared LLVM mean the application uses a JIT or needs LLVM?
>
> No. The shared library belongs to compiler processes. Sela still finishes native compilation and linking before application execution, and the generated program does not depend on the compiler library.

> [!faq]- Why clear `SELA_SDK_ROOT` in the relocation lab?
>
> Otherwise the moved compiler may select the original developer SDK instead of its sibling SDK.

> [!faq]- Does a relocatable compiler make old outputs relocatable?
>
> No. Native output retains the managed interpreter/runtime paths chosen when it was linked.

Read [development SDK reference](../../reference/development-sdk.md) for host assumptions, [compiler distribution reference](../../reference/compiler-distribution.md) for the bundle's supported contract,
`src/consumer/Main.cpp` for SDK discovery and overrides, and `src/support/Sdk.cpp` for subprocess environments and native link construction.

[Previous: Artifact validation and robustness](22-artifact-validation-and-robustness.md) · [Next: Maintaining and evolving Sela](24-maintaining-and-evolving-sela.md)
