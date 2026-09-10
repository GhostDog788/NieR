# Nier

Nier is a pre-alpha, source-private native publication toolchain. **Nier code**
is its independent, architecture-neutral format, implemented with a custom MLIR
dialect. A producer emits one standalone `.nier` archive. The separate,
language-blind `nierc` compiler turns it into ordinary native output using
LLVM and stock LLD.

Our C producer is a plugin for **actual stock Clang**, not a replacement Clang
executable or an `aot publish` wrapper. Other producers can construct Nier
directly through the public APIs without LLVM captures.

Everything is pre-alpha, with **zero backward-compatibility obligations**.
There are no legacy command aliases, artifact readers, or migration layers.
This is a qualified pre-alpha C implementation, not unrestricted C or the full product.
Requirements are in [01](docs/01-architecture-design.md); implementation scope,
remaining gates, and evidence are in [02](docs/02-implementation-plan.md).

## Build

The pinned SDK uses prebuilt Ubuntu 24.04 x86-64 packages and LLVM/Clang/MLIR/LLD
18.1.3. Bootstrap extracts verified packages locally; it does not install system
packages or patch upstream compilers.

```sh
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
cmake -S . -B build/prealpha -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/prealpha --parallel 2
ctest --test-dir build/prealpha --output-on-failure
```

See [SDK details](sdk/README.md) for host requirements and the non-hermetic
development environment. Build products, downloaded SDK packages, and private
workspaces are untracked.

## VS Code

Open the repository root and enable the recommended **clangd** and **CMake
Tools** extensions. After bootstrapping the SDK, select the **Nier pre-alpha
(pinned SDK)** configure preset if prompted. CMake configures on open; a full
build is not needed for code completion. You do not need to launch VS Code
from a shell that sourced `sdk/env.sh`.

The workspace uses clangd for C/C++ diagnostics and completion; Microsoft's
duplicate IntelliSense engine is disabled, not clangd's error checking.
`.clangd` reads the real flags, definitions, and LLVM/MLIR header paths from
`build/prealpha/compile_commands.json`. C examples and shell-driven fixtures
remain C even when clangd borrows a C++ build command. Special test fixtures
still need their test-specific flags or generated headers; diagnostics for
intentionally invalid test inputs are not suppressed.

To regenerate the database manually, run **CMake: Configure**, or:

```sh
./scripts/cmake-sdk.sh --preset prealpha
```

This preset uses the standard repository-local `.sdk` layout. The small CMake
and CTest launchers load the SDK environment before executing the stock tools,
including version probes and test discovery. They do not change the publication
compiler flow or your global VS Code settings. If the editor already had this
folder open, run **Developer: Reload Window** once to pick up the workspace settings.

## Multi-file Hello World

The example has `main.c`, `hello.h`, and `hello.c`. From the repository root,
after sourcing the SDK environment:

```sh
mkdir -p artifacts
clang --config="$PWD/build/prealpha/nier.cfg" -O2 \
  examples/hello/main.c examples/hello/hello.c -o artifacts/hello.nier
build/prealpha/nierc inspect artifacts/hello.nier
build/prealpha/nierc artifacts/hello.nier -o artifacts/hello
env -u LD_LIBRARY_PATH artifacts/hello
```

Expected output: `Hello world`.

The publication file is a standard tar archive containing a manifest and
Nier bytecode for both translation units—not source, original LLVM modules,
native application objects, or whole per-target program copies.

Normal separate compilation works too:

```sh
clang --config="$PWD/build/prealpha/nier.cfg" -O2 -c examples/hello/main.c -o artifacts/main.o
clang --config="$PWD/build/prealpha/nier.cfg" -O2 -c examples/hello/hello.c -o artifacts/hello.o
clang --config="$PWD/build/prealpha/nier.cfg" artifacts/main.o artifacts/hello.o -o artifacts/hello.nier
```

In this mode `.o` files contain relocatable Nier artifacts. Final publication
does not retain references to those intermediate files.

The native executable runs directly; neither the artifact nor compiler is
needed at runtime. The supplied glibc and its unmodified loader must remain
available at the paths selected during native linking. Clearing the developer
shell's `LD_LIBRARY_PATH` prevents its tool-library directories from overriding
the program's ordinary native loader configuration.

## Libraries and native dependencies

Stock Clang's Nier mode accepts `-shared` and SONAME/version-script options;
`nierc` emits a native shared library from a shared-kind artifact. Explicit
native dependencies are resolved from the supplied SDK and optional
`nierc --library-dir DIR` locations. Missing declared libraries fail before
linking; arbitrary host libraries are not an automatic fallback.

The Make/CMake integration can also select a native static-library output.
Its standalone Nier artifact preserves all archive members and their order;
`nierc` generates an ordinary native archive with stock `llvm-ar`, including
duplicate member names. Tests check that later linking remains lazy.

Application library code must be published separately and compiled into the
supplied native dependency environment. This manual prototype is not a
production dependency installer or updater. Qualification of the complete
library/build/ABI matrix remains part of the open C MVP gate.

