# Project reference

[Documentation home](../README.md) · [Learning guides](../guides/README.md)

These are the detailed operational references, kept inside the `docs/` Obsidian vault.
For compiler terminology, use the [glossary](../guides/reference/glossary.md).

- [Building Nier and setting up VS Code](building-nier.md) — SDK bootstrap, the standard build, tests, and editor configuration.
- [Development SDK](development-sdk.md) — host requirements, pinned packages, sysroots, environment, and provenance.
- [Existing-build publication](build-integration.md) — exact Make/CMake integration arguments and current restrictions.
- [Compiler-only distribution](compiler-distribution.md) — assembling and running the independent compiler bundle and understanding its runtime paths.
- [Broad-C qualification corpus](qualification-corpus.md) — locked cJSON/zlib configurations, commands, and evidence requirements.
- [Native aggregate and variadic ABI evidence](native-abi-matrix.md) — native fixtures, observed signatures, implemented proofs, and remaining gates.

Source paths mentioned in these references are relative to the repository root and are intended for VS Code.
Terminal commands also run from the repository root unless a reference explicitly names a different working directory.
