# Hello: starter project and NieR solution

The [Hello project walkthrough](../../docs/guides/02-toolchain-users/hello-project-walkthrough.md) takes the independent `hello/` starter through your choice of Make or CMake publication.
The [general integration guide](../../docs/guides/02-toolchain-users/using-nier-with-your-c-project.md) explains how to apply the same approach to your own C project.
Read both guides in Obsidian with `docs/` open as the vault; use VS Code for these project files.

- `hello/` is an ordinary C project with native Make and CMake builds and no NieR dependency.
- `hello-nier/` contains exactly the same source and native build files, plus `nier/Makefile` and `nier/CMakeLists.txt` for publication on demand.

The solution includes both independent configurations for reference.
Choose one build system and add or use only its publication configuration; completing the other path is optional.

Copy either project directory anywhere outside this repository and build it there.
Copy its source and configuration files without existing `build/` outputs; the walkthrough does this explicitly so CMake caches do not retain paths from the old location.
Neither project uses source files or build rules from its parent directories.
The solution locates the separately installed development checkout through the explicit `NIER_ROOT` setting, not through its location inside this example tree.

Choose Make or CMake for the native build and run only its commands below.
Either choice prints `Hello world`:

```sh
# Using Make
make hello
./build/native-make/hello

# Using CMake
cmake -S . -B build/native-cmake -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/native-cmake --target hello
./build/native-cmake/hello
```

Run your chosen commands from either copied project root.
For NieR publication, source the toolchain's `sdk/env.sh`, then run one of these from the copied solution root:

```sh
# Using Make
make -f nier/Makefile NIER_ROOT=/absolute/path/to/nier

# Using CMake
cmake -S nier -B build/nier-cmake -G Ninja -DNIER_ROOT=/absolute/path/to/nier
cmake --build build/nier-cmake --target publish
```

The artifacts are `build/nier-make/hello.nier` and `build/nier-cmake/hello.nier` respectively.
Compile an artifact with the separate `nierc` program before executing the native result; the walkthrough shows the complete commands and runtime requirements.
The Make integration's SDK include requires a NieR checkout path without whitespace.
If your tools use a different build directory, override `NIER_BUILD_TOOL` as a Make variable or CMake cache setting.

All generated files stay under each project's ignored `build/` directory.
When comparing with the solution, ignore generated `build/` contents and only the unchosen configuration under `hello-nier/nier/`.
Your chosen publication configuration and every starter file must match; the unused alternative is not a missing step.
