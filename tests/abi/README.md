# Native aggregate and variadic ABI evidence

These are unchanged C programs, compiled with stock pinned Clang through the
ordinary native ABI. The baseline scripts below are **native-only evidence**;
the separate producer, consumer and public-pipeline gates qualify the
implemented Nier boundary support. No failed Nier import is counted as a
passing qualification test.

Run from the repository root:

```sh
bash tests/abi/native.sh "$PWD/build/prealpha/nier-capture.so" "$PWD/.sdk"
```

The script executes x86-64 and i686 native binaries at O0/O2. Each program
links an independently built native shared library which invokes application
callbacks. Private preoptimization LLVM captures and readable `.ll` files are
retained in the printed temporary directory. The snapshot does not use the
Nier source importer or alter the native function ABI.

The scalar-only variadic baseline is independent of the aggregate matrix:

```sh
bash tests/abi/scalar-native.sh "$PWD/build/prealpha/nier-capture.so" "$PWD/.sdk"
```

`scalar_varargs.c` covers default promotions, int/i64/pointer/double retrieval,
both register-bank overflows, and copied-list traversal. `va_forward.c`
separately forwards an ordinary native `va_list` to `vsnprintf`. Both run at
O0/O2 on both native widths and retain pristine captures for the positive Nier
tests. Aggregate `va_arg` extraction remains a separate, future qualification;
these native baselines alone do not claim any Nier importer support.

## Current qualification status

| Gate | Evidence |
| --- | --- |
| Unchanged native baseline | x86-64/i686 at O0/O2, including a separately built native callback library |
| Ordinary-record entry/call/result proof | Complete signatures and closed storage shims; negative tests retain or reject unmatched effects |
| Native materialization and inverse | Exact normalized native-to-common-to-native comparison on both widths at O0/O2 |
| Native callback execution | Regenerated application boundaries linked to an unchanged native shared library, both widths at O0/O2 |
| Shared Nier artifact and core-only consumer | Fixed-argument main, boundary and bridge units merge with both strict inverses; the same Nier units execute on both widths at O0/O2 |
| Public stock-Clang publication | `aggregate_pipeline` passes Clang to standalone `.nier` to `nierc`, plus a stock-native caller of a Nier-produced DSO, on x86-64 at O0/O2 |
| Aggregate variadic extraction | Native-only baseline; not qualified by the aggregate importer |
| Union/packed/bitfield fixed by-value boundaries | Explicit classifier and native baselines pass; producer/core boundary integration is not yet qualified |

`fixed_main.c` isolates the fixed-argument matrix; it does not replace or
weaken the original `main.c`/`varargs.c` native baseline. Union and packed or
bitfield **storage** have separate positive Nier pipeline tests; those tests
do not establish their by-value calling convention support.

These are qualified fixture shapes, not every ABI combination or general C
coverage. Private i686 specialization/execution is reference evidence, not an
installed i686 product claim. No gate here establishes the complete 01
product, native-performance parity, RE/privacy parity, or security enforcement.

## Matrix

| Fixture | What it checks |
| --- | --- |
| `Pair { int, int }` | Integer aggregate coercion, expanded inputs, hidden return storage |
| `Mixed { double, int }` | Split floating/integer aggregate inputs and results |
| `Large { double, double, long }` | Memory argument/result, word-sized field and target-dependent size |
| Direct and indirect transforms | Entry, argument packing, result unpacking, ordinary function pointers |
| Native shared-library bridges | Native library calls back into application code without an adapter ABI |
| Five versus six preceding integers | Integer register-to-stack transition |
| Six integers and eight doubles before `Mixed` | Aggregate falls back to stack memory after register-bank exhaustion |
| Ten variadic int/double pairs | Both variadic register banks and overflow stack are exercised |
| Variadic Pair/Mixed/Large with zero/eight prefix pairs | Register, split-bank, and overflow aggregate `va_arg` paths |

## Actual pinned preoptimization LLVM shapes

The following are observed shapes, not deductions from record names. Logical
types come from layout-checked debug type graphs and parameter ordinals.

| Logical boundary | x86-64 LLVM | i686 LLVM |
| --- | --- | --- |
| Pair input | One `i64` | Two `i32` values |
| Pair result | `i64` result | Hidden `sret(Pair)` pointer, align 4; `void` result |
| Mixed ordinary input | `double`, `i32` | `double`, `i32` |
| Mixed result | `{ double, i32 }` result | Hidden `sret(Mixed)` pointer, align 4; `void` result |
| Mixed input after both register banks fill | `byval(Mixed)` pointer, align 8 | `double`, `i32` |
| Large input/result | `byval` plus hidden `sret`, align 8, size 24 | `byval` plus hidden `sret`, align 4, size 20 |

