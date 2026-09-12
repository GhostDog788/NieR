# 15 — Capturing native programs

[Course index](../README.md) · [Previous: testing, debugging, and features](../03-contributors/14-testing-debugging-and-features.md) · [Next: recovering a common program](16-recovering-a-common-program.md)

## Objective and prerequisites

This chapter begins the full maintainer track.
Its objective is to explain what our C producer captures, why the capture point matters,
and how a private native build supplies evidence without becoming the published application.
You should understand the stock-Clang entry point, LLVM IR, and the difference between publication and native execution.

Remember the direction of dependency: NieR defines a public contract, and our LLVM merger is one producer of it.
Independent producers do not need Clang captures.
Everything in this chapter is machinery for **our reference producer**, not a requirement to identify a source language on the device.

## Native LLVM is an observation, not the original program text

Consider this small C example:

```c
unsigned pointer_bytes(void) { return sizeof(void *); }
unsigned literal_eight(void) { return 8; }
```

Clang targeting x86-64 may already represent both results as the integer 8 before the ordinary LLVM optimization pipeline begins.
Compiling for i686 distinguishes them: pointer size is 4, while the literal stays 8.

Capturing earlier in LLVM does not reverse frontend decisions that have already happened.
Preprocessing selected source branches, source constant evaluation, record layout, and native calling-convention lowering all affect the emitted module.
“Before optimization” does not mean “before the target matters.”

Our producer therefore captures two qualified native profiles. The captures are observations of the program under those profiles.
Later algorithms try to construct one common representation that specializes back correctly.
Two observations are not a proof for every architecture or a way to recover the unique original source expression.
The finite target domain is part of the resulting claim.

## The capture point

The LLVM pass plugin in `src/capture/Snapshot.cpp` registers a pipeline-start callback.
Its `Snapshot` pass writes the module as LLVM bitcode to the private `NIER_CAPTURE_PATH`.
It is marked as a required pass so the intended observation also occurs for the qualified O0 path.

The pass observes the module; stock Clang still continues producing the ordinary native object.
This distinction is especially important for existing builds. A configure probe must run as a native program.
Replacing its output with a NieR archive would break the build before publication selection even begins.

The requested optimization setting is not discarded.
An O0 capture can carry attributes such as `optnone`; an O2 invocation has different code-generation settings even though the snapshot precedes the main optimizer pipeline.
The tests check captures across O0, O1, O2, O3, Os, and Oz rather than assuming one hook behavior covers every option.

Do not confuse the production hook with an inspection command using `-Xclang -disable-llvm-passes`.
The latter is useful in targeted native experiments, but the production capture pass is designed to coexist with ordinary native compilation.
The native reference is not fabricated by disabling optimization everywhere.

## The direct source path

When a developer uses the supplied `nier.cfg`, stock Clang selects our publication frontend action.
That action does not translate a C AST into NieR.
Its `captureSource` helper copies a Clang invocation and asks unmodified Clang to compile for each qualified native profile.

The helper must distinguish project choices from target configuration.
It retains explicit project include paths, while rebuilding the configured SDK system-header search for the selected profile.
The x86-64 and i686 invocations must not accidentally include the same host-specific libc headers because some absolute host search path leaked into the command.

It also rejects explicit CPU, feature, or ABI tuning outside the qualified neutral producer contract.
Silently replacing `-march=native` with a baseline target would change the request.
Silently accepting it would create evidence outside the merger's stated profile assumptions.
A precise rejection is therefore the correct behavior until that configuration is supported.

Private full debug information is useful here.
Native lowering may turn a source aggregate argument into several scalar arguments or a hidden pointer; debug type and parameter records can propose the relationship.
Those records are hints to be checked against layouts and actual instructions, not authority to rewrite a program.
They are not copied into the public NieR module as a source-language dependency.

After both captures exist, the action calls the shared merger and packages the resulting common bytecode.
The stock driver invokes the internal publication linker for normal separate compilation and final publication.
The capture objects and private debug material do not become ordinary application payloads inside the `.nier` archive.

## Existing builds need stronger evidence

An existing Make or CMake build is not just a list of source files.
It may run generators, compile different files in different configurations, create archives, move objects, and link only some members of those archives.

The SDK integration therefore runs real native builds in separate private profile workspaces.
The capture observer records compile information and dependencies while native output remains usable by the build.
Later publication works from the selected native graph, not a guessed inventory of all `.c` files in a directory.

The following relationship is the important invariant:

```text
source/configuration/dependencies
              |
       native compilation
          /         \
 pristine capture   completed native object
          \         /
       private evidence binding
              |
      actual native selection
              |
         NieR publication
```

During integrated native capture, the snapshot is written **before** the private object-provenance marker is added.
The marker lives in a non-executable `.nier.capture` section of the native evidence object and refers to its private journal.
It lets later selection recover the evidence even when an object has been moved or archived.
Because it is added after the pristine snapshot, it is not ordinary program data in the captured publication input.

