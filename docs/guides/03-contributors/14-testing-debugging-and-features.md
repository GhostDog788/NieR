# 14 — Testing, debugging, and following a feature

[Course index](../README.md) · [Previous: following `nierc`](13-following-nierc.md) · [Next: capturing native programs](../04-maintainers/15-capturing-native-programs.md)

## Objective and prerequisites

This is the contributor checkpoint. You should be able to trace a small semantic feature through its public contract, producer, consumer, and tests; then explain what evidence a safe change would require.
You should know basic SSA and how the two sides of NieR connect. This chapter introduces the integer semantic details needed for its worked example.

We will follow **the existing `nier.bswap` feature**. It is already implemented.
The exercise is not to add a fictional missing operation or copy a recipe without understanding it.
A completed feature is useful precisely because its implementation includes the less visible rejection and verification work.

## First define what the operation means

A byte swap reverses the bytes of an integer value. For a 32-bit input:

```text
0x11223344 -> 0x44332211
```

This is not bit reversal. It is also not a request to change the machine's endianness.
The operation has a meaning for the integer's bit pattern before we decide how a particular CPU implements it.

In the current contract, the native integer width must be 16, 32, or 64 bits, and the result has the same type as the input.
A `!nier.word` value can be used because it specializes to an admitted width in the current target domain.
An independent producer can emit the operation directly.

This is a genuine generic-assembly **fragment**, not a complete module.
`%x` must already be defined in the surrounding function:

```mlir
%reversed = "nier.bswap"(%x) : (i32) -> i32
```

There are no source-language tags or captured native instruction sequences in that operation. The contract says what to compute; the selected native backend decides how to compute it.

## Integer semantics before rewriting integer code

An LLVM integer type such as `i32` describes a 32-bit value.
Signedness is expressed through operations and relevant attributes, rather than by choosing between two different `i32` storage types.

Plain integer addition operates on the fixed-width bit pattern. Arithmetic flags add stronger promises.
`nuw` says an operation has no unsigned wrap; `nsw` says it has no signed wrap.
Violating such a promise can produce LLVM *poison*: a value with special invalid-result semantics that later uses can turn into undefined behavior.
Poison is not an ordinary random integer that we may replace with any convenient value. The `exact` flag similarly adds a restriction to applicable division or shift operations.

C has its own defined and undefined cases. For example, unsigned arithmetic and signed overflow do not carry the same source-language guarantees.
Clang has already translated some of those distinctions into LLVM by the time we see a capture.
A merger must preserve the admitted contract, not assume that similar-looking arithmetic is interchangeable.

That is why the byte-swap recognizer rejects candidate arithmetic with poison-generating flags. Its bounded proof is for the plain bit manipulation idiom.
It does not prove that all flag-bearing variants have the same behavior, so it does not silently remove those flags.

## The public and private halves are different features

`nier.bswap` is registered in the public dialect, admitted by the closed schema, checked by the consumer, and lowered to LLVM's `bswap` intrinsic.
The consumer checks operand/result agreement and the selected width. It does not need to rediscover a shift-and-mask idiom.

Our LLVM producer has another responsibility: recognizing supported native inputs that should become that operation. A capture may already contain a qualified LLVM `bswap` intrinsic.
Or it may contain an explicit collection of ANDs, shifts, and ORs. The latter needs a private normalization rule before the two target profiles can be paired conveniently.

Think of these as two independent contracts:

```text
public:   one integer -> reverse its bytes -> same-width integer
private:  prove this captured expression implements that public meaning
```

A language producer that emits `nier.bswap` directly only needs the first. An improvement to our captured-idiom recognizer does not necessarily require changing the public format at all.

## Work through the captured idiom

For a 32-bit value `x`, a typical expression isolates four bytes and shifts them into reversed positions.
In C-like notation, using unsigned values and appropriate constants, the idea is:

```c
((x & 0x000000ffu) << 24) |
((x & 0x0000ff00u) <<  8) |
((x & 0x00ff0000u) >>  8) |
((x & 0xff000000u) >> 24)
```

The implementation does not search for that source text or a function named `byte_swap`. It walks candidate LLVM expression graphs.

`normalizeNativeByteSwaps` first gathers suitable integer OR instructions, then considers outer expressions before their interior OR nodes.
Weak value handles prevent a previously deleted interior node from becoming a dangling worklist entry.

For each candidate, the proof collects a bounded graph of allowed operators.
Operands must have the same integer type.
Shift counts must be constant and smaller than the width.
AND masks must be constants. The graph must ultimately obtain its input from one function argument.

At low optimization levels, the argument may first be stored in a local stack slot and loaded repeatedly.
The recognizer handles a specific version of that pattern: one static scalar allocation, one ordinary initializing store of the argument, ordinary loads, no escaping pointer, and initialization dominating every relevant load.
This is not a general memory promotion pass.

Next comes the closed-use check.
Interior expression nodes cannot have users outside the candidate graph. Otherwise deleting the graph could remove a value needed elsewhere.
Relevant loads from the admitted slot must also belong to the graph.

