# 11 — Implementing the Sela contract

[Series](../README.md) · [Previous: Reading the repository](10-reading-the-repository.md) · [Next: The developer-side compiler](12-developer-side-compiler.md)

## Objective and prerequisites

This chapter shows how Sela's public representation becomes an implemented, checked contract rather than just a file extension.
You will read a small Sela function, distinguish parsing from semantic validation, and follow the public API used by an independent producer.
Basic C++ ownership and LLVM error handling from the preceding chapter are enough; no Clang frontend knowledge is required.
The optional lab uses the already built independent-producer fixture and `selac` in `build/prealpha`.

## A dialect supplies vocabulary; the contract supplies meaning

MLIR is infrastructure for representing and transforming intermediate code. A **dialect** is a registered family of operation and type names.
Sela's dialect uses the `sela` namespace: examples include `sela.func`, `sela.load`, and `!sela.word`. Its registration is visible in `src/ir/Dialect.cpp`.

An **operation** has operands, results, attributes, and possibly nested regions or successor blocks.
Operands are values it uses; results are values it defines.
Attributes describe information attached to the operation rather than values computed at runtime. A region contains blocks, and a block contains operations.
This is enough vocabulary to read a tiny function without knowing MLIR's full framework.

Sela deliberately uses MLIR's generic operation syntax.
Quoted operation names are not an opaque escape hatch: they name real registered operations.
Custom type syntax is parsed by `SelaDialect::parseType`; the operation definitions declare structural properties such as result count and whether an operation terminates a block.

Registration alone does not make every combination valid. The public contract also restricts allowed attributes, data types, source locations, target domains, control flow, and native semantics.
Those checks live principally in `src/ir/Compiler.cpp` and its consumer-safe helpers.

## Read one complete function

This is a minimal function returning zero in the current schema, shown as readable MLIR rather than as the binary publication payload:

```mlir
module attributes {sela.schema = 1 : i32} {
  "sela.func"() ({
    %zero = "sela.constant"() {value = 0 : i64} : () -> i32
    "sela.return"(%zero) : (i32) -> ()
  }) {id = "main", type = () -> i32,
      declaration = false, variadic = false,
      internal = false, dso_local = true,
      attributes = [[], []]} : () -> ()
}
```

The module carries the current Sela schema number.
`sela.func` has no ordinary operands or results of its own; its region holds the function body.
Its `type` attribute says the function accepts no arguments and returns an `i32` value. The `id` is the linkable function identity.

`%zero` is an SSA value: its definition occurs once, and later operations refer to that value rather than reassigning a source-language variable.
Its printed name is not an application variable that must be preserved.
The constant's attribute is stored as an `i64` integer attribute here, while the operation's declared result is `i32`; the verifier and lowerer interpret that combination according to Sela's constant rules.

`sela.return` consumes that `i32` and produces no result. It ends the block.
The function-level `attributes` array contains the supported function/return/parameter attribute slots; the zero-argument example has two empty slots.
These are explicit fields of the current schema, not optional decorations to guess at when writing a producer.

For native-width behavior, the existing independent fixture adds a function returning this pair:

```mlir
%width = "sela.constant"() {value = "pointer_bytes"} : () -> !sela.word
"sela.return"(%width) : (!sela.word) -> ()
```

`!sela.word` remains unresolved until a target is selected.
By contrast, an `i32` constant eight remains fixed. This distinction is the representation's meaning, not a convention based on the spelling of `%width`.

## Validation happens at several levels

Parsing checks whether bytes or text can form an MLIR structure. MLIR's structural verifier checks framework invariants.
Sela's schema checks then reject unknown operation/attribute combinations, disallowed private locations, and other violations of the publication contract.

`sela::verifyModuleStructure` checks the entire public graph, including target-conditional regions that are inactive on this device.
`sela::verifyModule` goes further: for every explicitly requested native target, it lowers and verifies the specialized LLVM result.
Unknown, duplicate, empty, or unavailable native-target requests fail; there is no fallback that calls unavailable native validation successful.

Native-validation APIs require an explicit target list. `sela::supportedNativeTargets()` returns the implementations actually linked into this build, not every domain understood by the shared schema.
The publisher has both `x86_64` and `i686` implementations and proves both profiles. A thin destination compiler has only its own native implementation.
Its inspector can structurally admit a foreign-only artifact, but must report that the foreign native plan was not validated; compiling or lowering it rejects.
Do not confuse selected-target native compilation with independent certification of every other target's execution.

Validation also has a privacy boundary.
Published operation and block-argument locations must be unknown, rather than pointing into the developer's source tree.
Only admitted metadata belongs in the representation.
External symbols and application string literals still have legitimate semantic roles; removing source locations does not make an artifact impossible to reverse engineer.

Finally, there are file and archive checks.
Module reading uses bounded regular files and requires MLIR bytecode rather than arbitrary textual input.
Archive validation checks the current manifest, member inventory, digests, and limits.
Digests detect inconsistent bytes; they are not signatures establishing who published them. These are robustness and contract checks, not a compiler sandbox or the future security platform.

## The independent-producer API, worked through

A producer does not have to generate LLVM captures. It can construct registered MLIR operations directly, or parse known Sela text into an MLIR module.
The public headers expose the same validation and serialization path in either case.

The following function illustrates the complete packaging portion, using real current APIs. The caller supplies a constructed module and keeps its context alive.
It should choose a fresh output path for this example.

