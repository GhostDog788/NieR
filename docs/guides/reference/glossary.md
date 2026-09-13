# NieR glossary

[Guide series](../README.md) · [Practical C-project guide](../02-toolchain-users/using-nier-with-your-c-project.md)

Use this page when a term interrupts your reading.
The definitions describe the current project unless marked as general compiler vocabulary.
They are not a second format specification; the linked chapters explain the examples, implementation, and limits.

## A–C

**ABI — application binary interface.** The native rules that separately compiled code must agree on: argument and result passing, record layout, alignment, symbol conventions, and more.
An ABI is more than an instruction set or pointer width.
See [chapter 19](../04-maintainers/19-native-calling-conventions.md).

**Alignment.** A constraint on an address, usually expressed as a multiple of a number of bytes.
Alignment can introduce padding between record fields and affect which native accesses are valid.
See [chapter 18](../04-maintainers/18-native-storage.md).

**Architecture-neutral.** Representing a program's meaning without fixing all native choices before a destination is selected.
In NieR this is a bounded claim over a declared target domain, not automatic support for every CPU.
See [chapter 05](../01-foundations/05-architecture-neutral-meaning.md).

**Archive.** A container of named members.
A `.nier` publication is a tar archive; a native `.a` static library uses a different archive format and participates in native link selection.
See [chapter 06](../01-foundations/06-publication-artifacts.md) and [chapter 21](../04-maintainers/21-build-and-link-semantics.md).

**Artifact.** The standalone publication file passed from a producer to `nierc`.
It contains a manifest and NieR bytecode, not the producer's private source tree or LLVM captures.
See [chapter 06](../01-foundations/06-publication-artifacts.md).

**Attribute.** Information attached to an MLIR operation, such as a constant value, function identity, or operation mode.
Unlike an operand, it is not a value produced by executing another operation.
See [chapter 03](../01-foundations/03-reading-a-nier-program.md).

**Backend.** The compiler part that turns a lowered program into code for a native target.
NieR uses LLVM's native code-generation machinery after its own specialization and lowering.
See [chapter 02](../01-foundations/02-compiler-foundations.md).

**Basic block.** A sequence of operations entered at its beginning and ended by a terminator.
Branches connect blocks into a control-flow graph.
See [chapter 04](../01-foundations/04-values-control-flow-and-memory.md).

**Block argument.** An SSA value supplied when control enters a block.
Branch operands can supply different values along different incoming edges, without reassigning one SSA value.
See [chapter 04](../01-foundations/04-values-control-flow-and-memory.md).

**Bytecode.** A binary encoding of an intermediate representation.
NieR uses MLIR bytecode for module payloads; it is not native CPU machine code and does not imply a bytecode interpreter at execution time.
See [chapter 06](../01-foundations/06-publication-artifacts.md).

**Capture.** Private producer-side evidence from a native compilation, including LLVM and associated build information.
Captures help the C producer recover NieR Code; they are not the public program given to the device.
See [chapter 15](../04-maintainers/15-capturing-native-programs.md).

**CFG — control-flow graph.** Blocks connected by possible transfers of control, such as conditional branches and loop backedges.
See [chapter 04](../01-foundations/04-values-control-flow-and-memory.md).

**Clang.** The existing C-family frontend and compiler driver used by NieR's C producer.
The publication entry point is actual stock Clang configured with the NieR integration, not a renamed replacement compiler.
See [chapter 12](../03-contributors/12-developer-side-compiler.md).

**Compilation unit.** Code grouped for one native compilation.
A publication's unit plan preserves the appropriate fragment grouping for each target rather than assuming every stored module should be optimized together.
See [chapter 21](../04-maintainers/21-build-and-link-semantics.md).

**Consumer.** The program or library that accepts the public NieR contract.
The standalone compiler `nierc` is a consumer; it does not need the original source-language frontend.
See [chapter 13](../03-contributors/13-following-nierc.md).

**Contract.** The explicit rules for what a representation means and which inputs are valid.
NieR's contract includes types, operations, attributes, target semantics, and artifact rules—not just a file suffix.
See [chapter 11](../03-contributors/11-implementing-the-nier-contract.md).

## D–L

**Data layout.** Rules for the size, alignment, and arrangement of data on a native target.
A record's total size alone does not explain where its fields live.
See [chapter 18](../04-maintainers/18-native-storage.md).

**Dependency.** Something required beyond the application's own code.
The developer SDK, compiler runtime dependencies, and application runtime libraries are different dependency sets.
See [chapter 23](../04-maintainers/23-sdk-and-compiler-distribution.md).

