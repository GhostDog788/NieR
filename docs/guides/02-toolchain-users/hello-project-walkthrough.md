# Hello: turn an ordinary C project into a NieR publication

[Guide series](../README.md) · [Publish your own C project](using-nier-with-your-c-project.md) · [Build instructions](../../reference/building-nier.md)

Start with a normal Clang C project, choose **Make or CMake**, and add one small publication configuration file.
These are two supported alternatives: you need only one, not both.
Your chosen approach produces a `.nier` artifact.
Then compile that artifact with the independent `nierc` program and run the resulting native executable.
Every application run in this walkthrough should print:

```text
Hello world
```

You do not need compiler internals or earlier course chapters.
You need basic Bash, a text editor, and a working NieR development checkout with its SDK and publisher tools already built.
The current tested setup is x86-64 Ubuntu 24.04, repository-local `.sdk`, and tools in `build/prealpha`.
If that setup is missing, complete the [build instructions](../../reference/building-nier.md) first.

## Know the starter and the solution

The repository contains two independently usable projects:

- `examples/hello/hello` is the starter: ordinary C sources and native Make/CMake builds, with no NieR dependency.
- `examples/hello/hello-nier` is the solution: exactly the same six files, plus `nier/Makefile` and `nier/CMakeLists.txt`.

The solution includes both alternatives for reference, not because publication requires both files.
In your practice copy, add only `nier/Makefile` for Make or only `nier/CMakeLists.txt` for CMake.
The comparison below checks your chosen configuration against that solution.

The shared files are `main.c`, `hello.c`, `hello.h`, `Makefile`, `CMakeLists.txt`, and `.gitignore`.
`main.c` calls `hello()`, `hello.h` declares it, and `hello.c` prints the message through the normal C library.
Neither project reads source files or build rules from a parent directory.
You can copy either project outside this repository and build it there.

Open these source paths in VS Code when you want to inspect them; the documentation remains readable inside the `docs/` Obsidian vault.
We will work in a fresh copy, so the checked-in starter and solution stay untouched.

## 1. Copy the starter into a fresh workspace

Start **Bash in the NieR repository root** and keep using that shell throughout the exercise.
After this setup, follow either step 2A for Make or step 2B for CMake, then continue at step 3.
Skip the other approach entirely; if you later want to try it too, start with a fresh practice copy.
The `nier_repo` variable records the toolchain's absolute location before we change directory.
Use a NieR checkout and scratch path without whitespace for this simple Make integration and traditional configure tools.

```bash
nier_repo="$PWD"
source "$nier_repo/sdk/env.sh"
hello_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-hello-project-XXXXXX")
practice_project="$hello_work/hello"
mkdir "$practice_project"
cp -a "$nier_repo/examples/hello/hello/"{main.c,hello.c,hello.h,Makefile,CMakeLists.txt,.gitignore} \
  "$practice_project/"
cd "$practice_project"
printf 'Practice project: %s\nToolchain: %s\n' "$practice_project" "$nier_repo"
```

Copy only the six project files, not any existing `build/` outputs from earlier experiments.
In particular, a copied CMake cache would still refer to the old source and build directories.
These commands leave the original project's build outputs untouched.
The source command selects the SDK's stock Clang and other build tools for this shell; it does not replace a system compiler.
Keep this shell open and retain the printed directory if you want to inspect or rerun your results later.
All generated project files will go under its ignored `build/` directory.

## 2A. Make approach

Choose this approach if you want to publish through Make.
You will add only `nier/Makefile`; no CMake configuration or CMake commands are needed for this route.

First check the starter's existing native Makefile, without any NieR configuration:

```bash
make hello
env -u LD_LIBRARY_PATH -u LD_PRELOAD ./build/native-make/hello
```

`hello` is the Make target; `build/native-make/hello` is the actual executable it produces.
The Makefile uses `clang`, now selected from the SDK by `PATH`.
It compiles `main.c` and `hello.c` separately, then links their native objects.

The application should print `Hello world`.
If the build or run fails, resolve it before adding publication.
The `env` command clears development-shell library overrides; the binary still uses its ordinary native runtime dependencies.

Create the publication directory in the copied project:

