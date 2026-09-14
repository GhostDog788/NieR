# 16 — Recovering a common program

[Course index](../README.md) · [Previous: capturing native programs](15-capturing-native-programs.md) · [Next: control-flow sharing and specialization](17-control-flow-sharing-and-specialization.md)

## Objective and prerequisites

This chapter explains the reference producer's central algorithm: turning a finite set of private native LLVM observations into one public Sela program.
You should understand SSA, native-width types, capture provenance, and the integer flags introduced in [chapter 14](../03-contributors/14-testing-debugging-and-features.md).

The result is neither a source reconstruction nor a general equivalence proof.
It is one Sela publication whose shared and target-specific semantics are checked against every declared native profile.
The algorithm fails when it cannot establish that relationship. Those failures are compiler limitations, not security policies.

## Inputs, output, and the claim being made

`mergeProfiles` accepts an array of `CaptureObservation` records, each binding one target ID to one LLVM capture, and a bytecode output path.
Its optional summary is descriptive, not a proof token.
On success, the output is Sela bytecode; capture paths and mandatory producer provenance are not part of the public module contract.

Each capture is first checked for the pinned target assumptions and verified as LLVM.
The producer imports each capture independently, optionally normalizes and factors common structure, lowers the result back for each profile,
and compares the admitted normalized native forms before writing the bytecode.

The essential shape is:

```text
captures for N targets --> independent Sela projections
          |                         |
          |                 optional common factoring
          |                 or target-scoped definitions
          |                         |
          +---- compare <--- N reconstructed native contracts
```

The per-target comparisons are separate. Comparing only program stdout would miss ABI mistakes and target-specific behavior.
Checking one reconstruction leaves every other specialization unproved.

## A small worked correspondence

The following two-target example isolates width differences for readability.
The public entrypoint accepts an N-observation inventory, currently up to four targets; its result must reconstruct every observation, including same-width targets with different ABIs.
`LLVMImporter` in `src/ir/Producer.cpp` imports one observation at a time, without requiring matching function inventories or CFGs.
`src/ir/CommonMerge.cpp` then attempts common graph factoring.
If factoring cannot be proved, explicit target-scoped Sela definitions remain valid publication content.
They contain ordinary Sela operations, never opaque LLVM or native payloads.

Return to two functions: one returns `sizeof(void *)`, the other literal 8. Suppose their result type in C is `size_t`.

The native result types are `i64` on x86-64 and `i32` on i686. The merger can pair that type correspondence as `!sela.word`.
For the first function, the native values 8 and 4 become the symbolic `pointer_bytes` expression.
For the second, equal values 8 and 8 become a fixed literal.

These are valid generic Sela fragments inside functions; `%width` and `%eight` are different values even on a target where they happen to agree:

```mlir
%width = "sela.constant"() {value = "pointer_bytes"} : () -> !sela.word
%eight = "sela.constant"() {value = 8 : i64} : () -> !sela.word
```

The `i64` on the attribute is the representation of that literal attribute, not a declaration that the result must always be 64 bits.
The operation's result type is `!sela.word`.

Other differing values use a finite `target_cases` dictionary whose cases bind explicit target sets to semantic values.
Equal cases are coalesced, and the same mechanism can represent differences between targets of equal word width.
This is not a symbolic algebra engine that discovers the original source formula.
A pair of numbers alone cannot tell us whether the author wrote `sizeof`, a conditional macro, or something else.
The admitted expression is valid only within the checked domain.

Equally important, equal `i32` types remain `i32`.
The merger does not change every integer into a native word to make the program look more portable.
Fixed-width controls prevent that tempting but incorrect generalization.

## Correspondence is a relationship, not matching names

The factoring pass maintains relationships among imported values, blocks, symbols and storage types.
When two native instructions produce one common value, later uses must refer to that same established relationship.
A value cannot change its counterpart halfway through a function simply because another instruction has a convenient type.

