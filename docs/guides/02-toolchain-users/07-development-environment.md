# 07 — Setting up a development environment

[Series](../README.md) · [Previous: Publication artifacts](../01-foundations/06-publication-artifacts.md) · [Next: Hello World end to end](08-hello-world-end-to-end.md)

## Objective and prerequisites

By the end of this chapter, you should understand which tools run on the developer's machine,
which files describe the application target, and how to recognize an environment problem before investigating compiler internals.
You need ordinary Bash, Linux filesystem, and C build experience. You do not need LLVM experience.
The optional lab assumes the SDK has been bootstrapped; it creates a fresh build directory and does not modify an existing build.

The current development baseline is an **x86-64 Ubuntu 24.04 host**. This is not the full list of environments required by [01](../../01-architecture-design.md).
The distinction matters: a portable publication format does not make every development tool executable on every host.

## Three environments, not one

When a conventional C build says “the compiler,” it often hides several different concerns.
Nier makes those concerns easier to see because publication and native compilation happen at different times.

The **development host** executes CMake, Ninja, Clang, and the Nier tools. These programs have their own native library dependencies.
The **application target environment** supplies headers, startup objects, libraries, and ABI rules for the application being compiled.
The **runtime environment** supplies the loader and libraries used when the resulting native application starts.

An ABI, or application binary interface, is the machine-level agreement between compiled components:
argument passing, return values, data layout, and related rules.
A header describes source-level declarations; the ABI determines how those declarations become calls and memory accesses.
A matching header alone does not guarantee a matching runtime library.

For example, a 64-bit Clang process can compile an i686 application.
Clang still runs as a 64-bit host program, but it uses the 32-bit target's headers and layout rules.
The application has four-byte pointers even though the compiler process has eight-byte pointers. That is cross-compilation, not emulation.

Nier currently uses both x86-64 and i686 target environments as private evidence when publishing C. Its normal native-output command is qualified for x86-64.
Having an i686 sysroot in the SDK does not by itself establish a complete 32-bit product deployment.

## What the local SDK supplies

An SDK is a software development kit: a coordinated set of tools and target development files.
This repository's SDK is a local extraction of pinned, unmodified Ubuntu packages.
Bootstrap verifies package hashes and extracts them; it does not install system packages, execute their maintainer scripts, or patch Clang.

The important layout is:

```text
.sdk/
  host/usr/lib/llvm-18/        host Clang, LLVM, MLIR, LLD and their libraries
  host/usr/bin/               CMake and Ninja
  sysroots/x86_64-linux-gnu/  x86-64 application headers, CRT and libraries
  sysroots/i686-linux-gnu/    i686 application headers, CRT and libraries
  sdk-lock.sha256            receipt checked against the pinned SDK contract
```

A **sysroot** is a directory treated as the root of the target's development filesystem.
It lets the compiler find target headers and libraries without accidentally using `/usr/include` and libraries from the developer's host.
The CRT, or C runtime startup code, includes the small native objects that connect the loader's entry point to the application's `main` function.

The pinned LLVM/Clang/MLIR/LLD package version is 18.1.3-1ubuntu1. This precision is important for a C++ plugin: the plugin is loaded into Clang's process and must match the frontend interfaces it was built against.
“Some Clang 18” is not the same assurance as the SDK's coordinated package set.

The SDK is not a fully isolated or hermetic build environment.
Building Nier still uses some host development facilities, including C++ standard-library headers and startup files.
SDK CMake has host networking dependencies. The package lock records extracted inputs, not every fact about the machine.
Avoid converting “pinned SDK” into an unsupported reproducibility claim.

## Preparing a shell and an editor

The normal initial setup is to run `bash scripts/bootstrap-sdk.sh` from the repository root, then source `sdk/env.sh` in the Bash session used for work.
Bootstrap downloads a substantial SDK; consult [its guide](../../reference/development-sdk.md) for prerequisites and disk requirements before doing so.

Sourcing the file changes the current shell. It places SDK tools on `PATH`, sets `NIER_SDK_ROOT`, and exports paths used by CMake and dynamic library loading.
Executing the file in a child shell would not update its parent. That is why the command is `source sdk/env.sh`, not `bash sdk/env.sh`.

