# 23. SDK and compiler distribution

[Series](../README.md) · [Previous: Artifact validation and robustness](22-artifact-validation-and-robustness.md) · [Next: Maintaining and evolving NieR](24-maintaining-and-evolving-nier.md)

## Objective and prerequisites

This chapter explains which dependencies belong to the developer's SDK, which belong to an on-device compiler, and which the resulting native application still needs.
You should understand ELF interpreters and shared libraries, Clang/LLVM's frontend and backend roles, and the independent NieR producer and consumer programs.

The maintainer objective is to follow dependencies through three different executions: building the publisher, running `nierc`, and running its output.
Successfully moving the compiler directory does not automatically make every already-compiled application relocatable.
Likewise, downloading pinned tools does not make the publisher a fully hermetic build environment.

## Three dependency sets, not one toolchain blob

The development SDK contains the stock Clang frontend, matching Clang plugin development interfaces, LLVM, MLIR, LLD, compiler-rt, build utilities, and native development/runtime sysroots for the two private profiles.
These are inputs to building NieR and compiling normal C source.
Headers and configure tools make sense here because this side is allowed to know that it is compiling C.

The compiler-only distribution needs a different set:
`nierc`, the core NieR semantics, ordinary native optimization/code generation/linking tools,
their host runtime libraries, and the selected target's native link/runtime files.
It does not need Clang, the producer merger, C source headers or the publication capture plugins.
The absence of those components is a functional boundary, not just a reduction in archive size.

The application needs a third set. It is an ordinary native executable or library.
A dynamically linked executable still needs its ELF interpreter, libc, declared native dependencies and any application resources.
It must not need the NieR artifact, `nierc`, `opt`, `llc`, Clang, or a publication-time source tree merely to execute.

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
Building NieR still uses host C++ standard-library headers and startup files.
Publisher executables use the host glibc/loader, and CMake depends on the host networking-library family.
Pinning the extracted package inputs narrows variability; it does not pin every transitive host input or prove bit-reproducible application builds.

Receipts also have a limited meaning.
A package digest binds the downloaded bytes to the checked-in lock.
An extraction receipt records completion.
Neither prevents a user-writable SDK file from being changed afterward.
Lock validation is a consistency check between the compiler's expected SDK contract and the selected SDK, not runtime tamper protection.

## Worked case: constructing the compiler-only closure

Follow `scripts/package-consumer.sh` as a dependency closure algorithm rather than a long copy command.

First, it installs the project's Consumer component into a new staging directory.
The requested final bundle must not already exist, and its parent must be a directory.
The script checks the matching SDK lock and verifies the installed `nierc` has the expected origin-relative runtime search path.
It is assembling an already-built compiler, not rebuilding one with a smaller language subset.

Second, it copies the unmodified `opt`, `llc`, `ld.lld`, and `llvm-ar` tools and checks the copied bytes against their originals.
These tools do different jobs: optimize native LLVM, generate native objects, link ELF outputs, and construct static archives.
Removing `llvm-ar` merely because Hello World is an executable would break a qualified output kind.

Third, the script walks `DT_NEEDED` dependencies recursively.
For each required library outside the explicitly allowed host glibc/loader family,
it resolves the library from the pinned extracted SDK, copies its bytes, and adds it to the work queue.
It does not use whatever an opportunistic host `ldd` resolution happens to find.
Symlinks are resolved while copying so a flattened library directory cannot accidentally point outside the bundle.

Fourth, it retains the current x86-64 native link/runtime library set, including glibc linker scripts, startup objects, archives and runtime modules, plus the qualified compiler-rt builtins and CRT files.
Source headers and the i686 sysroot are excluded.
This is a compiler-only *x86-64 product prototype*, not the full two-profile publisher SDK.

Fifth, dependency identities are associated with package copyright notices.
An unknown library-to-package mapping rejects assembly instead of quietly shipping an unaccounted component.
The bundle records its SDK lock and copied file digests, retains notices, and is moved into the requested output path only after successful assembly.
Release license/source-offer review is still a release obligation; copying notice files is not a blanket legal conclusion.

The closure is intentionally conservative.
It includes a monolithic LLVM library containing unused backends and a broad native library set.
“Thin” here means separated from publication/frontends, not a demonstrated minimum-size binary.
Removing more components requires dependency evidence and output-kind tests, not just observing that one executable happened to compile.

