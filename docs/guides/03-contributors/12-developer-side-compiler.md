# 12 — The developer-side compiler

[Series](../README.md) · [Previous: Implementing the Nier contract](11-implementing-the-nier-contract.md) · [Next: Following nierc](13-following-nierc.md)

## Objective and prerequisites

This chapter follows the reference C producer closely enough that you can locate
a publication failure and understand what a correct fix must preserve. You
should understand translation units, native targets, Nier modules, and the
basic C++ reading conventions from earlier chapters. The optional lab needs
the pinned SDK and existing `build/prealpha` publisher tools.

The key boundary is unchanged: the developer invokes **stock Clang** to publish
C. `nierc` is a separate, language-blind consumer. The two native LLVM profiles
described here are private inputs to this reference producer, not requirements
imposed on every future producer of Nier code.

## Why the Clang executable stays stock

Clang's command-line driver coordinates frontend and linker jobs. Its frontend
parses C, performs source-language checks, and lowers the program to LLVM IR.
Plugins extend supported points in that process without changing the installed
Clang executable.

The generated `nier.cfg` selects target development paths, loads
`libnier-clang.so`, explicitly selects the `nier` frontend action, and points
the link stage to `nier-ld`. The `-Xclang` spelling forwards an argument through
the driver to its frontend. The configuration is generated with paths to the
matching build and SDK; it is not a universal file to copy between unrelated
installations.

When invoked with `-c`, this selected action writes an object-kind Nier archive.
Without `-c`, the driver passes the produced units to the publication linker,
which creates the requested complete artifact. The usual developer-facing
compile/link sequence is preserved, but the intermediate output format changes.
`nier-ld` packages and validates Nier inputs; it is not the native LLD backend
used later by the destination compiler.

This distinction avoids a misleading shortcut: renaming a custom wrapper
`clang` would not satisfy the chosen developer interface. The implementation
uses actual Clang extension interfaces, and the source compilation inside the
producer is also performed by stock Clang.

## Direct source mode: one translation unit, two private captures

In `ClangPlugin.cpp`, `NierAction` handles the selected publication action.
The default mode accepts ordinary C source. It does not implement its own
C-to-Nier compiler by walking Clang's abstract syntax tree. Instead,
`captureSource` clones the relevant compiler invocation, configures each native
profile, and delegates real source compilation to stock Clang's `-cc1` frontend.

`-cc1` names Clang's internal frontend entry point. It is useful implementation
plumbing here, not the interface an application developer should assemble by
hand. The adapter removes its own plugin action from the delegated invocation
so the child performs native compilation rather than recursively republishing.

Each child uses the matching target headers and sysroot. Project include paths,
defines, and qualified semantic flags must survive. Native CPU/feature/ABI
tuning that the neutral source producer does not qualify is rejected, rather
than silently cleared and forgotten. The same principle applies to unknown
LLVM pass plugins and unqualified host include overrides.

The child loads `nier-capture`, the small LLVM pass plugin in
`src/capture/Snapshot.cpp`. Its pipeline-start callback writes the LLVM module
before the main optimization pipeline. Native frontend decisions have already
happened: pointer widths, layouts, ABI lowering, preprocessing, and some
constant folding are already present. This is not a magical pre-target IR hook.

Requested optimization settings still matter. Frontend decisions and attributes
can differ between `-O0` and `-O2`, even though the snapshot precedes the main
optimizer. The producer records the unit's requested optimization, and the
consumer applies the qualified native optimization pipeline after target
specialization. Capturing everything as `-O0` and deleting inconvenient
attributes would not faithfully preserve the user's build.

The snapshot may include private debug evidence used to recover record layouts
and native-width intent. That information is publisher-only. The final artifact
is not a tar file containing these two native LLVM modules.

## Shared merging and the inverse check

