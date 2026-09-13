# Retrospective: native device compilers, build time, and footprint

[Project reference](README.md) · [Compiler distributions](compiler-distribution.md)

Written on 2026-09-13 for future contributors and coding agents.
This reviews the work recorded in commit `63592ab` (`x86-32 bit support`).
That work predates the Sela rename; the original evidence record is preserved in commit `be63890`.
Compiler names and local paths below are described generically rather than relabeling historical binaries or claiming they were built as Sela.
It records both the working result and mistakes in how I selected and sequenced the work.
It is not an implementation of a smaller SDK or a claim that the size regression has been fixed.

The subsequent footprint work is recorded separately in [Compiler distributions](compiler-distribution.md#footprint-measurements-and-current-checkpoint).
That reference contains the newer shared-LLVM measurements and qualification status; the original measurements and conclusions below remain historical evidence.

## Outcome and the missed expectation

The functional result was real: separate native x86-64 and i686 compiler bundles consumed the same portable artifact within the then-qualified C subset.
The i686 compiler and its LLVM tools are ELF32 programs, and their outputs were tested under a real 32-bit Linux kernel.
Each device compiler contained only its own native lowering and ABI implementation.
The publisher remains independent of the device compiler binaries.

However, the complete bundles became larger, not smaller.
Calling this a footprint optimization would be incorrect.
The user expected device specialization to remove unnecessary material; physical backend separation alone did not satisfy that expectation for the whole distribution.

I combined two different changes:

1. Separating shared artifact validation from per-device native lowering.
2. Replacing shared LLVM linkage with statically linked LLVM components in several executables.

The second change was an implementation choice, not a requirement of the first.
The packaging script also made the absence of `libLLVM` a mandatory check, although a shared LLVM library is not inherently incompatible with a target-specific device compiler.
That encoded a packaging strategy as if it were the architectural requirement.

## Why the task took about 3 hours 15 minutes

The overall duration is the user's reported wall-clock duration.
The retained records support the following explanation, not an exact accounting of every minute:

| Evidence | What it establishes |
| --- | --- |
| Roughly 3,500 object outputs in the retained Ninja logs | Substantial upstream compiler infrastructure was built, not just the project's own code. |
| Approximately 13:48–16:23 UTC on 2026-09-12 | Source-build activity spanned about 2 hours 35 minutes, including pauses, diagnostics, and retries. This is not continuous CPU time. |
| Fresh corpus report: 16:32:57–16:48:54 UTC | The complete final source-to-both-devices corpus gate took 15 minutes 57 seconds. |
| Final publisher and device component results | The 39 publisher tests took about 17 seconds; each nine-test device suite took about four seconds. These were not the hours-long stage. |
| Final SDK refresh logs | The cache-safety change caused no LLVM object rebuilds or tool relinks. The refreshes together took about 22 seconds and preserved all eight LLVM tool hashes. |

### Work that was genuinely needed

An i686 device cannot run the existing amd64 compiler executables or reuse their machine-code object files.
It needed genuine ELF32 tools and matching runtime dependencies.
The project's native implementations needed a physical separation, with shared artifact validation retained for both public word domains.
Native executables, shared libraries, static archives, relocation, malformed inputs, and real 32-bit execution needed verification.

The real-kernel test was useful evidence, not interchangeable with running an ELF32 application under a 64-bit kernel's compatibility mode.
The full fresh corpus was also required to qualify the final compiler changes; old artifacts and replay tests were not substitutes.

### Choices and problems that added time

- I built LLVM/MLIR/LLD source SDKs for both device ABIs, including private build-time generators, rather than first validating the native-backend separation with the existing x86-64 SDK.
  Rebuilding x86-64 LLVM was part of the chosen source-SDK recipe, not inherently necessary merely to remove the project's i686 lowering from the x86-64 compiler.
- Heavyweight builds were serialized and limited to two compilation jobs, then mostly one after intermittent stock-Clang crashes.
  One later crash occurred even at one job.
  Preserved diagnostics and successful unchanged retries establish intermittent failures, not their root cause.
  Neither an LLVM source defect nor a hardware fault was established.
- I combined backend refactoring, new source-SDK infrastructure, packaging, provenance checks, VM infrastructure, qualification, and documentation in one long operation.
  Some of this work overlapped, but the LLVM build remained the critical path.
- There was avoidable coordination delay between completion of the first fresh device matrix and the start of the full corpus.
  I eventually took over that command directly.
  Critical-path commands need an explicit owner and an observed start, not an assumed handoff.
- The size consequence of static linking was not measured early enough to steer the packaging design before both expensive source builds were completed.

The lesson is not to discard tests or blindly increase compilation parallelism.
It is to choose and measure the packaging architecture earlier, reuse existing build inputs where valid, and communicate the expected upstream-build cost before committing to it.

## Why specialization produced larger deliverables

It was not specialization alone.
The old and new packages differ in LLVM linkage and upstream build configuration as well as in the project's backend selection.
They are not a controlled before/after measurement of just removing one backend.

The retained installed executables have these sizes:

| Measurement | Previous x86-64 | New x86-64 | New i686 |
| --- | ---: | ---: | ---: |
| Compiler executable | 3,344,472 bytes | 6,169,968 bytes | 6,640,492 bytes |
| Compiler executable, rounded | 3.2 MiB | 5.9 MiB | 6.3 MiB |
| Complete bundle, rounded disk usage | 190 MiB | 244 MiB | 265 MiB |

These are historical compiler-package sizes, not generated application or portable artifact sizes.
The old x86-64 package is the comparison baseline; there was no previous equivalent standalone i686 package in this comparison.

### The compiler executable grew for a concrete reason

`readelf -d` shows that the old executable depends on `libLLVM.so.18.1`.
The new executables do not: selected LLVM implementation code is linked into them instead.
Removing a project backend can therefore be outweighed by moving LLVM code from a shared file into the executable.

This is not merely a debug-information explanation.
For x86-64, the `.text` section grew from 1,697,652 to 2,927,749 bytes, and `.rodata` grew from 79,872 to 707,520 bytes.
Relocation, unwind, and other sections changed as well.
These observations show real embedded-code and data growth, but do not isolate the exact contribution of every source or build-option change.

### The whole bundle grew because tools no longer share LLVM code

Previously, the device compiler, `opt`, `llc`, and other tools could use one shared LLVM library in the bundle.
That library occupied 123,215,144 bytes.
The new build embeds required LLVM components separately in multiple executables.
Removing unused target backends does not deduplicate code copied into different executable files.

For example, the x86-64 `opt` grew from 251,368 to 55,641,200 bytes, and `llc` grew from 164,560 to 55,739,056 bytes.
The old small executables did not contain a tiny implementation of LLVM: their implementation was largely in the shared library.
The new larger executables carry much more of it themselves.

The regular-file payloads reconcile as follows, in MiB:

| Component | Previous x86-64 | New x86-64 | New i686 |
| --- | ---: | ---: | ---: |
| Compiler executable | 3.190 | 5.884 | 6.333 |
| Four LLVM tools | 5.426 | 174.548 | 198.873 |
| Shared `libLLVM` | 117.507 | 0 | 0 |
| Other compiler-host dependencies | 38.924 | 38.340 | 38.491 |
| Native runtime | 23.247 | 23.247 | 18.897 |
| Compiler-rt | 0.253 | 0.253 | 0.134 |
| Documents and receipts | 0.413 | 0.428 | 0.426 |
| Total | 188.959 | 242.700 | 263.155 |
| Increase over previous x86-64 | — | 53.741 | 74.195 |

Exact regular-file totals are 198,138,359, 254,489,482, and 275,937,545 bytes respectively.
These totals exclude directory and symlink entries and measure file lengths, not allocated disk blocks.
That is why they differ from the rounded `du -sh` figures of 190, 244, and 265 MiB.

For x86-64, the four tool executables gained 169.123 MiB while removal of the shared LLVM file saved 117.507 MiB.
That group alone therefore grew by 51.616 MiB.
Add the compiler executable's 2.695 MiB increase and the small net changes to other files, and the full payload increase is 53.741 MiB.
This reconciles the observed growth; it is not a claim that all of the group's increase is caused by static linking alone.

### A further packaging mismatch: stripped versus unstripped tools

The old distribution's four LLVM tools are stripped; the new four tools are not.
The new tools' ordinary `.symtab` and `.strtab` sections occupy 27.140 MiB on x86-64 and 25.441 MiB on i686.
Those sections are part of the tool totals above, not an additional amount to add to the reported regression.
They are symbol and name tables, not evidence that the SDK was built in Debug mode.

Both old and new compiler executables are unstripped, so the main compiler's growth cannot be explained by that stripped/unstripped distinction.
The code and read-only data measurements above remain important.

This means my earlier explanation emphasizing only static duplication was incomplete.
I also shipped source-built tools with a different symbol-handling policy from the baseline, and should have caught that in an early packaging audit.
No stripping experiment was performed during this retrospective, so the table-section measurements are not a measured post-stripping package size.
Simply removing symbol tables would not address all the observed growth.

There is also a measurable difference between the new architectures: the i686 tools contain 20.268 MiB more `.eh_frame` data than their x86-64 counterparts.
That is unwind metadata and explains a substantial section-level part of their size difference.
It is not evidence that every 32-bit build must be larger, nor permission to delete unwind information without understanding and testing its uses.

The new source SDK uses Release `-O3`, RTTI, and no LLVM LTO; equivalent settings for the old distribution build were not established.
A proper specialization-only experiment must control these settings and symbol handling, as well as linkage and architecture.

Both distributions still contain LLVM's stock X86 family, including both x86 widths, and LLD's stock multi-format and relocation support.
The native C runtime set was intentionally retained rather than minimized.
Those are separate footprint boundaries from removing the project's opposite native backend.

The correct conclusion is that native implementation isolation passed, while the package-size result regressed.
The evidence at that checkpoint did not establish how small a specialized shared-LLVM bundle could be.

## How I would shorten and improve this work

### 1. Establish separate acceptance criteria before implementation

Track these as independent properties:

- The compiler actually runs on its device ABI.
- The device compiler contains only its own native implementation.
- The publisher does not invoke a device compiler.
- Upstream dependencies have the intended capability inventory.
- Total distributed bytes meet an agreed footprint expectation.
- The same artifacts pass the required tests on both devices.

Do not replace a size requirement with an easier-to-test proxy such as “no shared LLVM library.”
Record the baseline executable sizes, dependency closure, complete bundle size, build flags, and expected cold-build cost first.
If the proposed distribution grows instead of shrinking, surface that tradeoff before treating it as the chosen final packaging design.

### 2. Change one architectural variable at a time

First validate the project's backend separation using the existing x86-64 SDK and existing linkage where practical.
Measure the effect of that change with a consistent build configuration.
This is an intermediate engineering checkpoint, not a claim that the old multi-backend SDK is the final minimal distribution.

Then evaluate dependency packaging separately.
The pinned LLVM source supports a shared-library/tool-linking configuration; retaining a shared library is not the same as retaining both project-native backends in the device compiler.
A shared LLVM library restricted to the X86 backend family is a candidate to measure, not an already demonstrated size fix.
It would still need separate native 32-bit and 64-bit builds.

### 3. Put a footprint measurement before the expensive rollout

Inspect existing SDK libraries and upstream build options before assuming a full SDK replacement is necessary.
Prototype and measure the proposed linking strategy before committing both device builds to it.
Compare entire dependency closures, not only the main executable or individual static archives.
Keep optimization settings and symbol-handling policy comparable, or explicitly report differences.
Check `file`, `readelf -SW`, and `readelf -d` on the installed tools, not just the CMake options or build-tree archive sizes.
Separate ordinary symbol-table overhead, unwind metadata, and loadable machine code when explaining a regression.

This would reveal the risk of per-tool LLVM duplication before describing the result as a thin compiler.
It does not justify promising a particular smaller size or completion time without measurements.

### 4. Keep the critical path explicit and use safe parallel work

Retain early native-ABI and small MLIR link/run probes before building the complete tool suite.
Start the required i686 infrastructure build as soon as its input/configuration decision is stable.
Use the waiting time for the backend split, tests, packaging, and documentation.
Do not start incompatible rebuilds of shared build directories in parallel.

Assign one owner to each long-running command and record its log, session, and next gate.
Start the next required gate promptly when its prerequisite completes.
Delegate independent audits rather than leaving the critical command waiting for an acknowledgement.

Use measured resource headroom to choose concurrency, and preserve diagnostics on failure.
Do not add an unlimited retry loop, hide compiler crashes, or assume more jobs would have been safe on this host.

### 5. Reuse valid evidence without weakening final qualification

Use fast component tests and the small same-artifact matrix during development.
Use retained-artifact replay for quick feedback while infrastructure builds, but label it accurately.
Run the complete fresh corpus against the final functional candidate.

Do not rerun an hours-long LLVM build for a metadata-only change when Ninja correctly reports no changed object commands.
The final receipt refresh already demonstrated this correctly.
For documentation-only repackaging, compare compiler, SDK, runtime bytes, and symlinks with the qualified package and rerun relocation/matrix checks as appropriate.
Do not silently extend an old qualification claim to changed compiler or runtime bytes.

### 6. Communicate the result and cost honestly

Separate “project changes implemented,” “LLVM SDK still building,” and “final qualification passed” in progress reports.
Explain cold source-build time separately from ordinary test time.
Report a size regression immediately and numerically; do not use “specialized” or “thin” as evidence that the total package became smaller.

I cannot responsibly promise that this alternative sequence would turn the whole task into a particular number of minutes.
Genuine 32-bit tools and full qualification still cost time.
It would, however, expose the packaging tradeoff earlier, reduce avoidable rebuilding and coordination delay, and keep a functional checkpoint from being confused with a successful size optimization.

## Evidence locations and review commands

The comparison used an ignored local baseline and the original x86-64 and i686 device bundles from commit `63592ab`.
Those local directories are not included in a fresh Git checkout, and the later rename does not rename their historical contents or receipts.
The measurements above are therefore recorded here rather than relying on their indefinite availability.

Source build histories are under `.sdk/consumer/build-native-generators/.ninja_log`, `.sdk/consumer/build-i686/.ninja_log`, and `.sdk/consumer/build-x86_64/.ninja_log`.
Bootstrap, retry, and receipt-refresh logs are under `.sdk/consumer/`.
The complete corpus report and real-kernel VM evidence were retained in private temporary directories for the 2026-09-12 run.
Their original exact paths, along with this original report, can be inspected with `git show be63890:docs/reference/device-compiler-retrospective.md`.
Temporary evidence can disappear; the [qualification reference](qualification-corpus.md) records the durable checkpoint and replay procedure.

For newly built Sela bundles, read-only checks from the repository root include the following; replace the example paths with actual current outputs.
These commands do not reconstruct the old measurements or convert historical packages into current-format bundles:

```bash
python3 -B scripts/bundle-size.py /path/to/current-bundle --compare /path/to/comparison-bundle
python3 -B scripts/bundle-size.py /path/to/current-bundle --json --compressed
stat -c '%s %n' /path/to/current-bundle/bin/selac
size -A /path/to/current-bundle/bin/selac
readelf -d /path/to/current-bundle/bin/selac
du -sh /path/to/current-bundle
```

The bundle reporter counts hardlinked file data once and does not follow symlinks.
It separates regular-file bytes, filesystem allocation, and ELF sections, and identifies archive-exclusive dependencies without adding them to the total twice.
`--compressed` measures a deterministic tar/gzip-1 stream without writing an archive; omit it for the fastest report.
New measurements describe the files currently present, not a replacement for the historical measurements above.

This retrospective changed documentation only.
It did not rebuild, strip, relink, or replace the qualified binaries.