Pair still has an `i64` LLVM parameter after six preceding integer arguments;
LLVM's backend assigns it to the stack. This case must not be incorrectly
rewritten to `byval`. Mixed's memory fallback really changes its LLVM
signature. Physical classification therefore depends on the complete logical
signature, including earlier arguments and hidden result parameters, not
only on the aggregate type.

## Implemented bounded ordinary-record normalization

Keep native ABI lowering in target compiler code, not captured LLVM snippets
or per-target executable payloads in the public artifact. A public logical
signature needs aggregate value types, native calling-convention identity,
fixed/variadic argument distinction, and admitted semantic attributes.
Private per-target matching recipes can prove capture correspondence; they
are not a second program shipped inside Nier.

1. Recover the logical signature from `DISubprogram` / `DISubroutineType`,
   unwrapping typedefs and qualifiers. Match `DILocalVariable::arg` ordinals,
   `dbg.declare`, and whole-address `dbg.assign` data to physical parameter
   stores; fragmented debug locations are not admitted by this rule.
   Validate field layouts against LLVM types/data layout in each
   profile. Names, paths, line numbers and typedef spelling are not keys.
   Debug hints propose a mapping; inverse checking, not debug data alone,
   must establish that the mapping preserves the captured program.
2. Represent each logical aggregate parameter/result as local storage while
   matching the common body. Recognize only these entry templates initially:
   a coerced integer stored into its aggregate slot; scalar pieces stored via
   constant field GEPs; or a typed `byval` parameter already serving as that
   slot. Do not turn a by-value object into an alias of the caller's storage.
3. Recognize a native result as either a typed hidden `sret` argument or a
   local aggregate result slot loaded into the native return representation.
   i686 also emits a dead hidden-result-pointer spill in these fixtures;
   remove it only after proving its stores have no other uses. Both captured
   and regenerated native forms must pass that same proof before comparing
   their canonical bodies; lowering need not invent the dead spill again.
4. At each direct/indirect call, match argument loads to their complete
   aggregate storage origins. Match the result as an `sret` destination, an
   integer store, or an `extractvalue`/field-store sequence. Scalar arguments
   and evaluation order remain unchanged. Consumed shim values may have no
   extra uses outside the exact template and its logical body anchor.
5. Derive the same logical callable signature for indirect calls and native
   callback exports. External prototypes are not reliably retained in capture
   debug data, so call proof uses complete packing/storage anchors plus exact
   full-signature native classification. A raw LLVM opaque `ptr`, symbol name
   or debug hint alone does not establish the callable ABI.
6. Re-emit each target's original native entry/call/return form inside the
   original function. This is compiler lowering, not a wrapper function,
   boxed runtime interface, trampoline or altered native callback ABI.

Definition and call-site attributes differ: hidden result parameters in these
definitions include `noalias`, whereas the corresponding call operands do
not. Preserve `sret`/`byval` types, alignments, `noundef`, `writable`,
`dead_on_unwind` and their attachment sites independently. Copying all
definition attributes onto calls is not an acceptable shortcut.

Mixed has 16-byte storage on x86-64 and 12-byte storage on i686. Large is
24/20 bytes. Preserve body copies and observable storage semantics through
symbolic record sizes/alignments; do not zero, discard or invent padding as
part of ABI cleanup.

## Variadic evidence for the shared normalizer

x86-64 `va_list` is a 24-byte record containing GP/FP offsets, overflow pointer
and register-save pointer. Scalar integer retrieval checks GP offset `<=40`,
advances the register position by 8, or advances overflow by 8. Double
retrieval checks FP offset `<=160`, advances the register position by 16, or
advances overflow by 8. Each retrieval has a register/overflow diamond and
pointer PHI before its load.

Pair uses one GP save slot. Mixed checks both offsets, assembles a temporary
record from distinct GP/FP save slots, or consumes 16 overflow bytes. Large
always consumes 24 overflow bytes. i686 uses one pointer, advancing by 4 for
int, 8 for double/Pair, 12 for Mixed and 20 for Large, without those diamonds.
The common operation must describe the requested logical type and `va_list`
state transition, not preserve either profile's incidental CFG as the public
program. Aggregate vararg call packing must also honor register exhaustion.

## Required inverse checks before qualification

For each accepted capture pair, lower the common body back to **both** native
forms and compare full function signatures, calling convention, parameter and
call attributes, native memory operations/layouts, control/dataflow, and
remaining semantic metadata against the original captures. Captured and
regenerated ABI shims must independently satisfy the same admitted semantic
templates before their normalized forms are compared. Incidental entry GEP
spelling or proved dead spills may differ; normalization cannot silently
absorb unrelated instructions or side effects.