`nier::mergeProfiles` in `src/ir/Producer.cpp` receives the two captures. It
checks their expected target contracts and LLVM validity, applies narrowly
proved normalizations, and constructs a common Nier graph.

**Normalization** means expressing equivalent behavior in a consistent form
that can be compared. For example, the two targets may expand a byte reversal
into different mask/shift/or sequences. The byte-swap helper proves a supported
idiom before replacing it with a common primitive. It does not recognize a
function by its source name and replace its body on trust. Volatile accesses,
escaping storage, side effects, and unsupported shapes must not disappear.

Other helpers address qualified native aggregate boundaries and scalar varargs.
These are difficult because native lowering can change both instruction shapes
and control flow. A storage pointer in the common body does not authorize a
boxed runtime calling convention: the target lowering must recover the ordinary
native entry, call, and return ABI inside the original functions.

After merging, the producer specializes the common graph back to each native
profile. It compares reconstructed and captured semantics after the same
justified normalizations. This **inverse check** asks whether the proposed
portable representation reconstructs what the native frontend actually meant.
It is a bounded sufficient check, not a general theorem prover for arbitrary
program equivalence.

If correspondence cannot be proved, publication fails. The producer does not
fall back to packing both original programs, invent source-language semantics,
or insert a native binary payload automatically. If the rejected feature is a
required milestone item, that diagnostic identifies unfinished work; it does
not satisfy the milestone.

## Optional lab: look at the private side without distributing it

This lab keeps one direct-publication workspace deliberately. Everything is
private diagnostic material. Run it from the repository root in Bash:

```bash
source sdk/env.sh
guide_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide12-XXXXXX")
guide_config="$PWD/build/prealpha/nier.cfg"

clang --config="$guide_config" -O2 \
  -Xclang -plugin-arg-nier -Xclang keep-work \
  -c tests/fixtures/width.c -o "$guide_work/width.o" \
  2> "$guide_work/publisher.log"

rg 'Private Nier producer workspace:' "$guide_work/publisher.log"
guide_private=$(sed -n 's/^Private Nier producer workspace: //p' \
  "$guide_work/publisher.log")
test -d "$guide_private"
llvm-dis "$guide_private/x86_64.bc" -o "$guide_work/wide-capture.ll"
llvm-dis "$guide_private/i686.bc" -o "$guide_work/narrow-capture.ll"
rg -n 'target (triple|datalayout)|DICompileUnit|DILocalVariable' \
  "$guide_work/wide-capture.ll" "$guide_work/narrow-capture.ll"

clang --config="$guide_config" "$guide_work/width.o" \
  -o "$guide_work/width.nier"
build/prealpha/nierc inspect "$guide_work/width.nier"
tar -tf "$guide_work/width.nier"
build/prealpha/nierc "$guide_work/width.nier" -o "$guide_work/width"
env -u LD_LIBRARY_PATH "$guide_work/width"
printf 'Public output and diagnostic copies: %s\n' "$guide_work"
printf 'Additional retained private workspace: %s\n' "$guide_private"
```

The capture files expose different target layouts and private debug information.
The final artifact listing contains the manifest and common Nier bytecode,
not those capture paths. Keep both printed directories private. They are useful
for diagnosis but are not suitable files to attach indiscriminately to a public
bug report.

The lab uses `keep-work`, a current pre-alpha plugin diagnostic option. Its
existence does not create a stable capture interchange format. An independent
producer should use the public Nier API, not depend on this workspace layout.

## Existing-build mode: observe native work before publishing it

Direct mode can compile the C files the developer names. It cannot reproduce an
arbitrary project's configure probes, generators, archive extraction, or
profile-selected source list merely by inspecting that command. The SDK
integration therefore runs the project's normal native build twice privately.

Here the plugin is loaded as an **observer**, not selected as the main Nier
publication action. Native compilation must continue to produce real objects
and executables. The before-main observer collects dependencies and creates a
capture journal. The snapshot writes pristine LLVM first, then adds a readonly,
non-executable `.nier.capture` reference to the private native object.

