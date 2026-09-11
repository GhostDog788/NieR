# 11 — Implementing the Nier contract

[Series](../README.md) · [Previous: Reading the repository](10-reading-the-repository.md) · [Next: The developer-side compiler](12-developer-side-compiler.md)

## Objective and prerequisites

This chapter shows how Nier's public representation becomes an implemented, checked contract rather than just a file extension.
You will read a small Nier function, distinguish parsing from semantic validation, and follow the public API used by an independent producer.
Basic C++ ownership and LLVM error handling from the preceding chapter are enough; no Clang frontend knowledge is required.
The optional lab uses the already built independent-producer fixture and `nierc` in `build/prealpha`.

## A dialect supplies vocabulary; the contract supplies meaning

MLIR is infrastructure for representing and transforming intermediate code. A **dialect** is a registered family of operation and type names.
Nier's dialect uses the `nier` namespace: examples include `nier.func`, `nier.load`, and `!nier.word`. Its registration is visible in `src/ir/Dialect.cpp`.

An **operation** has operands, results, attributes, and possibly nested regions or successor blocks.
Operands are values it uses; results are values it defines.
Attributes describe information attached to the operation rather than values computed at runtime. A region contains blocks, and a block contains operations.
This is enough vocabulary to read a tiny function without knowing MLIR's full framework.

Nier deliberately uses MLIR's generic operation syntax.
Quoted operation names are not an opaque escape hatch: they name real registered operations.
Custom type syntax is parsed by `NIERDialect::parseType`; the operation definitions declare structural properties such as result count and whether an operation terminates a block.

Registration alone does not make every combination valid. The public contract also restricts allowed attributes, data types, source locations, target domains, control flow, and native semantics.
Those checks live principally in `src/ir/Compiler.cpp` and its consumer-safe helpers.

## Read one complete function

This is a minimal function returning zero in the current schema, shown as readable MLIR rather than as the binary publication payload:

```mlir
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({
    %zero = "nier.constant"() {value = 0 : i64} : () -> i32
    "nier.return"(%zero) : (i32) -> ()
  }) {id = "main", type = () -> i32,
      declaration = false, variadic = false,
      internal = false, dso_local = true,
      attributes = [[], []]} : () -> ()
}
```

The module carries the current Nier schema number.
`nier.func` has no ordinary operands or results of its own; its region holds the function body.
Its `type` attribute says the function accepts no arguments and returns an `i32` value. The `id` is the linkable function identity.

`%zero` is an SSA value: its definition occurs once, and later operations refer to that value rather than reassigning a source-language variable.
Its printed name is not an application variable that must be preserved.
The constant's attribute is stored as an `i64` integer attribute here, while the operation's declared result is `i32`; the verifier and lowerer interpret that combination according to Nier's constant rules.

`nier.return` consumes that `i32` and produces no result. It ends the block.
The function-level `attributes` array contains the supported function/return/parameter attribute slots; the zero-argument example has two empty slots.
These are explicit fields of the current schema, not optional decorations to guess at when writing a producer.

For native-width behavior, the existing independent fixture adds a function returning this pair:

```mlir
%width = "nier.constant"() {value = "pointer_bytes"} : () -> !nier.word
"nier.return"(%width) : (!nier.word) -> ()
```

`!nier.word` remains unresolved until a target is selected.
By contrast, an `i32` constant eight remains fixed. This distinction is the representation's meaning, not a convention based on the spelling of `%width`.

## Validation happens at several levels

Parsing checks whether bytes or text can form an MLIR structure. MLIR's structural verifier checks framework invariants.
Nier's schema checks then reject unknown operation/attribute combinations, disallowed private locations, and other violations of the publication contract.

`nier::verifyModule` goes further: for each declared semantic target, it attempts the supported lowering and verifies the specialized LLVM result.
An operation that is legal for one target but invalid for another cannot be published with both targets merely because the generic syntax parsed.