```cpp
#include "sela/Artifact/Artifact.h"
#include "sela/IR/Compiler.h"

llvm::Error emitOneModule(mlir::ModuleOp module,
                          const sela::driver::fs::path &output) {
  using namespace sela::driver;

  // Parsed text has parser locations; they are not publication metadata.
  auto *context = module.getContext();
  module.walk([&](mlir::Operation *operation) {
    operation->setLoc(mlir::UnknownLoc::get(context));
    for (auto &region : operation->getRegions())
      for (auto &block : region)
        for (auto argument : block.getArguments())
          argument.setLoc(mlir::UnknownLoc::get(context));
  });

  auto scratch = Scratch::create();
  if (!scratch)
    return scratch.takeError();
  auto bytecodePath = scratch->path / "module.selabc";

  // writeModule validates before serializing the current Sela contract.
  if (auto error = sela::writeModule(module, bytecodePath.string(),
                                    sela::supportedNativeTargets()))
    return error;

  auto bytes = read(bytecodePath);
  if (!bytes)
    return bytes.takeError();

  std::vector<ArtifactModule> modules;
  modules.push_back({std::move(*bytes), "O2"});
  auto artifact = createArtifact("executable", modules);
  if (!artifact)
    return artifact.takeError();
  return writePackage(output, *artifact);
}
```

The location walk is not a general-purpose scrubber that makes arbitrary MLIR acceptable.
Unsupported operations, attributes, or semantic shapes still fail validation.
Producers should construct only the contract they understand.

The `Scratch` object owns the temporary bytecode file.
Reading it transfers an owning byte string into `ArtifactModule`.
`createArtifact` constructs the public package metadata; the producer does not copy a publisher's manifest or invent private capture records.
`writePackage` writes the standalone archive.
The module serializer itself writes its requested file directly after validation,
which is why this example uses it inside private scratch rather than promising a transactional installed-release update.

The complete executable example is `tests/independent.cpp`. It parses two small modules, publishes them through these APIs, and requires no source language, Clang plugin, or LLVM merger.
It links the consumer-safe libraries. This is a stronger independence demonstration than hiding a call to the C publisher behind a new command name.

## Optional lab: consume an artifact with no C publication step

Run from the repository root in Bash. The producer executable is a test fixture, not an installed public command-line product.

```bash
source sdk/env.sh
guide_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-guide11-XXXXXX")

build/prealpha/sela_independent_producer "$guide_work/independent.sela"
build/prealpha/selac inspect "$guide_work/independent.sela"
tar -tf "$guide_work/independent.sela"
build/prealpha/selac "$guide_work/independent.sela" \
  -o "$guide_work/independent"
env -u LD_LIBRARY_PATH "$guide_work/independent"
printf 'Native exit status: %s\n' "$?"

build/prealpha/sela_reference_lower lower "$guide_work/independent.sela" \
  --target i686 --output-dir "$guide_work/narrow"
opt -passes=verify -disable-output "$guide_work/narrow/0.ll"
opt -passes=verify -disable-output "$guide_work/narrow/1.ll"
printf 'Independent-producer workspace: %s\n' "$guide_work"
```

The application prints no greeting: success is exit status zero. Its `main` checks a native-width helper and a fixed-eight helper from the other module.
The final narrow inspection uses the publisher-only `sela_reference_lower` test helper, not a foreign-target mode of the public native-only `selac`.
This tests both target-dependent and fixed semantics across a public module boundary. It is not evidence that all potential language runtime semantics already fit the current dialect.

## Extending the contract is a coordinated change

Adding a type or operation usually needs a representation, parser/printer or registration changes, schema verification, target lowering, and positive and negative tests.
If the reference LLVM producer emits it, that producer also needs a justified recovery rule and inverse checks.
Do not make the consumer guess a missing rule from a source filename or language name.

Pre-alpha permits breaking changes without legacy readers. It does not permit inconsistent current readers and writers.
A maintainer should ask: can an independent producer express this feature, can each admitted target interpret it, and do malformed forms fail with a diagnostic rather than being silently accepted?

## Recap and check your understanding

MLIR supplies representation machinery. Sela supplies the vocabulary and checked semantics.
The public boundary is usable without Clang, but every producer must obey the current contract and honestly declare its supported target domain.

> [!faq]- Is adding an operation name to Dialect.cpp enough to support a feature?
>
> No. Structural registration must be accompanied by schema rules, semantics, target lowering, and tests. A producer needs its own correct emission path too.

> [!faq]- Why does the independent producer call writeModule before createArtifact?
>
> `writeModule` validates and serializes the Sela module.
> `createArtifact` packages those byte strings with public metadata.
> Code semantics and archive structure are different layers.

> [!faq]- Does deleting source locations prove native-equivalent RE resistance?
>
> No. It enforces a specific publication minimization rule.
> Comparative information exposure and reverse-engineering resistance require the separate acceptance evidence defined in 01 and 02.

## Guided reading

Read `include/sela/IR/Dialect.h` and `src/ir/Dialect.cpp` together.
Follow `verifyModule`, `writeModule`, and `readModule` in `src/ir/Compiler.cpp`.
Native lowering is implemented separately in `src/ir/NativeLowering.cpp`; the destination build links only its selected target specialization.
Compare `include/sela/IR/Compiler.h` and `include/sela/Artifact/Artifact.h` to the complete independent producer (`tests/independent.cpp`).
`tests/ir.cpp` contains valid and invalid contract examples; `tests/package.cpp` checks the archive layer independently.
