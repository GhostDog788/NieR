# Sela documentation

Open this `docs/` directory as the **Obsidian vault**.
Open the **repository root** separately in VS Code for source code, builds, and terminal commands.
The checked-in Obsidian settings belong to this documentation vault; there is no need to move the vault or import the source tree into it.

## Start here

- [Work through the Hello project](guides/02-toolchain-users/hello-project-walkthrough.md) — start with a normal Make/CMake project and follow its Sela publication and native execution.
- [Publish your C project](guides/02-toolchain-users/using-sela-with-your-c-project.md) — the practical Make/CMake route from an existing application to a Sela artifact and native executable.
- [Learn Sela](guides/README.md) — the complete course, from Sela Code fundamentals to compiler-maintainer details.
- [Build Sela](reference/building-sela.md) — prepare the SDK, build the tools, and set up VS Code.
- [Reference documents](reference/README.md) — SDK, integration, distribution, qualification, and ABI details.
- [Glossary](guides/reference/glossary.md) — quick terminology lookup.

## Requirements and implementation plan

[01 — Project requirements](01-architecture-design.md) defines what Sela must achieve.
[02 — Implementation plan](02-implementation-plan.md) describes the implementation approach and its current qualification boundaries.
The guides explain these documents; they do not replace them.
Sela is the standalone format and toolchain; SESela (SES) is the separate, not-yet-implemented security platform planned above it.

## Links and source references

Local documentation links lead to Markdown files inside this vault.
External web links open the browser.
Source files, build scripts, and configuration files are shown as repository-relative code paths, such as `src/consumer/Main.cpp`.
They are deliberately not Obsidian note links: Obsidian is for the documentation, and VS Code is for the code.

To inspect one of those files, focus VS Code with the repository root open, press **Ctrl+P**, paste the path, and press **Enter**.
These paths work regardless of where a reader cloned the repository; no machine-specific file URLs are needed.
Run documented terminal commands from that repository root unless the instructions explicitly say otherwise.

When editing the docs, keep internal links relative to the current note and inside `docs/`.
Keep prose line breaks at natural sentence or clause boundaries, with related inline links together.
