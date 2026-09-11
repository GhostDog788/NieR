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

## Build only the independent compiler

After sourcing the same SDK environment, configure a separate consumer-only build:

```bash
cmake -S . -B build/consumer-only -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNIER_BUILD_PUBLISHER=OFF
cmake --build build/consumer-only --parallel 2
ctest --test-dir build/consumer-only --output-on-failure
```

This excludes the Clang frontend plugin, capture/replay tools, and LLVM-to-NieR producer from the configured product.
Stock Clang may still compile NieR's own C++ sources; that does not make a language frontend an input to the resulting `nierc`.
Use this build's own CTest inventory, not the publisher-only CI smoke selection.
See the [compiler distribution reference](compiler-distribution.md) to assemble its independent runtime bundle.

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