`PATH` answers “which executable does this command name select?”
`LD_LIBRARY_PATH` answers a different question: “where should the native loader look for shared libraries?”
Finding the expected `cmake` executable does not prove it can load its libraries. A startup error about `librhash.so.0` is an environment problem, not evidence that Nier's IR is invalid.

The checked-in VS Code configuration uses clangd for C/C++ language services.
clangd reads `build/prealpha/compile_commands.json`, a database generated by CMake containing real compilation flags for individual source files.
This is more accurate than adding every directory in the repository to an editor's include path: the real command carries definitions, language mode, and SDK headers together.

The `prealpha` CMake preset sets the standard repository-local SDK paths. The small CMake and CTest launchers load the SDK environment even for initial tool probes, allowing VS Code to start from a desktop session.
They are development conveniences, not replacement C compilers. Publication still invokes stock Clang.

C examples and shell-driven fixtures are not all CMake targets.
`.clangd` keeps those files in C mode even when it borrows a C++ command.
Some fixtures deliberately need a generated header or a test-specific macro. Their missing context must be supplied by the relevant test; suppressing all diagnostics would hide genuine errors along with the missing context.

## Optional lab: inspect and configure without touching a shared build

Run this in Bash from the repository root after bootstrap. It uses a fresh scratch directory; it does not run the entire suite or rebuild shared tools.

```bash
source sdk/env.sh
guide_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide07-XXXXXX")

command -v clang
clang --version
command -v cmake
cmake --version
printf 'x86-64 sysroot: %s\n' "$NIER_SYSROOT_X86_64"
printf 'i686 sysroot:   %s\n' "$NIER_SYSROOT_I686"

cmake -S . -B "$guide_work/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
test -s "$guide_work/build/compile_commands.json"
rg -n 'src/consumer/Main.cpp' "$guide_work/build/compile_commands.json"
printf 'Independent configuration retained at %s\n' "$guide_work"
```

Configuration discovers dependencies and creates build rules. It does not compile the whole project.
Inspect the database entry: its include paths point to this checkout and SDK, while its working directory is the new scratch build.
If you later choose to build this configuration, its generated `nier.cfg` and executables belong together.
Do not combine an old plugin with a newly built configuration simply because both filenames look familiar.

For the later labs, the prerequisite is a completed publisher build in the standard `build/prealpha` directory.
The [build instructions](../../reference/building-nier.md) and preset describe creating it.
Each later lab uses fresh application outputs, so reading the course does not require repeatedly rebuilding Nier itself.

## Troubleshooting in the right order

First check executable selection and SDK environment. Next check that CMake configured against the intended checkout and SDK.
Then check the generated configuration and compiler versions. Only after those checks should you debug a failing Nier transformation.

Pre-alpha has no compatibility promise. A changed package lock or public contract may require rebuilding tools and republishing artifacts.
The solution is not to add a legacy reader merely to make an old local experiment run.
Conversely, a failing test on the current matching toolchain is not excused by the pre-alpha label; current semantics still require validation.

## Recap and check your understanding

The host runs tools, a sysroot describes a target development environment, and the native loader supplies application runtime dependencies.
Keep those roles separate when diagnosing paths, headers, and library failures.

<details>
<summary>Why does a 64-bit Clang process need a separate i686 sysroot?</summary>

The compiler's own executable architecture does not determine its output's ABI. The i686 target needs its own headers, layouts, startup objects, and libraries.

</details>

<details>
<summary>Does a successful CMake configuration prove the publication flow works?</summary>

No. It establishes that dependencies and build rules can be configured.
Compilation, publication, native linking, and runtime tests are separate checks.

</details>

<details>
<summary>Why not disable editor error checking for all test fixtures?</summary>

Some fixtures intentionally fail or need generated context, but ordinary source errors must remain visible.
Use the real compile database and identify special fixture context rather than suppressing diagnostics globally.

</details>

## Guided reading

Read `sdk/env.sh` to match each environment variable to its role, then [development SDK reference](../../reference/development-sdk.md) for the host boundary.
Follow the dependency setup in `CMakeLists.txt` and the editor choices in `.clangd` and `CMakePresets.json`.
`sdk/check-sdk.sh` demonstrates SDK plumbing checks; it is not a substitute for the publication tests introduced next.
