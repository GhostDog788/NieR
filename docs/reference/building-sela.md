# Building Sela and setting up VS Code

[Documentation home](../README.md) · [Project reference](README.md)

Keep `docs/` open as the Obsidian vault and the repository root open in VS Code.
Run the commands below in **Bash from the repository root**, not from the `docs/` directory.

## Host and SDK prerequisites

The supplied, qualified publisher-host baseline is **x86-64 Ubuntu 24.04**.
The SDK extracts pinned, unmodified Ubuntu packages locally; it does not install system packages or patch upstream compilers.
Read the [development SDK reference](development-sdk.md) for the required host tools, disk space, and non-hermetic host dependencies before bootstrapping.

```bash
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
```

Bootstrap creates the repository-local `.sdk` by default.
Source `sdk/env.sh` again in each new terminal session used for these commands.
The pinned Clang, LLVM, MLIR, and LLD versions must stay coordinated with the Sela build.
The host PC can cross-build all four device compilers.
Device target metadata is independent of build-host identity; developing on ARM is not structurally excluded, but a qualified ARM publisher-host package set is not supplied in this milestone.
An ARM or future RISC-V consuming device does not imply that Sela itself must be built on that architecture.

## Build the publisher and consumer

Use the checked-in preset to create the standard `build/prealpha` build:

```bash
cmake --preset prealpha
cmake --build --preset prealpha
```

The result includes the separate `build/prealpha/selac` compiler and the developer-side Clang integration configured by `build/prealpha/sela.cfg`.
Keep that configuration and its matching plugin/build products together.
After a pre-alpha contract or SDK change, rebuild the tools and regenerate application artifacts rather than mixing versions.

To run the normal test suite after building:

```bash
ctest --preset prealpha
```

The full upstream [qualification corpus](qualification-corpus.md) is a separate, longer-running workflow.
It is not required merely to read the guides or try the first application.

### Application-publication emulation prerequisites

The Make/CMake publishing workflow runs real native builds for every admitted target.
Configure probes and project generators may execute those native outputs directly, including nested `./generator` commands.
Before running that workflow on x86, explicitly provision the matching i686 loader and QEMU user-mode/binfmt handlers for ARM32 and AArch64.
On the Ubuntu baseline, `libc6-i386` supplies the i686 loader and `qemu-user-binfmt` supplies user emulators and their system registration machinery; administrator installation/setup is a separate, deliberate host operation.
Package installation alone is not assumed sufficient on every host: the publisher's real direct-execution probes must pass.
Sela does not install packages, register global handlers, or silently replace failed configure results.

`SELA_EMULATOR_ARMV7` and `SELA_EMULATOR_AARCH64` can select existing emulator executables for explicit CMake probes.
They do not remove the requirement for automatic direct execution of generated target programs.
The publisher uses target-aware CMake settings and `CHOST` for the supported zlib configuration while retaining host-native Clang and its matching plugin.
The host's `uname` still describes the host; a project that independently treats it as target identity needs appropriate supported cross-configuration arguments or a project adapter.
This is not a promise that every arbitrary target-native build script works unchanged on an x86 host.

These prerequisites are for application publication, not for compiling Sela's four device bundles.
The separate device acceptance harness uses `qemu-system-*` with real target kernels; user emulation is not substituted for that qualification.

## Build the independent native device compilers

The distributable consumer presets use their own target-specific, source-built LLVM/MLIR SDKs.
After bootstrapping and sourcing the publisher SDK above, select a device product:

| Target/preset suffix | Native application policy | Stock LLVM backend family |
| --- | --- | --- |
| `x86_64` | Linux/glibc, little-endian SysV AMD64 | X86 |
| `i686` | Linux/glibc, little-endian SysV i386 | X86 |
| `armv7` | Linux/glibc, little-endian ARMv7-A, hard-float VFPv3-D16 | ARM |
| `aarch64` | Linux/glibc, little-endian ARMv8-A AArch64 | AArch64 |

The ARMv7 baseline does not require NEON.
The authoritative triples, layouts, flags, and runtime names are in `sdk/targets.json`, accessed through `sdk/targets.py` and the generated C++ registry.
The same commands work for each row; the example builds and packages ARMv7 entirely on the x86 PC:

```bash
device_target=armv7
bash scripts/bootstrap-consumer-sdk.sh "$device_target"
cmake --preset "consumer-$device_target"
cmake --build --preset "consumer-$device_target"
mkdir -p artifacts
bash scripts/package-consumer.sh "build/consumer-$device_target" \
  "artifacts/selac-$device_target" ".sdk/consumer/$device_target"
```

Repeat with another target ID to build another independent product.
The output directory must not already exist; use a fresh path when comparing package candidates.
These are substantial upstream source builds. They retain unmodified LLVM/MLIR 18.1.3 and register only the selected backend family.
The default shared-LLVM layout lets compiler tools share one native library while keeping MLIR static; `SELA_CONSUMER_LLVM_LINKAGE=static-components` selects the comparison layout.
Consumer SDK identity checks and native Sela specialization are independent of that linkage choice.
See the distribution reference for the measured and qualified checkpoint; a newly configured candidate does not inherit an older qualification result.
`SELA_CONSUMER_COMPILE_JOBS` accepts 1–8 and defaults to 2 compile jobs per target.
`SELA_CONSUMER_PARALLEL_TARGETS=0|1` defaults to 0; set it to 1 for each concurrent process as shown below.
There is no requirement that x86-64 finish or pass qualification before another target starts.
Each SDK is `.sdk/consumer/<target>` and its compiler build is `build/consumer-<target>`.
Build-host TableGen, Clang, CMake, Ninja, and LLD remain host-native.
Bootstrap and packaging inspect cross-built ELF files and provenance receipts without executing the target `llc`, `opt`, linker, or compiler.

Each product excludes the Clang frontend plugin, capture/replay tools, and LLVM-to-Sela producer.
Stock Clang compiles Sela's own C++ sources on the build host; it is not an input or dependency of the resulting on-device compiler.
A device `selac` compiles and lowers only its own native target, even though the shared artifact schema describes all four domains.
Each bundle contains only its native runtime and backend family, not the other three device implementations.
Shared X86 backend internals are retained for the two x86 products; this does not mean either compiler admits the other target ABI.
Use each build's own CTest inventory, not the publisher-only CI smoke selection, and execute foreign-target tests through the matching device environment.
The four consumer CTest presets describe their respective inventories; simply invoking an ARM preset on x86 is not real-device qualification.
For a native x86-64 build, the local component command is `ctest --preset consumer-x86_64`.
The i686 host-side tests exercise compatibility mode and are not genuine 32-bit-kernel acceptance.
They also require the build host's 32-bit glibc loader at `/lib/ld-linux.so.2`, supplied on Ubuntu by `libc6-i386` or a matching multiarch libc installation.
Extracting an SDK sysroot does not install that system loader; the device-qualification CI workflow installs the host prerequisite explicitly.
Device acceptance compiles the identical published artifacts on all four consuming architectures and runs the resulting native applications under their real target kernels.
The current C fixture inventory and complete cJSON/zlib corpus remain part of that gate; same-width targets are not treated as substitutes for each other.
Functional acceptance and a performance sanity check are distinct from broad performance-parity or production-readiness claims.

See the [compiler distribution reference](compiler-distribution.md) for runtime prerequisites and the manually dispatched `Device compiler qualification` GitHub workflow.
After assembly, run a bundle's `bin/selac --print-target` and `bin/selac --check-sdk` on its matching device to check target selection and installation before compiling an artifact.
Clear `SELA_SDK_ROOT` when checking sibling-SDK discovery; an old compiler and a newly built SDK must not be mixed merely because both target the same architecture.
Measured bundles retain their assembly-time README snapshots and are not changed merely to refresh qualification text.
The live distribution reference distinguishes subsequent results for those same payloads from newly built packages.
It also preserves the historical test-only VM dependency correction and cJSON report's initial-versus-final test-tools lock transition at `be63890`; those compiler packages were not changed to repair the test harness.

### Historical x86 checkpoints

The pre-rename static-component bundles passed their component tests, two-device matrix, and complete corpus at commit `63592ab` on 2026-09-12, including a real i686 kernel.
The pre-rename shared-LLVM checkpoint recorded in `be63890` measured 91.89 MiB for x86-64 and 98.28 MiB for i686 and passed the full 50-artifact corpus on 2026-09-13, establishing the shared default.

