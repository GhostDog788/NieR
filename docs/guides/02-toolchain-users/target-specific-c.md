# Publish target-specific C

[Guide series](../README.md) · [Publish your C project](using-sela-with-your-c-project.md) · [Build integration reference](../../reference/build-integration.md)

Sela Code can represent different code for different targets in one artifact.
Your C sources can retain their ordinary target macros, native types, supported intrinsics and inline assembly.
Clang evaluates those choices for each requested target; the Sela stage preserves the resulting semantics.
It does not pretend that x86 assembly works on ARM or that every target needs the same source files.

This guide describes the current pre-alpha increment, not complete Clang-level C support.
See [current boundaries](../../reference/toolchain-status.md) before applying it to a larger project.

## Choose architectures separately from build goals

The four target IDs are `x86_64`, `i686`, `armv7`, and `aarch64`.
All currently use Linux and the supplied glibc runtime.
Leaving architecture selection unset publishes all four.
An explicit selection must succeed completely; Sela never silently drops a failing target.

For direct Clang publication, use the comma-separated `SELA_ARCHS` environment variable:

```bash
source sdk/env.sh
set -euo pipefail
target_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-target-guide-XXXXXX")
SELA_ARCHS=x86_64,aarch64 clang --config="$PWD/build/prealpha/sela.cfg" -O2 \
  tests/fixtures/target-asm.c -o "$target_work/assembly.sela"
build/prealpha/selac inspect "$target_work/assembly.sela"
```

Run these commands from the repository root after building the tools.
The supplied fixture uses x86 or ARM addition instructions, a memory constraint, and `asm goto`.
Both requested variants live in one `.sela` file as Sela Code, not embedded LLVM bitcode or native objects.

On the qualified x86-64 development machine, compile and execute the artifact:

```bash
build/prealpha/selac "$target_work/assembly.sela" -o "$target_work/assembly"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$target_work/assembly"
printf 'Assembly fixture passed\n'
```

The fixture exits successfully without printing anything itself.
For AArch64, transfer the same artifact to an AArch64 Linux device with its matching `selac` bundle and invoke that compiler there.
The x86-64 device compiler does not gain an ARM backend because the artifact also declares ARM support.

For an existing Make project, set `SELA_ARCHS=x86_64,aarch64` when invoking its Sela Make adapter.
Keep `SELA_BUILD_TARGETS=all` or your actual Make goal separate.
For the CMake adapter, add `ARCHS x86_64 aarch64` to `sela_add_publication`; `BUILD_TARGETS` still names application build goals.
Choose either Make or CMake as described in [the project guide](using-sela-with-your-c-project.md).

## Keep native project choices native

A project may select different `.c` files, macros, compile flags, archive members and libraries for each architecture.
Sela's build integration observes those actual native builds and records target-specific module and link plans.
It does not require fake configure results or a common function inventory.
Configure probes and generated programs still need to run, using the provisioned native or emulated build environment.

Shared Sela definitions reduce duplication when the producer can prove the relationship.
Otherwise, explicit target-scoped definitions or complete translation units preserve the original behavior and optimization boundaries.
The publisher checks reconstruction for every selected target before accepting the artifact.

## CPU features are not the same as architectures

An `x86_64` package may contain baseline code and optional AVX implementations selected by the application's own runtime dispatch.
Function CPU attributes and supported intrinsics survive publication; dispatch remains ordinary native application code.
The on-device compiler does not invent missing dispatch or rewrite unsupported instructions into portable algorithms.

Minimum-CPU compatibility metadata and admission are still unfinished.
For now, explicitly using flags such as `-march=haswell` does not produce a package that checks whether the destination CPU is suitable.
Do not advertise such an artifact as runnable on every CPU with the same architecture ID.

## What still fails

Standalone `.s` and `.S` files, file-scope assembly and assembler file dependencies are not implemented yet.
The intrinsic registry is finite; unsupported intrinsics fail instead of being stored as arbitrary LLVM IR.
Aliases, ifunc, TLS, additional calling conventions, atomics, VLAs and other general-C facilities still need implementation or qualification.
The product goal remains every Clang-supported C program on its supported targets; these are temporary gaps, not a permanent restricted C language.

> [!faq]- Does target-specific code mean shipping native binaries?
>
> No. In this increment, every application operation still goes through Sela Code and is compiled on the destination. Publishing prebuilt native dependencies is separate work.

> [!faq]- Must I rewrite my target checks to Sela-specific macros?
>
> No. Keep ordinary Clang/native target checks. Each requested native build evaluates them before LLVM-to-Sela import.
