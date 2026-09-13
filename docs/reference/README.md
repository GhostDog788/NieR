# Project reference

[Documentation home](../README.md) · [Learning guides](../guides/README.md)

These are the detailed operational references, kept inside the `docs/` Obsidian vault.
For compiler terminology, use the [glossary](../guides/reference/glossary.md).

- [Building Sela and setting up VS Code](building-sela.md) — SDK bootstrap, the standard build, tests, and editor configuration.
- [Development SDK](development-sdk.md) — host requirements, pinned packages, sysroots, environment, and provenance.
- [Existing-build publication](build-integration.md) — exact Make/CMake integration arguments and current restrictions.
- [Compiler-only distribution](compiler-distribution.md) — native compiler packages, release stripping, source/delivery receipts, size reporting, and qualification checkpoints.
- [Native targets and adding an architecture](adding-native-targets.md) — the four Linux/glibc targets, shared registry, ABI adapters, and equal qualification obligations.
- [Device-compiler retrospective](device-compiler-retrospective.md) — lessons for contributors and coding agents about build time, specialization, and the package-size regression.
- [Broad-C qualification corpus](qualification-corpus.md) — locked cJSON/zlib configurations, commands, and evidence requirements.
- [Native aggregate and variadic ABI evidence](native-abi-matrix.md) — native fixtures, observed signatures, implemented proofs, and remaining gates.
- [Current toolchain status](toolchain-status.md) — supported outputs, publication and runtime boundaries, evidence, and failure behavior.
- [GitHub presentation and maintenance](github-presentation.md) — branding assets, the recorded demo, CI, and repository settings.

Source paths mentioned in these references are relative to the repository root and are intended for VS Code.
Terminal commands also run from the repository root unless a reference explicitly names a different working directory.
