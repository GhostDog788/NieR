# Current toolchain status and boundaries

[Documentation home](../README.md) · [Project reference](README.md) · [Implementation plan](../02-implementation-plan.md)

Sela is pre-alpha.
This page summarizes the implemented toolchain and its boundaries; the [implementation plan](../02-implementation-plan.md) remains the detailed account of milestones and open gates.
There are no backward-compatibility obligations between pre-alpha revisions.
Rebuild matching tools and regenerate artifacts after a contract change.

## What is available

Sela Code is an independent, architecture-neutral format implemented with a custom MLIR dialect.
A producer emits one standalone `.sela` archive; the separate, language-blind `selac` compiler generates ordinary native output using LLVM and stock LLD.
The current C producer is a plugin for stock Clang, not a replacement compiler executable.
Other producers can construct Sela through the public APIs without LLVM captures.
That producer API is implemented; complete Rust, Go, and Kotlin integrations are not.

The development baseline is x86-64 Ubuntu 24.04 with the pinned LLVM 18.1.3 SDK.
Separate genuinely native x86-64 and i686 compilers use unmodified, source-built LLVM/MLIR SDKs with the X86 backend family registered.
The pre-rename bundles passed the fresh matrix and complete dual-destination corpus at commits `63592ab` and `be63890`, including real 32-bit-kernel execution.
The fresh Sela builds separately pass all 40 publisher tests, all 11 component tests per device, the new two-device matrix, and the complete dual-device zlib corpus.
cJSON's fresh publication/native/x86-64 stages plus a separately passing unchanged-fixture real-i686 continuation cover all 42 artifacts; the original command's VM boot failure remains recorded unchanged in the distribution reference.
Each public `selac` compiles and lowers only its own native target. Shared structural inspection can report foreign domains, but must explicitly mark their native validation unavailable.
The C publisher's private two-profile reference builds and publisher-only diagnostic helper are separate evidence; by themselves they establish neither a deployment product nor portability to arbitrary CPUs and ABIs.
See the [compiler distribution reference](compiler-distribution.md) for current source-SDK, packaging, VM, corpus commands, and qualification boundaries.

The archive carries a manifest and Sela bytecode for separate translation units.
It does not contain application source, original LLVM modules, native application objects, or whole per-target program copies.
Separate compilation produces relocatable Sela artifacts; final publications do not retain references to those intermediates.
This is source exclusion from the published artifact, not closed-source licensing of the Sela toolchain.

## Existing C projects and native libraries

The SDK adapters support Make or CMake as independent alternatives.
They run an existing project's native build in two private trees, keeping configure probes, generators, and target-generated headers native.
Only the selected application's captures pass through the stock-Clang Sela publication path.
A global `CC="clang --config=sela.cfg"` is not suitable for a build that executes compiler probes or generators.
See the [integration reference](build-integration.md) and [practical user guide](../guides/02-toolchain-users/using-sela-with-your-c-project.md).

Supported output kinds include executables, native shared libraries, and static archives within the qualified subset.
Stock Clang's Sela mode accepts qualified shared-link, SONAME, and version-script settings.
Static publications retain archive-member ordering, duplicate member names, and lazy extraction when later linked.
Ordinary/thin/group/whole archive selection, renamed objects, and independently published native dependencies have regression coverage.
These are bounded supported cases, not a claim that every native linker operation is implemented.

Native dependencies are resolved from the supplied SDK and optional `selac --library-dir DIR` locations.
Missing declared libraries fail before linking; arbitrary host libraries are not an automatic fallback.
Application libraries must be published or otherwise supplied separately in the chosen native dependency environment.
Dependency installation and updates are not automated by this prototype.

## Independent consumption

The compiler-only build does not configure or link language frontend libraries, capture plugins, build replay, or the LLVM-to-Sela merger.
The separate [compiler distribution reference](compiler-distribution.md) describes its assembly and relocation checks.
The development SDK conveniently contains both sides, but publisher assets are not consumer inputs.

The generated application runs without Clang, `selac`, or its `.sela` file.
It still needs the native loader, runtime, and declared dependencies selected during compilation.
Keep those runtime paths in place; moving an already compiled executable does not relocate its dependencies automatically.
Clearing the development shell's library overrides prevents compiler-tool libraries from changing application loading.

The public artifact and IR APIs are under `include/sela/Artifact` and `include/sela/IR`.
The optional LLVM producer APIs are separate under `include/sela/Producer`.
The independent-producer regression constructs an artifact without Clang, source captures, or a copied publisher manifest, then compiles and runs it.

## Evidence and remaining coverage

The [ABI evidence reference](native-abi-matrix.md) records implemented native calling and storage rules alongside remaining gates.
Regression coverage includes artifact validation, independent producers, CFG/SSA matching, native-width values, floating-point cases, aggregate storage, nonlocal jumps, promoted variadic arguments, build probes, source selection, and compiler/runtime access boundaries.
Qualified aggregate calls include integer, mixed floating/integer, and larger native-width records across translation units and native shared-library boundaries.
Union, packed, and bitfield records by value, aggregate `va_arg`, and general divergent function inventories remain qualification limits.

The [locked qualification corpus](qualification-corpus.md) records historical pre-rename fresh passing checkpoints for cJSON and zlib on both independent native device compilers:
cJSON's static and shared configurations each produced 21 artifacts and passed all 19 original CTests on each destination; zlib's eight outputs passed the original static, shared, and 64-bit-offset recipes on both.
Each selected artifact was published once and supplied unchanged to both consumers, after rebuilding and testing the original native references.
That is recorded configured functional evidence, not a fresh qualification claim for every later commit.
The complete corpus must be rerun when qualifying compiler changes.
The shorter CI smoke suite is a regression signal, not a substitute for corpus, performance, or reverse-engineering qualification.

Broader language integrations, additional deployment targets, complete general-C coverage, performance parity, and native-equivalent reverse-engineering resistance remain work to be demonstrated.
Source exclusion by itself does not prove reverse-engineering resistance.

## Failure behavior

The SDK coordinator stages its final Clang publication and replaces an existing artifact only after success.
`selac` likewise stages and validates output before replacing the requested native file.
Input/output aliases, including hard links and symlinked parents, are rejected by the compiler and internal publication linker.

Direct stock-Clang calls retain Clang's own failure cleanup: a failed frontend or linker operation can remove the requested output.
Use distinct input and output paths and disposable outputs for experiments.
Do not use a source or input pathname as the output pathname.
These are ordinary compiler-file behaviors, not an installed-release registry or rollback manager.

Private diagnostic captures can contain source and debug information.
Do not distribute them as application artifacts or include sensitive captures in public issue reports.

## Security is a separate axis

SESela, shortened to SES, is the planned security platform above the standalone Sela toolchain.
It is not implemented yet, and using Sela does not require it.
The publication flow adds no Sela-specific interpreter, JIT, resident compiler, custom loader, or post-link ELF transformation.
It does not prohibit the application's ordinary native capabilities.
Signing, stores, executable-memory enforcement, and production lifecycle management are separate future platform work, not prerequisites for this toolchain.
Bounded readers and artifact validation are robustness measures, not a sandbox or an implemented security-enforcement platform.
