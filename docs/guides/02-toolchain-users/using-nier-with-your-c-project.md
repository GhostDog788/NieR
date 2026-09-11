# Publish your C project with Nier

[Guide series](../README.md) · [SDK integration reference](../../../sdk/share/nier/README.md)

You have a normal C project that already builds with Clang and Make or CMake.
You want one `.nier` publication, then a native executable compiled from that
artifact. This guide is the practical route; no compiler background or previous
course chapters are required.

For a qualifying simple project, your application source and existing build
files do **not** change. You tell the external SDK integration where the project
lives, which target to build, and which native output represents the application.
The integration invokes stock Clang to produce Nier. You then invoke the separate
`nierc` program to produce the native executable.

## Prerequisites

This is the current pre-alpha development checkout, not an already released
system-wide SDK. The tested development baseline is x86-64 Ubuntu 24.04. You
need the bootstrapped SDK and a completed publisher build in `build/prealpha`;
follow the repository's [Build instructions](../../../README.md#build) if those are
missing. The examples assume the standard repository-local `.sdk` layout.

Run the blocks below in **Bash from the Nier repository root**. Each block loads
the SDK environment and creates a new scratch directory, so you can try either
build system independently. The defaults use an existing two-file sample
project. Replace the clearly marked application settings to use your project.
Keep the printed output directory if you want to rerun its native executable.

Your project should already have a successful ordinary native build. These
recipes make a private baseline copy to check that first; they do not run
`make clean` or configure inside your original application directory.

## Make: one external command, no Makefile edits

Choose these three values:

| Setting | Meaning | Example |
| --- | --- | --- |
| `app_source` | Absolute directory containing the existing Make project | `/home/me/code/myapp` |
| `app_target` | Target you normally give Make | `hello` |
| `app_native_output` | File produced, relative to that project's build/source directory | `hello` or `bin/hello` |

The target and output are not always the same: `make all` might create
`bin/hello`. Nier needs the actual selected output, not just the target name.

Copy and run this block unchanged for the included sample, or replace its
three application settings:

```bash
source sdk/env.sh
nier_repo="$PWD"
make_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-user-make-XXXXXX")
app_source="$nier_repo/tests/fixtures/build"  # Replace with your absolute path.
app_target=hello                           # Replace with your Make target.
app_native_output=hello                    # Replace with its relative output.

# First check the ordinary native build in a copy.
mkdir "$make_work/native-source"
cp -a "$app_source/." "$make_work/native-source/"
make -B -C "$make_work/native-source" \
  CC="$NIER_LLVM_ROOT/bin/clang" "$app_target"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$make_work/native-source/$app_native_output"

# Publish through the external SDK Makefile.
make -f "$nier_repo/sdk/share/nier/Nier.mk" \
  NIER_BUILD_TOOL="$nier_repo/build/prealpha/nier-build" \
  NIER_SOURCE_DIR="$app_source" \
  NIER_TARGETS="$app_target" \
  NIER_NATIVE_OUTPUT="$app_native_output" \
  NIER_ARTIFACT="$make_work/application.nier"

# Compile the standalone artifact to a normal native executable.
"$nier_repo/build/prealpha/nierc" inspect "$make_work/application.nier"
"$nier_repo/build/prealpha/nierc" "$make_work/application.nier" \
  -o "$make_work/application"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$make_work/application"
printf 'Make publication and executable: %s\n' "$make_work"
```

The sample prints `Hello from a normal build: 8`. Your application prints its
own output; supply its usual runtime arguments where appropriate. `make -B`
forces the copied baseline to rebuild instead of trusting old copied objects.
If any step fails, resolve that diagnostic before continuing to compilation or
execution.

The samples need no runtime files and run from the Nier repository root. For
your own application, run both the baseline and Nier-built executable from the
working directory it expects, with its usual data files and configuration.
Use the absolute executable paths shown above after changing directory; return
to the Nier repository root before starting another recipe.

`Nier.mk` is an external Makefile, not a replacement for your project's Makefile.
It privately runs your normal project. `NIER_BUILD_TOOL` locates its internal
coordinator in this development checkout; it is not a new public C compiler.
The actual Nier-emitting compiler remains stock Clang with the Nier plugin.

If your native project needs a configure script, run its normal configure step
in the baseline copy before Make. The SDK adapter runs an existing `configure`
script in its private builds; use `NIER_CONFIGURE_ARGS` for qualified configure
arguments and `NIER_CFLAGS` for requested compiler flags. See the
[argument rules](../../../sdk/share/nier/README.md) for details.

## CMake: a small external publication coordinator

CMake uses a separate, tiny coordinator project that includes the SDK module.
Your application's existing `CMakeLists.txt` stays unchanged. The coordinator
looks like this; values are supplied during configuration:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyPublication NONE)
include("${NIER_INTEGRATION_FILE}")
nier_add_publication(published
  SOURCE_DIR "${APPLICATION_SOURCE}"
  NATIVE_OUTPUT "${APPLICATION_NATIVE_OUTPUT}"
  OUTPUT "${APPLICATION_ARTIFACT}"
  BUILD_TOOL "${NIER_BUILD_TOOL}"
  TARGETS "${APPLICATION_TARGET}")
