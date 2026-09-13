# Building NieR and setting up VS Code

[Documentation home](../README.md) · [Project reference](README.md)

Keep `docs/` open as the Obsidian vault and the repository root open in VS Code.
Run the commands below in **Bash from the repository root**, not from the `docs/` directory.

## Host and SDK prerequisites

The current development baseline is **x86-64 Ubuntu 24.04**.
The SDK extracts pinned, unmodified Ubuntu packages locally; it does not install system packages or patch upstream compilers.
Read the [development SDK reference](development-sdk.md) for the required host tools, disk space, and non-hermetic host dependencies before bootstrapping.

```bash
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
```

Bootstrap creates the repository-local `.sdk` by default.
Source `sdk/env.sh` again in each new terminal session used for these commands.
The pinned Clang, LLVM, MLIR, and LLD versions must stay coordinated with the NieR build.

## Build the publisher and consumer

Use the checked-in preset to create the standard `build/prealpha` build:

```bash
cmake --preset prealpha
cmake --build --preset prealpha
```

The result includes the separate `build/prealpha/nierc` compiler and the developer-side Clang integration configured by `build/prealpha/nier.cfg`.
Keep that configuration and its matching plugin/build products together.
After a pre-alpha contract or SDK change, rebuild the tools and regenerate application artifacts rather than mixing versions.

To run the normal test suite after building:

```bash
ctest --preset prealpha
```

The full upstream [qualification corpus](qualification-corpus.md) is a separate, longer-running workflow.
It is not required merely to read the guides or try the first application.

## Build the independent native device compilers

The distributable consumer presets use their own target-specific, source-built LLVM/MLIR SDKs.
After bootstrapping and sourcing the publisher SDK above, build the desired device product; these commands show both independent alternatives:

```bash
bash scripts/bootstrap-consumer-sdk.sh x86_64
cmake --preset consumer-x86_64
cmake --build --preset consumer-x86_64
ctest --preset consumer-x86_64

bash scripts/bootstrap-consumer-sdk.sh i686
cmake --preset consumer-i686
cmake --build --preset consumer-i686
ctest --preset consumer-i686
```

These are substantial upstream source builds. They retain unmodified LLVM/MLIR 18.1.3 and register only the X86 backend family.
The default shared-LLVM layout lets compiler tools share one native library while keeping MLIR static; `NIER_CONSUMER_LLVM_LINKAGE=static-components` selects the comparison layout.
Consumer SDK identity checks and native NieR specialization are independent of that linkage choice.
See the distribution reference for the measured and qualified checkpoint; a newly configured candidate does not inherit an older qualification result.
`NIER_CONSUMER_COMPILE_JOBS` accepts 1–8 and defaults to 2 compile jobs per target.
`NIER_CONSUMER_PARALLEL_TARGETS=0|1` defaults to 0; set it to 1 for both processes to allow the two architectures to build concurrently as shown below.
The order of the examples above is not a requirement that x86-64 finish or pass qualification before i686 can start.
The resulting SDKs are `.sdk/consumer/x86_64` and `.sdk/consumer/i686`; corresponding compiler builds are `build/consumer-x86_64` and `build/consumer-i686`.

Each product excludes the Clang frontend plugin, capture/replay tools, and LLVM-to-NieR producer.
Stock Clang compiles NieR's own C++ sources on the build host; it is not an input or dependency of the resulting on-device compiler.
A device `nierc` compiles and lowers only its own native target, even though the shared artifact schema can describe both domains.
Use each build's own CTest inventory, not the publisher-only CI smoke selection.
The i686 host-side tests exercise compatibility mode and are not genuine 32-bit-kernel acceptance.
They also require the build host's 32-bit glibc loader at `/lib/ld-linux.so.2`, supplied on Ubuntu by `libc6-i386` or a matching multiarch libc installation.
Extracting an SDK sysroot does not install that system loader; the device-qualification CI workflow installs the host prerequisite explicitly.
The original static-component bundles passed their component tests and the fresh publish-once two-device matrix, including the real i686 kernel.
Their complete fresh dual-destination corpus also passed at the recorded 2026-09-12 checkpoint.
The new shared-LLVM packages measure 91.89 MiB for x86-64 and 98.28 MiB for i686 in regular-file payloads, smaller than both compact static-component packages.
Each passed all 11 consumer CTests, and the fresh same-artifact matrix passed on both devices, including the real i686 kernel; all 40 publisher tests also passed.
The final shared packages also passed the full 50-artifact dual-device corpus through complete fresh cJSON and zlib project runs on 2026-09-13; shared LLVM is the adopted default layout.
A component-test pass alone is not a substitute for that separate gate, and these bounded qualification results are not a production-release claim.

