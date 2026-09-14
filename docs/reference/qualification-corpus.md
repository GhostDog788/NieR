# Broad-C qualification corpus

These are locked qualification inputs.
Both configured projects passed the complete fresh dual-destination gate at the pre-rename 2026-09-12 checkpoint recorded in commit `63592ab`:
cJSON's static and shared configurations each produced 21 artifacts and passed all 19 original CTests on each destination;
zlib's eight outputs passed the original static, shared and 64-bit-offset Make recipes on both destinations.
All 50 artifacts were published once and supplied unchanged to the separate native x86-64 and i686 compilers; i686 ran under a real 32-bit Linux kernel.
Fresh native reference tests also passed on both private profiles. This is configured functional coverage, not full general-C, performance, RE or security acceptance; later compiler changes must requalify it.
The [compiler distribution reference](compiler-distribution.md#historical-sela-rename-checkpoint) records the separate historical Sela qualification; the historical artifacts, tool names, paths, and hashes are not retroactively renamed.

Source archives must match `corpus/releases.lock`. Never patch upstream application/test sources or disable a failing required test.

The current runner targets all four registered profiles: x86-64, i686, ARMv7-A hard-float, and AArch64, all Linux/glibc.
The separate [four-target checkpoint on 2026-09-13](compiler-distribution.md#four-target-corpus-checkpoint-on-2026-09-13) passes all 50 artifacts and the original configured tests on every destination, including genuine 32-bit kernels for i686 and ARMv7.
It combines two complete fresh project preparations and separate VM acceptance receipts; initial failed attempts and passing unchanged-input continuations remain recorded rather than being relabeled as one combined command.

Run the complete gate with the built publisher, publisher SDK, and a directory containing `selac-x86_64`, `selac-i686`, `selac-armv7`, and `selac-aarch64` bundles:

```sh
bash corpus/qualify.sh /path/to/sela-build /path/to/sdk \
  --bundle-parent /path/to/artifacts
```

During independent compiler bring-up, append `--project cjson` or `--project zlib` to run one entire pinned project.
This still requires every selected project's configuration, output, and original test; it does not filter individual tests.
Single-project success is not full two-project corpus acceptance.

The runner downloads and verifies the exact source archives, retains private evidence, runs the original native tests in all four profiles,
publishes every required executable/static/shared output once, and runs the original tests against the destination binaries under each target's real Linux kernel.
It fails at the first unimplemented compiler contract; an expected rejection never counts as corpus acceptance.
It does not install software or modify upstream sources.
The publisher host must already support real native-build probes and generators through explicitly provisioned QEMU/binfmt where necessary; see [native execution prerequisites](build-integration.md#native-execution-prerequisites).
Each selected publication currently repeats fresh native builds; this qualification path prioritizes traceable evidence over incremental build speed.

Retain the complete runner log, downloaded archives, every per-publication log and private capture lane, destination test/loader logs, and artifact hashes.
The runner writes `qualification.txt`, recording the exact replay command and terminal pass/fail status
plus SHA-256 hashes of the actual publisher, plugins, linker helpers, Clang configuration, SDK lock, target registry and corpus recipes, and every Sela artifact and native configuration.
Each VM's retained log separately verifies its actual compiler-bundle payload, common fixture hashes, generated native outputs, kernel and boot image.
It uses the explicit `sela.cfg` beside the supplied built publisher and records each private native configuration.
Use one coherent publisher build directory for the helper, plugins, linker and configuration. A git revision alone does not identify binaries built from a dirty pre-alpha worktree.
Core and corpus preparation retain a publisher identity receipt and recheck its hashes before and after every publication and before completing preparation.
Do not rebuild these tools during a run: changed or missing inputs fail the run as a mixed checkpoint; preserve that failure and start a fresh preparation after the build finishes.
The initial passing runs also have retrospective local `qualification.txt` reports alongside this evidence; reports are not publication recipes or security attestations.

The pre-rename publisher was additionally checked against all 50 retained native selections using the test-only `corpus_replay_tests` executable, as recorded in commit `be63890`.
That replay revalidated original capture/dependency/native witnesses, invoked actual stock Clang with the plugin and linker from the same checkpoint, and required byte-identical unit and final artifacts.
All 170 unit-artifact occurrences and 50 publications matched. This read-only regression replay is not a fresh configure/native-build/test run or a public publication interface.
The ordinary corpus command above remains the complete source-to-native qualification path.

Current Sela runs use the SDK's stock Clang `-ffile-prefix-map=<private-lane>=/sela` setting for every profile.
Thus upstream `__FILE__` values retain their relative source identity without incorporating different temporary profile-directory names.
Native reference tests use this same configuration; neither upstream sources nor captured string literals are rewritten.

## cJSON 1.7.19

Run two independent CMake Release configurations, initialized with `corpus/cjson-static.cmake` and `corpus/cjson-shared.cmake`.
Use the same preset and effective compiler options for native references and Sela publication.
Each configuration builds the upstream default targets and runs **all 19 registered CTests** with `ctest --output-on-failure`, without filters.
The default test-enabled build also builds `fuzz_main`; it does not register that target as a CTest.

The shared configuration must preserve `libcjson.so.1`, public API visibility, native imports, and the actual libc/libm dependencies selected by stock LLVM/LLD.
The static configuration must exercise real archive extraction. The runner also compares the original demonstration program's native-reference and destination output byte-for-byte.
In the shared configuration a separately linked native caller verifies the Sela-produced DSO's C ABI; loader traces verify that both callers resolve that exact destination DSO.

Unity's default `setjmp`/`longjmp` assertion control remains enabled.
Defining `UNITY_EXCLUDE_SETJMP_H` changes test semantics and is not an allowed shortcut.
Optional cJSON_Utils, AFL, sanitizer and Valgrind suites are outside these pinned configurations; required tests must not be disabled after a failure.

Upstream definitions:

- [Build options](https://github.com/DaveGamble/cJSON/blob/v1.7.19/CMakeLists.txt)
- [CTest inventory](https://github.com/DaveGamble/cJSON/blob/v1.7.19/tests/CMakeLists.txt)
- [Unity control flow](https://github.com/DaveGamble/cJSON/blob/v1.7.19/tests/unity/src/unity_internals.h)

## zlib 1.3.2

Use the unchanged upstream configure/Make path, `CFLAGS=-O3`, `./configure
--shared`, `make`, `make test`, and `make test64`, independently in all native profile trees.
Record the compiler and all flags that configure adds.
Both static and shared outputs are required; shared-library detection falling back to static is a failure.
Preserve `libz.so.1` and the upstream `zlib.map` version script.

`make test` covers `example`/`minigzip` and `examplesh`/`minigzipsh`, including the upstream compression/decompression roundtrips.
`make test64` covers the additional `_FILE_OFFSET_BITS=64` programs.
Leave upstream tables, native-width CRC paths, gzip/stdio interfaces and variadic APIs enabled.
Do not select `Z_SOLO`, `NO_GZCOMPRESS`, `BUILDFIXED`, `DYNAMIC_CRC_TABLE`, or fixed `Z_TESTW` overrides to make compilation easier.

This selected Make suite is not a claim to qualify zlib's separate CMake package, installation, coverage instrumentation, and coverage-summary test suite.

- [Configure](https://github.com/madler/zlib/blob/v1.3.2/configure)
- [Make tests](https://github.com/madler/zlib/blob/v1.3.2/Makefile.in)
- [Official release digest](https://zlib.net/)

## xxHash 0.8.3 target-specific C increment

The separate `corpus/xxhash.lock` pins the unmodified upstream release.
Run its preparation or device gate with:

```sh
bash corpus/qualify-xxhash.sh /path/to/sela-build /path/to/sdk --prepare-only
# Or publish and then run the four real-kernel device jobs:
bash corpus/qualify-xxhash.sh /path/to/sela-build /path/to/sdk \
  --bundle-parent /path/to/artifacts
```

The six outputs are `xxhsum`, `tests/sanity_test`, static and shared libraries,
and separate static/shared API clients. Upstream Make chooses its normal
target-specific sources and flags, including x86 dispatch and ARM SIMD;
qualification must not turn these off to make publication succeed.
Device checks run the upstream CLI's built-in sanity checks, the unmodified
standalone sanity executable, four CLI hash variants, streaming APIs, archive
membership, SONAME and an ordinary native caller against the generated DSO.
Only Sela artifacts, native reference binaries and generated data reach guests.
This is functional/ABI evidence, not a benchmark or all upstream `make check`
recipes. The new runner does not replace the complete cJSON/zlib regressions.
Consult the [active implementation record](../02-implementation-plan.md#174-target-specific-c-implementation-and-remaining-gates)
for what has actually passed; adding the runner alone is not qualification.

## Destination execution

Every selected executable/shared-library link and static-library output produces its own Sela artifact.
Compile those artifacts with `selac`, then stage native dependencies in the qualified fixture library root and run the original selected tests against them.
Only test data and the ordinary native runtime/dependencies accompany execution; compiler capture data and build-time generators remain private.
Native reference runs are separate from testing the independently installed device compilers.

Publication and device acceptance can run on separate machines or CI jobs:

```sh
bash corpus/qualify.sh /path/to/sela-build /path/to/sdk --prepare-only
# Use the exact fixture directory printed by the successful preparation.
bash tests/consumer-vm.sh armv7 /path/to/selac-armv7 /path/to/consumer-fixtures
```

Run the second command independently for every target with its own matching bundle and the exact same fixture bytes.
`--prepare-only` records `Result: PREPARED`, never `PASS`; it does not require a device compiler and does not claim destination acceptance.
The fixture SHA-256 manifest travels with the fixture directory. CI additionally transfers a hash-checked tar archive so executable modes and bytes survive between jobs.

Every disposable offline guest stages its compiler at `/opt/sela`, providing a stable runtime location independent of host checkout or download paths.
The guest receives only the compiler bundle, published artifacts, ordinary native reference binaries, original generated test recipes/data, and separately pinned test-only runners.
It receives no application C sources, captured LLVM IR, publisher frontend, network mount or host filesystem mount.
The genuine i686 and ARMv7 kernels establish 32-bit-kernel execution; every guest also verifies its native ELF class/machine and rejects a foreign executable through the actual kernel `execve` boundary.

Destination CMake recipes retain every original test and property. Only the absolute native build root and the exact host-only cross-emulator prefix are relocated for direct guest-native execution.
The complete 19-test inventory is checked before each cJSON run. zlib runs its unchanged `test` and `test64` recipes with rebuilding disabled.
Test-only CTest/make/readelf dependency closures are independent of the compiler package and never increase the shipped product payload.

The VM defaults to 3 GiB RAM and a 3600-second limit for corpus fixtures containing `corpus.list`; core fixtures retain the 900-second default.
`SELA_VM_RAM_MIB` and `SELA_VM_TIMEOUT` allow explicit supported resource choices; a supplied timeout overrides either default and must remain between 30 and 3600 seconds.
Timing is recorded for observability, not treated as a performance acceptance threshold. Logs and failed boot images are retained rather than overwritten with later retry results.
PR CI requires the core matrix on all four targets; main/manual CI additionally requires all 50 unchanged corpus artifacts on each target.
The target-specific C increment adds a separate six-artifact xxHash fixture set
and four equally required destination jobs on main/manual runs. Every device
receives the same publication archive; no per-device source rebuild or automatic
crash retry can turn a failure into a CI pass. Publisher preparation and guest
execution have explicit time limits, with bounded diagnostic retention.
This describes the checked-in workflow; local passing runs are not evidence
that the updated GitHub workflow has run successfully.

## Historical evidence distinctions

Before the historical 2026-09-12 fresh run, a retained-artifact i686 regression also compiled all 50 prior artifacts under the real 32-bit kernel and passed the same original selected test inventories.
Its input hashes matched that checkpoint's publisher replay; source publication and native-reference builds were not rerun for that consumer-only regression.
The subsequent complete fresh dual-destination command at `63592ab` passed separately, with new source publication/native-reference builds and `Result: PASS (all)` in its `qualification.txt`.
Its actual i686 VM also passed the native-caller, loader, SONAME/version, archive-order, and checksum checks and produced the required explicit serial PASS receipt.
Keep these evidence categories separate: replay, retained consumer regression, and fresh source-to-both-devices qualification are not interchangeable claims.