```

For the first run, the commands below use the repository's
[ready-made coordinator](../../../tests/fixtures/sdk-integration/CMakeLists.txt).
It is ordinary CMake glue, despite living with the test fixtures; it accepts
your application directory as input. For a permanent project setup, copy this
small pattern into a separate publication directory you own, then point the
second configure command's `-S` there. Do not replace your application's
`CMakeLists.txt` with the coordinator.

```bash
source sdk/env.sh
nier_repo="$PWD"
cmake_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-user-cmake-XXXXXX")
app_source="$nier_repo/tests/fixtures/build"  # Replace with your absolute path.
app_target=hello                           # Replace with your CMake target.
app_native_output=hello                    # Relative to its private build dir.

# Check an ordinary native build in a source copy and fresh binary directory.
mkdir "$cmake_work/native-source"
cp -a "$app_source/." "$cmake_work/native-source/"
cmake -S "$cmake_work/native-source" -B "$cmake_work/native-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$NIER_LLVM_ROOT/bin/clang"
cmake --build "$cmake_work/native-build" --target "$app_target"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$cmake_work/native-build/$app_native_output"

# Configure the external coordinator, not the application's CMake project.
cmake -S "$nier_repo/tests/fixtures/sdk-integration" \
  -B "$cmake_work/publication-build" -G Ninja \
  -DNIER_INTEGRATION_FILE="$nier_repo/sdk/share/nier/Nier.cmake" \
  -DNIER_BUILD_TOOL="$nier_repo/build/prealpha/nier-build" \
  -DAPPLICATION_SOURCE="$app_source" \
  -DAPPLICATION_TARGET="$app_target" \
  -DAPPLICATION_NATIVE_OUTPUT="$app_native_output" \
  -DAPPLICATION_ARTIFACT="$cmake_work/application.nier"
cmake --build "$cmake_work/publication-build" --target published

"$nier_repo/build/prealpha/nierc" inspect "$cmake_work/application.nier"
"$nier_repo/build/prealpha/nierc" "$cmake_work/application.nier" \
  -o "$cmake_work/application"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$cmake_work/application"
printf 'CMake publication and executable: %s\n' "$cmake_work"
```

Here `published` is the coordinator's target, while `app_target` is your
application's native target. The output path is relative to the application's
private **binary directory**, not the coordinator directory. Add application
options such as `CONFIGURE_ARGS "-DENABLE_FEATURE=ON"` to `nier_add_publication`
when needed. Do not override its profile compilers, sysroots, or build tools.
Sourcing `sdk/env.sh` selects the matching CMake and Ninja; an application does
not need to adopt Nier's own development preset.

## Why the adapter instead of globally changing CC?

Some normal builds compile and immediately execute probes or generators. A
`.nier` artifact is not a native executable, so globally setting every compiler
invocation to direct Nier mode would break them. The adapter runs two private
native builds, preserving those programs and target-generated headers, then
publishes the selected application. Private source copies, LLVM captures, and
build records are not required by `nierc` and must not be distributed as the
application artifact.

For a small program with no build-time programs, direct stock Clang is also
available. This independent example uses the existing Hello World sources:

```bash
source sdk/env.sh
direct_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-user-direct-XXXXXX")
clang --config="$PWD/build/prealpha/nier.cfg" -O2 \
  examples/hello/main.c examples/hello/hello.c -o "$direct_work/hello.nier"
build/prealpha/nierc "$direct_work/hello.nier" -o "$direct_work/hello"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$direct_work/hello"
printf 'Direct publication: %s\n' "$direct_work"
```

## Rebuilds, runtime, and common problems

Repeat the adapter's publication command, or rebuild the CMake coordinator
target, to republish. The SDK runs fresh private builds; a failed rebuild
preserves the prior valid artifact. Run `nierc` again to update native output.
Direct Clang retains its own failure cleanup, so always use distinct input and
output paths and avoid valuable existing output files for experiments.

The native executable runs without Clang, `nierc`, or its `.nier` file. It still
needs the supplied glibc/loader and declared native libraries at the paths chosen
during compilation. Keep the SDK/runtime directory in place. Clearing the
development shell's `LD_LIBRARY_PATH` prevents tool libraries from overriding
the application's normal native loading. This prototype is not an automatic
dependency installer or a guarantee that a compiled ELF relocates everywhere.

- **Selected output not found:** check the native target and relative output
  separately. For CMake, look under the native binary directory.
- **Cannot execute `.nier`:** compile it with `nierc`; execute the resulting
  native file, not the archive.
- **Missing tools, plugin mismatch, or SDK receipt error:** use one current SDK
  and matching Nier build; regenerate/rebuild after pre-alpha contract changes.
- **Flags with spaces:** the Make adapter splits target/configure/flag lists on
  whitespace. Use the CMake adapter's quoted argument lists for complex values.
- **Configure compiler path breaks:** traditional scripts that expand `$CC`
  without shell evaluation require whitespace-free SDK and scratch paths.
  Application source and artifact paths can still contain spaces.
- **Unsupported construct or build operation:** preserve the diagnostic and
  minimize a private reproducer. Current C coverage is bounded; do not silence
  a failed proof or assume every ordinary C ABI construct already works.

The current native-output target is x86-64. These examples do not establish
other CPU deployments, full performance parity, or reverse-engineering
resistance. Security enforcement is separate and is not a prerequisite for
this publication workflow. See [current scope](../../02-implementation-plan.md)
and [the SDK reference](../../../sdk/share/nier/README.md) before expanding beyond
the simple project demonstrated here.
