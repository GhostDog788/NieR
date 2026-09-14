# Target-specific C checkpoint — 2026-09-13

[Current status](toolchain-status.md) · [Implementation and remaining gates](../02-implementation-plan.md#174-target-specific-c-implementation-and-remaining-gates) · [User guide](../guides/02-toolchain-users/target-specific-c.md)

This is a partial implementation of the target-specific C plan, not completion
of Clang-level C support. It adds independent LLVM import, optional sharing,
target-scoped Sela definitions/units, selected architecture sets, differing
build/link graphs, fixed vectors, wide integers, selected intrinsics, inline
assembly/asm goto, and constructors. Standalone/file-scope assembly, minimum-CPU
admission, further C/ABI facilities and final qualification remain open.

## Component and core evidence

The publisher passes all 50 CTests (23.86 seconds after the dependency-buffer
change described below). Each target's consumer build passes all
14 component CTests. The expanded smoke command passes all 15 selected tests
(23.66 seconds after the dependency-buffer and valid-C rejection checks).
Additional malformed inline-assembly checks were rebuilt and passed in every
consumer after the full component runs; package validation includes malformed
foreign-target link records and script digest checks without foreign backends.
The Hello regression separately proves that Clang accepts a TLS C fixture
on all four targets while this partial Sela implementation rejects its
unsupported storage contract; invalid C alone is not used as importer evidence.

Four fresh, target-only bundles are available in the repository under
`artifacts/target-c-prealpha-2-20260913/selac-TARGET`. They are verified copies
of the tested `/tmp/sela-target-bundles-HAStLl/selac-TARGET` bundles. No previous
bundle directory was overwritten. LLVM SDK inputs and backend-family
boundaries are unchanged.
SHA-256 of each bundle's `payload.sha256` receipt:

```text
x86_64   4cb3dc8cdbf351377a37c1fbe3b3e1001d5dd2139f4b30c9bff14220d14da2f6
i686     3c7732a9ecfad8f0f9b14ee0b3b1ed1f9cb6528a95af8970716110c57fe1b90b
armv7    4750252db95aa65d5142af98f0f1457e378b1bea95370b9ee2e7621f9c337f39
aarch64  2d6bdf2500be7137fc2907bd7908be0171381e891990d8feb9659ec02b8698f8
```

The core fixture preparation is `/tmp/sela-multi-consumer-NDcift/fixtures`.
Its `fixtures.sha256` receipt hashes to
`c437b4d6af2173013688c5e00f6ab38bcda1bdb328fdbe4cca626a4eda07f27c`.
All four matching-kernel guests passed the same fixture bytes, including
the new vector/intrinsic, inline-assembly/asm-goto and constructor fixtures
at O0/O2. There are 30 matrix executables plus four basic executables, three
DSOs, one static archive and 24 negative checks per device.
The saved core logs' final summary still prints the old hardcoded 28 count;
the actual loop asserts 30 matrix entries and the logs show all 37 ELF
outputs. The runner now derives the executable count from that loop.

| Target | Core VM evidence directory under `/tmp/` | Result |
| --- | --- | --- |
| x86_64 | `sela-consumer-vm-64rQ1w` | PASS |
| i686 | `sela-consumer-vm-SfzuQD` | PASS |
| armv7 | `sela-consumer-vm-tPwvHw` | PASS |
| aarch64 | `sela-consumer-vm-qghIE1` | PASS |

## New xxHash qualification

Fresh preparation `/tmp/sela-xxhash-yDpO5R` passed all four native builds and
published six artifacts, including the upstream 49,948-case sanity executable.
Its source-free fixture receipt hashes to
`64fffe8b9509cd57f2e298ef2c3b96c7b07f2ebad0a0f012b3b12e785937be74`.
The original preparation report remains `PREPARED`; destination jobs are
separate receipts, not a relabeled single-command qualification.

| Target | xxHash VM evidence directory under `/tmp/` | Result |
| --- | --- | --- |
| x86_64 | `sela-consumer-vm-Rt73vW` | PASS, six outputs and tests |
| i686 | `sela-consumer-vm-qwi9uh` | PASS, six outputs and tests |
| armv7 | `sela-consumer-vm-chG3GF` | PASS, six outputs and tests |
| aarch64 | `sela-consumer-vm-M4mnqJ` | FAIL; LLVM `opt` crashed reading sanity-test IR |
| aarch64 retry | `sela-consumer-vm-yvVhrh` | PASS, same bundle and six unchanged artifacts |

The initial publisher attempt `/tmp/sela-xxhash-NOvxrG` failed because native
link capture did not accept `-soname=NAME`. That spelling is now handled and
covered by the shared-build regression. The successful preparation above was
started fresh after rebuilding, with an immutable publisher identity receipt.

The ARM64 parser crash is not yet explained. The same reconstructed IR passed
with the same AArch64 SDK under user-mode emulation, and five standalone LLVM
parse/verify runs under the actual ARM64 kernel passed in
`/tmp/sela-llvm-probe-xtXa8w`. The diagnostic probe is not a substitute for
source-free application qualification. The complete unchanged-input retry
passed all six outputs, CLI/sanity tests and native-library checks. Thus every
target has a passing application run, but the initial intermittent ARM64
failure remains unexplained; this is not evidence that its cause was fixed.

## Existing corpus regression

Fresh zlib preparation `/tmp/sela-corpus-YAYf52` produced all eight outputs
and passed the original native tests on every profile. Its fixture receipt
hash is `0c7d17d1e9827cf1f98075c88595200b158c51c53d40d639f4bc0284fc980067`.
x86-64 and i686 device jobs passed in `sela-consumer-vm-bNZulQ` and
`sela-consumer-vm-EDUWLH`. ARMv7 passed all eight outputs and tests in
`sela-consumer-vm-5qDDtC` (729 seconds), and AArch64 passed in
`sela-consumer-vm-tOD9yZ` (802 seconds). These ARM runs used 1536 MiB guests.

The first fresh cJSON preparation `/tmp/sela-corpus-8oEzic` failed during
the 26th of 42 publications. Native ARMv7 capture of upstream `print_value.c`
crashed inside `SourceManager::translateFile`, called by Sela's dependency
observer. The input then passed 20 unchanged capture retries in
`/tmp/sela-capture-repro-Pl7RDP`; the original failure remains unexplained.
No complete cJSON destination qualification follows from that partial run.

Dependency hashing now requests Clang's per-file cached buffer directly,
without a full scan of its source-location/macro-expansion inventory.
This retains consumed-buffer provenance and avoids the failing call path;
it is not proof that the underlying intermittent failure is diagnosed.
Four-target regression checks cover canonical include aliases, forced
includes, system headers and target-selected header hashes.
This publisher-only change does not change the consumer bundles above.
The recorded core, xxHash and zlib preparations precede this change;
fresh cJSON preparation `/tmp/sela-corpus-5Sf0Rl` uses the rebuilt publisher.
It completed all 42 publications and all original native-profile tests from
18:49:26 to 19:15:46 UTC. Its fixture receipt hashes to
`bc8fdbeb4d03ed5b9153be76e0e87d6ba60b783c0915ac4377ff4ef448b60c77`;
its publication-input receipt hashes to
`e1f0d7a5e41ec75d002fd2da2c592591ca6fa845fe75306f9f9546009356470f`.
The preparation report is `PREPARED`; device receipts are separate.

| Target | cJSON VM evidence directory under `/tmp/` | Result at 19:24 UTC |
| --- | --- | --- |
| x86_64 | `sela-consumer-vm-z3HEsW` | PASS, all 42 outputs and both 19-test suites; 102 seconds |
| i686 | `sela-consumer-vm-um04Ax` | PASS, all 42 outputs and both 19-test suites; 120 seconds |
| armv7 | `sela-consumer-vm-fIRAnE` | Running; no final acceptance receipt yet |
| aarch64 | `sela-consumer-vm-0921Kq` | Running; no final acceptance receipt yet |

All four jobs use the same fixture bytes and 1536 MiB guests. Each has a
3600-second VM limit. The two ongoing ARM host logs are
`/tmp/sela-cjson-armv7-final-6JXGAI.log` and
`/tmp/sela-cjson-aarch64-final-w69Iyy.log`; their respective VM directories
retain `serial.log`. Do not turn these pending entries into passing claims
without the terminal checksum, upstream-test and `SELA_VM_PASS` receipts.
Historical cJSON/zlib acceptance does not establish acceptance of these builds.

After that publisher-only change, fresh xxHash preparation
`/tmp/sela-xxhash-VASIre` and fresh zlib preparation
`/tmp/sela-corpus-f5FzlI` both completed all native builds and publication.
Every one of their six and eight Sela artifacts respectively is byte-for-byte
identical to the artifacts consumed in the device runs above.
Their complete fixture receipt hashes are
`01c63ec6126f9631bce99a1c13886f460565c1a4f49ad9792676f4b656861840`
and `bebc510a9121b65659f3e69ca57e625fd0af06e62a8f723f4c16403a4cf060cd`.
Fresh native reference/provenance bytes make these new fixture receipts;
artifact equality is not a claim that those whole refreshed fixture sets
have already rerun every guest. Both preparation reports remain `PREPARED`.
An additional AArch64 run of the refreshed xxHash fixtures passed all six
outputs and tests in `/tmp/sela-consumer-vm-n5lY3E` (344 seconds, 1536 MiB).
This is a second successful full ARM64 run after the initial failure, not
proof that the original failure's cause was fixed.

## CI wiring

The checked-in main/manual workflow now publishes xxHash once and runs its
six unchanged artifacts in separate equally required jobs on all four targets,
alongside the existing 50-artifact cJSON/zlib jobs. Preparation and device runs
are time-bounded; failures are not automatically retried away. The diagnostic
collector retains xxHash reports and logs without copying source/fixture trees.
All 11 collector tests and local YAML/matrix checks pass. This is local workflow
validation, not a claim of a completed remote GitHub Actions run.
The publisher CTest preset now has the same 180-second per-test default limit
as the consumer presets; full corpus jobs keep their separate longer bounds.

These temporary paths identify retained local evidence, not distributable
product dependencies. Publisher captures can contain source/debug material.
The new user-guide command was separately executed successfully with an
`x86_64,aarch64` artifact in `/tmp/sela-target-guide-djtpXV`.