The journal is then finalized against the completed native output.
Capture digests, dependency records, and native-object identity bind the pieces together.
A stale path alone is not enough: a file at the same path can have changed since capture.

This is correctness provenance inside the publisher workflow.
Hashing a file does not prove who authored it or that a hostile compiler is trustworthy.
The optional signed security platform is a different axis, and is not implemented by these private journals.

## A worked generator case

The generated-input fixture includes a small native generator that writes a header containing `GENERATED_WIDTH`.
On the wide profile it writes 8; on the narrow profile it writes 4.

The correct sequence is to build and run the generator separately in each native lane,
then capture the application compiled with that lane's generated header.
Reusing the wide header in the narrow build would not merely make the captures look more similar.
It would capture a different configured program.

Likewise, SDK file-prefix mappings give private source/build roots stable virtual names before compilation, including deliberate `__FILE__` behavior.
The common merger must not repair different runtime strings by deleting or rewriting them after capture.
Observable strings are program data, even if they resemble paths.

The next stages prove whether the resulting application captures can share one common graph.
A generator succeeding on both targets does not guarantee that their application graphs are representable by the current merger.

## Failure cases and maintainer discipline

A changed header after native capture must invalidate the corresponding evidence rather than silently bless the old module.
A completed object whose provenance marker does not match its journal is not a substitute for the recorded output.
A selected object with no usable capture is not permission to hide its ordinary application logic in a native fallback payload.

There is a second class of failure at the compiler boundary.
`validateCapture` checks the Linux CPU profile, the pinned data layout, admitted CPU attributes, module flags, and unsupported module features.
Even a structurally valid LLVM module can violate these assumptions.
For example, an unsupported calling-convention environment cannot be repaired by relabeling the module's target triple.

Keep failures classified. A dependency mismatch is different from a type correspondence failure.
Preserve private diagnostics for investigation,
but do not confuse a developer's debugging convenience with permission to export their source and debug information in a publication.

## Optional independent lab

This lab observes two native compilations without publishing their objects.
Use Bash from the repository root with the SDK and capture plugin already built:

```sh
source sdk/env.sh
set -euo pipefail
guide15_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide15-XXXXXX")
for guide15_target in x86_64 i686; do
  NIER_BUILD_METADATA= \
  NIER_CAPTURE_PATH="$guide15_work/$guide15_target.bc" \
  clang --target="$guide15_target-unknown-linux-gnu" \
    --sysroot="$NIER_SDK_ROOT/sysroots/$guide15_target-linux-gnu" \
    -O0 -fPIC -g -fstandalone-debug \
    -fpass-plugin="$PWD/build/prealpha/nier-capture.so" \
    -c tests/fixtures/width.c -o "$guide15_work/$guide15_target.o"
  llvm-dis "$guide15_work/$guide15_target.bc" \
    -o "$guide15_work/$guide15_target.ll"
done
rg -n 'target triple|target datalayout|pointer_size|ret i(32|64) [48]' \
  "$guide15_work/x86_64.ll" "$guide15_work/i686.ll"
printf 'Private lab evidence: %s\n' "$guide15_work"
```

The captures are LLVM bitcode, not NieR bytecode. They intentionally contain private debug information.
Observe that the native-width function changes width and value. Do not distribute this directory as an application artifact.

## Recap and questions

The capture layer provides native observations with a precise configuration and build origin.
It cannot recover information the frontend already erased, and it does not establish a common program by itself.

> [!faq]- Why is preoptimization LLVM still target-specific?
>
> Frontend constant evaluation, preprocessing, layout and ABI lowering have already occurred.

> [!faq]- Why keep native outputs during a NieR build?
>
> Probes and generators must run, and actual native selection provides evidence for publication.

> [!faq]- Why add the private marker after writing the snapshot?
>
> The marker is build provenance, not application semantics to be merged and published.

> [!faq]- Can an independent producer omit all this machinery?
>
> Yes. It can emit valid NieR directly, provided it satisfies the public semantic and dependency contract.

## Guided source and evidence

Read `captureSource` and the capture observer actions (`src/publisher/ClangPlugin.cpp`), then the snapshot pass (`src/capture/Snapshot.cpp`).
Compare the capture test (`tests/capture.sh`), publisher options test (`tests/publisher.sh`), and generator fixture (`tests/fixtures/generated/generator.c`).
Build rejection tests (`tests/build-rejections.sh`) and retained-selection tests (`tests/retained-selection.sh`) teach why native evidence must remain bound to its actual inputs.
See [02's build integration contract](../../02-implementation-plan.md) for the current scope rather than assuming arbitrary build commands are supported.

[Next: recovering a common program](16-recovering-a-common-program.md)