## Existing Make and CMake projects

Use the small external integration described in
[SDK build integration](sdk/share/nier/README.md). Make projects include
`Nier.mk`; CMake coordinator projects call `nier_add_publication(...)`.

The SDK runs the existing project normally in two private native build trees.
Configure probes and generators are real native executables, using real stock
Clang. Target-generated inputs stay distinct: a pointer-width generator yields
8 on x86-64 and 4 on i686. Only selected application captures go through Clang's
paired-input Nier producer and final publication linker.

A plain global `CC="clang --config=nier.cfg"` is not sufficient for a project
that needs to execute build-time probes or generators: a Nier artifact is not a
host executable. The SDK integration handles those native builds instead of
requiring per-target project rewrites.

The onboarding target is at most 15 minutes of developer effort for qualifying
projects with a working native build and installed SDK. Automated build time is
separate; passing our fixtures is not a universal onboarding guarantee.

## Independent consumer and producer APIs

```sh
cmake -S . -B build/consumer-only -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNIER_BUILD_PUBLISHER=OFF
cmake --build build/consumer-only --parallel 2
ctest --test-dir build/consumer-only --output-on-failure
```

This build does not configure or link Clang frontend libraries, the capture
plugin, build replay, or the LLVM-to-Nier merger. Stock Clang is still the
default C/C++ compiler used to build the project itself. The development SDK
conveniently contains both publisher and consumer assets, but publisher assets
are not consumer inputs.

A compiler-only distribution can be assembled separately:

```sh
bash scripts/package-consumer.sh build/consumer-only /absolute/new/nier-compiler .sdk
```

The [distribution guide](scripts/package-consumer.md) describes its Ubuntu
24.04 host baseline and runtime paths. The relocation test builds and runs
native output with no publication tools or language frontends in the bundle.

Public Nier APIs are under `include/nier/IR` and `include/nier/Artifact`.
The optional LLVM producer API is separate under `include/nier/Producer`.
The independent-producer test constructs two modules through the public APIs,
without Clang, source captures, or copied publisher manifests, then compiles
and runs the artifact.

`nierc lower INPUT.nier --target i686 --output-dir DIR` exposes diagnostic
LLVM output. Initial product native execution remains x86-64; private i686
references and specialization checks do not imply complete i686 deployment.
Artifact target constraints prevent extrapolating the current two-profile
proof to ARM or different ABIs.

## Current correctness and limitations

Tests cover artifact bounds/digests/schema rejection, independent producers,
stock-Clang options and profile-dependent dependency files, native-width/fixed
literal controls, matching CFG/SSA and shared target-conditional switch arms,
floating-point cases, record/global and overlapping union storage, nested
packed/bitfield storage, native aggregate calls/returns and callbacks,
native nonlocal jumps, promoted scalar/pointer
varargs and native `va_list` forwarding at O0/O2, native build probes and
generators, unequal translation-unit inventories, per-profile archive order
with independent per-TU settings, static/shared outputs, and compiler/runtime
input-access boundaries.
See 02 and the test results for the current qualified subset.

The unchanged cJSON static/shared configurations each publish all 21 outputs
and pass all 19 original tests on Nier-generated executables, including normal
Unity nonlocal jumps. Stock-native and Nier callers use the Nier shared library.
The complete configured zlib gate also passes: its eight native outputs run
the original static, shared, and 64-bit tests in a source-free destination.

The qualified aggregate ABI includes integer, mixed floating/integer, and larger
native-width records across TUs and native shared-library boundaries. It does
not yet include union/bitfield/packed records **by value** or aggregate `va_arg`
extraction. Conditional graphs and unequal source sets have bounded proof rules,
not blanket coverage. Unsupported required features fail explicitly; an expected
diagnostic is not completion of their milestone.

Publication-specific interpreters, JITs, resident compilers, custom loaders,
and post-link ELF transformations are absent. Ordinary native capabilities
are not prohibited by a security policy. Signing, stores, executable-memory
enforcement, production lifecycle, other languages/targets, and full
performance/RE qualification remain later work. Source exclusion alone does
not establish native-equivalent reverse-engineering resistance.

## Output and failure behavior

Writers stage and validate successful artifact/native output before replacing
the requested file. The SDK coordinator stages its final Clang publication;
a failed SDK rebuild preserves the previous valid artifact. `nierc` likewise
does not replace a valid output after a failed native compile/link.
The compiler and internal publication linker reject input/output aliases,
including symlinked parents and hard links, before replacing an artifact.

Direct stock-Clang commands retain Clang's own failure cleanup: the driver may
remove its requested output when a frontend or linker job fails. A frontend
plugin cannot override that behavior. Never use an input/source pathname as
the output pathname. This is ordinary compiler output, not a production
installed-release registry.

Private diagnostic captures can contain source/debug information and must not
be distributed. Bounded readers and validation are robustness measures, not a
sandbox or security-platform enforcement.
