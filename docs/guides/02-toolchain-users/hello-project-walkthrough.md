# Hello: turn an ordinary C project into a Nier publication

[Guide series](../README.md) · [Publish your own C project](using-nier-with-your-c-project.md) · [Build instructions](../../reference/building-nier.md)

Start with a normal Clang C project, add two small configuration files, and produce a `.nier` artifact using either Make or CMake.
Then compile that artifact with the independent `nierc` program and run the resulting native executable.
Every application run in this walkthrough should print:

```text
Hello world
```

You do not need compiler internals or earlier course chapters.
You need basic Bash, a text editor, and a working Nier development checkout with its SDK and publisher tools already built.
The current tested setup is x86-64 Ubuntu 24.04, repository-local `.sdk`, and tools in `build/prealpha`.
If that setup is missing, complete the [build instructions](../../reference/building-nier.md) first.

## Know the starter and the solution

The repository contains two independently usable projects:

- `examples/hello/hello` is the starter: ordinary C sources and native Make/CMake builds, with no Nier dependency.
- `examples/hello/hello-nier` is the solution: exactly the same six files, plus `nier/Makefile` and `nier/CMakeLists.txt`.

The shared files are `main.c`, `hello.c`, `hello.h`, `Makefile`, `CMakeLists.txt`, and `.gitignore`.
`main.c` calls `hello()`, `hello.h` declares it, and `hello.c` prints the message through the normal C library.
Neither project reads source files or build rules from a parent directory.
You can copy either project outside this repository and build it there.

Open these source paths in VS Code when you want to inspect them; the documentation remains readable inside the `docs/` Obsidian vault.
We will work in a fresh copy, so the checked-in starter and solution stay untouched.

## 1. Copy the starter into a fresh workspace

Start **Bash in the Nier repository root** and run the following blocks in that same shell, in order.
The `nier_repo` variable records the toolchain's absolute location before we change directory.
Use a Nier checkout and scratch path without whitespace for this simple Make integration and traditional configure tools.

```bash
nier_repo="$PWD"
source "$nier_repo/sdk/env.sh"
hello_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-hello-project-XXXXXX")
practice_project="$hello_work/hello"
cp -a "$nier_repo/examples/hello/hello" "$practice_project"
cd "$practice_project"
printf 'Practice project: %s\nToolchain: %s\n' "$practice_project" "$nier_repo"
```

The source command selects the SDK's stock Clang and other build tools for this shell; it does not replace a system compiler.
Keep this shell open and retain the printed directory if you want to inspect or rerun your results later.
All generated project files will go under its ignored `build/` directory.

## 2. Check the ordinary native builds

First use the starter's existing Makefile, without any Nier configuration:

```bash
make hello
env -u LD_LIBRARY_PATH -u LD_PRELOAD ./build/native-make/hello
```

`hello` is the Make target; `build/native-make/hello` is the actual executable it produces.
The Makefile uses `clang`, now selected from the SDK by `PATH`.
It compiles `main.c` and `hello.c` separately, then links their native objects.

Check the independent native CMake build as well:

```bash
cmake -S . -B build/native-cmake -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/native-cmake --target hello
env -u LD_LIBRARY_PATH -u LD_PRELOAD ./build/native-cmake/hello
```

Both commands should print `Hello world`.
If either build or run fails, stop and resolve it before adding publication.
The `env` commands clear development-shell library overrides for the application; the binaries still use their ordinary native runtime dependencies.

Nothing you have done so far is Nier publication.
This baseline shows that the application is a normal C project with two normal build-system choices.

## 3. Add only the publication configuration

Create one directory in the copied project:

```bash
mkdir nier
```

Use your editor to create the following two files exactly as shown.
Do not change the application sources, its original Makefile, or its original `CMakeLists.txt`.
You are adding an optional publication path, not converting the ordinary build into something else.

### Create `nier/Makefile`

