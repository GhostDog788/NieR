# 20. Variadics and native idioms

[Series](../README.md) · [Previous: Native calling conventions](19-native-calling-conventions.md) · [Next: Build and link semantics](21-build-and-link-semantics.md)

## Objective and prerequisites

This chapter explains how Sela represents operations whose native LLVM shapes differ substantially between targets:
extracting variadic arguments, forwarding a native `va_list`, and recognizing selected compiler idioms.
It also explains why ordinary nonlocal jumps remain native calls rather than becoming a Sela runtime feature.

You should know the preceding chapter's distinction between logical and physical call signatures, and be comfortable following SSA values through a branch and join.
The maintainer skill to develop is recognizing a *complete state transition*.
A familiar-looking instruction sequence is not enough to justify replacing a subgraph with one common operation.

## Variadic calls are not an untyped byte stream

In C, the fixed parameters of a variadic function still have an ordinary prototype.
Values in its trailing argument list undergo default argument promotions.
In the tested cases, a `float` is passed as `double`, and the provided narrow integer values are retrieved as `int`.
A function receiving `...` must request compatible promoted types; Sela does not make an invalid `va_arg` expression defined.

This matters before target differences enter the picture.
A common operation returning an eight-bit integer directly from variadic state would not describe the tested C promotion contract.
The current `sela.va_arg` result domain is promoted 32-bit or 64-bit integers, native pointers, and `double`.
Support for fixed scalar calls, variadic calls, variadic function bodies, and aggregate variadic extraction are separate questions.

The state behind `va_list` is target-dependent.
In the pinned SysV x86-64 configuration it is represented by a one-element array of a record containing general-purpose and floating-point offsets, an overflow-area pointer, and a register-save-area pointer.
In the pinned i686 configuration it is a pointer cursor.
ARMv7 uses a pointer-containing record; AArch64 uses stack/register-area pointers and signed general/floating register offsets.
Shipping any one representation as the architecture-neutral meaning would make other targets imitate an implementation detail.

The public `!sela.va_list` type therefore describes variadic state, not a portable serialization of one platform's C header definition.
The consumer chooses the native representation for the selected target.
No Clang header or language identifier needs to accompany that state on the device.

## Worked case: retrieving a promoted integer

Consider the loop in `tests/abi/scalar_varargs.c`. For each group it retrieves an `int`, a `long long`, an `int *`, and a `double`.
Ten groups deliberately exercise register capacity and overflow storage.
The program copies the list and traverses both copies, checking equal results.

For one integer extraction, the i686 capture is comparatively small:
load the current cursor, compute the next cursor using the ABI's rounded size,
store the updated cursor, and load the requested value from the original address.
The result and the state update both matter.
Preserving the loaded integer but forgetting the cursor store would make the next extraction repeat it.

The x86-64 capture has a control-flow diamond.
It reads the relevant register offset and tests whether a register-save slot remains.
The register branch computes an address in the save area and advances that offset.
The overflow branch takes an address from the stack overflow area and advances its pointer.
A PHI selects the address used by the final value load.

For the qualified integer pattern, the general-purpose comparison uses the observed threshold 40 and advances a register position by 8.
For the qualified `double` pattern, the floating-point threshold is 160 and the register position advances by 16;
the overflow path advances by 8.
These numbers belong to the pinned native ABI pattern being recognized.
They are not public Sela operands which a future target must reproduce.

`src/ir/Varargs.cpp` first identifies candidate state objects through the native `vastart`, `vacopy` and `vaend` intrinsics.
It then checks a whole extraction: state layout, instruction ordering, addressing, alignments, comparison, increments, successor relationships, PHI inputs, and uses.
The recognized memory operations must be nonvolatile and nonatomic.
Every target's pattern is independently proved before it can become the same logical extraction.