The subsequent, pre-ARM Sela packages measured approximately 91.9 MiB and 98.3 MiB.
They separately passed all 11 consumer CTests per device, all 40 publisher tests, the two-device matrix, and the complete dual-device zlib corpus.
cJSON completed publication/native/x86-64 checks plus a separately passing real-i686 continuation using the same 42 artifacts after a failed first VM boot; that initial failed command remains recorded unchanged.
These sizes, test counts, and executions describe those historical payloads, not the new four-target products.
New packages need their own byte measurements, hashes, and qualification records; a successful configure or component test does not inherit an older gate's result.

### Parallel SDK source builds

To build two selected SDKs concurrently, use this opt-in example after preparing the publisher SDK.
It allows 2 compile jobs per architecture, up to 4 target compilation jobs combined, and records separate logs:

```bash
parallel_sdk_logs=$(mktemp -d "${TMPDIR:-/tmp}/sela-sdk-build-XXXXXX")
SELA_CONSUMER_PARALLEL_TARGETS=1 SELA_CONSUMER_COMPILE_JOBS=2 \
  bash scripts/bootstrap-consumer-sdk.sh armv7 \
  >"$parallel_sdk_logs/armv7.log" 2>&1 &
sdk_armv7_pid=$!
SELA_CONSUMER_PARALLEL_TARGETS=1 SELA_CONSUMER_COMPILE_JOBS=2 \
  bash scripts/bootstrap-consumer-sdk.sh aarch64 \
  >"$parallel_sdk_logs/aarch64.log" 2>&1 &
sdk_aarch64_pid=$!
sdk_build_status=0
wait "$sdk_armv7_pid" || sdk_build_status=1
wait "$sdk_aarch64_pid" || sdk_build_status=1
printf 'SDK build logs: %s\n' "$parallel_sdk_logs"
test "$sdk_build_status" -eq 0
```

Both processes are awaited even if one fails.
If either fails, inspect its log and resolve the error before configuring the corresponding Sela preset; do not repeatedly retry a compiler crash without diagnosis.
After both complete successfully, run the matching configure/build/package commands above and arrange consuming-device tests.

Job limits are per architecture, so higher values multiply CPU and memory demand when both builds are active.
Use available memory as well as CPU utilization to choose them; launching all four targets multiplies demand again.
This example is not a claim that any particular job count fits every host.
Separate profile build locks prevent simultaneous writers to one profile's cache, and native-generator builds remain serialized.
The target C/C++ linker launchers share a global lock so the memory-heavy links do not run concurrently with each other, though compilation may continue beside a link.
Do not run different linkage variants against the same profile's SDK/build directories in parallel.

## Configure VS Code

Open the repository root and enable the recommended **clangd** and **CMake Tools** extensions.
Select **Sela pre-alpha (pinned SDK)** if CMake Tools asks for a configure preset.
You do not need to launch VS Code from a shell that sourced `sdk/env.sh`: the checked-in CMake/CTest launchers load the SDK environment for the editor.

clangd reads the real compile database at `build/prealpha/compile_commands.json`.
The `.clangd` configuration keeps C examples in C mode even when clangd borrows a C++ build command.
The workspace disables duplicate Microsoft IntelliSense diagnostics, not clangd's error checking.
Fixtures requiring generated headers or special test flags still need their own build context.

To regenerate the database without a full build, run **CMake: Configure** in VS Code, or:

```bash
./scripts/cmake-sdk.sh --preset prealpha
```

If the editor was already open when its configuration changed, run **Developer: Reload Window**.
To inspect a source reference from the docs, press **Ctrl+P** in VS Code and enter its repository-relative path.

## Continue to your first application

Follow [Hello World end to end](../guides/02-toolchain-users/08-hello-world-end-to-end.md) for an explained walkthrough,
or [publish your existing C project](../guides/02-toolchain-users/using-sela-with-your-c-project.md) for Make/CMake recipes.
The [development-environment chapter](../guides/02-toolchain-users/07-development-environment.md) explains why the host, target sysroots, and native runtime are separate concerns.
