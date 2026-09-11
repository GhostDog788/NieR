# Existing-build publication

These publisher SDK integrations run each project's normal native configure and build twice privately, with the actual stock Clang executable.
Native probes and generators run in their respective x86-64/i686 trees. The selected application link is then published by stock Clang into one independent `.nier` artifact.
No application source edits, compiler wrapper, public publisher CLI, or JSON publication recipe is required.

Source `sdk/env.sh` and build the publisher tools first.
In this development tree, set `NIER_BUILD_TOOL` to the absolute path of the built `nier-build` executable; the installed SDK will locate that internal helper on `PATH`.

## Make

For a project whose ordinary `make hello` creates `hello`:

```sh
make -f /path/to/nier/sdk/share/nier/Nier.mk \
  NIER_BUILD_TOOL=/path/to/nier/build/prealpha/nier-build \
  NIER_SOURCE_DIR=/path/to/application \
  NIER_TARGETS=hello NIER_NATIVE_OUTPUT=hello \
  NIER_ARTIFACT=/path/to/output/hello.nier
```

`NIER_TARGETS`, `NIER_CONFIGURE_ARGS`, and `NIER_CFLAGS` accept whitespace-separated target/argument lists. The source, native-output, artifact, and helper paths each support spaces.
Complex individual arguments containing spaces should use the CMake integration's quoted argument lists; this Make adapter does not guess how to split them.
The selected native output is relative to the project's private source/build directory. A project's `configure` script, when present, runs before its Makefile.
The destination artifact's parent directory must exist.

## CMake

Create a small separate publication configuration; leave the application's existing CMake project unchanged:

```cmake
cmake_minimum_required(VERSION 3.20)
project(ApplicationPublication NONE)
include("/path/to/nier/sdk/share/nier/Nier.cmake")
nier_add_publication(hello
  SOURCE_DIR "/path/to/application"
  NATIVE_OUTPUT hello
  OUTPUT hello.nier
  BUILD_TOOL "/path/to/nier/build/prealpha/nier-build"
  TARGETS hello
  CONFIGURE_ARGS "-DENABLE_FEATURE=ON")
```

Configure that small project normally, then build target `hello`.
Optional `CFLAGS` and `CONFIGURE_ARGS` are CMake argument lists. The SDK supplies the native profile compilers, archiver, sysroots, and Ninja; configure arguments must not override those inputs.
The selected output is relative to the private CMake build directory. `OUTPUT` is relative to the publication project's binary tree.

## Rebuild and execution

Requesting publication again runs fresh private builds and atomically replaces a valid existing Nier output only after success.
Failed builds leave the previous artifact intact. The integrations do not yet cache private build trees between requests.
Original source directories are not modified.

Both native reference builds use the stock Clang option `-ffile-prefix-map=<private-lane>=/nier`.
Source locations such as `__FILE__` therefore have stable `/nier/source/...` or `/nier/build/...` identities instead of random, profile-specific temporary paths.
Relative names, case, and genuine application string differences are unchanged.
Private dependency and object provenance checks still use their original physical paths. This is an observable SDK compilation setting, not runtime string redaction.

Run `nierc hello.nier -o hello`, then execute `./hello` directly.
Selected ordinary shared-library links likewise produce their own artifacts and compile to native DSOs.
Selecting a native static archive output produces its own static Nier artifact; `nierc library.nier -o library.a` restores the ordered native members, including duplicate basenames, without eagerly linking them.
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

Current verification covers direct-object Make/CMake programs, real native configure probes, per-profile generated headers, ordinary/thin/group/whole archive selection,
renamed objects, versioned shared libraries and independently published dependent applications,
plus Make/CMake static-library outputs with observable duplicate-member ordering and lazy extraction.
Different profile-selected source paths can fill the same explicit object role when the common IR merger proves their correspondence.
A bounded differing-source-count case also passes Make and CMake:
separate x86-64 translation units and their joined i686 counterpart share the same common function fragments.
Explicit ordered compilation-unit plans restore each target's original native units before its per-TU optimization; this does not introduce LTO.
A uniquely proven permutation also preserves different archive-extraction orders and each matched unit's own compiler settings; this is exercised by Make/CMake fixtures and zlib's real minigzip link.
The initial repartitioning proof requires matching effective settings and self-contained external scalar definitions, without cross-fragment private/global identities or differing static-member inventories.
General divergent function/CFG inventories and repeated-journal archive ambiguity remain qualification gates; they are not silently treated as native payloads.
The locked cJSON static/shared and zlib configure/Make corpus gates have passed their original destination tests; this does not establish unrestricted C support.
The end-user integration target is under fifteen minutes of configuration effort, not a claim established by an automated elapsed-time test of this implementation.

Static output members containing directory paths are not yet qualified.
Traditional configure scripts that expand `$CC` without shell evaluation require whitespace-free SDK and scratch paths; application source and artifact paths can still contain spaces.