**Dialect.** An MLIR vocabulary of operations and types.
The custom NieR dialect is part of NieR's implementation; MLIR itself does not supply NieR's portability contract.
See [chapter 11](../03-contributors/11-implementing-the-nier-contract.md).

**Dominance.** A block dominates another when every path from the entry to the latter passes through the former.
SSA uses must have definitions available on the paths that reach them, not merely somewhere in the function.
See [chapter 04](../01-foundations/04-values-control-flow-and-memory.md).

**DSO — dynamic shared object.** A native shared library, normally a `.so` on Linux, resolved by ordinary native linking and loading.
A NieR-built program can still depend on native DSOs.
See [chapter 09](../02-toolchain-users/09-beyond-hello-world.md).

**ELF.** The native file format used here for Linux objects, executables, and shared libraries.
A `.nier` artifact is not an ELF executable, even when the producer was asked to publish an executable application.
See [chapter 08](../02-toolchain-users/08-hello-world-end-to-end.md).

**Fixed-width integer.** An integer type whose bit width stays fixed during target specialization: for example, `i32`.
Width alone does not specify signed versus unsigned interpretation for every operation.
See [chapter 05](../01-foundations/05-architecture-neutral-meaning.md).

**Fragment.** A stored portion of the public program referenced by compilation plans.
Its storage boundary does not necessarily equal an original source file or a native optimization boundary.
See [chapter 21](../04-maintainers/21-build-and-link-semantics.md).

**Frontend.** The compiler part that understands a source language and turns its program into an intermediate representation.
`nierc` is not a C frontend.
See [chapter 02](../01-foundations/02-compiler-foundations.md).

**GEP — get element pointer.** An operation for computing an address using layout and indexing relationships.
Computing an address is distinct from loading the memory there.
NieR has a corresponding `nier.gep` operation.
See [chapter 18](../04-maintainers/18-native-storage.md).

**IR — intermediate representation.** A compiler's structured description of a program between source text and machine code.
IR can be textual or binary; neither encoding determines its abstraction level.
See [chapter 02](../01-foundations/02-compiler-foundations.md).

**Inverse check.** Checking that specializing the recovered common program back to each private native profile reproduces the admitted normalized native contract.
This is bounded validation, not a general proof of arbitrary program equivalence.
See [chapter 16](../04-maintainers/16-recovering-a-common-program.md).

**JIT / AOT.** Just-in-time compilation generates code during program execution; ahead-of-time compilation generates it before that execution.
NieR's destination compilation is AOT even though it happens on the device: the resulting program runs as native code without a NieR execution engine.
See [chapter 01](../01-foundations/01-meet-nier-code.md).

**Linker.** The tool that resolves symbols and combines selected code and data into an output.
NieR publication linking and the later native LLD link are separate steps.
See [chapter 21](../04-maintainers/21-build-and-link-semantics.md).

**Loader.** The operating-system and runtime machinery that maps and starts a native executable and loads its native dependencies.
It does not compile NieR Code.
See [chapter 23](../04-maintainers/23-sdk-and-compiler-distribution.md).

**Logical / physical signature.** A logical signature expresses the values a function accepts and returns; a physical signature describes their native ABI carriers, including split values or hidden pointers when required.
See [chapter 19](../04-maintainers/19-native-calling-conventions.md).

**Lowering.** Transforming a representation into a more concrete one.
NieR lowering selects native meanings and produces LLVM IR before optimization, machine-code generation, and native linking.
See [chapter 13](../03-contributors/13-following-nierc.md).

**LLVM.** The compiler infrastructure providing an IR, optimizations, and native backends used in this project.
Ordinary LLVM IR can already contain target layout and ABI choices; having the same IR syntax is not enough for portability.
See [chapter 02](../01-foundations/02-compiler-foundations.md).

**LTO — link-time optimization.** Optimizing across compilation boundaries at link time.
Packaging multiple NieR modules together does not by itself authorize whole-program optimization or erase native compilation boundaries.
See [chapter 21](../04-maintainers/21-build-and-link-semantics.md).

## M–R

**Manifest.** The artifact's structured metadata: current format identity, module inventory and digests, target domain, dependencies, and compilation plans.
A digest checks consistency; it is not a publisher signature.
See [chapter 06](../01-foundations/06-publication-artifacts.md).

