# Building NieR and setting up VS Code

[Documentation home](../README.md) · [Project reference](README.md)

Keep `docs/` open as the Obsidian vault and the repository root open in VS Code.
Run the commands below in **Bash from the repository root**, not from the `docs/` directory.

## Host and SDK prerequisites

The current development baseline is **x86-64 Ubuntu 24.04**.
The SDK extracts pinned, unmodified Ubuntu packages locally; it does not install system packages or patch upstream compilers.
Read the [development SDK reference](development-sdk.md) for the required host tools, disk space, and non-hermetic host dependencies before bootstrapping.

```bash
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
```

Bootstrap creates the repository-local `.sdk` by default.
Source `sdk/env.sh` again in each new terminal session used for these commands.
The pinned Clang, LLVM, MLIR, and LLD versions must stay coordinated with the NieR build.

## Build the publisher and consumer

Use the checked-in preset to create the standard `build/prealpha` build:

```bash
cmake --preset prealpha
cmake --build --preset prealpha
```

The result includes the separate `build/prealpha/nierc` compiler and the developer-side Clang integration configured by `build/prealpha/nier.cfg`.
Keep that configuration and its matching plugin/build products together.
After a pre-alpha contract or SDK change, rebuild the tools and regenerate application artifacts rather than mixing versions.

To run the normal test suite after building:

```bash
ctest --preset prealpha
```

The full upstream [qualification corpus](qualification-corpus.md) is a separate, longer-running workflow.
It is not required merely to read the guides or try the first application.

## Build the independent native device compilers

The distributable consumer presets use their own target-specific, source-built LLVM/MLIR SDKs.
After bootstrapping and sourcing the publisher SDK above, build the desired device product; these commands show both independent alternatives:

```bash
bash scripts/bootstrap-consumer-sdk.sh x86_64
cmake --preset consumer-x86_64
cmake --build --preset consumer-x86_64
ctest --preset consumer-x86_64

bash scripts/bootstrap-consumer-sdk.sh i686
cmake --preset consumer-i686
cmake --build --preset consumer-i686
ctest --preset consumer-i686
```

These are substantial upstream source builds. They retain unmodified LLVM/MLIR 18.1.3, register the X86 backend family, and link selected components instead of a monolithic multi-backend LLVM shared library.
`NIER_CONSUMER_COMPILE_JOBS=1|2` controls bootstrap compilation parallelism; the bootstrap bounds heavyweight source builds and links.
The resulting SDKs are `.sdk/consumer/x86_64` and `.sdk/consumer/i686`; corresponding compiler builds are `build/consumer-x86_64` and `build/consumer-i686`.

Each product excludes the Clang frontend plugin, capture/replay tools, and LLVM-to-NieR producer.
Stock Clang compiles NieR's own C++ sources on the build host; it is not an input or dependency of the resulting on-device compiler.
A device `nierc` compiles and lowers only its own native target, even though the shared artifact schema can describe both domains.
Use each build's own CTest inventory, not the publisher-only CI smoke selection.
The i686 host-side tests exercise compatibility mode and are not genuine 32-bit-kernel acceptance.
They also require the build host's 32-bit glibc loader at `/lib/ld-linux.so.2`, supplied on Ubuntu by `libc6-i386` or a matching multiarch libc installation.
Extracting an SDK sysroot does not install that system loader; the device-qualification CI workflow installs the host prerequisite explicitly.
Both source-built bundles have passed their component tests and the fresh publish-once two-device matrix, including the real i686 kernel.
The complete fresh dual-destination corpus also passed at the recorded 2026-09-12 checkpoint.
A component-test pass alone is not a substitute for that separate gate, and these bounded qualification results are not a production-release claim.

See the [compiler distribution reference](compiler-distribution.md) for packaging, runtime prerequisites, the real i686 VM, and the manually dispatched `Device compiler qualification` GitHub workflow.
After assembly, a bundle's `bin/nierc --print-target` identifies its native target and `bin/nierc --check-sdk` checks its selected installation without compiling an artifact.
Clear `NIER_SDK_ROOT` when checking sibling-SDK discovery; an old compiler and a newly built SDK must not be mixed merely because both target the same architecture.

## Configure VS Code

Open the repository root and enable the recommended **clangd** and **CMake Tools** extensions.
Select **NieR pre-alpha (pinned SDK)** if CMake Tools asks for a configure preset.
You do not need to launch VS Code from a shell that sourced `sdk/env.sh`: the checked-in CMake/CTest launchers load the SDK environment for the editor.

clangd reads the real compile database at `build/prealpha/compile_commands.json`.
The `.clangd` configuration keeps C examples in C mode even when clangd borrows a C++ build command.
The workspace disables duplicate Microsoft IntelliSense diagnostics, not clangd's error checking.
Fixtures requiring generated headers or special test flags still need their own build context.

To regenerate the database without a full build, run **CMake: Configure** in VS Code, or:

```bash
./scripts/cmake-sdk.sh --preset prealpha
```

If the editor was already open when its configuration changed, run **Developer: Reload Window**.
To inspect a source reference from the docs, press **Ctrl+P** in VS Code and enter its repository-relative path.

## Continue to your first application

Follow [Hello World end to end](../guides/02-toolchain-users/08-hello-world-end-to-end.md) for an explained walkthrough,
or [publish your existing C project](../guides/02-toolchain-users/using-nier-with-your-c-project.md) for Make/CMake recipes.
The [development-environment chapter](../guides/02-toolchain-users/07-development-environment.md) explains why the host, target sysroots, and native runtime are separate concerns.
