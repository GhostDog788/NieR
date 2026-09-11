# 09 — Widths, libraries, and real build systems

[Series](../README.md) · [Previous: Hello World end to end](08-hello-world-end-to-end.md) · [Next: Reading the repository](../03-contributors/10-reading-the-repository.md)

## Objective and prerequisites

This chapter explains why a successful Hello World is only the beginning.
You will distinguish fixed values from native-width values, understand why build-time generators must stay native, and see what static and shared libraries mean in the current toolchain.
You need C and basic Make/CMake experience, plus a built SDK and publisher in `build/prealpha` for the optional labs.
Each lab creates its own scratch workspace and can run independently.

If you want to publish your own project immediately, use the standalone [Make/CMake user guide](using-nier-with-your-c-project.md). This chapter explains the native semantics behind that workflow.

## Portability does not mean identical answers on every target

Consider two C expressions: `sizeof(void *)` and the integer literal `8`.
Both can produce eight on an x86-64 system. They do not mean the same thing.
The first asks about the selected target's pointer representation. The second spells a fixed value.
On i686 the pointer size becomes four; the literal does not.

Ordinary LLVM IR is already affected by target choices. A frontend may have turned `sizeof(void *)` into a numeric constant and `size_t` into a fixed-width integer type.
Simply removing the module's target name would not recover the distinction. The existing C producer uses two native profiles and supporting evidence to recover qualified common semantics.
The public representation can then express a native word as `!nier.word` and a pointer-size constant as `"pointer_bytes"`, while preserving genuinely fixed integers.

This is a major responsibility of the producer.
Two numerical examples alone are not a mathematical proof of arbitrary target behavior. The merger uses bounded structural and semantic rules, then checks each admitted native projection.
Its output declares the target domain those rules support.
There is no automatic inference that the same artifact works on ARM because ARM also has a familiar pointer width.

## Optional lab: inspect both width specializations

The existing width fixture prints pointer size, native word size, and fixed controls.
Run this in Bash from the repository root:

```bash
source sdk/env.sh
guide_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide09-width-XXXXXX")

clang --config="$PWD/build/prealpha/nier.cfg" -O2 \
  tests/fixtures/width.c -o "$guide_work/width.nier"
build/prealpha/nierc "$guide_work/width.nier" -o "$guide_work/width"
env -u LD_LIBRARY_PATH "$guide_work/width"

build/prealpha/nierc lower "$guide_work/width.nier" \
  --target x86_64 --output-dir "$guide_work/wide"
build/prealpha/nierc lower "$guide_work/width.nier" \
  --target i686 --output-dir "$guide_work/narrow"
opt -passes=verify -disable-output "$guide_work/wide/0.ll"
opt -passes=verify -disable-output "$guide_work/narrow/0.ll"
diff -u "$guide_work/narrow/0.ll" "$guide_work/wide/0.ll" || test "$?" -eq 1
printf 'Width workspace: %s\n' "$guide_work"
```

The x86-64 executable prints `pointer=8 word=8 fixed=4,8`. The diff is expected to report differences, hence the explicit acceptance of `diff` status 1.
Read the constants and function signatures rather than treating every textual change as a bug. The generated LLVM files have target-specific layouts because specialization has now happened.

This lab verifies the narrow LLVM file but does not deploy an i686 application. The normal native-output command is presently qualified only for x86-64.
Private test scripts can perform more elaborate 32-bit reference checks; those checks and a supported end-user target are separate claims.

## A build can execute programs before your application exists

A configure script may compile and run a probe to discover a library feature. A source generator may compile, run, and emit a header used by the application.
These are ordinary build programs, not parts of the final application's selected link graph.

Imagine a generator that writes a macro containing `sizeof(void *)`.
Running only the x86-64 generator and sharing its output with the i686 compilation would silently bake the wrong answer into the narrow build.
Preserving normal semantics requires the generated inputs to remain distinct when they really depend on the target.

This is why a blanket `CC="clang --config=nier.cfg"` is insufficient for such projects. The probe would receive an archive where it expects a runnable native executable.
The SDK's Make/CMake integration instead coordinates two private **native** builds with real stock Clang.
Configure probes and generators run in their proper lanes.
Selected application captures are published only after successful native build and provenance checks.

A lane is simply one private build tree and its target configuration. The term does not describe a runtime component.
The two lanes are publisher-side work and are not shipped inside the final artifact.

## Optional lab: publish an existing generated-header Make project

This uses the repository's existing fixture without editing its Makefile or application source. The external Make include is the small SDK integration.

