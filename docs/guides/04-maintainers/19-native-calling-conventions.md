# 19. Native calling conventions

[Series](../README.md) · [Previous: Native storage](18-native-storage.md) · [Next: Variadics and native idioms](20-variadics-and-native-idioms.md)

## Objective and prerequisites

This chapter explains how one logical function becomes an ordinary native function on two targets without introducing a NieR calling convention into the installed application.
You should understand records, storage layout, LLVM function types, SSA uses, and the producer/consumer separation from earlier chapters.
The new difficulty is that the function's apparent LLVM parameters are not necessarily its source-language parameters.

By the end, you should be able to distinguish a classifier defect from a producer proof defect, explain why an indirect call needs a complete signature,
and review an aggregate-boundary change without treating passing arithmetic tests as evidence that the ABI is correct.

## A logical value is not a physical parameter

Consider ordinary C:

```c
struct Pair { int first; int second; };
struct Pair transform(struct Pair input);
```

The logical signature has one record argument and one record result. The native ABI decides how the bytes cross a call boundary.
In the pinned x86-64 capture, `Pair` is passed as one `i64` and returned as `i64`.
In the pinned i686 capture, its argument is expanded into two `i32` parameters; its result uses a hidden pointer parameter and a `void` LLVM return.
These are observed Clang shapes in [the ABI matrix](../../reference/native-abi-matrix.md), not rules inferred from the name `Pair`.

Two important LLVM attributes describe memory-based boundaries. An `sret` parameter points to result storage supplied by the caller.
A `byval` parameter represents a by-value argument carried through memory under the native ABI's copying and alignment rules.
A plain pointer parameter is not interchangeable with either.
Replacing a by-value argument with an arbitrary pointer could make the callee modify the caller's original object, changing valid C behavior.

NieR therefore separates the logical callable signature, the storage-oriented body used while representing it, and the final native function type.
The public `native_abi` type attribute describes the logical signature.
It is not a C source annotation, a profile-specific LLVM function body, or a request for a runtime wrapper.

The compiler must preserve all three relationships: each logical argument has the correct body storage;
that storage corresponds to the required native pieces;
and the installed symbol exposes exactly the native calling convention.
Knowing only the record's byte size is insufficient.

## Classify the whole signature

`src/ir/AggregateABI.h` makes the classification result explicit.
`NativeABIValue` records whether a value is direct, coerced, expanded, or indirect,
its storage extent and alignment, its physical pieces and their offsets, and its position in the native parameter list.
`NativeABISignature` contains the complete native function type, the result descriptor, every fixed argument descriptor,
a possible hidden-result index, and remaining register budgets.

That last state is essential.
Compare `Mixed { double, int }` as the first argument with the same record after enough integer and floating arguments have used the available register banks.
In the qualified x86-64 cases, an ordinary `Mixed` argument becomes a `double` and an `i32`;
after the relevant register pressure, the LLVM signature instead contains a `byval(Mixed)` pointer.
The classifier must account for earlier parameters and hidden result parameters before deciding what happens to this record.

Do not reduce this to “no registers means every record becomes byval.”
The fixture with `Pair` after six integer arguments still has an `i64` LLVM parameter; the backend puts that parameter on the stack.
The LLVM-level ABI description and the eventual machine register assignment are related but are not the same layer.
Tests must compare the actual pinned Clang signature, not a plausible hand-written approximation.

For naturally aligned ordered records, `classifyNativeABI` consumes explicit record assertions.
A listed type means an ordered, non-overlapping semantic record, including its nested records.
An arbitrary LLVM structure might instead be a storage carrier for a union or bitfields.
The classifier must not guess which interpretation is intended from LLVM's structure syntax.

`classifyNativeLayoutABI` has a different input: explicit selected-target field offsets, widths, record kind, total size, and alignment.
This supports separate classification experiments for packed and overlapping layouts.
Its existence does **not** make those types admissible through the complete producer pipeline.
Classification is one obligation; proving that real captured instructions implement that classification is another.

## Worked case: recovering and rebuilding Pair

Follow `transform` through the actual architectural boundaries.

First, stock Clang emits native LLVM for each private profile.
The producer's AggregateNormalize (`src/ir/AggregateNormalize.cpp`) discovers proposed logical signatures using private debug type graphs and parameter ordinals.
Debug declarations may point to the stack slot representing `input`.
Those records are useful evidence, but they do not authorize changing instructions.
They can be incomplete, ambiguous, or inconsistent with the actual IR.

Second, the producer proves the complete entry shape.
For the wide capture, the physical `i64` must be stored into the correct aggregate slot with the qualified offset and alignment.
For the narrow capture, the two `i32` pieces must populate the corresponding fields.
Every consumed shim instruction needs the expected uses.
A value also used by an unrelated instruction cannot simply be erased after its “main” store has been recognized.

Third, it proves the result shape. The narrow hidden `sret` destination can serve as the result-storage anchor.
The wide result may be built in owned local storage and loaded into its physical return value.
A dead spill of a hidden result pointer can be normalized away only when its uses and stores prove that it really is dead.
This is an explicit normalization rule, not permission to ignore arbitrary differences between captures.

