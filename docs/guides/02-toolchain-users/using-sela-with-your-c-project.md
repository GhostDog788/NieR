# Publish your C project with Sela

[Guide series](../README.md) · [Hello project walkthrough](hello-project-walkthrough.md) · [SDK integration reference](../../reference/build-integration.md)

You have a normal C project that builds with Clang and Make or CMake.
You want to publish a `.sela` artifact, then use the separate `selac` compiler to turn that artifact into a native executable.
This guide explains how to configure that flow for your own project; no compiler background is required.

For a complete exercise with supplied source code, start with the [Hello project walkthrough](hello-project-walkthrough.md).
It takes an independent, ordinary C project and adds one publication configuration for your chosen build system.
The finished example includes both independent alternatives for reference, so you can compare your chosen configuration with a working solution.

For a qualifying simple project, you keep your C sources and existing native Makefile or `CMakeLists.txt` unchanged.
Add a small, separate `sela/` configuration for the build system you use.
Follow either the Make section or the CMake section below; you do not need both configurations.
That configuration tells the SDK where your project is, what to build, and which native output to publish.
Stock Clang still performs C publication; the SDK adapter coordinates the existing build around it.

## Prepare the toolchain and check the native project

These instructions describe the current pre-alpha development checkout, not a released system-wide SDK.
The tested development baseline is x86-64 Ubuntu 24.04, with the repository-local `.sdk` and publisher tools in `build/prealpha`.
Follow the [build instructions](../../reference/building-sela.md) first if those are missing.

In Bash, set the toolchain checkout and your application to their actual absolute paths:

```bash
sela_repo=/absolute/path/to/sela
app_source=/absolute/path/to/myapp
source "$sela_repo/sdk/env.sh"
cd "$app_source"
```

Sourcing the environment selects the matching Clang, CMake, Ninja, and SDK tool libraries for this shell.
It does not edit your shell startup files or install a different system compiler.
Use one matching SDK and Sela build, rather than mixing a plugin from one checkout with tools from another.

Before adding publication, check your ordinary native build with Clang and run its tests.
For Make, that might be `make CC=clang all`; for CMake, configure a fresh binary directory with `-DCMAKE_C_COMPILER=clang`, then build your normal target.
Follow your project's own configure options, runtime arguments, data-file paths, and working-directory requirements.
A failing native build is not a useful starting point for debugging publication.
The [Hello walkthrough](hello-project-walkthrough.md) checks your chosen native build in a disposable copy if you want a safe first trial.

## Choose the target and the actual output

A build target is a request to the build system, not necessarily a filename.
For example, `make all` may produce `bin/myapp`, and a CMake target named `myapp` may place its executable in a `bin/` subdirectory.
Find the actual result of the successful native build before configuring publication.

| Setting | Make interpretation | CMake interpretation |
| --- | --- | --- |
| Source directory | Existing application project | Existing application source project |
| Native target | Target passed to Make, such as `all` | Application target, such as `myapp` |
| Native output | Path relative to the private source/build directory, such as `bin/myapp` | Path relative to the private native binary directory, such as `bin/myapp` |
| Sela output | Destination artifact path | Destination artifact path, relative to the publication binary directory unless absolute |

Do not point the native-output setting at a `.sela` file, an intermediate object, or an executable from an earlier local build.
The SDK will build the application privately and select the named result there.
Start with one executable; shared libraries and static archives are also supported within the [qualified SDK behavior](../../reference/build-integration.md), but dependency selection deserves its own deliberate setup.

## Add a Make publication configuration

Create `sela/Makefile` in your application, separate from its existing `Makefile`.
This template assumes `make all` produces `bin/myapp`; replace those two values with the target and output you just checked.
Choose an unused artifact destination under your ignored build directory.

```make
ifndef SELA_ROOT
$(error Set SELA_ROOT to the absolute path of your Sela checkout)
endif

SELA_BUILD_TOOL ?= $(SELA_ROOT)/build/prealpha/sela-build
SELA_SOURCE_DIR := $(CURDIR)
SELA_TARGETS := all
SELA_NATIVE_OUTPUT := bin/myapp
SELA_ARTIFACT := $(CURDIR)/build/sela/myapp.sela

include $(SELA_ROOT)/sdk/share/sela/Sela.mk

.PHONY: publication-directory
sela: | publication-directory
publication-directory:
	mkdir -p "$(CURDIR)/build/sela"
```

The `mkdir` recipe begins with a literal tab, as Make requires.
Run this configuration from your application root, because `$(CURDIR)` identifies that directory:

```bash
make -f sela/Makefile SELA_ROOT="$sela_repo"
```

`SELA_ROOT` is the explicit absolute toolchain path; it is not the application's parent directory.
The simple Make `include` above requires that toolchain path to contain no whitespace.
`SELA_BUILD_TOOL` locates the SDK's internal build coordinator, not a replacement public compiler command.
Override it if your Sela tools live outside `build/prealpha`.