## Compiler relocation and application runtime paths

At execution, `nierc` can locate a sibling `sdk` directory relative to its own executable.
An explicit `--sdk` or `NIER_SDK_ROOT` overrides that discovery.
This is helpful during development, but it creates a testing pitfall:
leaving the developer's `NIER_SDK_ROOT` set while testing a moved bundle can make the test use the original SDK accidentally.

The installed compiler uses an origin-relative RPATH for its bundled nonbaseline dependencies.
Its LLVM subprocesses receive an SDK-specific library search environment from `Sdk::toolEnvironment`.
That environment is for compiler processes, not a wrapper which must accompany the final application's execution.
The compiler's own host loader/glibc remains the documented Ubuntu baseline.

Native application linking is separate.
`Sdk::linkCommand` supplies startup objects, compiler-rt support, native libraries, an explicit managed glibc interpreter, and the runtime search paths used by the generated ELF.
Those paths describe the chosen runtime at the moment of compilation.
In this prototype, moving that runtime directory later does not rewrite an application's existing interpreter or library path.

For example, move a complete bundle from `original` to `relocated`, then compile an artifact using `relocated/bin/nierc`.
The compiler can find its new sibling SDK, and the new application uses that relocated runtime path.
If you then move the runtime again and execute the already-built application, compiler relocation success does not imply application success.
Its ELF still refers to the earlier path.
Production runtime installation, stable locations, upgrades and lifecycle management belong to future product work.

This distinction also explains why the initial result is not “one static binary with no dependencies.”
The public artifact is independent of the source language frontend; the installed output can still be an ordinary dynamically linked native program.
Native dependencies are explicit requirements, not an accidental return of the publication system at execution time.

## Verify absence as well as successful execution

`tests/consumer-install.sh` starts with an artifact made by an independent producer, assembles a bundle, and moves it to a path containing a space.
It checks the payload digests and rejects forbidden bundle contents such as Clang, publisher plugins, source headers or the i686 sysroot.
It also inspects symbols to catch producer/frontend code linked into `nierc`.

The test runs the moved compiler with a clean environment and traces file opens and subprocess execution.
It checks that the compiler does not access the original SDK, build directory or pre-relocation path.
Successful nonbaseline host library loads are distinguished from harmless failed loader probes.
Both executable and static-archive outputs are exercised.

Finally, the produced application is executed independently.
Its trace must show the intended managed libc and must not show the artifact or compiler programs being used.
A successful `Hello World` under a richly configured developer shell would not establish these absence properties.

## Optional independent lab

This lab copies a substantial runtime bundle but does not rebuild NieR.
Run it from the repository root after preparing the SDK and Release build:

```bash
source sdk/env.sh
distribution_lab=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide-distribution-XXXXXX")
clang --config="$PWD/build/prealpha/nier.cfg" -O2 \
  examples/hello/hello/main.c examples/hello/hello/hello.c \
  -o "$distribution_lab/hello.nier"
bash scripts/package-consumer.sh build/prealpha \
  "$distribution_lab/original" "$NIER_SDK_ROOT"
mv -- "$distribution_lab/original" "$distribution_lab/relocated bundle"
env -u NIER_SDK_ROOT -u LD_LIBRARY_PATH -u LD_PRELOAD \
  "$distribution_lab/relocated bundle/bin/nierc" \
  "$distribution_lab/hello.nier" -o "$distribution_lab/hello"
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

> [!faq]- Why clear `NIER_SDK_ROOT` in the relocation lab?
>
> Otherwise the moved compiler may select the original developer SDK instead of its sibling SDK.

> [!faq]- Does a relocatable compiler make old outputs relocatable?
>
> No. Native output retains the managed interpreter/runtime paths chosen when it was linked.

Read [development SDK reference](../../reference/development-sdk.md) for host assumptions, [compiler distribution reference](../../reference/compiler-distribution.md) for the bundle's supported contract,
`src/consumer/Main.cpp` for SDK discovery and overrides, and `src/support/Support.cpp` for subprocess environments and native link construction.

[Previous: Artifact validation and robustness](22-artifact-validation-and-robustness.md) · [Next: Maintaining and evolving NieR](24-maintaining-and-evolving-nier.md)
