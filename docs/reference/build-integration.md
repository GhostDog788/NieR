# Existing-build publication

These publisher SDK integrations run each project's normal native configure and build privately for every supported target, with the actual stock Clang executable.
The default target set is x86-64, i686, ARMv7-A little-endian hard-float with VFPv3-D16, and AArch64 little-endian ARMv8-A, all using Linux/glibc.
Native probes and generators run in their respective target trees. The selected application link is then published by stock Clang into one independent `.sela` artifact.
No application source edits, compiler wrapper, public publisher CLI, or JSON publication recipe is required.

Source `sdk/env.sh` and build the publisher tools first.
In this development tree, set `SELA_BUILD_TOOL` to the absolute path of the built `sela-build` executable; the installed SDK will locate that internal helper on `PATH`.

## Native execution prerequisites

On the supported x86-64 publisher host, i686 executables need working ordinary 32-bit Linux execution.
ARMv7 and AArch64 executables require explicitly provisioned QEMU user emulators and matching `binfmt_misc` handlers so ordinary Make generators can execute directly.
The integration checks real compiled target probes, including pointer size and the target's default plain-`char` signedness, before starting the project builds.
Each execution probe has a twenty-second bound; missing or nonworking execution support is an error, never a guessed configure answer.

`SELA_EMULATOR_ARMV7` and `SELA_EMULATOR_AARCH64` can select existing emulator executables for CMake.
They do not replace the requirement for working direct execution in ordinary Make recipes.
Sela never installs QEMU, registers global handlers, or silently changes host execution policy.

CMake receives a genuine cross configuration and an explicit emulator for its native probes/tests; include and library discovery is confined to the matching SDK sysroot.
Make/configure receives the matching `CHOST`, native Clang target settings, sysroot, and runtime.
Project tests and generators still execute; cross compilation does not mean declaring them successful without running them.

## Make

For a project whose ordinary `make hello` creates `hello`:

```sh
make -f /path/to/sela/sdk/share/sela/Sela.mk \
  SELA_BUILD_TOOL=/path/to/sela/build/prealpha/sela-build \
  SELA_SOURCE_DIR=/path/to/application \
  SELA_BUILD_TARGETS=hello SELA_NATIVE_OUTPUT=hello \
  SELA_ARTIFACT=/path/to/output/hello.sela
```

`SELA_BUILD_TARGETS`, `SELA_CONFIGURE_ARGS`, and `SELA_CFLAGS` accept whitespace-separated build-goal/argument lists. The source, native-output, artifact, and helper paths each support spaces.
Complex individual arguments containing spaces should use the CMake integration's quoted argument lists; this Make adapter does not guess how to split them.
The selected native output is relative to the project's private source/build directory. A project's `configure` script, when present, runs before its Makefile.
The destination artifact's parent directory must exist.

## CMake

Create a small separate publication configuration; leave the application's existing CMake project unchanged:

```cmake
cmake_minimum_required(VERSION 3.20)
project(ApplicationPublication NONE)
include("/path/to/sela/sdk/share/sela/Sela.cmake")
sela_add_publication(hello
  SOURCE_DIR "/path/to/application"
  NATIVE_OUTPUT hello
  OUTPUT hello.sela
  BUILD_TOOL "/path/to/sela/build/prealpha/sela-build"
  BUILD_TARGETS hello
  CONFIGURE_ARGS "-DENABLE_FEATURE=ON")
```

Configure that small project normally, then build target `hello`.
Optional `CFLAGS` and `CONFIGURE_ARGS` are CMake argument lists. The SDK supplies the native profile compilers, archiver, sysroots, and Ninja; configure arguments must not override those inputs.
The selected output is relative to the private CMake build directory. `OUTPUT` is relative to the publication project's binary tree.

## Architecture selection and differing builds

Architecture selection is separate from Make/CMake build goals.
Leave it unset to publish all four registered targets, or choose explicitly:

- Stock Clang: `SELA_ARCHS=x86_64,aarch64 clang --config=... source.c -o program.sela`.
- Make adapter: `SELA_ARCHS=x86_64,aarch64` alongside `SELA_BUILD_TARGETS=hello`.
- CMake adapter: `ARCHS x86_64 aarch64` alongside `BUILD_TARGETS hello`.
- Internal coordinator: repeated `--arch x86_64 --arch aarch64`; `--build-target hello` names a build goal.

Unknown, duplicate, or explicitly empty selections fail.
Each selected target must build and publish successfully; there is no automatic narrowing to whichever targets happen to work.
Native build lanes can have different source files, flags, libraries, version scripts and archive members.
Sela records module availability, native compilation-unit order and target-specific link settings explicitly.
Retained evidence includes `capture-targets.json`; replay uses that exact selection rather than guessing targets from directories.

