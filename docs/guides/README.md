# Learning NieR: from C programmer to compiler maintainer

NieR is a compiler project, a portable program representation, and a native publication workflow.
Understanding all three at once is difficult.
This series builds the ideas in dependency order: first understand what NieR Code means, then use it, then follow its implementation, and finally examine the algorithms and evidence a maintainer must preserve.

The intended reader has written small or medium Linux applications in C and used Make or CMake.
You should know functions, pointers, structs, libraries, and ordinary terminal commands.
You do **not** need previous knowledge of IR, SSA, compiler construction, LLVM, MLIR, or modern C++.
We introduce those ideas before relying on them.

## Just want to publish your C project?

Start with [Use NieR with your C project](02-toolchain-users/using-nier-with-your-c-project.md).
It is a standalone, command-oriented guide for existing Make and CMake applications: check the native build, configure the external integration, produce a `.nier` artifact, and compile and run it with `nierc`.
You do not need to read the compiler course first or rewrite your application's build files.
The examples include the current pre-alpha limitations and runtime requirements.

For a small project you can work through first, follow the [Hello project walkthrough](02-toolchain-users/hello-project-walkthrough.md).
It starts with the ordinary Make/CMake project in `examples/hello/hello/` and follows publication through native execution.

## Choose how far to go

| Reading path | Chapters | You should be able to… |
| --- | --- | --- |
| Foundations | 01–06 | Explain NieR's contract and read small NieR programs. |
| Practical user | 01–09 | Publish C applications, inspect artifacts, and understand native output and dependencies. |
| Contributor | 01–14 | Navigate the repository, trace a failure, and follow a feature across the producer, core, consumer, and tests. |
| Maintainer | 01–24 | Reason about the major algorithms, preservation obligations, rejection boundaries, and qualification evidence. |

These are stopping points, not separate competing tutorials.
Later chapters state their prerequisites so you can revisit a topic directly.
Reading the entire sequence gives the full path.
The [glossary](reference/glossary.md) is a quick lookup aid, not prerequisite reading.

## Browse by folder

The files are grouped by learning stage.
Each folder has its own short index; chapter numbers preserve the full reading order across folders.

- [01-foundations](01-foundations/README.md) — chapters 01–06: NieR Code and compiler concepts.
- [02-toolchain-users](02-toolchain-users/README.md) — chapters 07–09, the Hello project walkthrough, and the standalone Make/CMake project guide.
- [03-contributors](03-contributors/README.md) — chapters 10–14: repository, APIs, implementation flow, and testing.
- [04-maintainers](04-maintainers/README.md) — chapters 15–24: compiler algorithms, robustness, distribution, and qualification.
- [reference](reference/README.md) — glossary and quick terminology lookup.

## Part I — Understanding NieR Code

No SDK or build is required for these chapters.
Begin here even if you are mainly interested in the implementation: the C producer is one implementation of the public contract, not the definition of that contract.

1. [Meet NieR Code](01-foundations/01-meet-nier-code.md) — the problem, the public boundary, and the three distinct activities of publication, compilation, and execution.
2. [The Compiler Foundations](01-foundations/02-compiler-foundations.md) — frontend, IR, optimizer, backend, linker, loader, and the roles of Clang, LLVM, and MLIR.
3. [Reading a NieR Program](01-foundations/03-reading-a-nier-program.md) — read actual operations, values, types, attributes, and a complete small function.
4. [Values, Control Flow, and Memory](01-foundations/04-values-control-flow-and-memory.md) — SSA, blocks, branches, loops, dominance, and mutable memory.
5. [Architecture-Neutral Meaning](01-foundations/05-architecture-neutral-meaning.md) — native properties, fixed values, layout, ABI, and bounded portability.
6. [From NieR Code to a Publication Artifact](01-foundations/06-publication-artifacts.md) — bytecode, manifests, modules, dependencies, and compilation-unit plans.

## Part II — Using the real toolchain

7. [Preparing the Development Environment](02-toolchain-users/07-development-environment.md) — SDK, builds, environment, and VS Code.
8. [Hello World, Examined End to End](02-toolchain-users/08-hello-world-end-to-end.md) — real stock-Clang publication, artifact inspection, target compilation, and execution.
9. [Beyond Hello World](02-toolchain-users/09-beyond-hello-world.md) — separate compilation, libraries, dependencies, and existing Make/CMake projects.

## Part III — Understanding and contributing to the implementation

10. [Reading the Repository](03-contributors/10-reading-the-repository.md) — component ownership and the C++/LLVM vocabulary needed to read the code.
11. [Implementing the NieR Contract](03-contributors/11-implementing-the-nier-contract.md) — dialect, validation, serialization, and an independent producer.
12. [Following the Developer-Side Compiler](03-contributors/12-developer-side-compiler.md) — stock Clang, private captures, profile processing, and publication linking.
13. [Following `nierc`](03-contributors/13-following-nierc.md) — validation, specialization, LLVM, native linking, and the consumer boundary.
14. [Testing, Debugging, and Following a Feature](03-contributors/14-testing-debugging-and-features.md) — locate failures and follow the existing byte-swap feature through its implementation and evidence.

