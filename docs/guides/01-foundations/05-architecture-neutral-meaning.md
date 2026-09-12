# 05. Architecture-Neutral Meaning

[Series](../README.md) · [Previous](04-values-control-flow-and-memory.md) · [Next](06-publication-artifacts.md)

## What you will understand

You will distinguish fixed values from native properties, explain why layout and calling conventions matter beyond pointer width, and understand the scope of a portability claim.
Read chapters 01–04 first.
This chapter explains the contract; the producer's recovery algorithm comes later.

## Neutral does not mean unspecified

A portable representation cannot simply omit every native detail.
Omitting the result type of an addition leaves the consumer guessing whether overflow wraps after 32 bits or 64 bits.
Omitting a memory layout relationship leaves it guessing which field an access refers to.
A successful neutral representation preserves meaning while postponing only the decisions it explicitly models.

NieR distinguishes at least three ideas you should never merge into one:

- a fixed-width integer such as `i32`;
- a native-width integer such as `!nier.word`;
- a pointer such as `!nier.ptr`.

An `i32` remains 32 bits on both current targets.
A `!nier.word` specializes to the integer width associated with the target's native pointer width.
A pointer is still a pointer, not merely a word with a different spelling.
Operations and conversion rules express relationships between those types.

The `i32` spelling alone does not say “C signed int.”
Arithmetic and comparison operations also carry semantic choices.
Signed and unsigned comparisons can interpret the same bit pattern differently.
A compiler must preserve those choices alongside the width.

## Work through the pointer-size example

The complete C fixture in the repository includes these computations:

```c
size_t width = sizeof(void *);
printf("pointer=%zu word=%zu fixed=%u,%u\n",
       width, sizeof(size_t), 4u, 8u);
```

This is a **source excerpt**; its headers and surrounding function are omitted.
The fixed literals are deliberate controls, not incidental output formatting.

For the two currently modeled native profiles:

| Property | x86-64 Linux profile | i686 Linux profile |
| --- | --- | --- |
| Pointer size | 8 bytes | 4 bytes |
| `size_t` size | 8 bytes | 4 bytes |
| C `int` size | 4 bytes | 4 bytes |
| Literal `8u` | Value eight | Value eight |

The useful common expression is not “whichever constant differs must be a pointer size.”
It is an admitted symbolic expression whose specialization reproduces the required native contracts.
The current representation includes this actual operation shape:

```mlir
%width = "nier.constant"() {value = "pointer_bytes"} : () -> !nier.word
```

This **operation excerpt** has a 64-bit integer result with value eight when specialized to x86-64
and a 32-bit integer result with value four when specialized to i686.
Compare it with:

```mlir
%eight = "nier.constant"() {value = 8 : i64} : () -> i32
```

The second produces a fixed 32-bit value of eight in both cases.
The integer attribute's `i64` encoding does not change the result width; chapter 03 explained that distinction.

Substituting four for the second value on i686 would be a compiler bug.
Keeping eight for the first value on i686 would also be a bug.
Portability requires preserving the difference, not choosing a convenient global machine size.

## Target domain: the boundary around the claim

The current core admits profile IDs `x86_64` and `i686`.
The artifact declares the targets for which its semantics are intended, and the consumer validates that declaration.
A target domain is part of the meaning of a qualification claim, not an optional marketing label.

The current C producer obtains private native evidence from both profiles.
Two observations cannot uniquely reconstruct every original source expression.
For example, “pointer bytes” and “four times the number of four-byte pieces in a pointer” agree here.
The producer's task is not to recover the exact source spelling.
It must construct an admitted common representation and validate that representation against the profiles it claims.

This distinction permits useful bounded transformations without claiming a general theorem about all architectures.
A future target may differ in a way that neither existing profile exposes.
Adding it requires native rules, producer/consumer reasoning, and new tests—not simply adding a string to the manifest.

The product native-output path is initially x86-64.
The i686 lowering and private reference evidence are valuable, but they do not by themselves mean the complete on-device i686 product is qualified.
Keep target semantics and deployment support separate when reading test results.

## Data models explain more than pointer size

On the current x86-64 Linux profile, the common C data model is LP64: `long` and pointers are 64 bits while `int` is 32.
On i686 it is ILP32: those three categories are 32 bits.
The names summarize widths; they do not specify every ABI detail.

