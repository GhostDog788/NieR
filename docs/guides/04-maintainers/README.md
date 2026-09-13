# Compiler-maintainer deep dives

[All guides](../README.md) · [Glossary](../reference/glossary.md)

These chapters explain the algorithms, rejection boundaries, and evidence needed to maintain Sela.
They build on the [contributor section](../03-contributors/README.md).
Read them in order for the full story, or use the groups below to find a particular subsystem.

## Recovering and specializing a common program

15. [Capturing the Actual Native Program](15-capturing-native-programs.md) — private evidence, capture timing, and identity.
16. [Recovering One Common Program](16-recovering-a-common-program.md) — correspondence, normalization, and inverse checks.
17. [Sharing and Specializing Control Flow](17-control-flow-sharing-and-specialization.md) — common graphs, conditional regions, and loops.

## Preserving native semantics

18. [Reconstructing Native Storage](18-native-storage.md) — records, arrays, overlapping storage, and layouts.
19. [Preserving Native Calling Conventions](19-native-calling-conventions.md) — logical signatures, ABI forms, and callbacks.
20. [Variadic Calls and Other Native Idioms](20-variadics-and-native-idioms.md) — variadics, intrinsics, and nonlocal control flow.

## Building, distributing, and qualifying the product

21. [Preserving Build and Link Semantics](21-build-and-link-semantics.md) — selected inputs, archives, and compilation boundaries.
22. [Artifact Validation and Compiler Robustness](22-artifact-validation-and-robustness.md) — bounded readers, validation, and failure behavior.
23. [The SDK and Compiler Distribution](23-sdk-and-compiler-distribution.md) — pinned dependencies, compiler bundles, and native runtime paths.
24. [Maintaining and Evolving Sela](24-maintaining-and-evolving-sela.md) — qualification, format changes, and future security work.

Return to the [full course index](../README.md) to revisit earlier topics.