```make
ifndef NIER_ROOT
$(error Set NIER_ROOT to the absolute path of your Nier checkout)
endif

NIER_BUILD_TOOL ?= $(NIER_ROOT)/build/prealpha/nier-build
NIER_SOURCE_DIR := $(CURDIR)
NIER_TARGETS := hello
NIER_NATIVE_OUTPUT := build/native-make/hello
NIER_ARTIFACT := $(CURDIR)/build/nier-make/hello.nier

include $(NIER_ROOT)/sdk/share/nier/Nier.mk

.PHONY: publication-directory
nier: | publication-directory
publication-directory:
	mkdir -p "$(CURDIR)/build/nier-make"
```

The final `mkdir` recipe must begin with a **literal tab**, not spaces.
If Make reports `missing separator`, check that indentation first.

`NIER_ROOT` will be supplied explicitly on the command line and must be the absolute path to your Nier checkout.
`NIER_SOURCE_DIR` identifies the application root where you run Make.
`NIER_TARGETS` asks the original project to build `hello`, while `NIER_NATIVE_OUTPUT` names the resulting native executable inside each private build.
`NIER_ARTIFACT` is the separate publication destination; the directory prerequisite creates its parent before publication starts.

The included SDK file supplies the publication recipe.
`nier-build` is its internal coordinator, not a new public C compiler to use instead of Clang.
The default tool location is the current development build, and an explicit `NIER_BUILD_TOOL` override is available for other build directories.

### Create `nier/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
project(HelloPublication NONE)

set(NIER_ROOT "" CACHE PATH "Absolute path of your Nier checkout")
if(NOT IS_ABSOLUTE "${NIER_ROOT}")
  message(FATAL_ERROR "Set NIER_ROOT to the absolute path of your Nier checkout")
endif()
set(NIER_BUILD_TOOL "${NIER_ROOT}/build/prealpha/nier-build"
  CACHE FILEPATH "Nier build coordinator")

include("${NIER_ROOT}/sdk/share/nier/Nier.cmake")
nier_add_publication(publish
  SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.."
  NATIVE_OUTPUT hello
  OUTPUT hello.nier
  BUILD_TOOL "${NIER_BUILD_TOOL}"
  TARGETS hello)
```

This is a separate coordinator CMake project, not the application's original project.
It has no source language of its own, which is why `project` uses `NONE`.
`SOURCE_DIR` points one level up to this application's sources; `NIER_ROOT` independently locates the toolchain.
There is no dependency on where this project happens to sit inside the example repository.

`TARGETS hello` selects the original application's native target.
Here `NATIVE_OUTPUT hello` is relative to its private CMake binary directory, whereas `OUTPUT hello.nier` is relative to the coordinator's binary directory.
The two build systems therefore select the same program through different native output paths.

## 4. Compare your work with the supplied solution

From the copied project root, run:

```bash
diff -ru --exclude=build "$nier_repo/examples/hello/hello-nier" "$practice_project"
```

No output and a successful exit mean your project matches the solution, ignoring generated build directories.
If there is a difference, inspect it before continuing.
This comparison includes the unchanged C sources and native build files as well as the two new configurations.
Editor backup files inside the project will also appear in the comparison.

You have now made the entire integration change: two added files, zero modified application or native build files.
For a real project, you normally add only the adapter for the build system you use.

## 5. Publish with Make, then compile and run

Still in the copied project root, invoke the new Make configuration:

```bash
make -f nier/Makefile NIER_ROOT="$nier_repo"
```

The artifact is `build/nier-make/hello.nier`.
Publication may take longer than the initial native build because the SDK runs fresh private x86-64 and i686 builds, checks their captured program representations, and invokes stock Clang's Nier publication path.
The `.nier` output is a standalone artifact, not a renamed executable and not a package of your private native objects.

Inspect it, compile it with the independent device compiler, and run the resulting native file:

```bash
"$nier_repo/build/prealpha/nierc" inspect build/nier-make/hello.nier
"$nier_repo/build/prealpha/nierc" build/nier-make/hello.nier \
  -o build/nier-make/hello