The included SDK Makefile runs your original Make build in private directories.
If the project has a `configure` script, the adapter runs it there before Make.
Optional `SELA_CONFIGURE_ARGS` and `SELA_CFLAGS` supply qualified configure arguments and compiler flags.
Its list variables are whitespace-separated; they cannot represent a single complex argument containing spaces.
See the [SDK reference](../../reference/build-integration.md) before adding such options.

## Add a CMake publication configuration

For CMake, create `sela/CMakeLists.txt` beside, not in place of, your application's existing CMake project.
This small coordinator has no source language of its own: `project(... NONE)` does not configure a C compiler for it.
The SDK separately configures the actual application.

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyPublication NONE)

set(SELA_ROOT "" CACHE PATH "Absolute path of your Sela checkout")
if(NOT IS_ABSOLUTE "${SELA_ROOT}")
  message(FATAL_ERROR "Set SELA_ROOT to the absolute path of your Sela checkout")
endif()
set(SELA_BUILD_TOOL "${SELA_ROOT}/build/prealpha/sela-build"
  CACHE FILEPATH "Sela build coordinator")

include("${SELA_ROOT}/sdk/share/sela/Sela.cmake")
sela_add_publication(publish
  SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.."
  NATIVE_OUTPUT bin/myapp
  OUTPUT myapp.sela
  BUILD_TOOL "${SELA_BUILD_TOOL}"
  TARGETS myapp)
```

Replace `TARGETS myapp` and `NATIVE_OUTPUT bin/myapp` with your native project's target and output.
The relative source path points to your own application one directory above `sela/`; it makes no assumption about where the Sela toolchain lives.
Configure and build the coordinator from your application root:

```bash
cmake -S sela -B build/sela-cmake -G Ninja -DSELA_ROOT="$sela_repo"
cmake --build build/sela-cmake --target publish
```

This produces `build/sela-cmake/myapp.sela`.
`publish` is the coordinator target; `myapp` is the application's native target.
Add application options through `CONFIGURE_ARGS`, for example `"-DENABLE_FEATURE=ON"`, and use `CFLAGS` for supported compiler flags.
These are CMake argument lists, so individual arguments can be quoted.
Do not override the SDK's profile compilers, sysroots, archiver, or native build tools.
Your application does not need to adopt Sela's own development CMake preset.

## Compile the artifact and run the native result

Publication is now finished, but a `.sela` artifact is not an executable.
Choose the artifact from your Make or CMake configuration and give it to the independent device compiler:

```bash
artifact="$app_source/build/sela-cmake/myapp.sela"  # Or the Make destination.
native_output="${artifact%.sela}"
"$sela_repo/build/prealpha/selac" inspect "$artifact"
"$sela_repo/build/prealpha/selac" "$artifact" -o "$native_output"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$native_output"
```

Removing the `.sela` suffix places the native executable beside whichever artifact you selected, with a distinct filename.
Adjust the application's working directory and arguments as needed.
`selac` consumes Sela Code, not your C source, Makefile, or private LLVM captures.
The native result runs without Clang, `selac`, or the `.sela` file, but it still needs the native loader, runtime, and declared libraries chosen during compilation.
Keep the SDK/runtime paths in place for this development setup.
Clearing the development shell's library overrides prevents tool libraries from accidentally changing application loading; it does not remove legitimate application dependencies.

## Why not globally enable Sela mode in CC?

Ordinary builds may compile and immediately execute configure probes or code generators.
Those programs must be native executables, not publication artifacts.
The SDK therefore runs two private native builds, for its x86-64 and i686 profiles, and preserves their generated inputs and selected link evidence.
Stock Clang with the Sela plugin then publishes the selected application.
Private source copies, capture records, and native objects are not distributed as part of the `.sela` artifact.

For a small program without build-time executables, direct `clang --config=/absolute/path/to/sela.cfg ... -o app.sela` publication is also available.
That is a separate entry point, not a reason to inject Sela mode into every command in an existing build.
The [Hello World pipeline chapter](08-hello-world-end-to-end.md) explains the direct compiler flow.

## Rebuilds and current limits

Repeat the Make publication command or build the CMake `publish` target again after changing your project.
The adapter runs fresh private builds and replaces an existing artifact only after success; it does not yet maintain an incremental publication cache.
Run `selac` again to refresh the native executable.
Direct Clang retains its own failure cleanup behavior, so use disposable output paths when experimenting with that entry point.

- **Selected output not found:** check the target and relative output separately; CMake's selected path is relative to its private native binary directory.
- **Plugin, configuration, or SDK receipt mismatch:** use a matching current SDK and build, then regenerate and rebuild after pre-alpha contract changes.
- **Configure cannot run its compiler:** traditional scripts that expand `$CC` without shell evaluation require whitespace-free SDK and scratch paths.
- **Unsupported construct or build operation:** retain the diagnostic and make a minimal reproducer; do not suppress a failed portability proof.

The current native-output target is x86-64, and C/build integration coverage remains bounded.
This workflow does not prove every C program, arbitrary CPU support, performance parity, or reverse-engineering resistance.
SESela (SES) is the separate security platform planned above Sela.
It is not implemented yet, and this publication flow does not require it.
Consult the [implementation status](../../02-implementation-plan.md) and [SDK reference](../../reference/build-integration.md) before broadening deployment.