**MLIR.** Compiler infrastructure for defining and transforming multiple IR vocabularies.
NieR uses a custom MLIR dialect and its bytecode infrastructure; MLIR is not a source language or an execution runtime.
See [chapter 02](../01-foundations/02-compiler-foundations.md).

**Module.** A container of program definitions in the IR.
A module payload is one part of the publication archive, not necessarily the complete application or exactly one original C file.
See [chapter 06](../01-foundations/06-publication-artifacts.md).

**Native.** Using the selected target's machine instructions and ordinary binary interfaces.
Native does not mean statically linked, dependency-free, or universally relocatable.
See [chapter 08](../02-toolchain-users/08-hello-world-end-to-end.md).

**NieR Code / `nierc`.** NieR Code is the public architecture-neutral program representation.
`nierc` is the separate language-blind compiler consuming it.
Neither name denotes a replacement source language.
See [chapter 01](../01-foundations/01-meet-nier-code.md).

**Normalization.** Rewriting admitted native forms into a form suitable for comparison or common representation.
A normalization rule must preserve the relevant semantics and reject shapes it cannot justify.
See [chapter 16](../04-maintainers/16-recovering-a-common-program.md).

**Operand / result.** An operand is a value used by an operation; a result is a value defined by it.
An operation may have zero, one, or multiple results.
See [chapter 03](../01-foundations/03-reading-a-nier-program.md).

**Operation.** A structured IR node describing an action or definition.
In MLIR it can have operands, results, attributes, regions, and successors.
See [chapter 03](../01-foundations/03-reading-a-nier-program.md).

**Pass.** A compiler processing step that analyzes or transforms a program.
A pipeline orders passes so each receives the representation it expects.
See [chapter 12](../03-contributors/12-developer-side-compiler.md).

**PHI.** LLVM's way to select an SSA value according to the incoming control-flow edge.
MLIR block arguments can express the corresponding relationship without putting a PHI instruction in the block.
See [chapter 04](../01-foundations/04-values-control-flow-and-memory.md).

**Pointer / `!nier.ptr`.** A value used to refer to data storage or callable code.
NieR keeps the pointer type distinct from its native-width integer type; equal native widths do not make pointers and integers interchangeable in every operation.
See [chapter 05](../01-foundations/05-architecture-neutral-meaning.md).

**Pre-alpha.** The current development stage.
No backward compatibility is promised between commits; tools and artifacts must be regenerated together when the contract changes.
See [chapter 24](../04-maintainers/24-maintaining-and-evolving-nier.md).

**Producer.** Software that emits valid public NieR Code and artifacts.
The Clang integration is one producer; an independent producer can use the public core without private LLVM captures.
See [chapter 11](../03-contributors/11-implementing-the-nier-contract.md).

**Profile.** A concrete native configuration used by the C producer to gather evidence.
The current private profiles are x86-64 and i686; their existence does not mean the device reruns a language frontend.
See [chapter 15](../04-maintainers/15-capturing-native-programs.md).

**Publication.** Developer-side creation of the standalone artifact.
It is separate from destination compilation and from executing the resulting native program.
See [chapter 08](../02-toolchain-users/08-hello-world-end-to-end.md).

**Qualification.** Evidence that a specified implementation satisfies a bounded claim under stated conditions.
Passing a fixture or corpus does not prove every source language, target, or security objective is supported.
See [chapter 24](../04-maintainers/24-maintaining-and-evolving-nier.md).

**RAII / `llvm::Error` / `llvm::Expected<T>`.** C++ resource ownership and fallible-result patterns used throughout the repository.
RAII binds resource lifetime to an object; `Error` carries failure; `Expected<T>` carries either a value or failure.
See [chapter 10](../03-contributors/10-reading-the-repository.md).

**Region.** An MLIR container of blocks nested inside an operation.
A NieR function's body is a region; region semantics depend on the owning operation.
See [chapter 03](../01-foundations/03-reading-a-nier-program.md).

**Relocation.** In native linking/loading, information used to fix up addresses and symbol references.
Moving an installed compiler bundle is a different notion of relocation; it does not guarantee that previously linked executable paths remain valid.
See [chapter 23](../04-maintainers/23-sdk-and-compiler-distribution.md).

**RE — reverse engineering.** Recovering understanding from an artifact or binary.
Excluding source and private metadata is not encryption, nor proof that NieR artifacts resist analysis as well as native binaries.
See [chapter 01](../01-foundations/01-meet-nier-code.md).

**RPATH / RUNPATH.** Native executable or shared-library metadata influencing where the dynamic loader searches for libraries.
Compiler-tool library paths and application runtime paths serve different consumers.
See [chapter 23](../04-maintainers/23-sdk-and-compiler-distribution.md).

