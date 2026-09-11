# 21. Build and link semantics

[Series](../README.md) · [Previous: Variadics and native idioms](20-variadics-and-native-idioms.md) · [Next: Artifact validation and robustness](22-artifact-validation-and-robustness.md)

## Objective and prerequisites

This chapter follows an existing native build into a NieR publication.
You should understand compilation versus linking, static archives, shared libraries, the capture/merger boundary, and native specialization.
The new maintainer question is not merely “which source files exist?” It is “which native inputs actually contributed to this selected output,
in what order, under which settings, and with which translation-unit boundaries?”

The result of a build is evidence about program selection.
A directory of C files cannot substitute for that evidence.
Configure probes, generated headers, archive extraction and platform-selected sources can all change the program that the ordinary native toolchain would produce.

## Inputs, outputs, and responsibilities

The SDK integration receives an unchanged source directory, a build system, selected native targets, a relative native output path, and a destination NieR artifact path.
`sdk/share/nier/Nier.mk` and `sdk/share/nier/Nier.cmake` are small coordination interfaces.
The public CMake helper is `nier_add_publication`.
The internal `nier-build` service runs the private builds and assembles publication commands; it is not a new C frontend or a replacement language compiler.

Both private lanes use actual stock Clang with the appropriate SDK profile.
Native preprocessing, configure probes and generators must still work as native operations.
Replacing the compiler with a wrapper which emits NieR whenever it sees C would break probes that need to execute their result.
The native capture observer therefore accompanies normal code generation; direct publication uses the distinct Clang publication action.

The selected output may be an executable, shared library, or static archive.
The portable result is an independent NieR artifact for that output.
Native objects and private LLVM captures remain build evidence, not alternate executable payloads hidden in the artifact.
When paired or grouped capture units are emitted and finally assembled, the coordinator still invokes stock Clang.
Its configured linker selects `nier-ld` for publication assembly.

## Bind capture evidence to actual object bytes

Knowing that `source.c` was compiled once is insufficient.
A build may copy an object, rename it, archive it, postprocess it, or overwrite its original path.
Publication must identify the object bytes that the native linker consumed.

The snapshot pass (`src/capture/Snapshot.cpp`) first writes pristine LLVM bitcode.
Only afterward does it insert a private, read-only `.nier.capture` section into the native object.
The section contains the absolute pathname of a unique immutable journal.
Because the snapshot precedes insertion, the marker is not present in the LLVM program being merged.

The journal records capture identity and digest, source/compile role, profile, effective flags and optimization, and dependency hashes.
A successful after-codegen observer records the finished native object's digest.
Private builds use stock Clang's `-fno-temp-file` so that the completed object is available when that observer runs.
A failed frontend or backend must not finalize successful native provenance.

Copying, moving and ordinary archiving preserve the marked object bytes. Changing them after code generation does not.
`src/cli/Build.cpp` checks the marker, immutable journal, pristine capture, dependencies and native digest before accepting the consumed object.
This detects stale captures and ordinary postprocessing mismatches.
It is not a cryptographic claim against an attacker who can rewrite the entire private workspace.

## Worked case: archives select a program, not a bag of files

Suppose an application refers to `first()` and `second()`, provided by two static archives.
The selected native link visits `libfirst.a` before `libsecond.a` on one profile, and the reverse order on another.
Both programs may be correct, yet the native object order differs.
The link-order fixture (`tests/fixtures/link-order/Makefile`) makes this observable while compiling the caller at O0 and the helpers at O2.

First, the internal native-link recorder delegates to ordinary LLD with trace and extraction evidence enabled.
It does not assume that every member of an archive was linked.
Lazy archive extraction selects members to resolve symbols; groups and whole-archive options change that selection.
Eagerly including all members can introduce duplicate definitions or unresolved references from code the native linker never selected.

Second, the recorder relates the trace to the final native ELF's capture-marker sequence.
Each selected application object contributes its retained marker.
SDK startup objects and managed native dependencies are handled separately.
The marker sequence provides an additional witness when two archive members share the same basename.
A textual `lib.a(member.o)` trace alone cannot identify which physical occurrence supplied the bytes.

Third, the selector validates every selected object's provenance.
Ordinary, thin, grouped and whole archives have tests, but none removes the need for exact correspondence.
If repeated identical journal identities leave a member ambiguous, publication rejects instead of guessing from names or symbol-table convenience.

Fourth, the two profile selections are paired.
Positional correspondence is the simple case.
When order differs, a candidate pairing must be unique and match source or normalized compile role, effective flags, optimization and member identity.
These checks propose correspondence; the LLVM merger must still prove the actual programs match the common contract.

Finally, the publication preserves each profile's observed physical unit order.
Moving a whole paired unit also moves its optimization settings.
The caller must not become O2 merely because its libraries were O2.
The internal linker permutation is validated for exact count, range and uniqueness; it is not a general invitation to reorder executable bodies.

This is not just a synthetic concern.
The qualified zlib `minigzip` links selected the same native units in different orders across profiles.
The permutation path preserves that fact without making all translation units use one flag set or treating the entire program as one LLVM module.

## Static output is different from static input

Selecting a static archive as the *output* means publishing every physical member, in order, including duplicate basenames.
There is no application link yet to decide which members will later be extracted.
NieR's static compilation plan therefore associates each native unit with its archive-member identity.

