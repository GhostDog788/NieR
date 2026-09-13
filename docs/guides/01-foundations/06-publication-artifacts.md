# 06. From Sela Code to a Publication Artifact

[Series](../README.md) · [Previous](05-architecture-neutral-meaning.md) · [Next](../02-toolchain-users/07-development-environment.md)

## What you will understand

You will distinguish a Sela program, an in-memory module, serialized bytecode, and the standalone file exchanged between programs.
You will understand why the artifact contains a manifest and how modules differ from native translation units.
Read chapters 01–05 first.
No commands need to be run yet.

## One meaning, different representations

The previous chapters used readable Sela text.
During compilation, the same operation/type relationships live in MLIR data structures.
To exchange them between processes or machines, a producer serializes them into **bytecode**.
Serialization means encoding the compiler representation as bytes; it does not mean arranging a virtual machine to execute those bytes at runtime.

The project uses MLIR's bytecode infrastructure for its `.selabc` modules.
MLIR supports textual, in-memory, and serialized forms; Sela supplies the specific dialect semantics used here.
The [MLIR bytecode reference](https://mlir.llvm.org/docs/BytecodeFormat/) describes that infrastructure, not the complete Sela publication contract.

The standalone publication is another layer: a `.sela` file packages modules with their manifest and any specifically admitted ancillary members.
Do not confuse a module's bytecode file with the complete artifact that `selac` accepts as its application input.

## Open the conceptual envelope

The current multi-file Hello World artifact has this actual member structure:

```text
hello.sela                  ordinary tar archive
├── manifest.json           publication contract and module inventory
└── modules/
    ├── 0.selabc            first Sela module
    └── 1.selabc            second Sela module
```

“Single artifact” means this one independently transferable archive.
It does not require every source file to be flattened into one function or every module to be combined before optimization.
Internal structure can preserve important compilation boundaries while the publication remains one file.

The archive is not a tarball of the source tree.
The original C files, headers, private LLVM captures, and native application objects are not its fallback payload.
The prototype's accepted inventory is closed: unexpected members are rejected rather than ignored as harmless extras.

## What the manifest answers

The manifest tells the consumer what kind of output is requested, which semantic target domain is declared, what modules are present, and how those modules participate in native compilation.
It also records native dependency and link information allowed by the contract.

This **manifest excerpt** shows selected fields from the Hello artifact.
It is valid JSON for illustration, but not a complete accepted manifest because the remaining required fields are omitted:

```json
{
  "contract": "sela-prealpha-1",
  "format_version": 1,
  "kind": "executable",
  "targets": ["x86_64", "i686", "armv7", "aarch64"],
  "runtime": "glibc-2.39-0ubuntu8.8",
  "libraries": [],
  "link_options": []
}
```

The current contract strings are exact implementation facts, not permanent version promises.
The runtime field selects the currently supported supplied native runtime contract.
An empty `libraries` list does not mean the finished executable never uses libc: the SDK's native link arrangement supplies its baseline runtime.
Additional application dependencies have their own declared linking requirements.

Each module record names an exact archive member and its SHA-256 digest.
A digest binds the manifest's module record to those bytes.
It does not identify or authenticate a trusted publisher.
Someone who can change both the module and manifest can recompute a digest; signing policy belongs to a different part of the intended platform.

The implementation also requires its canonical JSON encoding.
A hand-edited manifest is not guaranteed accepted merely because an ordinary JSON parser can read it.
Chapter 22 explains why duplicate keys, unknown fields, exact inventories, and canonical encoding matter to robust input handling.

## Modules and translation units answer different questions

A C **translation unit** is the result of preprocessing one source input with its included headers and configuration.
An ordinary build can optimize each unit separately and then link the resulting objects.

A Sela **module** is a container of common operations, types, functions, and globals.
In the simple Hello example there are two source units and two modules, so a one-to-one mental model initially works.
It is not a universal format rule.

More complex profile builds can use different source inventories.
A source function set might be split across two units for one native profile and joined in one unit for another.
The common representation must not invent identical optimization boundaries merely because it has common code fragments.

The artifact therefore includes **compilation-unit plans** for its declared targets.
Each plan groups module indices into ordered native units and records the unit's optimization setting.
In Hello, all four plans group module zero by itself and module one by itself, at `O2`.

Here is **schematic notation, not JSON**, for the more general relationship:

```text
Shared fragments: A, B

Target one native units: [A], [B]
Target two native units: [A + B]
```

This describes grouping, not two embedded native programs.
The consumer reconstructs the selected target's native units before their optimization.
The current implementation only admits bounded cases it can prove; a plan shape does not grant arbitrary source-inventory correspondence.
Chapter 21 examines the actual checks.

It is also not an instruction to enable whole-program link-time optimization.
Preserving separate unit boundaries can be necessary to match the reference build.
“All code is somewhere in one archive” is not enough to justify changing how it is optimized.

## Output kinds and the confusing `.o` suffix

The current artifact kinds include executable, shared, static, and relocatable object publications.
A static output preserves member ordering so `selac` can produce a normal native archive.
A shared output carries the admitted shared-library contract.
The consumer must not interpret every `.sela` file as an executable request.

In direct stock-Clang Sela mode, a separate-compilation output named `main.o` can itself contain a relocatable **Sela artifact**, not a native ELF object.
The filename follows the compiler driver's usual workflow; the contents tell you which stage it belongs to.
Publication linking combines those Sela units into the final artifact.
`selac` rejects an object-kind artifact as a final native application input that still needs publication linking.

Later, `selac` privately creates real native `.o` files from specialized LLVM IR.
Those are different objects, at a different stage.
If a diagnostic says “object,” ask whether it means a private reference native object, a relocatable Sela publication, or a native object generated by the consumer.

## What can and cannot be left behind

Once publication has succeeded, target compilation should not need the developer's source tree, profile captures, or headers.
The artifact carries the common program and admitted contracts.
Once native compilation has succeeded, application execution should not need the artifact or the Sela compiler.
Native dependencies must still remain where the selected loader configuration expects them.

These are different independence checks.
Removing source and running `selac` tests the publication boundary.
Hiding the artifact and running the native executable tests the execution boundary.
Moving the runtime directory after linking is a separate relocation question; the current prototype does not promise that already linked native outputs follow it automatically.

Private diagnostic directories may intentionally retain source/debug evidence to explain a compiler decision.
They are useful to a maintainer but must not be confused with distributable artifacts.
The presence of strings and required symbol names in the public code is also not automatically a leak: programs often need those bytes to preserve behavior.
Privacy work distinguishes unnecessary evidence from necessary semantics rather than deleting all names.

## Optional paper exercise

For each item, choose publication input, public artifact content, consumer tool, or native execution dependency:
`hello.c`, a `sela.call` operation, `opt`, the supplied libc, a private debug type graph, and the manifest's module digest.
Some items are present in one development SDK directory but belong to different categories.

## Recap

A `.sela` file is a standalone package of serialized common code and explicit contracts.
Its manifest, module inventory, target plans, and native dependency information are meaningful compiler inputs.
A module is not necessarily a source translation unit, a digest is not a signature, and bytecode is not a runtime interpreter.
You now have the conceptual tools needed for the first real build and inspection exercises.

## Check your understanding

1. Why can one artifact contain several modules without violating the single-artifact goal?
2. Why keep a per-target compilation-unit plan if the code fragments are common?
3. Does deleting `hello.sela` after native compilation make the executable self-contained with respect to libc?

> [!faq]- Answers
>
> 1. The transfer boundary is the archive. Several internal modules preserve structure without requiring separate publication inputs at the destination.
> 2. Common functions can originate in different native unit groupings. Reconstructing those groupings preserves the admitted reference optimization and link contracts.
> 3. No. The application no longer needs the artifact, but it can still require the native loader and libraries selected during linking.
>
> For the paper exercise: `hello.c` and the private debug graph are publication inputs/evidence;
> the Sela operation and digest are public content;
> `opt` is a consumer compilation tool;
> supplied libc is a native execution dependency.

## Source and evidence trail

- Artifact API (`include/sela/Artifact/Artifact.h`) names modules and native unit plans separately.
- Artifact implementation (`src/artifact/Artifact.cpp`): read `createArtifact`, `validatePackage`, and `readCompilationPlan` when you reach the implementation track.
- Hello test (`tests/hello.sh`) checks separate publication, member inventory, exposure controls, and execution with the artifact hidden.
- Device boundary test (`tests/device-boundary.sh`) examines producer/consumer/execution separation beyond merely looking at the archive listing.

[Next: Preparing the Development Environment](../02-toolchain-users/07-development-environment.md)
