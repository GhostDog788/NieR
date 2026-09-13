# 17 — Control-flow sharing and specialization

[Course index](../README.md) · [Previous: recovering a common program](16-recovering-a-common-program.md) · [Next: native storage](18-native-storage.md)

## Objective and prerequisites

This chapter explains how NieR represents a supported target-conditioned region inside one shared function,
and how the consumer removes inactive regions without breaking the program.
You should understand basic blocks, SSA, dominance, PHIs/block arguments, and the finite-domain correspondence claim from the previous chapter.

We will distinguish three checks that are easy to confuse: pairing the native CFGs, translating the instructions within their blocks, and specializing the public graph.
Passing any one of these does not imply that the others pass.

## Why matching instruction lists is insufficient

A preprocessor condition can change the number of blocks before NieR sees the program.
In the existing conditional-switch fixture, the wide profile includes cases 8 and 16, while the narrow profile includes case 4.
Cases 1 and 2 and the default path are shared.

The arms perform ordinary observable work: they call `observe`, modify a volatile global through that function, update pointed-to storage, and rejoin shared code.
This is not a fixture in which the target-only blocks can be discarded because their results are unused.

A simplified view of the real case is:

```text
                     shared switch
                  /       |        \
          shared case   wide arm   narrow arm
                  \       |        /
                       shared join
```

The public representation needs to preserve common structure and describe which arms exist for each admitted target.
It must not store two complete native functions and select one at installation.

## Pairing establishes graph structure, not behavior

The producer-only `pairConditionalCFG` accepts two verified native functions and returns a graph correspondence.
It does not mutate either input.

The shared entry block anchors the process. Ordinary branches are paired by their corresponding orientation and successors.
A switch pairs its default edge and shared cases by exact integer labels, rather than by block names.
The current rule requires matching label widths and bounded integer labels; it is not a general expression equivalence test for case values.

The result records a shared sequence of block pairs, maps from each native block to that sequence, and paired switch cases.
A block or case can have a native object on both sides or only one side. Missing sides are allowed only under the admitted conditional-arm rule.

Ordering matters. Filtering the shared block and case sequences for a target must reproduce that target's original order.
Sorting blocks alphabetically would not establish that property, and source names are not stable semantic identities in any case.
The helper rejects order conflicts it cannot represent under its current rules.

At this point we know how graph pieces correspond. We do not yet know that the branch conditions, instructions, side effects, or metadata agree.
The main merger must still translate each instruction, and both native inverse comparisons must still succeed.

## The admitted one-sided arm

The current private pairing rule accepts a deliberately bounded shape:
a target-only switch arm forms a nonempty closed chain of unconditional branches,
entered by its qualified switch edge and eventually rejoining shared code.
It does not admit arbitrary target-only loops or general divergent regions.

There are further restrictions. A function with conditional arms cannot contain PHIs under this rule.
The arm's SSA values cannot escape into shared code in an unproved way.
Block addresses, unpaired unreachable regions, and other unmatched terminators are also outside the rule.
Fully shared paired graphs can still use the existing PHI handling.

Why restrict PHIs when the compiler already understands them? Because PHIs describe values arriving from specific predecessors.
Adding or removing a target-only predecessor changes their incoming-edge contract.
Generalizing that case requires a proof of the resulting value relationships, not merely the ability to parse a PHI instruction.

The source fixture's local `result` is represented in the preoptimization capture in a shape that the rule can qualify.
Ordinary LLVM optimization can introduce or simplify PHIs later, after target specialization.
A restriction on the captured producer template is not a ban on LLVM optimizing the final native program.

## The public domain representation

NieR records conditional presence with native-word domain masks:

| Mask | Meaning in the current contract |
|---|---|
| `1` | wide domain only |
| `2` | narrow domain only |
| `3` | both domains |

A function can carry `block_domains`, aligned with its block inventory. A switch can carry `case_domains`, aligned with its case inventory.
The shared entry remains present in both domains.
These masks describe the current semantic target domain; they are not C preprocessor directives or evidence about all 64-bit and 32-bit architectures.

The instructions themselves are ordinary NieR operations. A one-sided arm is not an opaque LLVM payload.
In its single-domain translation mode, the producer must still resolve values against established shared correspondences or supported values within that arm.
Unknown aggregate layouts or unmatched uses do not become acceptable just because only one target needs them.

The public common graph is also independent of how it was produced.
A direct NieR producer can construct a valid conditional graph without our LLVM CFG pairing helper.
The consumer checks the public contract, not a private certificate saying that Clang produced the code.

## Work through wide specialization

Start with one common function containing the shared switch, a wide-only case 8, a narrow-only case 4, and the shared join.
Ask the consumer to specialize the wide target.

`specializeConditionalCFG` clones the source module.
Cloning matters because verification or another target's lowering must not observe a source graph that was permanently pruned for the first target.

The helper reads and validates block domains. It then reconstructs each switch's selected cases, successor list, and argument segments.
The default edge is retained. The wide switch contains case 8 but not case 4.
A selector value of 4 therefore follows the wide program's default behavior, just as in the native wide capture.