env -u LD_LIBRARY_PATH -u LD_PRELOAD ./build/nier-make/hello
```

The final command should print `Hello world`.
Do not try to execute `hello.nier`: only the output of `nierc` is the native program.
Although this exercise runs both compilers on one machine, these are separate program invocations separated by a file.
The device compiler does not read the C sources or know that the artifact originated in C.

## 6. Publish with CMake, then compile and run

Configure the **publication** project by selecting `nier` as the source directory, not `.`:

```bash
cmake -S nier -B build/nier-cmake -G Ninja -DNIER_ROOT="$nier_repo"
cmake --build build/nier-cmake --target publish
"$nier_repo/build/prealpha/nierc" inspect build/nier-cmake/hello.nier
"$nier_repo/build/prealpha/nierc" build/nier-cmake/hello.nier \
  -o build/nier-cmake/hello
env -u LD_LIBRARY_PATH -u LD_PRELOAD ./build/nier-cmake/hello
```

This independently produces `build/nier-cmake/hello.nier`, then `build/nier-cmake/hello`, which also prints `Hello world`.
The CMake target `publish` belongs to the coordinator; its configuration tells the SDK to build the application's native target `hello` privately.
The SDK environment selects the matching CMake and Ninja.
This copied application does not need a Nier development preset.

## 7. Repeat publication and keep the boundaries clear

Request both publications again to exercise the rebuild path, then recompile their native outputs:

```bash
make -f nier/Makefile NIER_ROOT="$nier_repo"
cmake --build build/nier-cmake --target publish
"$nier_repo/build/prealpha/nierc" build/nier-make/hello.nier \
  -o build/nier-make/hello
"$nier_repo/build/prealpha/nierc" build/nier-cmake/hello.nier \
  -o build/nier-cmake/hello
env -u LD_LIBRARY_PATH -u LD_PRELOAD ./build/nier-make/hello
env -u LD_LIBRARY_PATH -u LD_PRELOAD ./build/nier-cmake/hello
diff -ru --exclude=build "$nier_repo/examples/hello/hello-nier" "$practice_project"
```

Both executions print the message, and the final comparison is still empty.
These commands intentionally replace only outputs you created in this disposable project's `build/` directory.
The SDK adapter runs fresh private builds for each request; a failed publication preserves the previous successful artifact.
This is not yet an incremental publication cache.

The ordinary builds remain available too: `make hello` and `cmake --build build/native-cmake --target hello` still use the unchanged native build files.
There is no need to make every compiler call emit Nier.
The adapter keeps configure probes and build-time generators native, then publishes the selected result; Hello is simply the smallest project exercising that integration.

The generated native executable needs no Clang, `nierc`, or `.nier` file to run, but it still depends on the chosen native loader and runtime paths.
Keep the development SDK/runtime in place.
This example neither installs dependencies on another machine nor proves that its compiled ELF can be moved anywhere.

Security enforcement is a separate part of the platform, not a hidden prerequisite for these commands.
This walkthrough demonstrates publication and native compilation, not signing enforcement, arbitrary CPU support, complete C coverage, or measured performance equivalence.

## Use the finished solution without doing the exercise

You can also start from the ready-made solution.
In a new Bash shell at the Nier repository root, copy it to a new scratch directory and publish directly:

```bash
nier_repo="$PWD"
source "$nier_repo/sdk/env.sh"
solution_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-hello-solution-XXXXXX")
cp -a "$nier_repo/examples/hello/hello-nier" "$solution_work/project"
cd "$solution_work/project"
make -f nier/Makefile NIER_ROOT="$nier_repo"
"$nier_repo/build/prealpha/nierc" build/nier-make/hello.nier \
  -o build/nier-make/hello
env -u LD_LIBRARY_PATH -u LD_PRELOAD ./build/nier-make/hello
printf 'Solution copy: %s\n' "$solution_work/project"
```

The same solution copy also supports the native builds from step 2 and the CMake publication commands from step 6.
It does not depend on anything you created in the practice directory.

If a command fails, check the first diagnostic rather than continuing with a stale output.
An absent selected output usually means a target/path mismatch; an SDK or plugin mismatch means the toolchain needs a matching build.
For more detailed troubleshooting and adapting the settings to your own application, continue with [Publish your C project with Nier](using-nier-with-your-c-project.md) and the [SDK integration reference](../../reference/build-integration.md).