## Rebuild and execution

Requesting publication again runs fresh private builds and atomically replaces a valid existing Sela output only after success.
Failed builds leave the previous artifact intact. The integrations do not yet cache private build trees between requests.
Original source directories are not modified.

Every native reference build uses the stock Clang option `-ffile-prefix-map=<private-lane>=/sela`.
Source locations such as `__FILE__` therefore have stable `/sela/source/...` or `/sela/build/...` identities instead of random, profile-specific temporary paths.
Relative names, case, and genuine application string differences are unchanged.
Private dependency and object provenance checks still use their original physical paths. This is an observable SDK compilation setting, not runtime string redaction.

The SDK also enables stock Clang's `-frecord-command-line` to retain the expanded driver settings privately.
Source publication regenerates genuine target-specific Clang invocations from those settings, preserving explicit user flags while applying each target's native ABI defaults.
For example, ordinary plain `char` is signed on these x86 profiles and unsigned on these ARM profiles; an explicit `-fsigned-char` or `-funsigned-char` remains explicit on all profiles.
The private recorded command is not published as Sela Code.
The AArch64 application profile explicitly disables outlined atomics and function multiversioning with `-mno-outline-atomics -mno-fmv`.
Source publication and native reference builds use the same settings; they are separate from the flags used to build the stock LLVM compiler implementation itself.

Run `selac hello.sela -o hello`, then execute `./hello` directly.
Selected ordinary shared-library links likewise produce their own artifacts and compile to native DSOs.
Selecting a native static archive output produces its own static Sela artifact; `selac library.sela -o library.a` restores the ordered native members, including duplicate basenames, without eagerly linking them.
Native runtime and dependency provisioning is separate from publication.

Private native objects carry a non-executable capture reference.
Stock `llvm-ar`, file copies, and renames preserve that reference.
The publisher uses stock LLD's selected-input and archive-extraction evidence to publish only the selected translation units, preserving their separate optimization boundaries.
Archive groups and `--whole-archive` therefore follow the native link's actual selection rather than embedding an archive's unrelated code.
The final private ELF's capture-reference sequence is checked against the native link trace to identify duplicate-basename members exactly.
Repeated identical journal identities or a sequence that cannot be proved are rejected.

The native SDK's after-codegen observer records the original object hash. A postprocessed object or transplanted capture marker is rejected before publication; copy/rename/archive storage preserves the original bytes.
This is build provenance, not signing or protection from an attacker rewriting the private workspace.
Native linker defaults use PIE/shared ELF, GNU hashes, new dtags, full stripping, default build IDs, RELRO/NOW and GNU-compatible undefined version-script names.
Supported nondefault options are preserved; unqualified semantic overrides fail explicitly.

The existing regression suite covers direct-object Make/CMake programs, real native configure probes, per-profile generated headers, ordinary/thin/group/whole archive selection,
renamed objects, versioned shared libraries and independently published dependent applications,
plus Make/CMake static-library outputs with observable duplicate-member ordering and lazy extraction.
Different profile-selected source paths can fill the same explicit object role when the common IR merger proves their correspondence.
A bounded differing-source-count case also passes Make and CMake:
separate x86-64 translation units and their joined i686 counterpart share the same common function fragments.
Explicit ordered compilation-unit plans restore each target's original native units before its per-TU optimization; this does not introduce LTO.
A uniquely proven permutation also preserves different archive-extraction orders and each matched unit's own compiler settings; this is exercised by Make/CMake fixtures and zlib's real minigzip link.
The initial repartitioning proof requires matching effective settings and self-contained external scalar definitions, without cross-fragment private/global identities or differing static-member inventories.
General divergent function/CFG inventories and repeated-journal archive ambiguity remain qualification gates; they are not silently treated as native payloads.
The earlier two-x86-profile implementation passed the locked cJSON static/shared and zlib configure/Make corpus gates.
The four-target extension must independently pass the same original tests for all four targets; the historical x86 result does not establish ARM qualification or unrestricted C support.
See [corpus qualification](qualification-corpus.md) for publish-once and independent real-kernel consumer commands.
The end-user integration target is under fifteen minutes of configuration effort, not a claim established by an automated elapsed-time test of this implementation.

Static output members containing directory paths are not yet qualified.
Traditional configure scripts that expand `$CC` without shell evaluation require whitespace-free SDK and scratch paths; application source and artifact paths can still contain spaces.