See the [compiler distribution reference](compiler-distribution.md) for packaging, runtime prerequisites, the real i686 VM, and the manually dispatched `Device compiler qualification` GitHub workflow.
After assembly, a bundle's `bin/nierc --print-target` identifies its native target and `bin/nierc --check-sdk` checks its selected installation without compiling an artifact.
Clear `NIER_SDK_ROOT` when checking sibling-SDK discovery; an old compiler and a newly built SDK must not be mixed merely because both target the same architecture.
Measured bundles retain their assembly-time README snapshots and are not changed merely to refresh qualification text.
The live distribution reference records later results for those same payloads.
It also records a test-only VM dependency correction and the cJSON report's initial-versus-final test-tools lock transition; the compiler packages were not changed to repair the test harness.

### Parallel SDK source builds

Instead of the two sequential consumer bootstrap commands above, use this opt-in example after preparing the publisher SDK.
It allows 3 compile jobs per architecture, up to 6 target compilation jobs combined, and records separate logs:

```bash
parallel_sdk_logs=$(mktemp -d "${TMPDIR:-/tmp}/nier-sdk-build-XXXXXX")
NIER_CONSUMER_PARALLEL_TARGETS=1 NIER_CONSUMER_COMPILE_JOBS=3 \
  bash scripts/bootstrap-consumer-sdk.sh x86_64 \
  >"$parallel_sdk_logs/x86_64.log" 2>&1 &
sdk_x86_64_pid=$!
NIER_CONSUMER_PARALLEL_TARGETS=1 NIER_CONSUMER_COMPILE_JOBS=3 \
  bash scripts/bootstrap-consumer-sdk.sh i686 \
  >"$parallel_sdk_logs/i686.log" 2>&1 &
sdk_i686_pid=$!
sdk_build_status=0
wait "$sdk_x86_64_pid" || sdk_build_status=1
wait "$sdk_i686_pid" || sdk_build_status=1
printf 'SDK build logs: %s\n' "$parallel_sdk_logs"
test "$sdk_build_status" -eq 0
```

Both processes are awaited even if one fails.
If either fails, inspect its log and resolve the error before configuring the corresponding NieR preset; do not repeatedly retry a compiler crash without diagnosis.
After both complete successfully, run the matching configure/build/test preset commands above.

Job limits are per architecture, so higher values multiply CPU and memory demand when both builds are active.
Use available memory as well as CPU utilization to choose them; this example is not a claim that 6 or 16 combined jobs fit every host.
Separate profile build locks prevent simultaneous writers to one profile's cache, and native-generator builds remain serialized.
The target C/C++ linker launchers share a global lock so the memory-heavy links do not run concurrently with each other, though compilation may continue beside a link.
Do not run different linkage variants against the same profile's SDK/build directories in parallel.

## Configure VS Code

Open the repository root and enable the recommended **clangd** and **CMake Tools** extensions.
Select **NieR pre-alpha (pinned SDK)** if CMake Tools asks for a configure preset.
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
or [publish your existing C project](../guides/02-toolchain-users/using-nier-with-your-c-project.md) for Make/CMake recipes.
The [development-environment chapter](../guides/02-toolchain-users/07-development-environment.md) explains why the host, target sysroots, and native runtime are separate concerns.