Fourth, calls require their own proofs.
Argument pieces must be traced to the complete input object, and the returned pieces must be stored into the correct result object.
Scalar arguments, evaluation order, remaining body copies and observable storage behavior must survive.
Once both captures satisfy these rules, the merger can describe a common storage-oriented body with the logical `native_abi` signature.

Fifth, the language-blind consumer specializes that signature for a target.
NativeABIBridge (`src/ir/NativeABIBridge.cpp`) materializes the native entry, call and return forms inside the original functions.
The result is not `transform` calling a NieR adapter which then calls a second function.
There is no additional callable wrapper, trampoline or boxed-argument runtime.

Finally, the producer's inverse check independently recognizes the regenerated native forms
and compares the normalized native contract with the original capture.
“The classifier accepted it” and “the native inverse matches” are distinct tests.
Execution with an independently compiled native caller supplies another distinct check: both sides must agree on the actual ABI.

## Ownership, alignment, and attributes are part of the proof

The piece load/store helpers consume existing aggregate storage.
They do not allocate hidden objects, initialize padding, invent a `memcpy`, or copy an indirect argument merely to make a type mismatch disappear.
Their caller must establish the storage extent and base alignment before emission.
Alignment at a piece offset must be conservative: an aligned base does not imply every offset has the same alignment.

Padding also matters. `Mixed` occupies 16 bytes on the wide target and 12 on the narrow target in this configuration.
Normalizing its call boundary must not silently zero those objects or discard a body copy.
Removing incidental packing instructions is a justified proof transformation; changing object contents is a program transformation requiring a different argument.

Attributes must remain attached to the correct site. A definition's hidden result parameter can carry `noalias` while its call operand does not.
Copying the definition's entire attribute list onto every call strengthens claims the original call did not make.
Typed `sret` and `byval`, alignment, `noundef`, `writable`, and other admitted properties need their own validated mapping.

An indirect callee is an opaque LLVM pointer, but “pointer” is not a callable signature.
NieR's indirect-call operation carries an explicit type; aggregate normalization additionally proves the full logical native signature.
A symbol name, debug hint, or one nearby record store cannot establish that contract.

## A rejected case that prevents a real miscompile

Suppose a scalar function returns an integer which the program stores into a record field.
That does not mean the function returned the entire record.
A normalizer looking only for “call result followed by record-related store” can misclassify ordinary scalar calls as aggregate returns.

The qualified scalar-coercion rule requires the admitted direct whole-object storage anchor.
A field-address store is not enough.
Negative cases, including pointer and integer results and one-field records, exercise this distinction in `tests/aggregate-normalize.cpp`.
Other rejections cover extra uses, intervening stores, volatile accesses, excessive alignment claims, and forged layout evidence.
Failed proofs must leave their input module unchanged.

Current complete-pipeline coverage is for the qualified ordinary-record fixtures,
including direct and indirect calls, native callbacks, register pressure, and hidden result storage.
Union, packed, and bitfield **by-value boundaries**, and aggregate variadic extraction, remain separate unsupported pipeline cases.
Positive storage tests do not change that boundary.

## Optional independent lab

With a prepared SDK and pre-alpha build, run this Bash lab from the repository root.
It creates its own output directory and does not require another lab's files:

```bash
source sdk/env.sh
abi_lab=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide-abi-XXXXXX")
clang --config="$PWD/build/prealpha/nier.cfg" -O2 \
  tests/abi/fixed_main.c tests/abi/boundaries.c tests/abi/native_bridge.c \
  -o "$abi_lab/fixed.nier"
build/prealpha/nierc "$abi_lab/fixed.nier" -o "$abi_lab/fixed"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$abi_lab/fixed"
build/prealpha/nierc lower "$abi_lab/fixed.nier" \
  --target i686 --output-dir "$abi_lab/narrow"
printf 'Lab files: %s\n' "$abi_lab"
```

The executable should report that the fixed aggregate matrix passed.
Inspect the lowered files for hidden result parameters and expanded arguments.
The `lower` command demonstrates diagnostic specialization; it does not claim an installed i686 native-output product.
For the stronger external-caller case, read `tests/aggregate-pipeline.sh`, which also builds a NieR DSO and an independently compiled native caller at O0 and O2.

## Recap and questions

Native ABI support is a three-part contract: describe a logical signature, prove its relationship to captured storage and shims,
and materialize the target's actual boundary without introducing a runtime ABI.

1. **Why cannot a record be classified in isolation?** Earlier arguments and hidden result parameters affect register availability and, in qualified cases, the physical LLVM signature.
2. **Why is debug type information insufficient?** It proposes identities and anchors but does not prove instruction effects, uses, aliasing or ordering.
3. **Does a passing packed-record classifier mean packed C calls publish?** No. Producer recognition, common semantics, inverse proofs and end-to-end native execution must also be qualified.

Read the descriptor definitions before the implementations: `src/ir/AggregateABI.h`, `src/ir/AggregateNormalize.h`, then `src/ir/NativeABIBridge.h`.
Compare the separate classifier (`tests/aggregate-abi.cpp`), normalizer (`tests/aggregate-normalize.cpp`), and public pipeline (`tests/aggregate-pipeline.sh`) tests; each proves a different part of the same boundary.

[Previous: Native storage](18-native-storage.md) · [Next: Variadics and native idioms](20-variadics-and-native-idioms.md)