```bash
mkdir nier
```

Use your editor to create only the file below, exactly as shown.
Leave the C sources and original native build files unchanged.

### Create `nier/Makefile`

```make
ifndef NIER_ROOT
$(error Set NIER_ROOT to the absolute path of your NieR checkout)
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

`NIER_ROOT` will be supplied explicitly on the command line and must be the absolute path to your NieR checkout.
`NIER_SOURCE_DIR` identifies the application root where you run Make.
`NIER_TARGETS` asks the original project to build `hello`, while `NIER_NATIVE_OUTPUT` names the resulting native executable inside each private build.
`NIER_ARTIFACT` is the separate publication destination; the directory prerequisite creates its parent before publication starts.

The included SDK file supplies the publication recipe.
`nier-build` is its internal coordinator, not a new public C compiler to use instead of Clang.
The default tool location is the current development build, and an explicit `NIER_BUILD_TOOL` override is available for other build directories.

Publish from the copied project root, then record the artifact path for the shared steps:

```bash
make -f nier/Makefile NIER_ROOT="$nier_repo"
artifact=build/nier-make/hello.nier
```

The artifact is `build/nier-make/hello.nier`.
Continue at step 3; do not create the CMake configuration below.

## 2B. CMake approach

Choose this approach if you want to publish through CMake.
You will add only `nier/CMakeLists.txt`; no Make publication configuration or Make commands are needed for this route.

First check the starter's existing native CMake project, without any NieR configuration:

```bash
cmake -S . -B build/native-cmake -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/native-cmake --target hello
env -u LD_LIBRARY_PATH -u LD_PRELOAD ./build/native-cmake/hello
```

The application should print `Hello world`.
If the build or run fails, resolve it before adding publication.
The `env` command clears development-shell library overrides; the binary still uses its ordinary native runtime dependencies.

Create the publication directory in the copied project:

```bash
mkdir nier
```

Use your editor to create only the file below, exactly as shown.
Leave the C sources and original native build files unchanged.

### Create `nier/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
project(HelloPublication NONE)

set(NIER_ROOT "" CACHE PATH "Absolute path of your NieR checkout")
if(NOT IS_ABSOLUTE "${NIER_ROOT}")
  message(FATAL_ERROR "Set NIER_ROOT to the absolute path of your NieR checkout")
endif()
set(NIER_BUILD_TOOL "${NIER_ROOT}/build/prealpha/nier-build"
  CACHE FILEPATH "NieR build coordinator")

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
The SDK environment selects the matching CMake and Ninja.
This copied application does not need a NieR development preset.

Configure the publication project by selecting `nier` as the source directory, not `.`.
Publish, then record the artifact path for the shared steps:

```bash
cmake -S nier -B build/nier-cmake -G Ninja -DNIER_ROOT="$nier_repo"
cmake --build build/nier-cmake --target publish
artifact=build/nier-cmake/hello.nier
```

The artifact is `build/nier-cmake/hello.nier`.
The target `publish` belongs to the coordinator; it tells the SDK to build the application's native target `hello` privately.
Continue at step 3; you do not need the Make approach as well.

## 3. Compare your chosen approach with the solution

You have made the entire integration change: one added file, zero modified application or native build files.
The checked-in solution contains both alternatives, so compare the shared project and your selected configuration separately.

First check the unchanged project files, excluding generated outputs and the publication directory:

```bash
diff -ru --exclude=build --exclude=nier "$nier_repo/examples/hello/hello-nier" "$practice_project"
```

Then run only the comparison for your chosen approach.

For Make, ignore the solution's unused CMake publication file:

```bash
diff -ru --exclude=CMakeLists.txt "$nier_repo/examples/hello/hello-nier/nier" "$practice_project/nier"
```

For CMake, ignore the solution's unused Make publication file:

```bash
diff -ru --exclude=Makefile "$nier_repo/examples/hello/hello-nier/nier" "$practice_project/nier"
```

No output and successful exits mean your files match the chosen solution.
If there is a difference, inspect it before continuing; editor backup files may also appear.
The missing configuration for the other approach is intentional, not an unfinished step.

## 4. Compile your artifact and run the native result