An after-main observer hashes the completed native object. The private SDK uses
stock Clang's `-fno-temp-file` so that completed output is available at its
requested path at this point. Failed compiles do not finalize valid provenance.
The journal and marker connect an object to the capture that produced it even
if a Makefile later moves the object or puts it in an archive.

**Provenance** here means evidence of build correspondence: source and dependency
identity, capture bytes, original object bytes, settings, and selected link
membership. It is not signing. An attacker who can rewrite the private evidence
workspace is outside this mechanism's trust model.

`Build.cpp` requires successful native builds and verifies selected objects,
archive members, and final-link evidence. A marker alone is insufficient.
Object postprocessing that changes bytes while retaining a marker is rejected.
Unused generators and unselected archive members do not become application
payload just because their compilation was observed.

The internal coordinator then invokes actual stock Clang with `mode=pair` and
LLVM input, or a bounded `mode=group` for qualified unequal translation-unit
partitions. Both paths use the shared LLVM-to-Nier producer. The destination
does not receive these private modes or learn which source language generated
the artifact.

## Preserve units and failure semantics

Some projects choose different source files or archive orders by target. The
current producer admits proved cases and emits explicit ordered compilation-unit
plans. Shared fragments are restored to each target's original units before
per-unit optimization. This is not automatic link-time optimization across the
whole program. Ambiguous membership, unsupported graph changes, and unproved
repartitioning remain errors.

The SDK coordinator also stages the final Clang output privately, validates it,
then replaces the requested artifact after success. That preserves an existing
valid artifact after a failed SDK rebuild. Direct stock-Clang commands retain
the driver's own failed-output cleanup, which a frontend plugin cannot override.
Do not use input files or valuable existing files as experimental output paths.

None of these checks impose a closed application security policy. The producer
must preserve qualified native semantics, including ordinary dynamic loading;
future executable-memory enforcement and admission rules belong to the separate
security platform.

## Recap and check your understanding

The C producer adapts stock Clang, observes real native semantics, and proves
a bounded shared representation. Direct source mode and private build mode
share a merger but have different responsibilities for collecting inputs.
The resulting public contract—not a source language or capture recipe—is what
the independent destination compiler consumes.

<details>
<summary>Does the capture pass run before Clang knows the target ABI?</summary>

No. It runs on LLVM IR after frontend semantic and ABI lowering, before the
main LLVM optimization pipeline. Recovering qualified portable meaning is the
producer's responsibility.

</details>

<details>
<summary>Why not publish every capture produced during a Make build?</summary>

Many captures belong to generators, probes, or unselected library members.
The native output's selected graph and validated provenance determine which
application units belong in the publication.

</details>

<details>
<summary>Does an inverse-check failure justify shipping both native LLVM profiles?</summary>

No. Ordinary application code must use the common Nier representation. A failed
proof is a diagnostic and possibly unfinished required scope, not permission
to create a disguised collection of per-target programs.

</details>

## Guided reading

Follow `NierAction::emit` and `captureSource` in
[ClangPlugin.cpp](../../../src/publisher/ClangPlugin.cpp), then the small
[Snapshot.cpp](../../../src/capture/Snapshot.cpp). Read `mergeProfiles` near the
end of [Producer.cpp](../../../src/ir/Producer.cpp) before exploring its individual
normalizers. [BuildMain.cpp](../../../src/publisher/BuildMain.cpp) shows the actual
paired/grouped Clang commands and final staging;
[Build.cpp](../../../src/cli/Build.cpp) owns native build evidence and selection.
[publisher.sh](../../../tests/publisher.sh),
[capture.sh](../../../tests/capture.sh), and
[build-rejections.sh](../../../tests/build-rejections.sh) are the most useful
first regression tests. The next chapter follows the other side of the boundary:
the independent `nierc` consumer.
