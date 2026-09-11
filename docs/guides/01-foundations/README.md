# Foundations: understanding Nier code

[All guides](../README.md) · [Glossary](../reference/glossary.md)

Start here to understand the representation before the implementation.
These chapters assume ordinary C programming experience, but no compiler background.
No SDK installation or hands-on work is required.

1. [Meet Nier Code](01-meet-nier-code.md) — why Nier exists and how publication, compilation, and execution differ.
2. [The Compiler Foundations](02-compiler-foundations.md) — IR, frontends, backends, linking, LLVM, and MLIR.
3. [Reading a Nier Program](03-reading-a-nier-program.md) — operations, values, types, and attributes.
4. [Values, Control Flow, and Memory](04-values-control-flow-and-memory.md) — SSA, blocks, branches, loops, and memory.
5. [Architecture-Neutral Meaning](05-architecture-neutral-meaning.md) — native widths, layout, ABI, and portability boundaries.
6. [From Nier Code to a Publication Artifact](06-publication-artifacts.md) — bytecode, manifests, modules, and compilation plans.

Continue with [using the toolchain](../02-toolchain-users/README.md), or jump straight to [publishing an existing C project](../02-toolchain-users/using-nier-with-your-c-project.md).