For records, the same native type must consistently correspond to the same other-profile type.
Identified storage types receive opaque public identities such as `r0`.
Public symbol names needed for native linking remain meaningful; private source type names are not the public layout contract.

The importer walks each native instruction and preserves its operands, flags, types and effects.
PHIs become block arguments and edge values.
The optional factoring path tries to establish corresponding imported instructions and uses only admitted normalization rules.
If that relationship cannot be established, scoped definitions preserve the distinct bodies instead of skipping instructions to force a match.

This is why implementation entry points are worth reading together.
In the importer, `type`, `expression`, `value`, and `mergeInstruction` encode native semantics; some internal helpers retain paired signatures from the earlier implementation but receive the same observation on both sides.
Common factoring is a separate pass over the imported Sela modules.
None of these alone proves the whole module.

## Normalize an asymmetry without deleting its meaning

Consider a wide native sequence that truncates a word to 32 bits and then zero-extends it back to a word.
On the narrow target, the corresponding word already has 32 bits, so those conversions may be absent.

The wide sequence does **not** equal an unconditional identity. For a 64-bit input with nonzero high bits, the round trip discards those bits.
Simply removing the conversions from both profiles would change the wide program.

The producer can instead represent an admitted native-width cast sequence whose narrow specialization is an identity
and whose wide specialization retains the original conversions.
Chained cases must resolve their input through a unique previously proved conditional correspondence.
A heuristic such as “use the most recent value of this type” would be unsound.

Arithmetic flags need the same care.
A source operation can become a plain wide operation but an `nsw` narrow operation because native source types and integer promotions differ.
The common contract can preserve the selected flag expression for each domain.
It must not remove the narrow promise just to make the textual instructions match.

Recall the prerequisite: `nsw`, `nuw`, and `exact` are semantic restrictions, not decorative optimizer preferences.
Violating an applicable flag can produce poison.
The consumer validates that selected flags are legal for the opcode, including values supplied for the other declared domain where the schema requires them.

## Why private normalization precedes generic matching

Some native differences are too structural for pairwise instruction matching.
An aggregate return can be a register value on one target and a hidden result pointer on the other.
A scalar `va_arg` can be a register/stack decision graph on x86-64 and a pointer step on i686.
A byte reversal can have a different number of shifts because the native word width differs.

The current pipeline discovers overlapping-storage evidence while private debug associations still exist,
proves aggregate boundary normalizations, normalizes qualified byte swaps, and recognizes qualified native vararg transitions before the main merger.
Later chapters explain those algorithms.

These normalizers are not broad optimization passes. They recognize bounded semantic templates with use, effect and layout conditions.
A rejected candidate must not leave a half-rewritten capture.
If optional ABI recovery fails, the importer restarts from the untouched native module and uses explicit concrete Sela signatures and storage.
The inverse comparison then checks that concrete form, without pretending that a common ABI normalization succeeded.
The public consumer needs the resulting generic semantics, not the producer's recognition algorithm or source debug types.

## What the inverse comparison preserves

After common construction, the core lowers the module for each target.
Native aggregate forms are independently normalized again using explicit generated storage anchors,
so both captured and reconstructed forms pass the same admitted semantic-template proof before comparison.

The final comparison uses canonical LLVM text, with deliberately defined normalization.
Source/debug spelling and unnecessary private names may be normalized; CPU/features/tuning are preserved as code-generation inputs.
When inline assembly can refer to symbol spellings, those names remain unchanged in both import and comparison.
Debug and the admitted TBAA optimization metadata are removed. That metadata policy is not a performance-parity result.

PHI incoming lists are ordered consistently without changing which incoming value belongs to which predecessor.
Qualified module flags are ordered consistently.
An empty self-referential loop marker carries no options and can be omitted,
whereas meaningful loop options and shared loop identity must survive through their explicit representation.