Only after these checks does the code ask LLVM's idiom recognizer for help. It probes a temporary module, not the real capture, because the upstream recognizer can insert trial instructions and can recognize forms broader than our contract.
NieR accepts exactly the complete, same-width byte swap here, not a masked or partial swap.

Commit is small: insert the intrinsic, replace the root's uses, and delete only the now-dead proved expression.
The allocation and initializing store remain.
Unrelated instructions and effects remain. The implementation is deliberately more conservative than “this code probably looks redundant.”

## Rejected cases teach the boundary

Suppose one repeated load is volatile.
In C and LLVM, a volatile access is an observable access obligation, not just a way of retrieving an integer.
Turning several such loads into a single argument use would change the program. The recognizer therefore leaves this candidate unchanged.

Likewise, suppose an interior shifted byte is also passed to another function. The graph is no longer closed.
Deleting that intermediate would affect an external user. A second store to the local slot also invalidates the simple single-initialization argument.

“Unchanged” is an important result.
Failure to recognize an idiom does not mean the entire program must fail if its remaining scalar operations are otherwise representable.
Conversely, it is not permission to skip those operations. The ordinary merger must still represent and verify the program.

## The evidence stack

The normalizer's tests construct native LLVM graphs and check recognized counts, module validity, and idempotence.
For rejected candidates, they compare the module text before and after: a failed speculative proof must not partly rewrite the input.

For accepted graphs, the tests evaluate all single-bit basis inputs plus deterministic sample values and compare with byte reversal.
This supplies targeted evidence about the admitted transformation. It is not a universal equivalence engine for arbitrary programs.
The implementation's graph and use constraints remain essential.

Other layers check the public operation and paired publication. The producer specializes its common result back to both private targets and compares the admitted normalized native forms.
A real-corpus example comes from zlib's width-dependent byte-swap implementation.
Passing that corpus does not justify weakening a rejection rule for some unrelated shape.

## Debug by identifying the failing boundary

When a program fails, first distinguish these situations:

- Clang could not produce an admitted capture.
- Native profiles could not be correlated or normalized.
- The resulting NieR program violated its public contract.
- Target lowering or native LLVM verification failed.
- Native tools failed, or the executed program behaved incorrectly.

These are different engineering problems.
Changing a consumer verifier to silence a producer mismatch usually removes evidence rather than fixing the mapping.
Preserve the exact diagnostic and small reproducer before changing code.
Private captures can contain source information; do not attach them to a public artifact or publish them casually in a bug report.

For a future feature, write its semantic contract and rejection boundary before changing both sides.
Then demand independent public-core tests, producer tests if capture recognition is involved, positive execution where appropriate, and negative tests for nearby invalid cases.
Unknown operations and unknown attributes must not become silent no-ops.

## Optional independent lab

From the repository root in Bash, using an already built pre-alpha tree:

```sh
source sdk/env.sh
set -euo pipefail
guide14_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide14-XXXXXX")
ctest --test-dir build/prealpha \
  -R '^(ir_validation|producer_validation|byteswap_normalization)$' \
  --output-on-failure 2>&1 | tee "$guide14_work/tests.txt"
rg -n 'nier.bswap|byteSwapPrimitive|normalizeNativeByteSwaps' \
  include/nier/IR/Dialect.h src/ir/Compiler.cpp src/ir/Producer.cpp src/ir/ByteSwap.cpp
printf 'Lab files: %s\n' "$guide14_work"
```

This runs existing bounded tests, not the full corpus or a compiler rebuild.
Inspect the command's exit status if a test fails; a printed log is not a passing result.
Read one accepted and one rejected fixture in `tests/byteswap.cpp`, then predict what the normalizer will preserve before reading its assertions.

## Contributor recap and questions

You can now follow an operation across its public meaning, a private recognizer, native lowering, and layered tests.
That is the contributor exit: not knowing every compiler algorithm, but knowing which contracts a scoped change must preserve.

1. **Why is a volatile-load candidate rejected?** Replacing its accesses with an argument would erase observable accesses; the admitted proof does not permit that transformation.
2. **Why use a temporary module for recognition?** The upstream helper may mutate trial IR or recognize a broader idiom.
   Rejected speculation must not change the real capture.
3. **Does adding a private recognizer teach `nierc` about C?** No. It makes our producer better at emitting an already-defined generic operation.
4. **What does a failed negative test mean?** It may reveal incorrect acceptance or mutation on rejection—even if all happy-path executions still print the expected output.

## Guided source and evidence

Trace `ByteSwapOp` (`include/nier/IR/Dialect.h`), the schema and `nier.bswap` branch in the core (`src/ir/Compiler.cpp`),
intrinsic handling in the producer (`src/ir/Producer.cpp`), and the private recognizer (`src/ir/ByteSwap.cpp`).
Compare normalizer tests (`tests/byteswap.cpp`), public IR tests (`tests/ir.cpp`), and paired producer tests (`tests/producer.cpp`).
The next chapter begins the full maintainer track by asking where the native evidence comes from.

[Next: capturing native programs](../04-maintainers/15-capturing-native-programs.md)
