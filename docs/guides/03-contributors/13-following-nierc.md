# 13 — Following `nierc`

[Course index](../README.md) · [Previous: the developer-side compiler](12-developer-side-compiler.md) · [Next: testing, debugging, and features](14-testing-debugging-and-features.md)

## Objective and prerequisites

By the end of this chapter, you should be able to follow a publication through `nierc`, distinguish its validation and compilation stages,
and explain why the result is an ordinary native program.
You should already understand the producer/artifact/consumer distinction, basic Nier operations, and translation units. You do not need to understand the native ABI reconstruction algorithms yet; later chapters open those boxes.

The starting point is deliberately **after publication**. The input is a standalone `.nier` archive, not a C source file.
The destination also has a matching compiler and native SDK. It does not need our Clang plugin, private LLVM captures, source headers, or build replay machinery.

## One command, several responsibilities

Consider the already familiar command:

```sh
build/prealpha/nierc hello.nier -o hello
```

The executable named `nierc` is a driver around the public Nier core, artifact handling, and existing native tools.
It is not a second C compiler. Its work can be read as this sequence:

```text
archive
  -> validate package and requested target
  -> read common fragments and compilation-unit plan
  -> validate/specialize Nier for that target
  -> reconstruct each native translation unit
  -> LLVM optimization -> native objects
  -> native link or archive creation
  -> publish the completed output file
```

This sequence contains two different kinds of linking.
Reconstructing a translation unit joins common fragments that the publication says belong to one original optimizer input.
Native linking later combines objects and libraries.
Confusing those stages can accidentally introduce whole-program optimization or change archive selection. The compilation-unit plan keeps them separate.

The implementation entry point is `execute` in the consumer driver (`src/consumer/Main.cpp`).
Read it first as a sequence of requests to other components. You can understand its control flow without reading every lowering rule in `Compiler.cpp`.

## Establish the contract before compiling it

The driver first calls `readPackage` and `validatePackage`. An archive being readable does not establish that it is a valid Nier publication.
Package validation checks the manifest and permitted members, including the declared module records and their digests.
A checksum detects changed bytes relative to the manifest; it is not a trusted developer signature.

For compilation, the requested target must occur in the artifact's target domain.
Today's native-output command is qualified for x86-64. The diagnostic `lower` command can also expose i686 LLVM output.
That diagnostic capability is not an installed i686 product promise.

The artifact kind matters too. A relocatable Nier object is an intermediate publication input: the normal stock-Clang publication link must combine it before `nierc` emits a final native product.
Executable, shared-library, and static-library artifacts select different final output paths.

Next, the driver stages the declared module bytecode in a private scratch directory and reads the compilation-unit plan.
These are copies of the publication's common fragments, not recovered source or publisher captures.
Staging allows the existing compiler APIs and native tools to work with normal files while keeping incomplete products away from the requested output path.

There are further checks inside the core.
`readModule` reads bounded bytecode, performs MLIR structural verification, and invokes Nier's closed schema and semantic validation.
Unknown operations or attributes do not become optional merely because MLIR can print them.
In particular, opening a dump with `mlir-opt --allow-unregistered-dialect` is a viewing convenience, not Nier validation.

## What specialization actually fills in

Suppose a common function contains a value of type `!nier.word`.
On the current wide target, lowering creates an LLVM `i64`; on the narrow target it creates `i32`.
A fixed Nier `i32` stays 32 bits. A symbolic `pointer_bytes` constant becomes 8 or 4, whereas a literal integer 8 remains 8.

Those examples are small, but the same responsibility applies to native record offsets, array extents, ABI argument forms, and qualified conditional regions.
Target specialization is where the neutral contract becomes one target's concrete program. It does not infer missing source intent.

The core's `Lowerer` creates an LLVM module with the selected triple and data layout. It resolves declarations and globals, creates native blocks, maps Nier values to LLVM values, emits instructions, and verifies the resulting module.
Nier block arguments become LLVM PHIs where needed.
Native aggregate calling conventions are materialized inside the original functions and calls, rather than through a language runtime wrapper.

`lowerCompilationUnit` applies this process to the fragments in one declared unit and uses LLVM's linker to reconstruct that unit.
Its grouped-unit checks reject duplicate or ambiguous definition identities. This is not an opportunity to perform normal weak-symbol selection between fragments: the fragments are parts of a declared program unit, not competing libraries.

## Follow the two-file Hello program

Our Hello example has a `main` definition in one source file and `hello` in another.
After publication, the archive carries common code for both units and their final-link relationship.

For this ordinary two-unit case, the consumer creates native LLVM input for each unit separately. The call from `main` to `hello` remains a native symbol reference until native linking resolves it.
A call from `hello` into libc is resolved using the supplied native dependency environment.

The destination never asks which source file extension produced those functions. A direct Nier producer can create equivalent public operations.
That is why the independent-producer test is important: it exercises the contract without our C capture path.

