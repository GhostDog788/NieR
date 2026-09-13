# 16 — Recovering a common program

[Course index](../README.md) · [Previous: capturing native programs](15-capturing-native-programs.md) · [Next: control-flow sharing and specialization](17-control-flow-sharing-and-specialization.md)

## Objective and prerequisites

This chapter explains the reference producer's central algorithm: turning two private native LLVM observations into one public NieR program.
You should understand SSA, native-width types, capture provenance, and the integer flags introduced in [chapter 14](../03-contributors/14-testing-debugging-and-features.md).

The result is neither a source reconstruction nor a general equivalence proof.
It is a common program whose admitted semantics are checked against both qualified native profiles.
The algorithm fails when it cannot establish that relationship. Those failures are compiler limitations, not security policies.

## Inputs, output, and the claim being made

`mergeProfiles` accepts an x86-64 LLVM capture, an i686 LLVM capture, and a bytecode output path.
Its optional summary is descriptive, not a proof token.
On success, the output is NieR bytecode; capture paths and mandatory producer provenance are not part of the public module contract.

Each capture is first checked for the pinned target assumptions and verified as LLVM.
The producer then applies specific private normalizations, constructs the common module, lowers it back for each profile,
and compares the admitted normalized native forms before writing the bytecode.

The essential shape is:

```text
left capture  ----\                 /---- reconstructed left
                  common NieR Code
right capture ----/                 \---- reconstructed right
        |                                    |
        +---- compare each corresponding ----+
                  native contract
```

The two comparisons are separate. Comparing only wide and narrow program stdout would miss ABI mistakes and target-specific behavior.
Comparing only the wide reconstruction would leave the narrow specialization unproved.

## A small worked correspondence

Return to two functions: one returns `sizeof(void *)`, the other literal 8. Suppose their result type in C is `size_t`.

The native result types are `i64` on x86-64 and `i32` on i686. The merger can pair that type correspondence as `!nier.word`.
For the first function, the native values 8 and 4 become the symbolic `pointer_bytes` expression.
For the second, equal values 8 and 8 become a fixed literal.

These are valid generic NieR fragments inside functions; `%width` and `%eight` are different values even on a target where they happen to agree:

```mlir
%width = "nier.constant"() {value = "pointer_bytes"} : () -> !nier.word
%eight = "nier.constant"() {value = 8 : i64} : () -> !nier.word
```

The `i64` on the attribute is the representation of that literal attribute, not a declaration that the result must always be 64 bits.
The operation's result type is `!nier.word`.

The implementation's `expression` helper also has a finite native-word form for other differing values: a dictionary with `word64` and `word32` entries.
This is not a symbolic algebra engine that discovers the original source formula.
A pair of numbers alone cannot tell us whether the author wrote `sizeof`, a conditional macro, or something else.
The admitted expression is valid only within the checked domain.

Equally important, equal `i32` types remain `i32`.
The merger does not change every integer into a native word to make the program look more portable.
Fixed-width controls prevent that tempting but incorrect generalization.

## Correspondence is a relationship, not matching names

The `Merger` maintains relationships among native values, blocks, symbols and storage types.
When two native instructions produce one common value, later uses must refer to that same established relationship.
A value cannot change its counterpart halfway through a function simply because another instruction has a convenient type.

For records, the same native type must consistently correspond to the same other-profile type.
Identified storage types receive opaque public identities such as `r0`.
Public symbol names needed for native linking remain meaningful; private source type names are not the public layout contract.

The ordinary instruction path pairs corresponding blocks and then walks their non-debug instructions.
PHIs are handled through block arguments and edge values.
When opcodes differ or one stream has an extra instruction, the algorithm only uses admitted normalization rules.
Otherwise it reports the function and instruction mismatch. It does not skip arbitrary instructions until the sequences happen to line up.

This is why implementation entry points are worth reading together.
`type` establishes a common type; `expression` represents a paired constant; `value` and the correspondence maps resolve operands; `mergeInstruction` constructs the operation.
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
The public consumer needs the resulting generic semantics, not the producer's recognition algorithm or source debug types.

## What the inverse comparison preserves

After common construction, the core lowers the module for each target.
Native aggregate forms are independently normalized again using explicit generated storage anchors,
so both captured and reconstructed forms pass the same admitted semantic-template proof before comparison.

The final comparison uses canonical LLVM text, with deliberately defined normalization.
Source/debug spelling, private local names, and checked pinned target configuration are not compared as application semantics.
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

## A rejected correspondence

Suppose the wide capture loads a global and the narrow capture calls an external function at the corresponding point.
Both might return an integer, and a sample execution might produce the same value.
That does not justify pairing them as one load or one call.
The call may have observable effects, different failure behavior, or depend on state not represented by the load.

Without an admitted common operation and a proof of its specialization, the correct result is a correspondence error.
Packaging both original bodies behind a target selector would evade the requested common representation, not solve the missing algorithm.

## Optional independent lab

Use the existing width fixture to see the same publication specialize twice.
Run from the repository root in Bash with the SDK and tools already built:

```sh
source sdk/env.sh
set -euo pipefail
guide16_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide16-XXXXXX")
clang --config="$PWD/build/prealpha/nier.cfg" -O0 \
  tests/fixtures/width.c -o "$guide16_work/width.nier"
build/prealpha/nierc inspect "$guide16_work/width.nier"
for guide16_target in x86_64 i686; do
  build/prealpha/nier_reference_lower lower "$guide16_work/width.nier" \
    --target "$guide16_target" --output-dir "$guide16_work/$guide16_target"
done
rg -n 'target triple|ret i(32|64) [48]|i32 4|i32 8' \
  "$guide16_work/x86_64" "$guide16_work/i686"
printf 'Lab files: %s\n' "$guide16_work"
```

Read the surrounding functions rather than treating a matching text pattern as a proof.
Both dumps come from the same `.nier` artifact.
`nier_reference_lower` is a publisher-only test helper with both native validators; public device `nierc` builds do not expose foreign-target lowering.
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

Read `mergeProfiles`, `Merger::type`, `expression`, the instruction pairing loop, and `canonicalize` in `src/ir/Producer.cpp`.
Compare the public core (`src/ir/Compiler.cpp`), paired producer tests (`tests/producer.cpp`), and the width fixture (`tests/fixtures/width.c`).
The aggregate normalization tests (`tests/aggregate-normalize.cpp`) provide a larger example of proving both accepted templates and rejected speculation.
Keep [02's verification contract](../../02-implementation-plan.md) alongside the code when deciding whether a proposed normalization is an explanation or an unjustified exception.

[Next: control-flow sharing and specialization](17-control-flow-sharing-and-specialization.md)