Only after those checks does the producer replace the native expansion with a logical operation.
The requested type and state identity are the meaning to preserve.
The x86 lowerers can use LLVM's `VAArgInst` for the native state type.
ARM lowering emits the explicit native cursor transition: ARM32 alignment/advance, or the Linux AAPCS64 register/stack selection graph.
This distinction matters because LLVM 18's generic AArch64 `va_arg` lowering is not the Linux AAPCS64 implementation required here.
LLVM's ordinary target pipeline then optimizes and compiles those native operations; there is no interpreted argument reader installed beside the executable.

The inverse check then specializes the common program back to each native profile and compares the qualified normalized contracts.
Execution tests add another layer: retrieving one argument is not sufficient to test offset updates, so the fixture intentionally crosses both register-bank boundaries.

## Why a closed graph is required

Imagine that an extra store has been inserted into the register branch of the native extraction diamond.
A pattern matcher might still find the expected offset comparison, pointer PHI and final load.
Erasing the diamond would now erase that store.
Likewise, if another instruction observes the intermediate cursor or a block's address escapes, replacing the whole subgraph changes more than one logical extraction.

The helper's closure check requires consumed intermediate values to have only the admitted uses; the final extracted value is the intended escape.
Branch and join relationships are checked, and address-taken blocks are excluded from this rewrite.
These conditions are the reason the transformation is safe, not annoying restrictions to remove when another sample fails.

A rejected pattern should trigger investigation of the unmatched semantics.
It must not trigger a fallback which embeds the wide and narrow LLVM diamonds inside the public artifact.
Nor is it correct to expand the matcher until it silently consumes any neighboring instruction.
A broader admitted shape needs its own proof and positive and negative examples.

## Forwarding a native list is a different operation

The `tests/abi/va_forward.c` fixture starts and copies a list, forwards the copy to `vsnprintf`, and ends both lists.
Forwarding does not extract one argument; it hands native list state to an ordinary native library.

The qualified x86-64 ABI passes the array-state address.
The i686 ABI passes the cursor value stored in the local state slot.
ARM32 passes a one-element integer carrier, while AArch64 passes an owned indirect copy of the native state record.
Thus the same apparent pointer-to-state cannot simply be passed on every target.
`sela.va_forward` expresses this adaptation explicitly, including forwarding an already incoming native list argument.
It does not add a wrapper around `vsnprintf` or require a specially rebuilt libc.

The AArch64 producer recognizes only a proved fresh ABI-copy temporary with the exact copy, alignment, lifetime, and forwarding uses.
It folds that implicit copy into forwarding semantics; the consumer reconstructs the owned native copy.
A source-level `va_copy` remains a distinct operation and must not be erased as if it were an ABI shim.

Copying the list is also meaningful.
A second traversal must have its own properly initialized native state; duplicating an arbitrary byte count is not a general implementation of `va_copy`.
The retained native intrinsics and qualified state representation keep this responsibility in the normal target toolchain.
The tests exercise both a copied traversal and forwarding to a separately supplied native API.

Aggregate `va_arg` extraction remains unsupported by the integrated producer, even though native-only baseline programs demonstrate how Clang handles it.
An aggregate may consume multiple register classes or require temporary assembly from distinct save areas.
The scalar transition proof does not cover that behavior, and passing the fixed-record ABI matrix from chapter 19 is not a substitute.

## Other native idioms need semantic names, not textual matching

Memory intrinsics show a smaller but important target difference.
LLVM's overloaded names can include a length type, such as an `i64` versus `i32` variant of `llvm.memcpy`.
The producer recognizes the intrinsic identity and compatible signature, and Sela records the memory-intrinsic kind.
The consumer constructs the target's correct LLVM declaration.
Merely treating the two spelled names as unrelated external functions would miss their shared meaning.
Conversely, renaming arbitrary functions that resemble `memcpy` would invent that meaning without proof.

Byte reversal is another example.
The public `sela.bswap` operation has a bounded 16-, 32-, or 64-bit integer domain.
`src/ir/ByteSwap.cpp` recognizes an admitted native integer idiom, including the distinction between a native-word carrier and the actual bit domain being reversed.
The zlib CRC work required that distinction: a wider carrier does not automatically mean a 64-bit byte reversal.
The consumer emits the ordinary LLVM `bswap` intrinsic.