Do not expect every instruction in a preoptimization dump to survive in the final executable. For example, normal optimization may simplify a printing call.
The native output should be compared with the corresponding native toolchain behavior, not with an assumption that every original call spelling must remain after optimization.

## Optimization, object generation, and final output

For each reconstructed unit, the driver invokes stock LLVM `opt` with the recorded optimization level and `-verify-each`.
That option asks LLVM to verify IR during its pass pipeline. It is valuable internal checking, but does not by itself prove semantic equivalence to the source program.

Stock `llc` then produces position-independent native object code. The driver maps the recorded optimization choice to a backend optimization level; it does not run a source-language frontend.
For executable and shared outputs, the SDK constructs the native LLD command, including the applicable runtime, libraries, and admitted link settings.

Static output follows a different path.
Stock `llvm-ar` creates an ordinary archive.
Members are staged separately so duplicate member names do not overwrite one another, and quick append preserves physical order.
A later native link can still perform lazy archive extraction.

Only after successful output construction does the driver replace the requested file. It checks input/output aliases, including filesystem identity where paths alone are insufficient.
These precautions prevent ordinary compiler mistakes from destroying inputs or replacing a previous successful output with a failed build.
They are not a production installation registry, and they are not runtime security enforcement.

## What remains after compilation

The resulting executable is loaded by the ordinary Linux ELF loader using the paths selected at native link time.
It does not open its `.nier` archive, run `nierc`, or invoke LLVM when the program starts.
There is no Nier bytecode interpreter or publication-specific JIT in this execution path.

Ordinary native dependencies can still be required. A dynamically linked Hello needs its selected libc and loader.
“No Nier runtime” does not mean “no libraries,” and language-blind compilation does not prohibit a future language from depending on a native garbage collector or other runtime code.

The current SDK and compiler-only bundle are qualified prototypes, not a finished solution for every Linux host or CPU.
Likewise, reading bytecode without source is not evidence that the artifact has native-equivalent reverse-engineering resistance.
Those are separate acceptance questions.

## Optional independent lab

Run this in Bash from the repository root, with the SDK and `build/prealpha` already built. It creates a fresh directory and does not depend on a previous chapter's outputs:

```sh
source sdk/env.sh
set -euo pipefail
guide13_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide13-XXXXXX")
clang --config="$PWD/build/prealpha/nier.cfg" -O2 \
  examples/hello/hello/main.c examples/hello/hello/hello.c -o "$guide13_work/hello.nier"
build/prealpha/nierc inspect "$guide13_work/hello.nier"
build/prealpha/nierc lower "$guide13_work/hello.nier" \
  --target x86_64 --output-dir "$guide13_work/native-ir"
rg -n 'target triple|target datalayout|define |declare ' "$guide13_work/native-ir"
build/prealpha/nierc "$guide13_work/hello.nier" \
  --sdk "$NIER_SDK_ROOT" -o "$guide13_work/hello"
env -u LD_LIBRARY_PATH "$guide13_work/hello"
printf 'Lab files: %s\n' "$guide13_work"
```

Expect `Hello world`. The diagnostic directory must not already exist; `lower` intentionally refuses to populate an existing output directory.
Inspect the two native units before looking at the final executable. The lowered files are diagnostic compiler output, not a replacement publication format.
Keep or remove this clearly identified temporary directory yourself.

## Recap and questions

The consumer validates a public artifact, specializes target-dependent semantics, preserves declared compilation-unit boundaries, and delegates ordinary native optimization/code generation/linking to existing tools.

1. **Why can `inspect` do more than list archive members?** It validates Nier modules and checks reconstruction of declared units across the artifact's target domain.
   It is contract inspection, not just `tar -t`.
2. **Why not combine every common module before optimization?** That can change the original translation-unit boundaries and silently enable additional cross-unit optimization.
   The manifest's unit plan is semantic input to compilation.
3. **Does `nierc lower --target i686` establish full i686 deployment support?** No. It exposes qualified specialization evidence; native product output is currently x86-64.
4. **Can the generated executable need libc but not need Nier?** Yes. Native dependencies and a publication-specific runtime are different things.

## Guided source and evidence

Start with `execute` (`src/consumer/Main.cpp`),
then follow `lowerCompilationUnit` (`src/ir/CompilationUnits.cpp`) into the core interfaces (`include/nier/IR/Compiler.h`) and lowering implementation (`src/ir/Compiler.cpp`).
Read the device-boundary test (`tests/device-boundary.sh`) to see how file accesses distinguish compilation from execution, and the independent producer (`tests/independent.cpp`) to check that C provenance is not a requirement.
The authoritative scope remains [02](../../02-implementation-plan.md), not this walkthrough's small example.

[Next: testing, debugging, and features](14-testing-debugging-and-features.md)
