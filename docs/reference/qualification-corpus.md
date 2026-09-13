# Broad-C qualification corpus

These are locked qualification inputs.
Both configured projects passed the complete fresh dual-destination gate at the recorded 2026-09-12 pre-alpha checkpoint:
cJSON's static and shared configurations each produced 21 artifacts and passed all 19 original CTests on each destination;
zlib's eight outputs passed the original static, shared and 64-bit-offset Make recipes on both destinations.
All 50 artifacts were published once and supplied unchanged to the separate native x86-64 and i686 compilers; i686 ran under a real 32-bit Linux kernel.
Fresh native reference tests also passed on both private profiles. This is configured functional coverage, not full general-C, performance, RE or security acceptance; later compiler changes must requalify it.

Source archives must match `corpus/releases.lock`. Never patch upstream application/test sources or disable a failing required test.

Run the complete gate with the built publisher, independent consumer, and SDK:

```sh
bash corpus/qualify.sh /path/to/nier-build /path/to/nierc /path/to/sdk
```

During independent compiler bring-up, append `--project cjson` or `--project zlib` to run one entire pinned project.
This still requires every selected project's configuration, output, and original test; it does not filter individual tests.
Single-project success is not full two-project corpus acceptance.

The runner downloads and verifies the exact source archives, retains private evidence, runs the original native tests in both profiles,
publishes every required executable/static/shared output, and runs the original tests against the destination binaries.
It fails at the first unimplemented compiler contract; an expected rejection never counts as corpus acceptance.
It does not install software or modify upstream sources.
Each selected publication currently repeats fresh native builds; this qualification path prioritizes traceable evidence over incremental build speed.

Retain the complete runner log, downloaded archives, every per-publication log and private capture lane, destination test/loader logs, and artifact hashes.
The runner writes `qualification.txt`, recording the exact replay command and terminal pass/fail status
plus SHA-256 hashes of the actual publisher, consumer, plugins, linker helpers, Clang configuration, SDK lock and corpus recipes, every NieR artifact and destination output.
It uses the explicit `nier.cfg` beside the supplied built publisher and records each private native configuration.
Use one coherent publisher build directory for the helper, plugins, linker and configuration. A git revision alone does not identify binaries built from a dirty pre-alpha worktree.
The initial passing runs also have retrospective local `qualification.txt` reports alongside this evidence; reports are not publication recipes or security attestations.

The final publisher was additionally checked against all 50 retained native selections using the test-only `corpus_replay_tests` executable.
It revalidates original capture/dependency/native witnesses, invokes actual stock Clang with the current plugin and linker, and requires byte-identical unit and final artifacts.
All 170 unit-artifact occurrences and 50 publications matched. This read-only regression replay is not a fresh configure/native-build/test run or a public publication interface.
The ordinary corpus command above remains the complete source-to-native qualification path.

Both profiles use the SDK's stock Clang `-ffile-prefix-map=<private-lane>=/nier` setting.
Thus upstream `__FILE__` values retain their relative source identity without incorporating different temporary profile-directory names.
Native reference tests use this same configuration; neither upstream sources nor captured string literals are rewritten.

## cJSON 1.7.19

Run two independent CMake Release configurations, initialized with `corpus/cjson-static.cmake` and `corpus/cjson-shared.cmake`.
Use the same preset and effective compiler options for native references and NieR publication.
Each configuration builds the upstream default targets and runs **all 19 registered CTests** with `ctest --output-on-failure`, without filters.
The default test-enabled build also builds `fuzz_main`; it does not register that target as a CTest.

The shared configuration must preserve `libcjson.so.1`, public API visibility, native imports, and the actual libc/libm dependencies selected by stock LLVM/LLD.
The static configuration must exercise real archive extraction. The runner also compares the original demonstration program's native-reference and destination output byte-for-byte.
In the shared configuration a separately linked native caller verifies the NieR-produced DSO's C ABI; loader traces verify that both callers resolve that exact destination DSO.

Unity's default `setjmp`/`longjmp` assertion control remains enabled.
Defining `UNITY_EXCLUDE_SETJMP_H` changes test semantics and is not an allowed shortcut.
Optional cJSON_Utils, AFL, sanitizer and Valgrind suites are outside these pinned configurations; required tests must not be disabled after a failure.

Upstream definitions:

- [Build options](https://github.com/DaveGamble/cJSON/blob/v1.7.19/CMakeLists.txt)
- [CTest inventory](https://github.com/DaveGamble/cJSON/blob/v1.7.19/tests/CMakeLists.txt)
- [Unity control flow](https://github.com/DaveGamble/cJSON/blob/v1.7.19/tests/unity/src/unity_internals.h)

## zlib 1.3.2

Use the unchanged upstream configure/Make path, `CFLAGS=-O3`, `./configure
--shared`, `make`, `make test`, and `make test64`, independently in both native profile trees.
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

## Destination execution

Every selected executable/shared-library link and static-library output produces its own NieR artifact.
Compile those artifacts with `nierc`, then stage native dependencies in the qualified fixture library root and run the original selected tests against them.
Only test data and the ordinary native runtime/dependencies accompany execution; compiler capture data and build-time generators remain private.
Native x86-64 and i686 reference runs are separate from testing the independently installed device compilers.

The opt-in `--i686-bundle /path/to/i686-bundle` gate extends destination execution to both independent native compilers, using a real 32-bit Linux kernel for i686.
Pass the x86-64 bundle's `bin/nierc` as the ordinary consumer argument and retain the publisher SDK as the third argument.
The runner publishes each selected artifact once, completes the x86-64 checks, and stages the same artifacts plus original generated test recipes and runtime data into the offline i686 guest.
The guest runs all original selected tests with separately pinned test-only runners; it contains no application source or publication frontend.
See the [compiler distribution reference](compiler-distribution.md#full-corpus-on-both-destination-compilers) for the complete command, prerequisites, resource limits, and evidence locations.
Before the fresh run, a retained-artifact i686 regression also compiled all 50 prior artifacts under the real 32-bit kernel and passed the same original selected test inventories.
Its input hashes matched the current publisher's byte-identical retained replay; source publication and native-reference builds were not rerun for that consumer-only regression.
The subsequent complete fresh dual-destination command passed separately, with new source publication/native-reference builds and `Result: PASS (all)` in its `qualification.txt`.
Its actual i686 VM also passed the native-caller, loader, SONAME/version, archive-order, and checksum checks and produced the required explicit serial PASS receipt.
Keep these evidence categories separate: replay, retained consumer regression, and fresh source-to-both-devices qualification are not interchangeable claims.