After that structural proof, run the same fixed-argument matrix with Nier-produced
application code and the unchanged native bridge library, on both targets and
at both optimization levels. The positive helper/core-only execution gates
and public publisher/consumer gate now exist separately; the native-only
checks above do not substitute for them. Packed/union/bitfield by-value
records, vector coercions, unusual calling conventions and ambiguous pointer
flows need further integrated proofs and positive tests before claiming
coverage.

`AggregateNormalize.*` contains producer-only discovery and closed graph
proofs. `NativeABIBridge.*` materializes native boundaries in the language-blind
consumer. The public `native_abi` logical signature accompanies an ordinary
owned-storage body; it does not carry source debug data, native instruction
snippets, wrappers or a separate runtime ABI. Independent-producer IR tests
exercise that public contract without Clang provenance.

```sh
bash tests/aggregate-normalize.sh "$PWD/build/prealpha/aggregate_normalize_tests" "$PWD/.sdk"
bash tests/aggregate-native.sh "$PWD/build/prealpha/aggregate_normalize_tests" "$PWD/.sdk"
bash tests/aggregate-pipeline.sh "$PWD/build/prealpha/nierc" "$PWD/.sdk" "$PWD/build/prealpha/nier.cfg"
```

The normalizer tests include volatile/excessively aligned piece accesses,
extra uses, intervening stores, unmatched attributes and forged native/debug
record layouts. Rejected proofs must not mutate the module. Qualified-void
debug metadata from pointer-only callbacks must remain harmless and unchanged.
An ordinary scalar call result stored into a record field must not be mistaken
for an aggregate return; direct external and indirect pointer/integer cases,
including one-field records, are checked on both widths. Scalar-coerced native
aggregate returns require the admitted direct whole-object store anchor;
an ordinary field-address store is not sufficient evidence.

## Implemented classifier gate

`src/ir/AggregateABI.*` is the language-blind, consumer-safe native ABI
classifier and conservative piece load/store library. Run its standalone
descriptor tests and the pinned-Clang correspondence gate with:

```sh
build/prealpha/aggregate_abi_tests
bash tests/aggregate-abi.sh "$PWD/build/prealpha/aggregate_abi_tests" "$PWD/.sdk"
```

The latter checks exact native function and indirect-call signatures plus
typed `sret`/`byval` placement and alignment on both widths at O0/O2. In
addition to the main matrix, `classifier.c` covers two-float SSE coercion,
separate/both register-bank exhaustion, hidden-result register pressure,
whole-aggregate register rollback, nested records and three-byte coercion.
Helpers reject malformed descriptors before emitting IR and copy only the
declared pieces, with no invented padding initialization.

The classifier takes an explicit list of logically ordered non-overlapping
records. This is a caller proof obligation, not a way to reinterpret arbitrary
LLVM union storage as a record. Packed/empty/vector-containing records and
source-only over-alignment or nontrivial class semantics remain outside this
classifier contract. Each admitted producer/consumer shape must still satisfy
the full inverse/body checks above; matching a signature alone does not make
an entire source program a qualified Nier program.

The implementation rules were checked against the pinned upstream
[Clang 18.1.3 x86 ABI implementation](https://github.com/llvm/llvm-project/blob/llvmorg-18.1.3/clang/lib/CodeGen/Targets/X86.cpp),
particularly the i686 expansion predicate and SysV64 eightbyte classification,
piece selection, indirect fallback and complete-signature register accounting.

## Explicit overlapping and packed layout classifier

`AggregateLayoutABI.cpp` adds a separate semantic-layout API without changing
the ordered-record contract. Its descriptor supplies selected-target field
types, bit offsets/widths, union alternatives, source size and alignment;
an LLVM storage surrogate alone is never sufficient evidence of these facts.

```sh
build/prealpha/aggregate_layout_tests
bash tests/aggregate-layout.sh "$PWD/build/prealpha/aggregate_layout_tests" "$PWD/.sdk"
bash tests/abi/extended-native.sh "$PWD/build/prealpha/nier-capture.so" "$PWD/.sdk"
```

The gates cover integer/floating unions, a union whose integer alternative is
present only on the wide target, two-eightbyte alternatives, ordinary and
cross-eightbyte packed bitfields, aligned/unaligned packed fields, native
callbacks and full-signature register pressure. The classifier gate compares
exact stock-Clang signatures and attributes; the native execution gate remains
independent evidence, not a substitute for producer proof and round-trip tests.
Descriptors with unproved layout, overlapping ordered fields, cycles, invalid
bit ranges or unsupported empty values are rejected before classification.