Public APIs default to the current `x86_64` and `i686` domain; artifact consumers pass the declared domain explicitly.
A thin destination compiler need only compile its requested supported target.
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

A producer does not have to generate LLVM captures. It can construct registered MLIR operations directly, or parse known Nier text into an MLIR module.
The public headers expose the same validation and serialization path in either case.

The following function illustrates the complete packaging portion, using real current APIs. The caller supplies a constructed module and keeps its context alive.
It should choose a fresh output path for this example.

```cpp
#include "nier/Artifact/Artifact.h"
#include "nier/IR/Compiler.h"

llvm::Error emitOneModule(mlir::ModuleOp module,
                          const nier::driver::fs::path &output) {
  using namespace nier::driver;

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
  auto bytecodePath = scratch->path / "module.nierbc";

  // writeModule validates before serializing the current Nier contract.
  if (auto error = nier::writeModule(module, bytecodePath.string()))
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
guide_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide11-XXXXXX")

build/prealpha/nier_independent_producer "$guide_work/independent.nier"
build/prealpha/nierc inspect "$guide_work/independent.nier"
tar -tf "$guide_work/independent.nier"
build/prealpha/nierc "$guide_work/independent.nier" \
  -o "$guide_work/independent"
env -u LD_LIBRARY_PATH "$guide_work/independent"
printf 'Native exit status: %s\n' "$?"

build/prealpha/nierc lower "$guide_work/independent.nier" \
  --target i686 --output-dir "$guide_work/narrow"
opt -passes=verify -disable-output "$guide_work/narrow/0.ll"
opt -passes=verify -disable-output "$guide_work/narrow/1.ll"
printf 'Independent-producer workspace: %s\n' "$guide_work"
```

The application prints no greeting: success is exit status zero. Its `main` checks a native-width helper and a fixed-eight helper from the other module.
This tests both target-dependent and fixed semantics across a public module boundary. It is not evidence that all potential language runtime semantics already fit the current dialect.

## Extending the contract is a coordinated change

Adding a type or operation usually needs a representation, parser/printer or registration changes, schema verification, target lowering, and positive and negative tests.
If the reference LLVM producer emits it, that producer also needs a justified recovery rule and inverse checks.
Do not make the consumer guess a missing rule from a source filename or language name.

Pre-alpha permits breaking changes without legacy readers. It does not permit inconsistent current readers and writers.
A maintainer should ask: can an independent producer express this feature, can each admitted target interpret it, and do malformed forms fail with a diagnostic rather than being silently accepted?

## Recap and check your understanding

MLIR supplies representation machinery. Nier supplies the vocabulary and checked semantics.
The public boundary is usable without Clang, but every producer must obey the current contract and honestly declare its supported target domain.

<details>
<summary>Is adding an operation name to Dialect.cpp enough to support a feature?</summary>

No. Structural registration must be accompanied by schema rules, semantics, target lowering, and tests. A producer needs its own correct emission path too.

</details>

<details>
<summary>Why does the independent producer call writeModule before createArtifact?</summary>

`writeModule` validates and serializes the Nier module.
`createArtifact` packages those byte strings with public metadata.
Code semantics and archive structure are different layers.

</details>

<details>
<summary>Does deleting source locations prove native-equivalent RE resistance?</summary>

No. It enforces a specific publication minimization rule.
Comparative information exposure and reverse-engineering resistance require the separate acceptance evidence defined in 01 and 02.

</details>

## Guided reading

Read `include/nier/IR/Dialect.h` and `src/ir/Dialect.cpp` together.
Follow `verifyModule`, `writeModule`, and `readModule` in `src/ir/Compiler.cpp`.
Compare `include/nier/IR/Compiler.h` and `include/nier/Artifact/Artifact.h` to the complete independent producer (`tests/independent.cpp`).
`tests/ir.cpp` contains valid and invalid contract examples; `tests/package.cpp` checks the archive layer independently.
