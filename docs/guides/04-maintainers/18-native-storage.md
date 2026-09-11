# 18 — Native storage

[Course index](../README.md) · [Previous: control-flow sharing and specialization](17-control-flow-sharing-and-specialization.md) · [Next: native calling conventions](19-native-calling-conventions.md)

## Objective and prerequisites

The objective is to understand how Nier preserves native memory layout without publishing one target-fixed LLVM record for every target.
You should understand the common type contract, pointer arithmetic, and the producer/core separation.
You should also distinguish fixed-width integer operations from target-native properties.

This chapter is about **storage**: objects, offsets, initialization, and memory accesses.
Passing an aggregate by value is a different problem.
Supporting a union or packed record in memory does not establish its native calling convention; the next chapter explains that boundary.

## A record is more than the sum of its fields

The existing storage fixture contains this ordinary C shape:

```c
struct State {
    char marker;
    size_t count;
    int (*callback)(int);
    int values[3];
};
```

For the currently pinned x86-64 and i686 Linux layouts, the native storage is:

| Property | x86-64 | i686 |
|---|---:|---:|
| `marker` offset | 0 | 0 |
| `count` offset | 8 | 4 |
| `callback` offset | 16 | 8 |
| `values` offset | 24 | 12 |
| Whole-object size | 40 | 24 |
| Whole-object ABI alignment | 8 | 4 |

The padding before `count` and after the final array follows native layout rules.
Nier must not reuse the wide byte offsets on the narrow target, and it must not pack the fields tightly to make both targets look the same.

A common storage type can instead retain the ordered field structure.
This is an example of actual Nier type syntax, with an illustrative opaque identity:

```mlir
!nier.record<"r0", 0, [i8, !nier.word, !nier.ptr, !nier.array<3, i32>]>
```

The `0` indicates nonpacked LLVM-style record storage. The integer array is fixed-width and fixed-length.
The native word and pointer become concrete under target lowering.
`r0` distinguishes an identified storage type without preserving the C source type name.

This representation does not say every C `long` on every 64-bit platform is 64 bits.
It describes a proved correspondence in the current profile domain. Other ABIs require their own qualified native-property rules.

## Pairing storage types

The producer's `type` routine pairs native types recursively.
Equal admitted integer widths remain fixed; `i64` paired with `i32` can become `!nier.word`.
Native address-space-zero pointers become `!nier.ptr`. Arrays pair their element types and preserve each target's extent.
Ordinary records require compatible field inventories and storage properties.

For identified records, the producer keeps a one-to-one correspondence map.
It cannot pair one wide record with two different narrow records depending on which use is currently convenient.
Reusing a record identity with conflicting fields would make later GEPs and initializers ambiguous, so both producer and consumer guard the identity contract.

The consumer creates the corresponding LLVM storage type and lets the selected LLVM data layout determine native size and alignment.
It does not consult C headers or debug records. Those are private producer evidence, not part of the language-blind layout algorithm.

Some record shapes are not covered by the ordinary pairing rule.
Opaque types, inconsistent field inventories, or conditional-only aggregate layouts without sufficient shared evidence fail.
Similar total byte sizes are not enough to prove a valid type correspondence.

## GEP preserves a path through storage

LLVM's `getelementptr`, usually called GEP, calculates an address using a source element type and indices.
It does not itself load memory. In the State example, a path to `values[1]` first selects the record field and then the array element.
The same structural path can produce different byte offsets under different native layouts.

Nier retains the relevant element type, indices and native semantics instead of reducing every address to an unexplained integer offset.
The consumer checks that the index path is valid for the reconstructed type before emitting the native GEP.
Constant address expressions in global initializers have a corresponding checked representation.

An `inbounds` GEP is not a runtime bounds check.
It carries a promise about the address calculation; violating LLVM's relevant conditions can yield poison.
As introduced in chapter 14, poison is not an ordinary value to be substituted freely.
Preserving or adding `inbounds` therefore needs semantic justification. Nier does not make arbitrary C pointer arithmetic memory-safe.

## Alignment belongs to the access too

The type's natural alignment and a particular memory access's guaranteed alignment are related but different.
A packed subobject can be accessed with less alignment than a naturally aligned standalone integer of the same type.
Conversely, an explicitly stronger access alignment is a promise that cannot be silently weakened or invented when comparing native contracts.

Nier load, store, and allocation operations carry admitted alignment expressions.
The selected value must be a nonzero, bounded power of two. Volatile accesses retain their observable-access character.
The producer compares corresponding access properties, and lowering emits the selected native operations.

The packed-bitfield fixture deliberately mutates a packed object through a pointer to the packed record.
It does not manufacture a misaligned ordinary `unsigned *` and demand that undefined behavior match.
A good differential test must exercise defined or appropriately compared implementation-defined behavior, not use source UB as a native-fidelity requirement.

## Globals and initializers are executable-program data

The State fixture also has a nonzero static initializer and a stored function pointer.
A successful publication must preserve more than field layout:
the callback symbol must resolve, the array values must survive, and mutable storage must remain mutable.

The initializer representation supports admitted scalar values, aggregate elements, symbol addresses, and checked constant address paths.
The consumer checks an aggregate's element count against its reconstructed type.
A symbol reference must resolve to a declared common symbol; a private source name or arbitrary native address is not sufficient.

Native zero, undefined, and poison values are distinct forms where admitted.
An uninitialized or padding region is not automatically a request to fill bytes with zero.
Inventing padding initialization can alter observable object copies or other native behavior.
The common representation must preserve the captured storage contract rather than “tidy up” its bytes.

Internal symbols may use opaque publication identities, while externally visible names needed for linking remain part of the native interface.
Removing private spelling is not permission to remove symbol relationships.