The consumer stages identically named members in separate ordinal directories, compiles them independently,
and uses ordinary `llvm-ar` quick append followed by indexing.
A replace-by-name operation would lose earlier duplicates.
An empty static archive is admitted as an empty inventory; an empty executable does not become valid by analogy.

The static tests use competing definitions whose order affects selection, and an unused member with an unresolved reference.
A test which merely checks that `ar t` prints two names cannot catch accidental eager linking.
The behavioral test checks the archive's future native linking semantics as well as its container shape.
General profile-dependent static member inventories remain a separate boundary rather than being silently renamed or flattened.

## Unequal translation-unit inventories without LTO

Sometimes one profile builds `main.c`, `first.c`, and `second.c`, while another builds `main.c` and `joined.c`.
The common program can contain the same functions even though it has three native optimization units on one target and two on the other.

`src/ir/Partitions.cpp` handles a bounded version of this case.
It establishes unique external definition ownership, intersects the two profiles' ownership partitions, and creates shared fragments.
It does not store two copies of every target's program.
It proves that the appropriate private fragments reconstruct each original native translation unit before running the ordinary paired merger for each fragment.

The artifact's `compilation_units` plan then says which shared fragment indices belong to each physical unit on each target.
Every fragment must occur exactly once per target.
The consumer's `src/ir/CompilationUnits.cpp` reconstructs the declared unit before its normal optimization pipeline.
LLVM linking here is part of reconstructing the original unit boundary, not permission to apply whole-program link-time optimization.

The initial split proof requires matching effective settings and self-contained external scalar definitions.
Cross-fragment function references, private/global identities and other unproved ownership cases reject.
Why? A file-local static object has identity, not merely a convenient name.
Splitting or joining it can change which functions share the object.
Likewise, combining native units and letting LLVM rename collisions is not a proof of original source semantics.

The source-inventory test (`tests/source-inventory.sh`) checks both the three-versus-two positive case and unsupported identity cases.
An expected rejection is useful robustness evidence, but it does not complete the broader source-inventory product requirement.

## Shared libraries and publication transactions

Native shared linking contributes more than a filename ending in `.so`.
SONAME, imports, exports, visibility, versioned symbols, PIC and actual `DT_NEEDED` dependencies affect loader behavior.
The recorder preserves the qualified contracts; private build-directory paths do not become portable dependency identities.
Exact SONAME dependencies use declared names, and the consumer receives an explicit native fixture library directory when resolving independently compiled DSOs.

Constructor-only dependencies are a useful trap.
A library may matter despite contributing no referenced symbol.
Once native linking has resolved its actual dependency list, the destination must not apply a different `as-needed` decision and silently drop it.
`tests/linking.sh` exercises this along with versioned exports and failed-link preservation.
Unsupported meaningful linker options must reject; ignoring them can change the product.

The coordinator also stages its final Clang output privately.
Stock Clang may delete its `-o` path when a link fails.
Pointing that cleanup at a user's previous valid NieR artifact would defeat an atomic writer inside `nier-ld`.
Only a successful, validated staged artifact replaces the requested publication.

## Optional independent lab

This Bash lab publishes the existing order-sensitive Make fixture using the SDK helper.
Run it from the repository root with the prepared build:

```bash
source sdk/env.sh
build_lab=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide-build-XXXXXX")
make -f sdk/share/nier/Nier.mk \
  NIER_BUILD_TOOL="$PWD/build/prealpha/nier-build" \
  NIER_SOURCE_DIR="$PWD/tests/fixtures/link-order" \
  NIER_NATIVE_OUTPUT=hello NIER_TARGETS=hello \
  NIER_ARTIFACT="$build_lab/order.nier"
build/prealpha/nierc lower "$build_lab/order.nier" \
  --target x86_64 --output-dir "$build_lab/wide"
build/prealpha/nierc lower "$build_lab/order.nier" \
  --target i686 --output-dir "$build_lab/narrow"
build/prealpha/nierc "$build_lab/order.nier" -o "$build_lab/order"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$build_lab/order"
printf 'Lab files: %s\n' "$build_lab"
```

Compare which helper is defined in `1.ll` and `2.ll` for each target.
The program's successful exit and the preserved O0 caller/O2 helpers are different observations.
Keep both when diagnosing an ordering regression.

## Recap and questions

The native build is an oracle for selected inputs, not a source-file scanner.
NieR must preserve selection, identity, physical order, per-unit settings and native dependencies while publishing only the common program.

1. **Why not merge every archive member?** Native lazy extraction may exclude members whose inclusion changes symbols, errors or behavior.
2. **Why is a marker not a signature?** It binds ordinary build evidence, but a workspace writer can rewrite that evidence; no trusted signing boundary has been established.
3. **Why is reconstructing a declared TU not implicit LTO?** The reconstruction restores the original optimization unit; optimization happens afterward, separately for each unit.

Read `src/cli/Build.cpp` for selection and matching, then `src/publisher/BuildMain.cpp` for actual Clang publication.
The best companion tests are `tests/build-integration.sh`, `tests/build-rejections.sh`, `tests/link-order.sh`, and `tests/retained-selection.sh`.

[Previous: Variadics and native idioms](20-variadics-and-native-idioms.md) · [Next: Artifact validation and robustness](22-artifact-validation-and-robustness.md)