You must not define NieR's meaning as “every C `long` is always a pointer-sized integer on every platform.”
That relationship holds for these particular models, not all possible C targets.
The neutral type has its own semantics; the producer is responsible for expressing source types correctly within the supported domain.

Similarly, two targets with equal pointer widths can disagree about alignment, record passing, or other native behavior.
A boolean “64-bit” switch is the current implementation's bounded target choice, not a sufficient design for arbitrary future target support.

## Layout is a relationship, not just a total size

Consider an ordinary record in our current profiles:

```c
struct Item {
    unsigned char tag;
    size_t count;
};
```

On these targets, the `count` field needs alignment appropriate to its native integer type.
There is padding after `tag`.
In the ordinary unpacked layout, the field lies at offset eight on x86-64 and offset four on i686; the struct's sizes are sixteen and eight respectively.

If public code described only “load the bytes at offset eight,” the i686 consumer could access the wrong storage.
The representation needs sufficient type/layout relationships to recover the correct address.
NieR record and array types, storage operations, and `nier.gep` participate in that work.
Later chapters show exactly which shapes the current importer can prove.

The example is about ordinary unpacked storage.
Packed records, overlapping union alternatives, bitfields, or platform-specific attributes introduce additional questions.
A successful layout classification is not automatically permission for every producer reconstruction or by-value call form.

## A native call has two descriptions

At the source level you might describe a function as “take an `Item`, return an `Item`.”
The native calling convention decides whether those values travel in registers, stack slots, pieces, or storage passed through an implicit pointer.
The complete signature and surrounding native rules influence that decision.

NieR must preserve a logical callable contract while `nierc` materializes the correct physical native form.
This is why language-blind compilation still needs ABI knowledge.
The consumer does not ask “what did C mean by struct?” but it must know how the represented aggregate participates in a native call.

Our intermediate owned-storage conventions are compiler-internal.
The finished program is not required to call a universal boxed runtime API to communicate with ordinary native code.
The current native ABI implementation and its proofs are examined in chapter 19.

## Neutral code still has dependencies and limits

A function calling libc does not magically stop needing libc because its body is neutral.
The consumer links against the declared/supplied native dependency environment.
A future language runtime would likewise need appropriate native support.
Portability of program representation is not independence from every operating-system or library interface.

When a source operation or native difference is unsupported, rejection is an important outcome.
It prevents unsupported meaning from silently entering a publication.
It is not proof that the required feature has been completed, and the compiler must not quietly wrap the original native program as a substitute for a common representation.

## Optional paper exercise

Predict which of the following should change between our profiles: `sizeof(void *)`, `sizeof(int)`, the literal `8`, and the field offset of `Item.count`.
Then ask which information the representation must retain to produce each answer.
Values alone are not enough to distinguish the first from the third.

## Recap

Neutrality means retaining target-independent meaning and explicit target relationships within a declared domain.
Fixed values remain fixed; native properties specialize; pointers remain typed pointers.
Layouts, calls, and dependencies cannot be reduced to one pointer-width substitution.

## Check your understanding

1. Does `!nier.word` mean all source-language integer types?
2. Why is checking both eight/four and eight/eight useful?
3. Does supporting one 64-bit target establish another 64-bit target's ABI?

> [!faq]- Answers
>
> 1. No. It models a specific native-width relationship. Fixed-width and other source types must retain their own correct meaning.
> 2. It checks that genuinely native-dependent values specialize while a coincidentally equal literal is not rewritten incorrectly.
> 3. No. Equal width does not establish equal alignment, calling conventions, instruction semantics, libraries, or deployment support.

## Source and evidence trail

- Width fixture (`tests/fixtures/width.c`) deliberately combines native properties and fixed literals.
- Hello/width pipeline test (`tests/hello.sh`) checks native x86-64 execution and diagnostic i686 lowering without conflating their qualification levels.
- Core target definitions (`src/ir/Compiler.cpp`): `configureModule` and `Lowerer` reveal the actual target domain, data layouts, and symbolic-expression handling.
- Record/layout types (`include/nier/IR/Dialect.h`) describe relationships rather than embedding only one host's byte offsets.

[Next: From NieR Code to a Publication Artifact](06-publication-artifacts.md)