## Different array extents without opaque target payloads

The current dialect prints an equal-extent array as `!nier.array<N, T>` and a native-word-dependent array as `!nier.word_array<N64, N32, T>`.
The latter is one element-type contract with two admitted extents, not two opaque modules.

The zlib CRC tables motivated an additional initializer form. A bounded shared element sequence can have a target-dependent selected count.
Common prefix elements are paired normally; a one-domain tail must contain pure literal data, not arbitrary symbol or address expressions.
The consumer checks both the selected native extent and the stored inventory's maximum extent.

Why restrict the tail?
Otherwise an apparently harmless inactive suffix could hide unvalidated references or semantics outside the shared initializer contract.
Data specialization needs explicit rules too.
The finite-domain array form is not permission to embed whole native programs inside a data attribute.

## Overlapping storage needs a different type

A union's fields overlap. An LLVM record used as its storage carrier does not necessarily list every source alternative.
Treating that carrier as an ordinary ordered record would lose important layout and future ABI facts.

Nier therefore has an explicit `!nier.overlap` type. It contains an opaque identity, scalar alternatives in semantic order, and their domain masks.
For example, this is valid type syntax for two alternatives present in both domains:

```mlir
!nier.overlap<"r1", [i32, f32], [3, 3]>
```

Our producer discovers qualified union evidence from actual global or local storage associations.
It checks member sizes, zero offsets, ordering and the native storage carrier.
Private member names help pair evidence; they are not emitted as public field names.
Contradictory or ambiguous evidence is rejected.

The language-blind carrier selection chooses among the active admitted scalar alternatives using maximum native alignment and then allocation size.
It checks that the chosen carrier also covers the maximum required extent; a case requiring unqualified extra tail padding is rejected rather than guessed.

The existing overlap fixture includes a wide integer alternative only in the wide profile.
The selected carrier can therefore differ between targets while the common type preserves the actual overlapping alternatives.
This is a bounded scalar-union storage rule, not support for every union layout.

## Storage success does not establish by-value ABI

The packed/bitfield storage fixture preserves nonzero initialization, a local aggregate copy, signed bitfield extraction,
and pointer-based mutation inside a larger record.
Clang has already expressed bitfield manipulation as native loads, shifts, masks and stores in the capture.
The admitted common graph preserves those operations and their storage layout.

That does not tell the consumer how to pass the same source record by value.
For example, a union or bitfield record's LLVM carrier can resemble an ordinary one-field record while native argument classification differs.
Current union/packed/bitfield **by-value ABI** remains outside the integrated producer contract even though storage and separate classifier tests pass.

This distinction is the bridge to the next chapter: layout answers where a value lives; a calling convention answers how a function boundary transports it.
Both must match the native target.

## Optional independent lab

Run this from the repository root in Bash with the SDK and tools already built:

```sh
source sdk/env.sh
set -euo pipefail
guide18_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide18-XXXXXX")
clang --config="$PWD/build/prealpha/nier.cfg" -O0 \
  tests/storage-native.c -o "$guide18_work/storage.nier"
for guide18_target in x86_64 i686; do
  build/prealpha/nierc lower "$guide18_work/storage.nier" \
    --target "$guide18_target" --output-dir "$guide18_work/$guide18_target"
done
rg -n '= type|global |getelementptr|alloca ' \
  "$guide18_work/x86_64" "$guide18_work/i686"
build/prealpha/nierc "$guide18_work/storage.nier" \
  --sdk "$NIER_SDK_ROOT" -o "$guide18_work/storage"
env -u LD_LIBRARY_PATH "$guide18_work/storage"
printf 'Native exit status: %s\n' "$?"
printf 'Lab files: %s\n' "$guide18_work"
```

Successful execution returns zero without printing an application message.
Inspect the native record fields and GEP paths; do not assume record numbering will remain unchanged between pre-alpha revisions.
This fixture also exercises callbacks and native nonlocal jumps, but its storage relationships are enough for this chapter's inspection.

## Recap and questions

Neutral storage preserves structure, native properties, initialization and access semantics.
The selected target supplies the concrete layout.
Explicit overlap is not interchangeable with ordered fields, and storage layout is not a complete function-boundary contract.

1. **Why not encode State's wide byte offsets directly?** Those offsets would be wrong for the narrow layout. Structural paths and native property types allow target-specific reconstruction.
2. **Does `inbounds` perform a bounds check?** No. It is a semantic promise; invalid uses can produce poison rather than a checked failure.
3. **Why validate an inactive array tail?** Its existence must still obey the restricted public initializer form; inactivity is not a loophole for hidden references or unvalidated payloads.
4. **Does a passing packed-record pointer test establish packed by-value ABI?** No. Native argument/result classification needs separate semantics and positive boundary tests.

## Guided source and evidence

Read the public storage types (`include/nier/IR/Dialect.h`), overlap type (`include/nier/IR/Overlap.h`), and their syntax implementation (`src/ir/Dialect.cpp`).
Follow `type`, `initializer`, and GEP handling in `src/ir/Producer.cpp` and `src/ir/Compiler.cpp`.
Compare private overlap evidence (`src/ir/OverlapEvidence.cpp`) with consumer carrier selection (`src/ir/OverlapLayout.cpp`).
Storage tests (`tests/storage.cpp`), the State fixture (`tests/storage-native.c`), overlap tests (`tests/overlap.cpp`), and the packed-bitfield fixture (`tests/fixtures/packed-bitfield-storage.c`) make the boundaries concrete.
The supported subset remains recorded in [02](../../02-implementation-plan.md).

[Next: native calling conventions](19-native-calling-conventions.md)
