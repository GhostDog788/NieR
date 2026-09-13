# Native targets and adding another architecture

[Project reference](README.md) · [Implementation plan](../02-implementation-plan.md#173-four-target-generic-infrastructure-increment) · [Compiler distribution](compiler-distribution.md)

Sela separates the machine running the publisher, the machine running a device compiler, and the native ABI that compiler emits.
The development publisher uses host-native Clang and can capture every declared target.
A distributed `selac` is a genuinely native executable for one target and compiles only for that target.
It does not contain language frontends or call the publisher.

## Four targets, one platform

| Target ID | Word size | Native ABI and CPU baseline | LLVM family |
| --- | --- | --- | --- |
| `x86_64` | 64 bits | Linux/glibc, System V AMD64, baseline x86-64 | X86 |
| `i686` | 32 bits | Linux/glibc, System V i386, i686 | X86 |
| `armv7` | 32 bits | Linux/glibc, little-endian ARMv7-A, AAPCS32 hard-float VFPv3-D16 | ARM |
| `aarch64` | 64 bits | Linux/glibc, little-endian ARMv8-A LP64, AAPCS64 | AArch64 |

These are equal products with the same admitted Sela operations and C qualification obligations.
ARM is not an Android port and does not use a different libc.
The two x86 products may share upstream LLVM's X86 backend internals without allowing either `selac` to compile for the other target.

Two targets having the same pointer width does not make their ABIs identical.
For example, ARM32 and i686 differ in `double` alignment and glibc's nonlocal-jump storage.
ARM and x86 also have different default plain-char signedness in Clang; explicit source compilation flags remain authoritative.
The publisher preserves the resulting operations, not a C language mode that a device compiler must interpret.

## Where target knowledge lives

| Concern | Repository location |
| --- | --- |
| Target IDs, triples, layouts, native ABI, CPU/features, sysroot/loader, ELF identity | `sdk/targets.json` |
| Shell/Python/CMake registry access and generated C++ consistency | `sdk/targets.py`, `sdk/test_targets.py`, `cmake/SelaTargets.cmake` |
| Shared C++ descriptors | `include/sela/Targets.h`, generated `src/targets/Targets.cpp` |
| Public target sets, local choices, specialization | `include/sela/IR/Domains.h`, `src/ir/Domains.cpp` |
| N-observation graph factoring | `src/ir/CommonMerge.cpp`, `src/ir/Producer.cpp` |
| Native ABI adapter selection | `src/ir/abi/*.cmake`, `src/ir/NativeTargetConfig.h` |
| ARM aggregate and variadic rules | `src/ir/NativeARMABI.cpp`, `src/ir/NativeARMVariadic.cpp` |
| Target SDK/bootstrap and distribution checks | `sdk/consumer/`, `scripts/bootstrap-consumer-sdk.sh`, `scripts/package-consumer.sh` |
| Matching-kernel functional qualification | `tests/multi-consumer.sh`, `tests/consumer-vm.sh`, `tests/vm/` |

Source paths are for VS Code; all Markdown navigation stays inside the `docs/` Obsidian vault.

## What is shared in the artifact?

A public module declares a finite `sela.targets` set.
Shared functions and control flow remain one program graph.
Native-word values and pointer-size relationships are symbolic; other supported differences use local target cases or guarded regions.
This includes attributes, type/layout alternatives, array extents, overlap membership, and module-flag presence.
There are no four complete target-specific LLVM modules hidden behind the manifest.

Each device validates the entire public structure, including inactive alternatives, then specializes its own target.
It reports foreign native validation as unavailable and rejects foreign compilation requests.
It does not need foreign native ABI adapters to validate the common schema.

The Clang producer privately observes every target in the requested publication domain and proves every native inverse before publishing.
The publisher's validation libraries can contain all target adapters; no distributed device compiler executable is needed for publication.
An independent producer can construct the same public format without Clang captures.
Adding a fifth registry entry does not qualify an existing four-target artifact for that new target: republish or independently establish its semantics.

## Building a device product

Bootstrap and source the publisher SDK first, as described in [Building Sela](building-sela.md).
Then select one target; the four checked-in consumer presets are independent alternatives:

```bash
device_target=armv7
bash scripts/bootstrap-consumer-sdk.sh "$device_target"
cmake --preset "consumer-$device_target"
cmake --build --preset "consumer-$device_target"
mkdir -p artifacts
bash scripts/package-consumer.sh "build/consumer-$device_target" \
  "artifacts/selac-$device_target" ".sdk/consumer/$device_target"
```

The package output directory must not already exist.
Source LLVM builds are substantial; use the [parallel build controls](building-sela.md#parallel-sdk-source-builds) with a total memory-aware job budget.
Neither an SDK build nor a component-test pass alone establishes product qualification.

On a foreign development host, application build-time executables need explicitly provisioned QEMU/binfmt support.
The toolchain checks that support and fails with instructions instead of installing handlers or inventing configure results.
User-mode emulation is a development convenience, not a replacement for the genuine 32-bit-kernel qualification of a 32-bit product.
An ARM-native development SDK is a separate qualification task; generic source/build infrastructure is not evidence that it has passed.

## Adding a fifth target

1. Choose an exact OS/libc ABI, endian model, baseline CPU/features, loader, and runtime contract.
   Confirm these with the pinned stock frontend rather than deriving them from word size.
2. Add its descriptor to `sdk/targets.json` and regenerate `src/targets/Targets.cpp` with `sdk/targets.py cpp`.
   Run the registry tests and inspect the diff; generated C++ must match the registry exactly.
3. Add authenticated, hash-pinned sysroot, compiler-runtime, and build dependency inputs.
   Reuse stock Clang/CMake and the appropriate LLVM backend family.
   Check library API ABIs separately from ELF machine identity: the pinned ARM32 libarchive uses 64-bit `time_t`, requiring matching definitions in its two C++ callers only.
4. Select a compatible existing native ABI adapter, or add a localized adapter and its CMake recipe.
   LLVM handles optimization and machine code generation, but Sela still owns record/variadic boundary semantics and their proofs.
5. Add the native consumer preset and matching-kernel test environment.
   Every shipped compiler tool must have the target's ELF class and machine, and the bundle must contain only its target runtime/sysroot.
6. Run the same source-to-artifact, native ABI, source-free device, and complete configured cJSON/zlib tests as every existing target.
   Include same-width cross-target controls and rejection of malformed inactive cases.

The expected work is localized, not automatically zero-code.
A backend name alone does not establish target support, and a difficult ABI case must not cause a reduced target-specific test tier.
The current shared semantics cover the admitted C subset; new language semantics remain a separate public-contract change even if LLVM can compile that language.

## Acceptance and performance claims

Prepare the core fixtures once, then give the printed fixture directory to each independent target job:

```bash
bash tests/multi-consumer.sh --prepare-only build/prealpha/sela.cfg .sdk
bash tests/consumer-vm.sh armv7 artifacts/selac-armv7 /absolute/path/to/fixtures
```

Use [the corpus workflow](qualification-corpus.md) for all 42 cJSON and eight zlib artifacts and their original tests.
Preparation is not a device PASS.
Retain identical artifact hashes, exact build/package identities, native-reference outputs, and explicit fresh VM receipts.

This increment records lightweight timing only; it does not enforce a numerical benchmark threshold.
Each target must agree with its own native behavior, not another CPU's integer widths or elapsed time.
Native-equivalent performance, reverse-engineering resistance, other libc platforms, and SES security remain separate acceptance obligations in 01.
