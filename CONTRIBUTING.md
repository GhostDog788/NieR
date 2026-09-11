# Contributing to NieR

NieR is an Apache-2.0 open-source project in active pre-alpha development.
Contributions to the implementation, tests, documentation, and examples are welcome.
The current working C toolchain is a qualified subset of the broader language-independent NieR vision.
Start with the [current requirements](docs/01-architecture-design.md) and [implementation status](docs/02-implementation-plan.md) before proposing a new product capability.

## Find a useful starting point

- Follow the [Hello project walkthrough](docs/guides/02-toolchain-users/hello-project-walkthrough.md) to try the real publication flow.
- Read the [contributor guide section](docs/guides/03-contributors/README.md) to understand the repository and public interfaces.
- Use [Testing, debugging, and following a feature](docs/guides/03-contributors/14-testing-debugging-and-features.md) to trace a change through its evidence.

Search existing issues before opening a new one.
For a large change to the public IR, target support, language integration, or build semantics, describe the problem and proposed contract in an issue before implementing it.
Small, focused fixes and documentation improvements can go directly into a pull request.
Discuss code and evidence respectfully; disagreement about a design should not become a personal attack.

## Build and test

The documented development baseline is x86-64 Ubuntu 24.04.
Read the [build instructions](docs/reference/building-nier.md) and [SDK prerequisites](docs/reference/development-sdk.md) first.
From the repository root in Bash:

```bash
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
cmake --preset prealpha
cmake --build --preset prealpha
ctest --preset prealpha
```

Keep the SDK, generated Clang configuration, plugins, and compiler binaries from one matching build.
Do not rebuild shared tools while another qualification or regression run is using them.
The [full cJSON/zlib qualification corpus](docs/reference/qualification-corpus.md) is a separate, longer-running check, not part of the ordinary test command above.
Record the tests appropriate to your change; report anything you did not run rather than implying complete qualification.

## Preserve the project boundaries

- Keep the public NieR format independent of source-language frontends. The device compiler must not need private C source or capture evidence.
- Add positive tests for newly admitted behavior and negative tests for nearby cases that must still reject.
- For native semantics, preserve the appropriate target comparisons, ABI behavior, native translation-unit boundaries, and end-to-end checks.
- Do not make a failing corpus pass by changing upstream source, disabling required tests, or shipping opaque per-target native payloads.
- Keep NieR toolchain correctness separate from SENieR (SEN), the planned signing and executable-memory enforcement platform. Source exclusion is not proof of reverse-engineering resistance.

There are no backward-compatibility obligations during pre-alpha.
An intentional contract change should update its producer, verifier, consumer, fixtures, and documentation together; it should not introduce a legacy alias or silently accept mismatched artifacts.
See [Maintaining and evolving NieR](docs/guides/04-maintainers/24-maintaining-and-evolving-nier.md) for the full reasoning and evidence requirements.

## Prepare a pull request

Keep unrelated refactors out of a focused change.
Explain the problem, the behavior you changed, the admitted scope and limitations, and the exact checks you ran.
For a bug fix, include a minimal regression test when practical.
Do not commit `.sdk/`, build outputs, corpus downloads, private capture workspaces, credentials, or source you are not authorized to share.

Keep documentation in the `docs/` Obsidian vault and open the repository root separately in VS Code.
Local links inside `docs/` must lead to Markdown inside that vault; source references are repository-relative code paths for VS Code.
Use natural sentence or clause line breaks, preserve code fences, and keep related inline links together.

## Report problems safely

For ordinary bugs, include the commit, whether local changes are present, host/target details, exact commands, expected behavior, and a small sanitized reproducer.
Private LLVM captures can contain source, debug data, and local paths; do not upload an entire capture directory by default.
For suspected vulnerabilities, follow [SECURITY.md](SECURITY.md) instead of opening a public technical report.

## Licensing

Project-owned contributions are under [Apache License 2.0](LICENSE); third-party components retain their own licenses and notices.
Submit only material you have the right to contribute under those terms, and preserve existing third-party attribution.
There is no separate contributor license agreement or copyright-assignment requirement.
“Source-private” describes the application publication format, not a closed-source license for the NieR toolchain.