Both approaches now reach this same step.
Your chosen branch set `artifact` to its `.nier` output; keep using the same shell.
The `.nier` file is a standalone artifact, not a renamed executable and not a package of your private native objects.

Inspect it, compile it with the independent device compiler, and run the native output beside the artifact:

```bash
native_output="${artifact%.nier}"
"$nier_repo/build/prealpha/nierc" inspect "$artifact"
"$nier_repo/build/prealpha/nierc" "$artifact" -o "$native_output"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "./$native_output"
```

The final command should print `Hello world`.
Do not try to execute `hello.nier`: only the output of `nierc` is the native program.
Although this exercise runs both compilers on one machine, these are separate program invocations separated by a file.
The device compiler does not read the C sources or know that the artifact originated in C.

## 5. Repeat publication through your chosen approach

To republish, run only the command for your chosen approach.
For Make:

```bash
make -f nier/Makefile NIER_ROOT="$nier_repo"
```

Or, for CMake:

```bash
cmake --build build/nier-cmake --target publish
```

Then, for either approach, refresh and run the native executable:

```bash
"$nier_repo/build/prealpha/nierc" "$artifact" -o "$native_output"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "./$native_output"
```

The execution prints the message again, and the comparisons from step 3 remain empty.
These commands intentionally replace only outputs you created in this disposable project's `build/` directory.
The SDK adapter runs fresh private x86-64 and i686 builds for each request, checks their captured program representations, and invokes stock Clang's NieR publication path.
This can take longer than the initial native build; a failed publication preserves the previous successful artifact.
This is not yet an incremental publication cache.

Your ordinary build remains available too: `make hello` for the Make approach, or `cmake --build build/native-cmake --target hello` for CMake.
There is no need to make every compiler call emit NieR.
The adapter keeps configure probes and build-time generators native, then publishes the selected result; Hello is simply the smallest project exercising that integration.

The generated native executable needs no Clang, `nierc`, or `.nier` file to run, but it still depends on the chosen native loader and runtime paths.
Keep the development SDK/runtime in place.
This example neither installs dependencies on another machine nor proves that its compiled ELF can be moved anywhere.

SENieR (SEN), the planned security platform above NieR, is not implemented yet and is not a prerequisite for these commands.
This walkthrough demonstrates publication and native compilation, not signing enforcement, arbitrary CPU support, complete C coverage, or measured performance equivalence.

## Use the finished solution without doing the exercise

You can also start from the ready-made solution, which includes both configurations so you can choose either one.
In a new Bash shell at the NieR repository root, copy it to a new scratch directory:

```bash
nier_repo="$PWD"
source "$nier_repo/sdk/env.sh"
solution_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-hello-solution-XXXXXX")
mkdir -p "$solution_work/project/nier"
cp -a "$nier_repo/examples/hello/hello-nier/"{main.c,hello.c,hello.h,Makefile,CMakeLists.txt,.gitignore} \
  "$solution_work/project/"
cp -a "$nier_repo/examples/hello/hello-nier/nier/"{Makefile,CMakeLists.txt} \
  "$solution_work/project/nier/"
cd "$solution_work/project"
printf 'Solution copy: %s\n' "$solution_work/project"
```

As in the practice setup, copy only project files so old build caches cannot follow you into the new directory.

Choose Make:

```bash
make -f nier/Makefile NIER_ROOT="$nier_repo"
artifact=build/nier-make/hello.nier
```

Or choose CMake:

```bash
cmake -S nier -B build/nier-cmake -G Ninja -DNIER_ROOT="$nier_repo"
cmake --build build/nier-cmake --target publish
artifact=build/nier-cmake/hello.nier
```

Then use step 4 to compile and run your selected artifact.
You do not need to create either configuration file or execute the other approach.
The solution copy does not depend on anything you created in the practice directory.

If a command fails, check the first diagnostic rather than continuing with a stale output.
An absent selected output usually means a target/path mismatch; an SDK or plugin mismatch means the toolchain needs a matching build.
For more detailed troubleshooting and adapting the settings to your own application, continue with [Publish your C project with NieR](using-nier-with-your-c-project.md) and the [SDK integration reference](../../reference/build-integration.md).