This whitelist is part of the proof boundary.
If a new mismatch appears, the default response is not “add it to canonicalization.”
Ask whether the difference is truly nonsemantic, whether the corresponding public operation is missing, or whether the reconstruction is wrong.
Attributes, volatile accesses, instruction ordering, native calls and dataflow cannot be waved away as formatting noise.

Equality after these checks is a useful sufficient test for the admitted programs.
Inequality does not prove that two arbitrary programs differ in behavior; the producer may simply lack a required normalization.
Either way, it cannot claim successful publication under the current rules.

## A rejected sharing attempt is not necessarily a rejected program

Suppose the wide capture loads a global and the narrow capture calls an external function at the corresponding point.
Both might return an integer, and a sample execution might produce the same value.
That does not justify pairing them as one load or one call.
The call may have observable effects, different failure behavior, or depend on state not represented by the load.

Without a proved common operation, these instructions must not be merged as if they were equivalent.
The producer can instead encode both bodies using ordinary Sela operations with disjoint target domains and validate each reconstruction.
This is a valid Sela representation; sharing is optional.
If either body's semantics are not representable in Sela, publication still fails. An LLVM blob is not an escape hatch.

## Optional independent lab

Use the existing width fixture to see the same publication specialize twice.
Run from the repository root in Bash with the SDK and tools already built:

```sh
source sdk/env.sh
set -euo pipefail
guide16_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-guide16-XXXXXX")
clang --config="$PWD/build/prealpha/sela.cfg" -O0 \
  tests/fixtures/width.c -o "$guide16_work/width.sela"
build/prealpha/selac inspect "$guide16_work/width.sela"
for guide16_target in x86_64 i686; do
  build/prealpha/sela_reference_lower lower "$guide16_work/width.sela" \
    --target "$guide16_target" --output-dir "$guide16_work/$guide16_target"
done
rg -n 'target triple|ret i(32|64) [48]|i32 4|i32 8' \
  "$guide16_work/x86_64" "$guide16_work/i686"
printf 'Lab files: %s\n' "$guide16_work"
```

Read the surrounding functions rather than treating a matching text pattern as a proof.
Both dumps come from the same `.sela` artifact.
`sela_reference_lower` is a publisher-only test helper with all registered native validators; public device `selac` builds do not expose foreign-target lowering.
This demonstrates the qualified two-profile specialization, not a new CPU port or installed i686 execution support.

## Recap and questions

The producer establishes consistent correspondences, represents supported target variation, and checks reconstruction before publication.
It does not recover arbitrary source intent from LLVM or prove universal portability.

> [!faq]- Why are equal constants not automatically symbolic?
>
> Equality alone does not identify a native property. Literal controls must remain literal.

> [!faq]- Why preserve a wide truncation followed by extension?
>
> It can discard high bits even when both operations are identities on the narrow target.

> [!faq]- Can a round-trip mismatch be solved by deleting more metadata?
>
> Only after establishing a justified, explicit normalization rule. Semantic evidence cannot be discarded to make a comparison pass.

> [!faq]- Does a correspondence failure mean the C program is invalid?
>
> No. A valid native program can be outside the current producer's proof rules.

## Guided source and evidence

Read `mergeProfiles`, `LLVMImporter::importModule`, `type`, `expression`, `mergeInstruction`, and `canonicalize` in `src/ir/Producer.cpp`.
Then follow optional factoring and scoped definitions in `src/ir/CommonMerge.cpp` and translation-unit reconstruction in `src/ir/CompilationUnits.cpp`.
Compare the public core (`src/ir/Compiler.cpp`), paired producer tests (`tests/producer.cpp`), and the width fixture (`tests/fixtures/width.c`).
The aggregate normalization tests (`tests/aggregate-normalize.cpp`) provide a larger example of proving both accepted templates and rejected speculation.
Keep [02's verification contract](../../02-implementation-plan.md) alongside the code when deciding whether a proposed normalization is an explanation or an unjustified exception.

[Next: control-flow sharing and specialization](17-control-flow-sharing-and-specialization.md)