```bash
source sdk/env.sh
guide_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide09-build-XXXXXX")

make -f "$PWD/sdk/share/nier/Nier.mk" \
  NIER_BUILD_TOOL="$PWD/build/prealpha/nier-build" \
  NIER_SOURCE_DIR="$PWD/tests/fixtures/generated" \
  NIER_TARGETS=hello NIER_NATIVE_OUTPUT=hello \
  NIER_ARTIFACT="$guide_work/generated.nier"

build/prealpha/nierc inspect "$guide_work/generated.nier"
build/prealpha/nierc "$guide_work/generated.nier" \
  -o "$guide_work/generated"
env -u LD_LIBRARY_PATH "$guide_work/generated"
tar -tf "$guide_work/generated.nier"
printf 'Generated-header workspace: %s\n' "$guide_work"
```

The native output prints `Generated width=8`. The final artifact contains the application module, not the generator executable.
The fixture's two private generators produce their respective widths; the integration test checks both build systems and the artifact inventory.

`NIER_BUILD_TOOL` names an internal coordinator used by the SDK integration. It is not a new public C source compiler.
The actual publication steps it orchestrates still use stock Clang's paired-input producer and publication linker.
For CMake, a small separate coordinator project calls `nier_add_publication(...)`; the application project can remain unchanged. The [integration guide](../../reference/build-integration.md) documents that interface and its argument rules.

The under-fifteen-minute integration target concerns developer configuration effort for qualifying projects with a working native build and installed SDK.
It excludes automated build duration. These fixtures demonstrate useful mechanisms, not a guarantee that every project's build assumptions fit them.

## Native libraries preserve different kinds of boundaries

A **static archive** is an ordered collection of native object members. A native linker normally extracts needed members rather than eagerly linking every member.
Two members may even have the same basename.
Preserving only a set of unique filenames would lose observable native behavior.

The SDK can select a static-library output and publish a static-kind Nier artifact.
`nierc` restores ordered native members with stock `llvm-ar`. This differs from publishing an executable that happened to use a static archive: that executable's selected graph contains the members the native link actually chose.
Selection and complete archive preservation are different tasks.

A **shared library**, or DSO, remains a separately loaded native component. Its SONAME identifies the dependency expected by other native outputs; a version script can control exported symbols and symbol versions.
The current publisher preserves qualified shared-link settings, and `nierc` can produce a native DSO from its own artifact.

Dependent applications declare native libraries.
In this prototype, separately publish and compile application libraries, then provide their native directory with `nierc --library-dir DIR`.
Missing declared libraries fail; arbitrary host libraries are not an automatic fallback. This is useful manual dependency provisioning, not the automatic package installer and updater required by the full product.

Dynamic loading is not prohibited merely because code was published through Nier. The standalone toolchain remains under ordinary OS rules.
A future security policy that constrains executable admission is a separate axis, not a hidden assumption behind these library examples.

## Know which boundary a test actually covers

The current suite exercises ordinary records by value, native callbacks, promoted scalar varargs, nonlocal jumps, overlapping storage, and nested packed and bitfield storage.
These phrases have deliberately different scopes.
Successfully loading fields through a pointer does not prove that the same record can be passed by value using every native calling convention.

Union, bitfield, and packed records **by value**, and aggregate extraction from `va_list`, remain open integration work.
Target-conditional control flow and unequal source inventories also have bounded proof rules. An explicit unsupported diagnostic is preferable to quietly changing the program, but it is not completion of a requirement that calls for the missing feature.

## Recap and check your understanding

Moving beyond Hello World is mainly about preserving distinctions:
native width versus fixed values, build tools versus application code, selected archive members versus complete static outputs, and storage layout versus calling convention.
The independent artifact is useful only if those native semantics survive the boundary.

<details>
<summary>Why must an integer literal 8 not become a pointer-size expression?</summary>

Equal values on one target do not imply equal meaning. The literal remains eight when a target's pointer size is four.

</details>

<details>
<summary>Why is a successful native configure probe not itself published?</summary>

It helped construct the build but is not necessarily in the selected application link graph.
Publishing every captured build program would include unrelated tools and could lose the intended output boundary.

</details>

<details>
<summary>Does packed-record storage coverage establish packed-record by-value ABI support?</summary>

No. Field memory access and native argument/return classification are separate obligations. The latter remains an explicit qualification gap here.

</details>

## Guided reading

Start with `tests/fixtures/width.c` and `tests/scalars.sh`.
Compare the generated fixture's `tests/fixtures/generated/Makefile` and `tests/fixtures/generated/CMakeLists.txt`,
then follow `tests/build-integration.sh` for generator and library assertions.
`tests/linking.sh` and `tests/packed-bitfield-storage.sh` illustrate why test names and assertions must state their exact boundary.
Consult [02's current status](../../02-implementation-plan.md) before claiming broader coverage from any one positive example.