## Part IV — Compiler-maintainer deep dives

15. [Capturing the Actual Native Program](04-maintainers/15-capturing-native-programs.md) — capture timing, private evidence, dependencies, and identity.
16. [Recovering One Common Program](04-maintainers/16-recovering-a-common-program.md) — cross-profile correspondence, symbolic values, normalization, and inverse checks.
17. [Sharing and Specializing Control Flow](04-maintainers/17-control-flow-sharing-and-specialization.md) — common graphs, conditional regions, SSA correspondence, and loop identity.
18. [Reconstructing Native Storage](04-maintainers/18-native-storage.md) — records, globals, arrays, overlapping storage, packed fields, and layouts.
19. [Preserving Native Calling Conventions](04-maintainers/19-native-calling-conventions.md) — logical signatures, physical ABI forms, aggregate proofs, and callbacks.
20. [Variadic Calls and Other Native Idioms](04-maintainers/20-variadics-and-native-idioms.md) — promotions, variadic cursors, forwarding, intrinsics, and nonlocal control flow.
21. [Preserving Build and Link Semantics](04-maintainers/21-build-and-link-semantics.md) — selected inputs, archive order, differing source inventories, and native translation units.
22. [Artifact Validation and Compiler Robustness](04-maintainers/22-artifact-validation-and-robustness.md) — bounded readers, schema validation, output staging, and failure behavior.
23. [The SDK and Compiler Distribution](04-maintainers/23-sdk-and-compiler-distribution.md) — pinned dependencies, compiler-only bundles, native loaders, and relocation boundaries.
24. [Maintaining and Evolving NieR](04-maintainers/24-maintaining-and-evolving-nier.md) — evidence, qualification, format changes, extension obligations, and the separate future security platform.

## How to use a chapter

Read these guides with `docs/` open as the Obsidian vault and the repository root open separately in VS Code.
Local documentation links stay inside the vault.
Source references appear as repository-relative code paths, such as `src/consumer/Main.cpp`; open them with **Ctrl+P** in VS Code.
See the [documentation home](../README.md) for the reading and editing conventions.

Read the objective and prerequisites first.
Follow the worked example before opening the source references in VS Code.
The example gives you a question to investigate in the implementation; a list of filenames alone does not explain an algorithm.
Finish with the recap and comprehension questions.
Answers are included so the series works for independent study.
Answer callouts start collapsed in Obsidian's Reading view and Live Preview; click the title or arrow to reveal them.
They remain readable as blockquotes in GitHub and other Markdown viewers that do not support Obsidian's folding syntax.

When editing an answer, use `> [!faq]- Title` and prefix every body line, including blank lines, with `>`.
The minus sign sets the default to collapsed; keeping the entire body quoted ensures all of it folds together.

Labs are optional.
Later explanations never depend on you having completed an earlier exercise.
Practical chapters include setup or point to an explicit prerequisite rather than assuming an unexplained file already exists.
Commands use Bash from the repository root unless stated otherwise.
Run them on the documented SDK host, not inside a source directory of an unrelated application.
Temporary work is intentionally kept separate from tracked files.

An **excerpt** is real syntax with surrounding context omitted; it is not necessarily a standalone input.
**Pseudocode** explains an algorithm and is not a supported CLI or format.
Example output may abbreviate changing paths, digests, timings, and generated names.
Those differences are not test failures.

## Four distinctions to keep visible

Throughout the course, distinguish:

- **General principle:** a compiler concept independent of this repository.
- **Implemented behavior:** what the current source and tests actually do.
- **Qualified limitation:** a supported domain or shape beyond which the current implementation must not be assumed correct.
- **Planned capability:** an objective that still requires implementation and acceptance evidence.

For example, language-independent input is implemented, but supporting every source language is not.
The current semantic target domain is x86-64/i686; initial product native execution is x86-64.
The C producer's two private profiles do not establish ARM support.
Native program execution needs no NieR interpreter, but ordinary native dependencies still exist.

Source exclusion is not encryption or proof of native-equivalent reverse-engineering resistance.
A well-formed artifact is not a signed, trusted artifact.
Correctness checks are not SENieR (SEN), the separate security platform planned above NieR and not implemented yet.
These distinctions prevent a successful Hello World from being mistaken for completion of the whole product.

## Relationship to the rest of the documentation

[01 — Project Requirements](../01-architecture-design.md) specifies what the project must achieve.
[02 — Implementation Plan](../02-implementation-plan.md) describes the chosen approach, milestones, and current status.
This series teaches those ideas and their implementation; it does not replace either document or silently relax requirements when the prototype has a limitation.

For command-oriented reference, use the [documentation overview](../README.md), [SDK documentation](../reference/development-sdk.md), [build integration reference](../reference/build-integration.md), and [qualification corpus reference](../reference/qualification-corpus.md).
The guides add the reasoning needed to understand those references.

Everything is pre-alpha.
Code, artifact contracts, and these explanations can change together without compatibility with older commits.
Rebuild the tools and regenerate examples after a format change.
Prefer source paths and symbol names over treating a copied bytecode dump or old test count as a permanent specification.

Start with [01 — Meet NieR Code](01-foundations/01-meet-nier-code.md).