Do not generalize these examples into “all LLVM intrinsics are portable.”
Intrinsic signatures, effects, operand bundles, calling conventions, and attributes still need admission.
Floating-point flags are especially dangerous to drop: permitting reassociation or assumptions about exceptional values can change the contract.
Current acceptance is the closed set implemented and tested by the producer and core, not every intrinsic available in LLVM 18.

## Nonlocal jumps remain native behavior

The compiler does not implement `setjmp` and `longjmp` with a Sela exception interpreter.
Qualified calls and their semantic attributes remain native.
The nonlocal fixture (`tests/fixtures/nonlocal.c`) uses `setjmp` in a permitted `switch` expression, jumps across frames, nests live environments,
and checks the required conversion of a zero `longjmp` argument to a nonzero return.
Locals whose changed values must survive the jump are volatile.

This is important test design. An example relying on an indeterminate nonvolatile local after `longjmp` would be an invalid correctness oracle.
Likewise, merely returning normally from a function containing `setjmp` would not test its unusual control transfer.
`tests/nonlocal.sh` publishes the qualified cases at O0 and O2 and executes the development-host output.
The four-device fixture matrix executes the same artifact on each target, while the full cJSON suite retains Unity's normal nonlocal assertion mechanism.

These results establish the tested native behavior, not every platform's nonlocal-jump extensions or an entire language exception implementation.

## Optional independent lab

From the repository root with a prepared build:

```bash
source sdk/env.sh
varargs_lab=$(mktemp -d "${TMPDIR:-/tmp}/sela-guide-varargs-XXXXXX")
clang --config="$PWD/build/prealpha/sela.cfg" -O2 \
  tests/abi/va_forward.c -o "$varargs_lab/forward.sela"
build/prealpha/selac "$varargs_lab/forward.sela" -o "$varargs_lab/forward"
env -u LD_LIBRARY_PATH -u LD_PRELOAD "$varargs_lab/forward"
build/prealpha/sela_reference_lower lower "$varargs_lab/forward.sela" \
  --target x86_64 --output-dir "$varargs_lab/wide"
build/prealpha/sela_reference_lower lower "$varargs_lab/forward.sela" \
  --target i686 --output-dir "$varargs_lab/narrow"
printf 'Lab files: %s\n' "$varargs_lab"
```

Expect `Native va_list forwarding passed`.
The two-target inspection uses the publisher-only `sela_reference_lower` test helper; the native-only public compiler does not provide foreign-target lowering.
Compare the state layout and the argument passed to `vsnprintf` in the two lowered modules.
Do not expect their native pointer manipulation to be textually identical; preserving the common logical operation is what makes the native differences correct.

## Recap and questions

The useful abstraction is a proved state transition or intrinsic operation, not the target-specific instructions which happened to implement it.

> [!faq]- Why does the tested variadic function retrieve `double` for a supplied `float`?
>
> Default argument promotions change the value's call-boundary type.

> [!faq]- Why must a cursor update be included in the proof?
>
> Extraction changes which argument the next extraction observes; the loaded value alone is not the operation's whole effect.

> [!faq]- Why is forwarding not just passing the same pointer on every target?
>
> The qualified wide ABI passes a state address, while the narrow ABI passes its stored cursor value.

> [!faq]- Can a passing native aggregate-varargs baseline establish Sela support?
>
> No. It is a reference oracle; integrated public semantics and proofs remain separate obligations.

Read `src/ir/Varargs.cpp` beside `tests/varargs.cpp` and `tests/varargs.sh`.
For idiom boundaries, compare `src/ir/ByteSwap.cpp` with `tests/byteswap.cpp`, then locate the corresponding native operation branches in `src/ir/NativeLowering.cpp`.

[Previous: Native calling conventions](19-native-calling-conventions.md) · [Next: Build and link semantics](21-build-and-link-semantics.md)