Switch operands are not only labels.
Branches can carry values for successor block arguments, so the `argument_counts` segments must stay consistent with the retained successors.
Removing a case without removing exactly its argument segment could silently bind the wrong SSA values to the next edge.

Before deleting blocks, the helper checks surviving edges and operand uses.
An active branch must not reach an absent block. An active operation must not use a value whose owning block is absent.
Only after these conditions hold does it drop references and erase inactive blocks.
It removes the consumed domain annotations from the specialized copy and structurally verifies that copy before ordinary LLVM lowering proceeds.

The narrow specialization makes the complementary selection.
It is checked independently against the narrow reference; a passing wide result does not establish the narrow result.

## Why erasing first is dangerous

Imagine a shared block uses `%temporary`, but `%temporary` is defined only in the narrow-only arm.
On the wide target, deleting the arm would leave a use without a definition.
Replacing that value with zero would invent semantics; retaining a dangling reference would create malformed IR.

The public consumer must reject this graph. It cannot assume an artifact is safe because our current producer would not emit it.
Public NieR construction is independent, and malformed input can arrive at the same API.

Another rejected case is a one-sided source branch with structure outside the closed-switch-arm template.
It may be valid C and may have an obvious meaning to a human, yet the producer lacks an admitted graph proof.
The appropriate diagnostic is an unsupported correspondence, not successful publication of a hidden target-specific body.

These are different rejection layers. The producer rejects an unproved native-to-common mapping; the consumer rejects an invalid public graph.
Their rules are related but do not substitute for each other.

## Loops and metadata identity

Control-flow maintenance also includes relationships that are not new blocks.
LLVM loop metadata can refer to one distinct node from more than one latch branch.
A latch is a branch returning control toward the loop header. Two latches can belong to the same loop identity.

If lowering creates a fresh metadata node for every branch, it can preserve the text of individual options while losing that shared relationship.
This was significant in the zlib evidence: metadata-node identity was not merely a pretty-printer number to ignore.

The producer therefore establishes one-to-one correspondence for meaningful native loop identities and emits an opaque loop identifier.
The consumer reuses the corresponding native metadata node only when its declared options agree.
A conflicting option set for one identity is rejected.

This is separate from solving arbitrary loop equivalence.
The implementation preserves the supported loop contract; it does not infer that unrelated loop graphs are equivalent or erase optimization metadata wholesale.

## Optional independent lab

From the repository root in Bash, using the already built tools:

```sh
source sdk/env.sh
set -euo pipefail
guide17_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide17-XXXXXX")
clang --config="$PWD/build/prealpha/nier.cfg" -O0 \
  tests/fixtures/conditional-switch.c -o "$guide17_work/switch.nier"
for guide17_target in x86_64 i686; do
  build/prealpha/nier_reference_lower lower "$guide17_work/switch.nier" \
    --target "$guide17_target" --output-dir "$guide17_work/$guide17_target"
done
rg -n -A10 'switch i32' "$guide17_work/x86_64" "$guide17_work/i686"
ctest --test-dir build/prealpha \
  -R '^(conditional_validation|conditional_cfg_pairing|loop_identity)$' \
  --output-on-failure 2>&1 | tee "$guide17_work/tests.txt"
printf 'Lab files: %s\n' "$guide17_work"
```

Compare the case lists, then follow an arm's effects to the join. This lab does not claim that any pair of differing CFGs can be published.
The two-target inspection uses the publisher-only `nier_reference_lower` test helper, not a public cross-target device compiler.
The negative fixtures are as important as the positive switch example.
Check the test command's exit status rather than treating log output as a success signal.

## Recap and questions

Conditional sharing needs a graph correspondence, ordinary instruction translation, safe public specialization, and both native inverse checks.
Domain annotations do not waive any of those obligations.

> [!faq]- Why preserve the default switch edge?
>
> A case absent on one target must fall through to that target's actual default behavior, not disappear from the program's possible inputs.

> [!faq]- Why inspect active uses before erasing inactive blocks?
>
> To reject surviving references to absent definitions without creating dangling IR or inventing replacement values.

> [!faq]- Why are PHIs restricted in conditional producer functions?
>
> Changing predecessor presence changes their incoming-value contract; the current one-sided-arm proof does not solve that general case.

> [!faq]- Why preserve shared loop identity?
>
> Identical option lists on separate fresh nodes do not necessarily reproduce the original shared relationship.

## Guided source and evidence

Start with the private CFG contract (`src/ir/ConditionalCFG.h`) and pairing implementation (`src/ir/ConditionalCFG.cpp`).
Then read public specialization (`src/ir/ConditionalSpecialization.cpp`), its shared structural checks (`src/ir/Compiler.cpp`), and its native lowering integration (`src/ir/NativeLowering.cpp`).
Compare the positive source fixture (`tests/fixtures/conditional-switch.c`), an escaping-value rejection fixture (`tests/fixtures/conditional-escape-rejected.c`),
public conditional tests (`tests/conditional.cpp`), and loop-identity tests (`tests/loop-identity.cpp`).
The producer's main translation and inverse steps remain in `src/ir/Producer.cpp`.

[Next: native storage](18-native-storage.md)
