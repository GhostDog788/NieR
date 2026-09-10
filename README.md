# On-target AOT

An experimental source-private publication toolchain: stock compilers capture
native LLVM profiles on the publisher, a shared merger produces one common MLIR
artifact, and a language-independent device compiler produces an ordinary native
ELF using LLVM and stock LLD. The executable uses the supplied glibc and its
unmodified Linux loader, with no publication runtime or JIT.

The first C Hello World/native-width checkpoint is working, with basic
Make/CMake integration. **It is not yet the broad-C MVP or the complete product.**
The authoritative requirements are in
[01](docs/01-architecture-design.md); the staged implementation and acceptance
gates are in [02](docs/02-implementation-plan.md).

## Build

The bootstrap is for an Ubuntu 24.04 x86-64 development host. It extracts pinned,
SHA-256-verified prebuilt packages into `.sdk`; it does not install system
packages or modify upstream compilers. See [SDK details](sdk/README.md) for host
dependencies and the non-hermetic publisher-host boundary.

```sh
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

## First Hello World

```sh
mkdir -p artifacts
build/aot publish --recipe examples/hello/hello.build.json -o artifacts/hello.aotpkg
build/aot inspect artifacts/hello.aotpkg
build/aot compile artifacts/hello.aotpkg --output-dir artifacts/hello-native
env -u LD_LIBRARY_PATH artifacts/hello-native/bin/hello
```

Expected output: `Hello world`.

This workspace already contains that demo artifact and executable. Run the last
command to try it now. To repeat publication/compilation, choose unused output
paths; the CLI deliberately refuses to overwrite existing outputs.

The final command executes the actual ELF directly. Clearing the SDK tool-shell's
`LD_LIBRARY_PATH` prevents development-tool library paths from overriding its
ordinary native library configuration. The artifact and compiler are unnecessary
when running the executable; the supplied runtime libraries must remain available
at the paths selected during linking.

Publication captures **both x86-64 and i686** through a stock LLVM pass plugin.
It merges matching operations and native-width differences, reconstructs and
checks both captured profiles, then publishes custom MLIR bytecode—not C source,
raw LLVM modules, publisher-built application binaries, or two program copies.
Only x86-64 device execution is initially qualified.

`aot compile` does not invoke Clang or read source/capture files. Its custom
transformations end at target-specific LLVM IR; stock `opt`, `llc`, and `ld.lld`
perform optimization, code generation, and linking. `aot lower` is a diagnostic
interface that emits reconstructed LLVM IR for either capture profile without
claiming i686 device-execution support.

## Experimental interfaces and limits

A private source-list recipe selects ordinary `.c` translation units, compiler
flags, an application name, and optional qualified native libraries. Each
translation unit retains its optimization setting and is compiled separately on
the device. The package contains a versioned JSON manifest and custom MLIR
bytecode with hashes. Unknown versions, unsupported IR, merge mismatches, and
undeclared payloads fail explicitly.

Existing outputs are never overwritten. Use a new output path for an experiment;
this prototype does not implement installed-release updates or a release
registry. Do not interpret repeated development builds as permission to
recompile an installed release.

The initial compiler slice supports matching, single-block scalar functions,
direct calls, readonly byte strings, basic scalar memory operations, and native
widths. Broader control flow, native aggregate ABI reconstruction, arbitrary
variadic definitions, callbacks, real-project acceptance, and complete
Make/CMake/archive qualification remain staged work. Unsupported features are
compiler diagnostics, not security policy bans.

Existing-build recipes use a `build` object instead of `sources`, with `system`
(`make` or `cmake`), `source_dir`, the executable `output`, optional
`configure_args`, and `targets`. See `tests/fixtures/build/*.build.json`.
Select a clean, scoped project source directory: it is copied into private
profile trees, excluding `.git` and `.sdk`; other existing generated directories
are not assumed disposable. The current integration accepts a single selected
executable linked from direct C objects and qualified SDK libraries. Archives,
shared outputs, divergent link graphs, and unqualified flags fail explicitly.
It preserves native configure probes and build-time generators, including
private i686 execution. No source-tree outputs are written back.

The current seven CTest groups cover 73 package-envelope checks, 13 IR-validation
cases, capture at six optimization settings on both profiles, the Hello/width
and cross-translation-unit fixtures, Make/CMake with generated headers and
native configure probes, and input-access tracing. The latter requires
`strace`; CMake omits that group if unavailable. Package JSON uses the canonical
publisher encoding to reject duplicate/hidden fields. Tests do not establish
full TAR-metadata sanitization, a hermetic publisher environment, or A1/A2.

Security, store integration, signing, and executable-memory enforcement are not
part of this checkpoint. Full performance and reverse-engineering resistance
requirements remain unproved; excluding source/debug data alone does not prove
native-equivalent information exposure.

Build products, downloaded packages, and private workspaces are ignored by Git.
`--keep-work` retains a private temporary compiler workspace for debugging and
prints its path; do not distribute those files.