## S–W

**Schema.** The allowed structure and fields of a representation.
Structural schema validity is necessary but does not establish all semantic obligations, publisher trust, or execution safety.
See [chapter 22](../04-maintainers/22-artifact-validation-and-robustness.md).

**SDK / sysroot.** A software development kit supplies the selected tools and dependencies.
A sysroot supplies a target's headers and libraries.
Neither term alone promises a completely isolated or hermetic build.
See [chapter 07](../02-toolchain-users/07-development-environment.md).

**SENieR / SEN.** The planned security platform above the standalone NieR toolchain.
The `SE` prefix means “Security Enhanced,” as in SELinux; SEN is the short name.
SENieR is not implemented yet. Its signing and executable-memory enforcement policies are separate from ordinary NieR publication, compilation, and artifact validation.
Using NieR does not require SENieR.
See [chapter 01](../01-foundations/01-meet-nier-code.md) and [chapter 24](../04-maintainers/24-maintaining-and-evolving-nier.md).

**SONAME.** The native shared-library identity recorded for dynamic linking.
It is not necessarily the same as every filename or symlink used to find that library during a build.
See [chapter 21](../04-maintainers/21-build-and-link-semantics.md).

**Source-private.** Publishing without the original source files, developer paths, and private capture evidence.
Application strings and required external symbols can remain; this is not a claim of secrecy against all analysis.
See [chapter 06](../01-foundations/06-publication-artifacts.md).

**Specialization.** Resolving an admitted common program for one selected target: for example, determining a native word's width and record layout.
It is not choosing an opaque stored native program for that target.
See [chapter 17](../04-maintainers/17-control-flow-sharing-and-specialization.md).

**`sret` / `byval`.** LLVM ABI attributes involved in indirect result storage and by-value arguments passed through pointers.
Their full signatures, attributes, and storage relationships matter; seeing a pointer alone is not enough to infer an aggregate ABI rule.
See [chapter 19](../04-maintainers/19-native-calling-conventions.md).

**SSA — static single assignment.** Each IR value has one definition.
Source variable assignments can become different SSA values, block arguments, or memory operations.
SSA does not prohibit mutable application memory.
See [chapter 04](../01-foundations/04-values-control-flow-and-memory.md).

**Symbol.** A named identity used to refer to code or data across definitions and link boundaries.
An external symbol is different from the temporary printed name of an SSA value.
See [chapter 03](../01-foundations/03-reading-a-nier-program.md).

**Target domain.** The set of native targets an artifact claims to represent.
The current admitted semantic domain uses `x86_64` and `i686`; separate device compilers each produce only their own native target.
See [chapter 05](../01-foundations/05-architecture-neutral-meaning.md).

**Terminator.** The operation ending a block, such as a branch or return.
It determines how control leaves that block.
See [chapter 04](../01-foundations/04-values-control-flow-and-memory.md).

**Translation unit.** In C, one source file after preprocessing, including its included headers.
Native translation-unit boundaries influence compilation and must not be confused with publication storage boundaries.
See [chapter 21](../04-maintainers/21-build-and-link-semantics.md).

**Variadic function / `va_list`.** A function accepting trailing arguments beyond its fixed parameters, and the native mechanism for walking those arguments.
Their ABI behavior differs between targets and is not just a portable array of words.
See [chapter 20](../04-maintainers/20-variadics-and-native-idioms.md).

**Verification.** Checking stated structural or semantic invariants.
A NieR verifier is not a general vulnerability detector, trusted-publisher check, or substitute for the separate security enforcement platform.
See [chapter 22](../04-maintainers/22-artifact-validation-and-robustness.md).

**W^X — write xor execute.** A memory-protection policy forbidding memory from being writable and executable at the same time.
It does not by itself prohibit later write-to-execute permission changes.
Requiring executable mappings to come from approved signed files is a further policy.
NieR's ordinary toolchain flow is independent of the separately planned SENieR security enforcement work.
See [chapter 24](../04-maintainers/24-maintaining-and-evolving-nier.md).

**Word / `!nier.word`.** NieR's native-width integer type, specialized according to the selected target's pointer width.
A fixed `i32`, a native word, and a pointer express different meanings even when widths coincide.
See [chapter 05](../01-foundations/05-architecture-neutral-meaning.md).

Return to the [course index](../README.md), or start with the [practical C-project guide](../02-toolchain-users/using-nier-with-your-c-project.md).
