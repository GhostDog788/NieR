# NieR: Implementation Plan

## 1. Authority, scope, and current status

[01-architecture-design.md](01-architecture-design.md) remains the approved
requirements contract. This document specifies the selected implementation
direction, the C MVP, and the subsequent work required to satisfy all of 01.
It does not change those requirements.

**NieR** is the standalone toolchain and portable code format.
**SENieR**, shortened to **SEN**, is the security platform layered above NieR;
the `SE` prefix follows the naming pattern of SELinux.
This plan delivers the independent NieR flow first, then implements SEN after standalone acceptance.

**Status on 2026-09-11:** the independent NieR publication/compiler flow works,
including stock-Clang publication, native ABI/build fixtures, and the complete
configured cJSON/zlib corpus. The previous combined program and publication
recipes have been removed. This is a qualified pre-alpha C implementation,
not unrestricted C or completion of 01, general dependency management,
performance parity, or RE parity. Section 17 records the verified shapes and
remaining limits rather than inferring broad support from test counts.

**Pre-alpha policy:** there are no backward-compatibility obligations anywhere.
Formats, commands, APIs, and configuration may change between commits. Remove
obsolete interfaces rather than preserving aliases, readers, or migrations.
Current-format validation and SDK integrity checks are correctness requirements,
not compatibility services.

The next deliverable is a working C MVP, not the complete product:

1. Establish stock-Clang publication and independent native compilation with
   multi-file Hello World and native-width probes.
2. Expand it into the agreed broad-C functional MVP, including normal build
   integration and the configured cJSON/zlib corpus.
3. Complete the remaining C facilities, languages, targets, platform services,
   release lifecycle, and full standalone acceptance gates.
4. Only after complete standalone acceptance, implement and qualify SEN.

The first C work does **not** wait for Rust, Go, all four runtime targets, or
full A1/A2 acceptance. Conversely, completing Hello World does **not** complete
the C MVP; completing the C MVP does **not** complete T1-T9.

All compiler and linker infrastructure is used unmodified. Do not fork or
patch existing compilers. Reuse supported tools, plugins, extension interfaces,
and libraries. Writing replacement source-language compilers would defeat the
chosen reuse strategy. SEN is not a dependency of any
standalone build, publication, compilation, or execution command.

## 2. Selected architecture and boundaries

### 2.1 The pipeline

~~~text
Publisher's private workspace
  ordinary source + normal project build
    -> existing compiler/build components for each captured native profile
    -> private native-profile LLVM IR and capture evidence
    -> one shared LLVM-to-common-IR merger
    -> common, target-parametric MLIR dialect
    -> common optimization and publication minimization
    -> versioned experimental publication artifact

Separate destination workspace
  artifact + matching device compiler + supplied native SDK
    -> common-IR validation and target specialization
    -> target-specific LLVM IR
    -> unmodified LLVM optimization and object-code generation
    -> unmodified LLD with normal SDK/linker configuration
    -> final native ELF
    -> direct execution through the normal unmodified Linux ELF loader
~~~

Our custom program transformation ends at target-specific LLVM IR. Producing
native objects and the final ELF is existing LLVM/LLD work, not a new compiler
backend or executable-format implementation.

There is no custom loader, launcher requirement, post-link ELF rewriting,
runtime interpreter, publication-specific JIT, resident compiler, or
language-specific device importer. LLD emits the final ELF directly, including
normal interpreter, library-search, and stripping settings.

The device compiler accepts one source-language-independent representation.
It must not dispatch to rustc, a Go compiler, or language-specific dialects to
finish compilation. Normal language runtimes may remain native dependencies
or linked native code; that does not make the device compiler language-specific.

### 2.2 NieR as the independent public boundary

NieR Code is the flagship format and pre-alpha standard. It consists of defined
source-language-independent semantics, public construction/validation APIs,
bytecode, artifact rules, and conformance tests. MLIR supplies infrastructure;
arbitrary MLIR dialects are not automatically NieR.

Our shared LLVM-to-NieR merger is one reference producer, not a mandatory
admission path. Independent producers may emit NieR directly without Clang,
native profile captures, or merger provenance. They must represent all required
semantics and provision their language runtime/dependency contract. No
language-specific importer, frontend, or extension implementation is required
on the destination for already-supported NieR semantics.

Producer-local extensions must lower to supported NieR before publication.
New required generic semantics evolve NieR itself; unknown required operations
fail. Platform-specific distributions implement the same supported semantics,
not language-specific feature subsets. They may omit publisher tools and
irrelevant target components.

For C, developers invoke actual stock Clang with a NieR configuration. A
replacement frontend action delegates source compilation to stock Clang,
captures both native LLVM profiles, and calls the shared merger. It does not
implement AST-to-NieR lowering. The stock driver invokes internal `nier-ld`
to combine NieR objects into the final artifact. Clang plugins and native
compiler inputs are confined to publisher-side targets.

The independent `nierc` links the NieR core, artifact support, and native
lowering, never the LLVM-capture producer or Clang plugin. Producer APIs live
separately from consumer/direct-NieR APIs. Consumer-only builds must work with
`NIER_BUILD_PUBLISHER=OFF`.

### 2.3 Why capture multiple native profiles

Ordinary frontend LLVM output is already target-specific. For example:

~~~c
unsigned pointer_bytes(void) { return sizeof(void *); }
unsigned literal_eight(void) { return 8; }
~~~

On x86-64 both can already be constant 8 before LLVM optimization. Native
i686 output distinguishes 4 from 8. A single x86-64 LLVM module does not
contain enough information to infer the distinction.

Private multi-profile compilation supplies additional observations. It does
not automatically recover source intent, prove portability, or provide a
general-purpose merger. Native ABI lowering, preprocessing, configuration,
layout, and generated code can change entire interfaces and graphs.

LLVM documents both the frontend commitments and the limits of target
independence. Its data layout also governs subsequent LLVM optimization.
[LLVM FAQ](https://llvm.org/docs/FAQ.html#can-i-compile-c-or-c-code-to-platform-independent-llvm-bitcode),
[LLVM target independence](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl10.html#target-independence),
[LLVM data layout](https://llvm.org/docs/LangRef.html#data-layout).

The permitted result is one common program with symbolic native properties
and explicit conditional semantics where necessary. It is not four ordinary
native programs, four target-fixed bitcode modules, or a compressed selector
over such modules.

## 3. C MVP contract

### 3.1 Targets and workspaces

Capture x86-64 Linux and i686 Linux profiles privately from the beginning.
Initial destination compilation and execution target **x86-64 only**.

Private native i686 builds and execution probes are allowed to establish
reference behavior and check capture/specialization. They are not a claim
that the publication product already supports i686 installation or execution.

Use the same physical machine initially, with separated publisher and
destination workspaces. The destination receives only the publication
artifact, device tool, and native SDK. It must not read source, capture
sidecars, original LLVM modules, original objects, or the publisher's build
tree. This separation is a compiler-input test, not a security sandbox.

C0 may support only matching graphs, simple scalar operations, strings,
native pointers, direct calls, and the native-width probe. Unsupported
operations must fail explicitly. This is an incremental subset, not a
redefinition of the agreed C MVP.

### 3.2 Functional scope at C MVP completion

| Included by C MVP completion | Deliberately deferred beyond this MVP |
|---|---|
| Ordinary C scalar arithmetic and control flow, native setjmp/longjmp | Threads, atomics, and TLS |
| Native pointers, fixed arrays, structs, and unions | Dynamic loading and plugin coverage |
| float and double | long double and complex arithmetic |
| Aggregates passed and returned by value | Advanced floating-point environment behavior |
| Function pointers, indirect calls, and native callbacks | Variable-length arrays |
| Variadic calls, scalar/pointer bodies, va_copy and native va_list forwarding | Additional nonlocal-jump extensions |
| Target-conditional sharing and differing profile source sets | Inline assembly and architecture intrinsics |
| Multiple TUs, static archives, executable and shared-library outputs | The remaining general-C and full-product matrix |
| Make and CMake integration | Production installation/update services |

The inclusion of a category is a test obligation, not a statement that the
initial compiler handles every instance. Each stage names its supported
operations and rejects unsupported constructs rather than silently emitting
incorrect native code.

Deferred ordinary features remain required where they fall under 01. The
native-payload exception is not a way to exempt difficult ordinary application
logic from the neutral path. Actual architecture-specific native components
remain a separate full-product capability.

### 3.3 Application corpus

Use unmodified upstream source releases:

- **cJSON 1.7.19**, with its configured build and test suite.
- **zlib 1.3.2**, with its configured build and test suite.

Qualify cJSON in separate static/shared CMake Release builds, running all 19
registered CTests in each with its normal setjmp/longjmp-based Unity harness.
Qualify zlib using upstream configure/Make, `CFLAGS=-O3`, `--shared`,
`make test`, and `make test64`; retain its version script and native
architecture-selected algorithms. This does not claim zlib CMake's separate
coverage/integration suite. Optional corpus configurations are documented, not
silently substituted when required tests fail.

Lock archive URLs, exact digests, build options, test commands, and expected
configuration in the corpus recipes. Upstream configuration options may be
used and must be recorded; do not edit application source to make the merger
pass. Do not silently disable a failing required test or replace a library
with a handcrafted look-alike.

cJSON and zlib are functional integration controls, not sufficient evidence of
privacy or general C completeness. Their configured tests complement the
target-semantics and native-ABI fixtures in section 11.
[cJSON releases](https://github.com/DaveGamble/cJSON/releases),
[zlib releases](https://zlib.net/).

## 4. Implementation components and pinned SDK

### 4.1 Components and implementation language

Use C++17 for the MVP, with LLVM/MLIR libraries, TableGen where appropriate,
CMake, and Ninja. Reuse LLVM's JSON support and libarchive for package handling.
Avoid a second implementation-language stack in the first slice.

The logical components are:

| Component | Responsibility |
|---|---|
| Native-profile capture plugin | Observe LLVM input and preserve private evidence before LLVM optimization |
| Publisher/build integration | Run private profile builds and collect translation-unit and link provenance |
| Shared merger | Construct and verify one target-parametric common graph |
| Common MLIR dialect | Types, operations, verifier, bytecode, and target specialization |
| Device compiler | Read common IR and emit target-specific LLVM IR |
| Stock LLVM tools/libraries | Native optimization and object generation |
| Stock LLD | Final native ELF linking |
| Stock-Clang adapter / publication linker | Emit NieR units and standalone executable/shared-library artifacts |
| Independent nierc | Inspect NieR and manually produce native output without the publisher |
| SDK/corpus/test support | Pinned inputs, native references, fixtures, and reproducible checks |

Shared libraries are permitted, but publication and native compilation are
independent programs and build targets from the first NieR checkpoint. Keep
NieR core/artifact/target lowering separate from LLVM-to-NieR capture merging.
Only publisher builds require Clang development libraries.

### 4.2 Prebuilt SDK supply

Use verified prebuilt packages in a user-local SDK, with no system package
installation and no compiler/glibc source build for the initial bootstrap.

The initial package family is Ubuntu 24.04, matching the development host:

- LLVM, Clang, MLIR, LLD, and compiler-rt 18.1.3; keep the matching vendor build,
  initially 1:18.1.3-1ubuntu1, together.
- glibc 2.39; initially the matching 2.39-0ubuntu8.8 runtime/development cohort.
- CMake 3.28.3 and Ninja 1.11.1, with exact package revisions pinned in the lock.
- Required host support libraries and libarchive development/runtime assets.

A lock records exact versions, architectures, archive locations, content
digests, and provenance. Verify downloaded bytes before extraction. Retain
sufficient repository/signature provenance to reproduce the trust decision;
a mutable latest-version URL is not a lock. Keep matching headers and binary
libraries rather than mixing different LLVM/MLIR vendor builds.

Use a retained archive or an appropriate Ubuntu snapshot when a pinned package
leaves the active mirror. This is a bootstrap choice, not the final minimum
host/kernel contract or a claim that these host tools already run on musl.
[Ubuntu snapshot service](https://snapshot.ubuntu.com/),
[Ubuntu MLIR 18 tools](https://packages.ubuntu.com/noble-updates/mlir-18-tools).

The SDK must make its compiler, runtime, library, header, and CMake package
paths explicit. Validate extracted symlinks and linker scripts; extraction
must not accidentally turn a supplied path into a reference to host files.
Use only the pinned files belonging to the selected SDK contract.

### 4.3 Native C SDK contents

Supply matching glibc headers for both capture profiles. Separate target
sysroots are the default to avoid accidental host include discovery. Matching
multiarch/multilib package layouts are acceptable when the profile recipe
resolves them explicitly and the tests demonstrate correct selection.

The x86-64 link SDK includes:

- glibc startup objects such as Scrt1.o, crti.o, and crtn.o for the selected
  normal executable mode;
- libc_nonshared.a and the appropriate standard library linker inputs;
- compiler-rt startup/end objects and builtins, or an explicitly pinned
  equivalent normal native support configuration;
- the matching unmodified libc.so.6, libm.so.6, and ld-linux-x86-64.so.2;
- LLD and any further native inputs actually selected by the configured build.

Keep 32-bit SDK assets sufficient for capture and the approved private native
probes. That does not expand the initial destination runtime target.

The stock Clang default link may discover host GCC/runtime paths. Do not
mistake that default for an isolated SDK. Record the actual inputs, use
explicit SDK/tool settings, and check linker-script resolution. In particular,
glibc library scripts can contain absolute paths whose resolution must remain
inside the intended link sysroot.
[Clang cross-compilation](https://clang.llvm.org/docs/CrossCompilation.html).

## 5. Private publication and normal build integration

### 5.1 Capture point and optimization placement

The selected default is minimally optimized native-profile LLVM capture,
followed by shared merging and common-dialect optimization/minimization.
Run the full applicable native LLVM optimization pipeline after destination
specialization.

Use an LLVM pass plugin at the earliest suitable pre-optimization extension
point exposed by the existing compiler. Private debug information and capture
sidecars may help associate types, declarations, layout, and lowered ABI
fragments across profiles. They are publisher inputs, not public payload.

An LLVM plugin observes LLVM IR; it does not run before preprocessing or
Clang semantic/ABI lowering. The merger must handle that reality with the
captured profiles and permitted evidence. Do not describe the plugin as a
universal pre-target frontend.
[LLVM pass plugins](https://llvm.org/docs/WritingAnLLVMNewPMPass.html#registering-passes-as-plugins).

Do not equate minimally optimized capture with blindly using -O0. Compiler-
introduced optnone/noinline attributes can suppress the later optimizer.
Choose capture settings that distinguish such artifacts from explicit user
attributes, and preserve explicit noinline, floating-point, aliasing, and
other semantic settings. Do not delete attributes indiscriminately.

Preserve original per-translation-unit settings. Optimized native builds remain
the references. If a setting materially changes frontend code generation,
record it as part of the capture contract rather than silently replacing it.

### 5.2 Native profile builds remain normal builds

Run an independent private build tree per captured native profile. Preserve
normal compiler object outputs, archive creation, and linking so that configure
checks, generated headers, build utilities, and ordinary project rules still
work.

Capture evidence alongside those normal outputs. The publisher's ordinary
native objects and executables are private build products, never the public
ordinary-code payload. A successful native build alone does not mean the
publication merger succeeded.

Record, at minimum:

- translation-unit identity, source/configuration digest, and profile;
- effective compiler version and meaningful flags;
- include/define configuration and generated-input provenance;
- capture module and private evidence identities;
- object/archive membership and order;
- executable/shared-library link commands and selected native dependencies;
- response-file expansion and other arguments needed to reproduce the build.

Preserve compiler flag ordering and archive/link semantics, including static
archive extraction, repeated libraries, groups, and whole-archive settings
when supported. Reject an unsupported build operation with its exact origin.
Do not silently pretend an arbitrary native object contains portable IR.

### 5.3 Make and CMake

Provide an external Make integration file and CMake
`nier_add_publication(...)` helper. Developers declare the source directory,
existing target/selected outputs, and existing configuration arguments.
Do not require application source edits, target-by-target rewrites, a renamed
Clang executable, or a separate developer-facing publication command.

The SDK internally coordinates two normal native project builds. Actual stock
Clang emits native objects so configure probes and generators execute normally.
A Clang observer and LLVM snapshot plugin collect provisional records; only a
successful native build/link with validated capture, dependency and output
identities makes those records eligible for publication. Internal native linker
recording preserves ordinary native behavior; archive creation still uses
stock `llvm-ar`. A readonly private object marker connects immutable capture
journals with actual native objects. An after-main Clang observer finalizes
the original native object hash after successful code generation. Private
native lanes use `-fno-temp-file` so that output is available at this point.
These checks establish build correspondence, not hostile-workspace security.

The SDK then invokes stock Clang on selected paired LLVM inputs to emit NieR
units, and invokes stock Clang again for the final NieR publication link.
Build-generated headers remain profile-specific: a pointer-width generator
must produce 8 for x86-64 and 4 for i686. No third source build or destination
execution of private generators is needed.

Preserve supported differing translation-unit inventories, native archive
extraction/link order, responses, per-TU flags, visibility, SONAMEs, version
scripts, and normal native dependency contracts. Matching-source-set-only
support does not complete this migration. Preserve target compilation
boundaries instead of introducing implicit LTO while matching across files.

Successful rebuilds replace the requested artifact atomically; failed builds
or merges preserve the previous valid output. Never overwrite project sources.
An output artifact is not a production installed release.

Measure fresh Make and CMake integration exercises against a 15-minute
developer-effort target, assuming an installed SDK and working native build.
Automated build duration is separate. Qualifying projects must honor documented
compiler/tool configuration; missing capture evidence or unsupported build
operations produce actionable diagnostics rather than guessed pairings.

## 6. Common MLIR and merger contract

### 6.1 Representation

Use one versioned experimental common MLIR dialect. MLIR is the implementation
infrastructure; its existing LLVM dialect alone does not solve native
parametric semantics or the publication privacy requirement.

The common representation needs source-language-independent forms for:

- fixed-width integer, float, double, and native pointer values;
- symbolic native-width integers and target-property expressions;
- size, alignment, field offset, and fixed-array extent relationships;
- global data, initialization, memory accesses, and address calculations;
- common functions, basic blocks, control flow, and calls;
- aggregates and their target-specialized layouts;
- native ABI signatures, by-value arguments/returns, indirect calls, and varargs;
- conditional target semantics where the original program genuinely differs;
- the semantic attributes necessary for correct LLVM lowering.

C0 implements only the smallest verified subset. Later C stages add the
remaining categories and their tests. A list of proposed operations is not
evidence that they are implemented.

Public identities should be opaque where names are unnecessary. Preserve
required exported/imported symbol names, runtime-required facts, strings, and
resources; removing a spelling must not remove program semantics.

### 6.2 Shared graphs, not disguised fat IR

The merger matches corresponding translation units, functions, globals, and
control-flow regions across the private profiles. It factors common structure
and replaces supported differences with explicit symbolic values, types,
layouts, or guarded regions.

For example, the pointer-size sequence 8/4 can become a native-pointer-size
expression, while a literal 8/8 remains fixed. Tests must include the literal
control; matching by a fixture's function name is not an implementation.

Target-dependent source behavior can require conditional common IR. A branch
on native properties is not inherently a fat artifact. Nevertheless, the
merger must not use an opaque original-module payload or wholesale
profile-selected function/module copies as its universal fallback. Sharing
must be real and inspectable, and original capture modules must be absent
from the publication.

Start with corresponding graphs, then add target-conditional sharing and
correspondence across differing source inventories. Tests must exercise real
preprocessing, configuration, and source-selection differences while retaining
common structure. A same-graph-only compiler does not complete the MVP.

Multiple profiles reveal examples, not a uniquely determined original source
expression. A proposed symbolic replacement must specialize correctly for the
validated profile domain. Do not extrapolate to uncaptured architectures,
ABI settings, or source configurations without evidence.

### 6.3 Native ABI and layout

Clang's LLVM output may already coerce aggregate arguments, split values,
introduce sret/byval attributes, fold offsets, and encode target-specific
alignment. Recover a consistent common contract using the private profile
evidence, then validate its lowering against each profile.

The installed output must use native calls, not a universal boxed ABI or
mandatory FFI adapter runtime. Float/double classification, aggregate returns,
callbacks, function pointers, and variadics need explicit tests.

Distinguish pointer width from other native properties. Equal-width targets
can differ in plain-char behavior, aggregate ABI, alignment, and floating-
point representation. The x86-64/i686 prototype does not establish ARM
correctness.

Do not erase aliasing, signed/unsigned operation behavior, initialization,
lifetime, or memory-access constraints needed for native semantics. Conversely,
private debug records are evidence for the publisher, not automatically
necessary public semantics.

The current implementation separates three concerns:

- `AggregateNormalize` is producer-only. Private debug type graphs propose
  logical signatures and storage anchors; instruction/use proofs must validate
  every admitted native entry, argument, result, and indirect-call shim before
  normalization. Debug data alone is not authority to change the program.
- The public `native_abi` type attribute records a logical native callable
  signature. Aggregate body arguments use owned-storage anchors, with a checked
  relationship to that signature. This is an intermediate compiler convention,
  not the installed program's calling convention or a language identifier.
- `AggregateABI` and `NativeABIBridge` are consumer-safe target-lowering code.
  They materialize native parameter/return forms inside the original functions
  and calls, without helper wrappers or a boxed runtime ABI. The producer
  independently reproves the generated forms before its final native inverse
  comparison. The device needs neither the debug evidence nor that producer.

Ordered-record ABI support and explicit packed/overlapping layout classification
have separate qualification tests. A passing classifier is not sufficient to
admit an unproved producer normalization shape.

### 6.4 Verification and failed merges

For each supported fixture and profile:

1. Verify capture completeness and the recorded input/configuration identity.
2. Verify the common dialect's structural and semantic invariants.
3. Specialize the common graph to that native profile.
4. Compare against the captured/reference LLVM contract, using justified
   normalization and targeted equivalence checks.
5. Run native behavioral/ABI checks where that stage supports execution.
6. Inspect the artifact for original profile modules and private evidence.

Structural equality after normalization is a useful sufficient check for
small cases; it is not a universal semantic equivalence procedure. Differences
must be explained, tested, and bounded rather than waived.

An unsupported instruction, inconsistent profile mapping, ambiguous ABI
reconstruction, or unrepresentable build difference fails publication with a
precise diagnostic. It does not automatically become a native payload, and
it remains unfinished scope if required by the current milestone.

## 7. Experimental artifact and CLI

### 7.1 Package format

Use shared common MLIR fragments in a standard tar archive with
a JSON manifest. Use libarchive and LLVM JSON support rather than inventing
archive or JSON parsers.

The envelope is explicitly pre-alpha, with only a current-contract discriminator
and no historical readers. Pin the implementation's LLVM/MLIR dependency, but
do not make Clang identity or capture provenance an artifact admission rule.
Document the NieR encoding and canonical JSON rules for independent producers.

A representative logical layout is:

~~~text
manifest.json
modules/<opaque-id>.nierbc
resources/<declared-relative-path>
native/<declared-payload-id>       # full-product extension where applicable
~~~

The manifest describes artifact identity, format/dialect version, module
digests, qualified target constraints, artifact kind, entry points, compile/link semantics, and
the declared native SDK/dependency contract. Preserve archive membership and
link ordering separately from portable code modules.

The current kinds are `object`, `executable`, `shared`, and `static`. Module
records carry only bytecode paths and digests. A mandatory `compilation_units`
table lists each qualified target's ordered native units: each unit names its
ordered fragment indices and optimization setting. Every fragment occurs once
per target. The usual one-fragment-per-TU case uses singleton units; genuine
source repartitioning can group shared fragments differently. Reassemble each
original unit before invoking stock optimization; do not silently enable LTO.

Static artifacts contain every ordered archive member, not only members used
by one application. Each compilation unit carries its public archive basename;
physical unit order distinguishes duplicate names. Executable/shared artifacts record
explicit native dependency names or exact SONAME imports. The selected native
build's actual `DT_NEEDED` list is retained without repeating as-needed
selection at the destination, including constructor-only dependencies.
Qualified link metadata includes SONAME, dynamic exports, GNU/both hash styles,
and a bounded, digest-checked C symbol version script with comments removed.
The native SDK explicitly uses LLD's `--undefined-version` to retain ordinary
GNU-linker behavior for version maps naming optional absent definitions.

Serialize deterministically where practical: stable entry ordering, canonical
field ordering defined by this schema, normalized archive metadata, bounded
sizes, and content digests. Define test vectors rather than relying on the
word JSON to imply canonical hashing.

Validate names, lengths, counts, paths, duplicate entries, symlinks, archive
expansion limits, and digests. Package validation is input correctness and
robustness; it is not secure-store admission or OS execution enforcement.

### 7.2 Publication minimization

Never include application source, preprocessed source, ASTs, private debug
information, original native-profile LLVM modules, private build scripts,
credentials, or unnecessary private paths and identifiers.

The private build recipe is not copied wholesale into the manifest. Export
only the validated compile/link/dependency information the destination needs.
Treat nested archives, resources, diagnostic metadata, and future native
payloads as part of the same export review.

Use common-dialect optimizations and an explicit metadata allowlist. Retaining
only minimized common IR does not by itself establish T9/A2; its semantic
structure may still aid reverse engineering. A2 remains a later acceptance
gate, and source/private-input exclusion applies from the first artifact.

Do not rely on a secret format, encryption with a bundled decoder, a hidden
compiler, or deletion after installation to establish RE parity.

### 7.3 Independent program interfaces

The first example consists of `main.c`, `hello.h`, and `hello.c`:
`main` calls the function declared in the header and defined in `hello.c`.

~~~sh
clang --config=/publisher-sdk/nier.cfg -O2 main.c hello.c -o hello.nier
nierc inspect hello.nier
nierc hello.nier --sdk /device-sdk -o hello
./hello
~~~

Normal separate compilation is also required: stock Clang `-c` emits
relocatable NieR objects, and its ordinary link invocation combines them into
one final archive. `-shared` selects shared-library publication. Each selected
executable or shared library has one standalone artifact containing all its
ordinary code and required public metadata; external native dependencies remain
explicit rather than implicitly bundled.

`nierc lower INPUT.nier --target i686 --output-dir DIR` provides diagnostic
LLVM output. It is not an installed i686 execution claim. `nierc` has no
publication/source-language mode. There is no `aot publish` compatibility path.

Successful writers use validated atomic replacement. The SDK coordinator stages
its final Clang output before replacing a valid existing artifact; nierc also
preserves prior output on failure. Direct stock-Clang commands retain the
upstream driver's failed-output cleanup, which can remove the requested output
after a failed job and cannot be overridden by the frontend plugin. Do not use
an input/source path as an output path. The artifact is independent of publisher
intermediates.
No installer, updater, store, launcher, or installed-release registry is part
of this migration; T8 remains later production work.

## 8. Destination compilation, linking, and execution

### 8.1 Input and transformation boundary

The destination selects a supported target from the artifact constraints and a
matching SDK. Reject unsupported versions, missing required modules,
incompatible native inputs, and unknown required semantics.

Specialize symbolic values, types, layout, conditional regions, and native
ABI contracts, then emit a valid target-specific LLVM module for each
translation unit. Set the correct data layout, triple, CPU/features, and
preserved semantic/optimization settings.

This is the final custom program representation. Thereafter use unmodified
LLVM optimization and code generation, followed by unmodified LLD. Diagnostics,
input staging, and normal tool invocation remain orchestration, not permission
to add another custom program transformation.

For a static-library output, stock `llvm-ar` replaces the final ELF link:
append each independently generated member in physical order, then generate
the archive index. Duplicate member names must not trigger replacement or
deduplication. Later native consumers perform normal lazy archive extraction.

### 8.2 Optimization and linking

Run native LLVM optimization after specialization. Preserve per-TU settings
and normal release behavior. Do not enable implicit LTO; keep ordinary
translation-unit and archive boundaries unless a later explicitly supported
project configuration requests LTO.

Reproduce native link semantics using generated objects, selected archive
members, startup objects, compiler support inputs, and managed native
libraries. Use ordinary LLD options and scripts. Let LLD produce the final
ELF, including normal symbol stripping; no post-link ELF rewriting follows.

Do not manufacture favorable results with different floating-point rules,
disabled language semantics, a changed dependency version, or workload-specific
flags absent from the reference.

### 8.3 Stock loader and supplied glibc

The selected runtime is platform-supplied glibc with its matching unmodified
standard loader. glibc and musl describe host environments as well as possible
application ABIs: supplying glibc on a qualifying musl host does not require
using the host's libc.

For the C MVP, use ordinary SDK/linker configuration:

- a target sysroot and explicit link inputs;
- the supplied loader selected through the normal interpreter option;
- ordinary RUNPATH configuration for the supplied runtime libraries;
- -z nodefaultlib where appropriate to the selected test configuration.

LLD already supplies these options. The runtime path belongs to the configured
SDK; this plan does not impose a new universal fixed-prefix policy or introduce
a custom interpreter, wrapper, patched loader, or post-link binding stage.
[LLD ELF options](https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-18.1.3/lld/docs/ld.lld.1).

Verify the actual dependency closure. The initial printf program may only
require libc; if libm loading is part of a test, ensure the configured program
really retains/uses that dependency. Do not count a dropped unused -lm as
libm coverage.

An explicit interpreter selects the loader, not all libraries. RUNPATH is
not a general transitive-resolution policy. -z nodefaultlib skips default
library directories but does not universally exclude nondefault cache entries.
Normal environment overrides and later dynamic loading also remain relevant.
[Linux loader search](https://man7.org/linux/man-pages/man8/ld.so.8.html).

Accordingly, the first acceptance claim is narrow: direct execution on the
qualified test environment loads the recorded supplied loader and library
files. Record those identities using ELF inspection and loader/file-open
diagnostics. Check missing/mismatched SDK inputs and deliberately broken
runtime setups, and report any host fallback.

Do not call this blanket host isolation or completion of T4. General managed
dependency resolution and updates remain explicit later work in section 13.
Normal standalone loading facilities are not disabled by security policy.

### 8.4 Execution checks

Run the final ELF directly. The artifact and compiler may be absent while it
runs. Preserve arguments, environment, exit status, standard I/O, and the tested
native library behavior. No publication compiler or runtime process remains
resident.

Hash/version checks before compilation validate inputs. They do not protect
files against later modification, authenticate executable pages, or enforce
application permissions. Those are SEN obligations, not implicit
properties of the C SDK.

## 9. First implementation checkpoints

### C0 — Real end-to-end Hello World

Implement and verify:

- x86-64/i686 private capture using the same shared capture boundary;
- a tiny common graph supporting the fixture's scalar/direct-call operations;
- a versioned experimental artifact containing common IR only;
- destination specialization and target-specific LLVM IR output;
- stock LLVM and LLD producing an x86-64 ELF;
- direct execution with the configured supplied native runtime.

Use a real dynamically linked printf fixture with native pointer arguments,
not a standalone system-call-only program that avoids the native ABI. Record
actual imports and output behavior rather than assuming an optimizer retained
a particular call.

The destination workspace must not consume the original LLVM capture or
native object as a shortcut. Inspect the public archive and its input accesses.

### C1 — Native-width and common-graph proof

Add the sizeof-versus-literal control, pointer storage alignment, native-width
integer values/types, memory access, and basic control flow.

Specialize the same common artifact for both captured profiles and compare
its native contract against both references. Private i686 native probes are
permitted. Product execution coverage remains x86-64 until explicitly expanded.

Demonstrate genuine common structure; two renamed original modules in an
archive fail this checkpoint.

### C2 — Broad core C and native ABI

Add fixed arrays, struct/union layout, aggregates by value, float/double,
indirect calls, callbacks, variadics including native va_list forwarding, and
setjmp/longjmp. Include calls crossing translation
units and boundaries with stock-native reference components.

ABI tests must expose wrong registers, hidden result pointers, alignment,
promotion rules, and return classifications. A program that merely prints the
right pointer size does not establish any of these.

### C3 — Target-conditional sharing and build integration

Implement differing target-conditioned regions in a shared graph. Add
multi-translation-unit projects, differing source sets, static/shared outputs,
native object/archive/link preservation, and
the selected Make/CMake integration. Include generated/configuration inputs
without source edits.

Source-list-only publication and matching-CFG-only merging remain incomplete
at this checkpoint.

### C4 — Configured upstream corpus and functional C MVP

Publish and run the locked cJSON 1.7.19 and zlib 1.3.2 builds and configured
tests, alongside all C0-C3 fixtures. Record source digests, options, test
commands, selected native inputs, and failures.

C4 completes the **functional C MVP** only when its declared scope actually
passes. Full performance/RE acceptance, the deferred C facilities, other
languages/targets, and production platform/lifecycle behavior remain open.

These checkpoints are sequential integration goals, not a demand to finish
all C support before attempting Hello World.

## 10. Diagnostics, robustness, and development discipline

Every unsupported feature must identify the translation unit, profile, and
operation/configuration responsible, with a useful private-source location
when available. Public diagnostics must not leak private capture evidence.

Separate capture failure, failed correspondence, unsupported common operation,
invalid artifact, failed specialization, native-tool failure, and runtime
mismatch. A native-tool error must not be reported as a successful publication.

Maintain small regression fixtures for each supported operation and each
rejected category. Test malformed archives/IR, size limits, unsupported
versions, missing modules, wrong profiles, incompatible SDKs, and truncated
inputs. Prefer existing library facilities for bounded decoding and parsing.

Build and test the compiler with ordinary sanitizers and fuzzing where
practical. Those checks protect tool correctness; they do not enforce the
application's runtime security policy.

Keep native reference and publication work directories separate and preserve
command logs and manifests needed to reproduce a result. Never overwrite
application sources or silently patch corpus code to pass a stage.

## 11. Functional test registry

| ID | Required functional evidence |
|---|---|
| C01 | Hello World: real shared capture/merge/artifact/device path, direct x86-64 ELF execution, printf/native-pointer boundary |
| C02 | sizeof/literal distinction, native-width values/types, pointer alignment, fixed-width controls, both private profiles |
| C03 | Integer/control-flow semantics, loads/stores, globals, initialization, pointer arithmetic, fixed arrays |
| C04 | Struct/union size/alignment/offset, nested aggregates, by-value arguments/returns, cross-TU native ABI |
| C05 | float/double arithmetic and arguments/returns, mixed aggregates, applicable native FP settings |
| C06 | Function pointers, indirect calls, native callbacks, variadic calls/functions and promotions |
| C07 | Target-conditioned regions with actual common-IR sharing; no original-module fallback |
| C08 | Make/CMake, multiple TUs, generated/configured inputs, object/archive/link behavior |
| C09 | Unmodified-source cJSON 1.7.19 configured build/tests |
| C10 | Unmodified-source zlib 1.3.2 configured build/tests |
| C11 | Public artifact/version/digest checks, malformed input rejection, no private source/debug/capture export |
| C12 | Actual supplied loader/library identities, direct execution, missing SDK/runtime diagnostics, no blanket fallback claim |

The test registry records exact commands, expected outcomes, native references,
and current coverage. Compare defined C behavior and implementation-defined
behavior against the corresponding native target; do not use undefined behavior
to demand identical results.

Some stages run only part of this registry. Record that subset explicitly.
Tests on private i686 reference outputs and common-IR specialization are
different from support for compiling/running an installed i686 application.

## 12. Completing languages, C coverage, and target support

### 12.1 Remaining C facilities

After C4, add and verify the deferred concurrency, atomics, TLS, dynamic
loading/plugins, advanced floating-point types/environment, variable-sized
objects, nonlocal control flow, and supported native extensions.

Preserve standalone freedom: dlopen, native helpers, ordinary debugging, and
application-owned native code generation remain available under host rules.
A missing compiler feature is unfinished engineering, not a new standalone
security prohibition.

Provide declared architecture-specific native library/executable payloads for
actual native needs. Preserve target/ABI/CPU/kernel and dependency constraints.
Standalone publication does not require store approval for those payloads.

### 12.2 Rust

Use unmodified existing Rust/Cargo components to produce native-profile LLVM
inputs for the shared merger. Run proc macros, build scripts, target cfg,
generated bindings, and source selection privately.

Prove handling of usize/isize, const evaluation, repr(C), arrays, generics,
drop/panic/unwind behavior, allocation, atomics, threads, and native FFI. Supply
matching runtime/standard-library assets.

No rustc or Rust-specific importer may be required on the device. Default
Rust ABI instability is not a promise of independently replaceable Rust
libraries; supported native interfaces need their own compatibility contracts.

Whether available no-fork components preserve sufficient information for this
shared pipeline is a gate. A language-specific fallback on the destination
would change the selected architecture, not finish this work.

### 12.3 Go

Evaluate an existing permitted LLVM-capable Go production route against the
normal Go compiler/runtime reference. The standard Go compiler must not be
assumed to emit LLVM IR. Do not select a replacement runtime merely because it
has an LLVM backend.

The route must preserve the required Go behavior and quality: collector,
goroutine scheduler, stack growth, preemption, pointer liveness, reflection,
panic/recover, generics, build constraints, normal standard-library behavior,
and applicable native build modes.

Build tags, GOARCH-sensitive source selection, generated code, unsafe
size/alignment/offset, cgo bridges, callbacks, and C-created threads require
explicit capture/runtime tests. The cgo C side uses the same C capture lane.

Reuse existing permitted components; do not write a new Go compiler or add a
Go backend/importer to the device. If no candidate meets the architecture and
01, report that unresolved feasibility failure. Go remains mandatory for the
first complete toolchain, but it is not a prerequisite for C0-C4.

### 12.4 All required targets and host families

Complete x86-64, ARM64, x86-32, and ARMv7. Pin exact triples, native ABI,
CPU/features, page-size assumptions, compiler/runtime versions, and kernel
floors in the qualified SDK contracts. These details are validated profiles,
not permission to omit a required CPU.

Distinguish source/build configuration from backend CPU tuning. Tuning cannot
select an uncaptured source branch or change a native ABI. Record the
published configuration and the actual generated CPU requirements.

Use the same publication digest on the qualified targets and both glibc/musl
host families. Test equal-pointer-width differences as well as 32/64-bit
differences. Real hardware is required for performance and destination
compilation resource evidence; emulation supplements functional testing.

The destination compiler must fit real 32-bit address-space/resource limits.
Substantial one-time compilation is permitted, but remote compilation or
shipping publisher-built ordinary executables is not a substitute.

Kotlin/Native and additional languages are later candidates using the same
common boundary. Reusing LLVM does not make every LLVM-based language
automatically supported.

## 13. Full standalone native platform and lifecycle

### 13.1 Platform-managed dependencies

Build and maintain native SDK/library packages for the qualified profiles.
Record provenance, exported interfaces, native requirements, dependency closure,
runtime modules/data, exact files, and content digests.

The platform supplies the application's native dependency environment; the
host distribution's libraries are not its contract. This includes transitive
libraries, locale/NSS and other runtime modules, desktop/client libraries,
helpers, and applicable userspace driver components.

Use unmodified standard native loaders and ordinary toolchain/package
configuration. The old custom loader/launcher/generation-bootstrap proposal
is not retained. No post-link ELF rewriting is part of the selected pipeline.

A production design must demonstrate automatic managed resolution through
initial and later loads without accidental host repair, while preserving normal
standalone dynamic-loading behavior. The C MVP's RUNPATH configuration does not
solve this general problem. If stock facilities cannot meet the required
contract, expose the concrete failure for a design decision rather than quietly
reintroducing a custom loader or weakening T4.

Host integration uses normal native interfaces and protocols. Desktop testing
must include display, input, clipboard, and relevant native callbacks. Native
GPU/device interfaces and loadable drivers require compatible platform inputs;
do not treat arbitrary host driver loading as proven compatibility.

### 13.2 Compatible dependency updates

A compatible update needs more than an unchanged SONAME. Check exported
symbols/versions, layout and calling conventions, supported behavior,
dependency closure, and integration tests. Keep incompatible interface epochs
available where required.

Preserve the matching libc/loader cohort and consistent transitive bindings.
Test updates racing with process startup, helper execution, and late dlopen.
Do not independently switch a loader and its libraries and assume the resulting
race is harmless.

Already-running processes and subsequent dynamic loads must retain a compatible
environment. Decide and verify the supported update/retention protocol using
the selected stock facilities before claiming this gate. A restart may be
needed; application modification or recompilation is not the repair.

Statically linked fixes and incompatible application changes require a new
application release. With security enabled later, interface compatibility also
needs verified capability-contract preservation and approval.

### 13.3 Production installation and release state

After the manual compiler flow is established, implement a transactional
installer:

~~~text
received -> validated -> dependencies staged -> compiling
         -> native outputs checked -> committed -> active
~~~

Stage inputs before compilation, run resource-limited workers, record outputs,
and commit atomically. A failed first installation must leave the prior active
release intact. Offline installation works with pre-staged compiler and
dependency bundles; no store is required.

Bind each committed installation to application/release identity, publication
digest, compiler/SDK versions, profile/configuration/features, static inputs,
selected dynamic contracts, and native output digests.

An installed application release is never recompiled for tuning, compiler
upgrades, dependency updates, or maintenance. Failed uncommitted first jobs may
retry. Repair of committed output restores the exact retained native output,
or requires a new application release; it must not erase lifecycle records to
evade T8.

Define durable records, backup/retention, reinstall/uninstall behavior, crash
recovery, and rollback consistent with that rule. Rollback reactivates retained
compatible output rather than rebuilding it. Do not add this production
registry to the initial manual C compiler as a prerequisite.

CLI entries, service units, and desktop entries point to completed native
applications. Compilation never moves into first launch or continues as a
resident execution service.

### 13.4 Complete standalone integration tests

Add:

- concurrent/threaded C applications, Go services, Rust CLI/desktop applications;
- files, sockets, timers, signals, subprocesses, pipes, argv/env, and shutdown;
- FFI in both directions, cgo, callbacks, allocation ownership, and unwind/TLS;
- managed and explicit standalone external dynamic loading/native helpers;
- host libraries absent, old, newer, or incompatible;
- compatible/incompatible updates and missing runtime modules;
- installation interruption, duplicate requests, repair, rollback, and recovery;
- complete operation with security/store components absent.

These tests, the full target matrix, and section 14 form the standalone release
gate. A representative CLI alone is not a complete native application platform.

## 14. Native quality and privacy acceptance

### 14.1 Functional-first does not weaken acceptance

C0-C4 establish functionality before full A1/A2 qualification. Record useful
performance/resource/exposure observations early, but do not label the C MVP
native-equivalent or RE-compliant without the required evidence.

The complete standalone release requires both functional and A1/A2 acceptance.
A later gate failing is an architecture/product problem to report, not
permission to redefine the reference or relax 01.

### 14.2 A1 performance protocol

Use the same original application, native compiler/runtime versions, managed
dependency versions, target configuration, and documented native release
settings for the direct-native and publication builds.

Measure execution speed/throughput, applicable latency distributions, startup,
and memory. Include compute, allocation, concurrency, I/O, CLI/service startup,
and desktop readiness workloads across C, Go, and Rust.

Predeclare the corpus, metric applicability, workload inputs, measurement
procedure, uncertainty treatment, and per-test acceptance checks before using
results to claim a pass. Use controlled real hardware, randomized paired runs,
sufficient independent repetitions, and appropriate tail-latency sampling.

Every agreed metric/test must meet the 5% regression limit. Use confidence
bounds or an equally justified predeclared uncertainty procedure; an
inconclusive result is not a pass. Do not average a failure away.

Keep link/runtime dependencies equivalent. Count publication-specific work
where it actually exists rather than adding that work to the reference.
No extra custom loader or post-link stage is assumed in either build.

Report installation wall time, peak memory, disk use, artifact/SDK size, and
download volume separately. Measure later security overhead separately from
the accepted standalone toolchain.

### 14.3 A2 information exposure and comparative RE

Prepare a versioned assessment protocol and representative authorized corpus
with independent review. Open-source functional corpus results alone do not
establish private-application RE parity.

Inventory the entire obtainable package: common IR, graph structure, types,
layout expressions, conditional paths, names, imports, strings, resources,
native payloads, manifests, and auxiliary information.

Compare against the corresponding fully stripped native releases of the same
application/language/dependencies. Assess the additional information available
from combining target paths in one artifact. Give analysts the public format
and compiler/decoder tools; secrecy is not a defense.

Use documented comparative tasks such as recovering an algorithm/data
structure, reconstructing call relationships, locating a seeded defect, and
modifying behavior. Record task correctness, effort/time, assistance,
methodology, uncertainty, limitations, and analyst expertise.

Information exposure matters even if a limited timed study did not exploit
it. Require no identified exposure or comparative advantage that defeats T9
within the agreed assessment. Inconclusive evidence is not a compliance claim.

This is the agreed evidence standard, not a universal mathematical proof.
Repeat it when representation, optimization facts, metadata, packaging, or
later security attachments materially change.

## 15. Security roadmap after standalone acceptance

This section proposes the later SENieR (SEN) implementation for S1-S5, not part of the
C MVP or a hidden restriction on standalone applications. Detailed mechanisms
must be validated and reviewed when this phase begins. They do not authorize
compiler forks, a custom application loader, or a language-specific device path.

### 15.1 Trust, admission, and native review

Add separate services for submission, validation, manual native review,
approval/revocation publication, protected device installation, and OS
enforcement. Keep upload/validation workers separate from signing authority.

The store is trusted to attest developer identity and authorize releases.
Do not revive a guarantee against a malicious store impersonating a developer.
Protect keys, authorization, rotation/recovery, and audit records using
established components and a documented trust boundary.

Represent release admission durably:

~~~text
submitted -> validating -> native review when required -> approved/rejected
approved -> revoked
~~~

A secure release declares its complete executable/dependency graph and
capability requirements, including runtimes, helpers, declared dynamic loads,
and native exceptions. Store acceptance must validate completeness and all
required checks/tests before approval.

Develop conservative capability summaries for portable/native interfaces,
indirect calls, helpers, and target-conditioned code. Opaque calls and native
payloads need adequate summaries or complete conservative declarations that
can be validated. Tests/manual review alone are not a proof of arbitrary
native-code completeness.

Installation must not discover missing capabilities and grant them. A
capability expansion requires a new validated application release.
Interpreters, scripts, and plugin engines need a complete executable-code
contract; signed CPU mappings alone do not establish closure.

For every submitted native library or executable, require technical
justification, exact identities/contracts, manual approval by store developers,
and store signing. Approval does not exempt native code from integrity or
permissions. Native payloads remain a tightly justified secure exception,
not the normal publication path.

### 15.2 Authenticate native output without rewriting it

The protected installation service authorizes only approved publication,
compiler, SDK, and dependency inputs. The ordinary toolchain still produces
the final ELF through stock LLVM and LLD.

After validation, a protected signer can issue separate attestation/signature
records binding final native-file digests, permitted executable ranges,
publication approval, and installation provenance. Do not modify the ELF
after LLD to add security metadata.

Workers do not hold signing keys and cannot swap validated files between
checking, sealing, and attestation. Use immutable/verified storage or an
equivalent protection model. A signature on a file is not authorization for
every application to execute it.

Security enrollment of an existing standalone output needs evidence for that
exact output. Do not recompile an installed release or blindly sign arbitrary
files to manufacture provenance.

### 15.3 Executable integrity and application permissions

Qualify OS/kernel enforcement that binds directly executed managed applications
to their domains and approved code, independently of loader behavior. A custom
launcher must not be required to enter the security domain.

Potential building blocks include verified file storage and Linux security
hooks; they are not complete implementations merely by being enabled.
[fs-verity](https://docs.kernel.org/filesystems/fsverity.html),
[IPE](https://docs.kernel.org/admin-guide/LSM/ipe.html).

Cover executable files, ELF interpreters, libraries, helpers, mapping ranges,
permission transitions, writable aliases, private/COW modifications, and
process-memory mutation paths. Prevent anonymous executable code, W+X,
application-controlled writable-to-executable conversion, and undeclared
executable admission. Removing X and later restoring it must not erase history.

Check fork/clone/exec, FD passing, ptrace/proc-memory access, cross-process
writes, asynchronous interfaces, device mappings, and compatibility syscalls.
Loader preload/search behavior must not bypass the secure closure.

Inventory kernel-provided executable mappings, including vDSO, architecture
helpers, and signal trampolines. S4 requires signed-file backing: silently
exempting those mappings is not acceptance. Prove a compliant implementation
on all required architectures or report the security gate failure.

Resource permissions cover resolved files, network operations, devices/ioctls,
IPC, process interaction, inherited/transferred descriptors, and other
applications' data. Use object-aware enforcement, not unsafe userspace
pathname-pointer inspection. Existing syscall filtering can supplement it,
not replace the resource model.

Return ordinary handleable errors for denied resources. Such a denial is not
by itself proof of compromise and must not trigger termination. Unauthorized
executable admission is a distinct integrity violation.

Native code signing does not make C memory-safe or eliminate code-reuse and
logic attacks. These limits must remain explicit.

### 15.4 Offline approvals and revocation

Keep authenticated approval and monotonically updated revocation state
locally. Secure launch/run works offline; no online launch token is required.

When the device learns of a revocation, persist denial, block new launches
and code admission, find all affected running application trees, and stop
them. Known revocation is not deferred to preserve availability or unsaved
work. Close races with process creation, dependency loading, daemon failure,
and reboot; replayed older state must not undo the denial.

Measure and enforce the kernel's termination boundary, including tasks blocked
inside the kernel. Do not promise physically instantaneous termination, and do
not let a known-denied task resume application execution while claiming it
has been stopped.

A compatible approved dependency replacement must preserve the application's
interface and capability contract without recompilation. Otherwise a new
application release is required. Offline devices cannot learn unreceived
revocations; that limitation does not excuse ignoring a received one.

### 15.5 Guest and supplied-OS deployment

Use the same SEN security model and product in two qualified deployment scopes:

- Guest integration governs managed applications on an existing OS, without
  claiming control of the whole host.
- The supplied Linux OS applies the same platform system-wide, including
  services, helpers, system packages, updates, and recovery.

Host/kernel/storage prerequisites must be explicit. A host that can run the
standalone toolchain but cannot enforce S4 is not a secure guest deployment.
The trusted OS/administrator and store remain within the declared trust model.

The supplied OS additionally requires reproducible images, authenticated
boot/kernel/policy state, protected trust roots, signed system code, coherent
updates, and recovery that cannot restore known revoked approvals.

### 15.6 Security acceptance registry

| ID | Required evidence |
|---|---|
| Q01 | Complete secure release/capability contract; missing declarations rejected before approval |
| Q02 | Justification, per-submission manual review, and signing for native libraries/executables |
| Q03 | Final native-output attestation/provenance and tamper/substitution rejection |
| Q04 | Executable mapping/mutation coverage, including kernel helpers and all required CPU ABIs |
| Q05 | Resource denial returns handleable errors; code admission violations remain distinct |
| Q06 | Direct exec, helpers, inherited domains, loaders, and injected code cannot escape policy |
| Q07 | Compatible approved dependency updates preserve interface/capability contracts |
| Q08 | Offline launch, persistent revocation, affected-process termination, and race/reboot tests |
| Q09 | Qualified guest integration and supplied-OS/system-wide coverage |
| Q10 | Full standalone flow remains usable with all security/store components absent |

Independent security review and real native applications are required; synthetic
attack fixtures alone do not establish the product.

## 16. Delivery order and exit gates

| Stage | Deliverable and exit condition |
|---|---|
| Bootstrap | Verified pinned prebuilt SDK and build/test environment; no system package mutation |
| C0 | Real two-profile capture/common artifact path; x86-64 Hello World through stock LLVM/LLD and direct ELF execution |
| C1 | Native-width/literal distinction and shared-graph specialization checks across the captured profiles |
| C2 | Broad core C/layout/native ABI/indirect call/variadic fixtures pass |
| C3 | Target-conditional IR sharing plus Make/CMake and object/archive/link integration pass |
| C4 | Configured unchanged-source cJSON/zlib tests and the complete declared functional C MVP registry pass |
| P1 | Deferred C facilities, Rust/Go producer/runtime feasibility, and the full required native target matrix demonstrated |
| P2 | Managed dependency resolution, native application integration, production installation/lifecycle/update/recovery pass |
| P3 | Full A1/A2 evidence and complete standalone T1-T9/A1-A3 acceptance on the qualified matrix |
| S0 | Security closure, provenance, executable-mapping, permissions, and revocation feasibility gates pass |
| S1-stage | Secure guest product accepted with independent review |
| S2-stage | Supplied OS/system-wide product accepted, including update/recovery |
| Extensions | Kotlin/Native and further languages/targets evaluated against the same contracts |

Do not implement the production SEN security/store stack before P3 acceptance.
Security planning and identifying eventual integration boundaries are not
permission to make C work depend on it.

Stages can reveal failures requiring revised engineering. Report the failing
fixture, affected requirement, measured evidence, and permitted alternatives.
Do not silently substitute source distribution, target-fixed ordinary builds,
compiler forks, device language backends, a runtime engine, weaker privacy,
looser performance, or mandatory security.

Estimate broader schedule/staffing after actual compiler and platform evidence.
Maintain ownership of the shared compiler/merger, native SDK/packages, build
integration, hardware CI, performance and RE evaluation, and later security/OS
operations. This is sustained compiler/platform work, not a trivial bitcode
container project.

## 17. Implementation-status record

The NieR migration replaces the old combined program and publication recipes;
old prototype artifact hashes and measurements are not evidence for the new
implementation. The implementation and tests, not this roadmap, determine
which gates have passed.

Verified checkpoints include separate core/producer targets, public NieR APIs,
bounded artifact handling, direct stock-Clang publication, and multi-file
Hello World. Positive tests now cover matching CFG/SSA, scalar floating point,
native-width/fixed-width controls, record and array storage, mutable globals,
function pointers and native callbacks. Native `setjmp`/`longjmp` tests cover
cross-frame calls, nested live environments, zero-to-one return conversion,
and preserved volatile locals on both private widths at O0/O2.
Promoted scalar/pointer `va_arg`, `va_copy`, register/overflow cases,
and native `va_list` forwarding also pass both private widths at O0/O2.
These tests do not establish the entire C04-C07 ABI/graph matrix. Conditional
switch fixtures now have actual target-only arms inside one shared function,
retain side effects and native case/block order, and execute correctly on
both widths at O0/O2. This qualified producer rule requires closed,
straight-line arms that rejoin shared code; general divergent CFGs are not
silently admitted. Overlapping union storage and shared loop-metadata
identities also have positive reconstruction tests. Nested packed/bitfield
storage fixtures cover nonzero static initialization, copying, signed field
extraction, unaligned accesses, and pointer-based mutation on both private
widths at O0/O2; they do not claim packed/bitfield by-value ABI coverage.

Make and CMake fixtures exercise native configure probes/generators, differing
source files selected into corresponding object roles, genuinely unequal
three-versus-two translation-unit inventories, ordinary/thin/group/
whole-archive extraction, shared-library outputs and native callers, and
static outputs with duplicate member names. Static tests check observable
member order and lazy extraction with both stock-native and NieR-produced
callers. Shared-link tests check SONAME, versioned exports, dynamic visibility,
constructor-only dependencies, and failed-link output preservation. Per-target
archive extraction order and independent per-TU optimization settings are
preserved through explicit compilation-unit plans. Unequal source counts
currently require provable self-contained fragments; general cross-fragment
private identities remain an explicit rejection, not a native-code fallback.
The SDK configures stock Clang's `-ffile-prefix-map` for each private lane,
giving transient source/build roots stable `/nier/source` and `/nier/build`
identities in `__FILE__`. It does not rewrite application strings after capture
or erase genuine source differences.

The unchanged pinned cJSON static and shared configurations each pass all 19
native-reference tests on both private profiles. Pinned zlib's original
`make test` and `make test64` likewise pass both native profiles. The cJSON
static/shared libraries and demonstration program additionally publish,
compile, and run through NieR with reference-identical stdout. A stock-native
caller also loads the NieR-produced DSO and matches the native reference.
The complete zlib gate now also passes through NieR: its static archive,
versioned shared library, and all six test programs are independently
published and compiled. The original `make test` and `make test64` recipes run
successfully in a source-free destination with build tools disabled. Loader
diagnostics confirm the shared tests use the NieR-generated `libz.so.1`.
The complete cJSON gate also passes: each static/shared configuration produces
21 NieR artifacts and its source-free destination passes all 19 original
CTests. Unity retains its normal `setjmp`/`longjmp` assertion control. Native
and NieR callers both load the destination `libcjson.so.1`; demonstration
stdout matches the native reference byte-for-byte.
The reproducible gate is `corpus/qualify.sh`; its original
test staging has been checked separately with native reference binaries.

The public aggregate pipeline passes at O0/O2: stock Clang emits a multi-TU
artifact, `nierc` builds the native executable and a separate native DSO, and
a stock-native caller/bridge verifies ordinary native calls and callbacks.
Qualified records include integer pairs, mixed integer/double values, and
larger native-width records, with register pressure and hidden-result storage.
The same shared NieR units also pass private core-only native execution on
both widths. Classifier-only and normalization-only tests remain separately
identified; neither is substituted for this artifact-pipeline evidence.

Union/bitfield/packed **by-value ABI** and aggregate `va_arg` extraction are
still unsupported, even though corresponding storage or classifier tests may
pass. General divergent CFGs and cross-fragment private identities remain
outside the bounded rules above. These are declared compiler limitations, not
security prohibitions or permission to publish native fallback bodies. Other
ordinary C facilities listed in section 12, languages, targets, production
dependencies/lifecycle, and A1/A2 remain future work. The 15-minute human
onboarding target has not been established by a user study; automated fixture
timings are not a substitute. Unknown required semantics fail publication.

Reproduction from the repository root:

~~~sh
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
cmake -S . -B build/prealpha -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/prealpha --parallel 2
ctest --test-dir build/prealpha --output-on-failure
mkdir -p artifacts
clang --config="$PWD/build/prealpha/nier.cfg" -O2 examples/hello/hello/main.c examples/hello/hello/hello.c -o artifacts/hello.nier
build/prealpha/nierc artifacts/hello.nier -o artifacts/hello
env -u LD_LIBRARY_PATH artifacts/hello
~~~

The command sequence is the new interface and must be exercised after each
integration change. A consumer-only build uses `NIER_BUILD_PUBLISHER=OFF`;
its independent-producer test must work without linking the merger or Clang.
The consolidated checkpoint passes all 39 publisher/core integration tests
and all eight consumer-only tests. The relocated compiler-only distribution
test also verifies source/frontend-free compilation and direct native execution.
The complete corpus is a separate, longer qualification command; its private
reports record the actual tool/configuration/recipe hashes and all 50 artifact
hashes across the two projects, not just the dirty worktree's Git revision.
After the final aggregate-call fixes, the current publisher also revalidated
all 50 retained native selections and reproduced all 170 unit-artifact
occurrences and all 50 final publications byte-for-byte. This test rechecks
immutable capture/dependency/native witnesses and both strict native inverses;
it does not rerun configure, native builds, or upstream tests, and does not
modify the original evidence. It links the final publisher checkpoint to the
complete corpus results above without presenting replay as a fresh corpus run.
Do not report broad-C completion from passing a subset of the C01-C12 registry.
Performance/RE, other languages/targets, production lifecycle, and security
remain separate future gates.

## 18. Requirement traceability

All 18 requirements/acceptance groups in 01 remain in scope.

| Requirement | Implementation plan | Required evidence/exit |
|---|---|---|
| T1 Languages/apps/workflow | Sections 3, 5, 12, 13; C MVP followed by required Rust/Go and native apps | C01-C10; complete language/build/runtime/app matrix at P1-P3 |
| T2 CPUs and Linux hosts | Sections 3.1, 4, 12.4 | Same artifact across all four CPUs and qualifying glibc/musl hosts at P1-P3 |
| T3 Neutral package/native exception | Sections 2, 6, 7, 12.1 | Shared/conditional common IR, no target-fixed module fallback, declared native libraries/helpers |
| T4 Managed native dependencies | Sections 4, 8.3, 13.1-13.2 | C12 initially; full transitive/late-load/host-independence/update evidence at P2-P3 |
| T5 Native fidelity and execution | Sections 2, 6, 8, 11-14 | Direct ordinary ELF, ABI/behavior tests, no publication runtime, A1 |
| T6 Standalone native freedom | Sections 1, 8.3-8.4, 12.1, 13.4 | Normal dlopen/helpers/debugging/app-owned code generation; security absent |
| T7 Installation priorities | Sections 8, 12.4, 13.3, 14.2 | Complete local compilation before execution; real 32-bit resources; separate install measurements |
| T8 Release lifecycle/updates | Sections 13.2-13.3, 15.4 | No committed-release recompilation; exact-output recovery; compatible updates without application changes |
| T9 Privacy/RE parity | Sections 5, 7.2, 14.3 | Private-input exclusion from first artifact; whole-package A2 gate before P3 |
| S1 Deployment/trust | Sections 15.1, 15.5 | Trusted-store model; qualified guest and supplied-OS coverage, Q09-Q10 |
| S2 Closed secure applications | Sections 15.1, 15.4 | Complete build-time contract, no install-time capability discovery, compatible approved updates, Q01/Q07 |
| S3 Native admission | Section 15.1 | Per-submission justification/manual store review/signing, Q02 |
| S4 Integrity and permissions | Sections 15.2-15.3 | Signed-file mapping/provenance/mutation checks and handleable resource denial, Q03-Q06 |
| S5 Offline and revocation | Section 15.4 | Offline execution and persistent stop/block-on-known-revocation behavior, Q08 |
| A1 Performance | Section 14.2 | Reproducible native references; every agreed metric/test within 5%; separate install/security costs |
| A2 Exposure evidence | Section 14.3 | Documented whole-package analysis and comparative RE tests with stated evidence limits |
| A3 Functional/portability coverage | Sections 11-13 | C registry plus full language/runtime/concurrency/Linux/native-component/target/lifecycle matrix |
| A4 Independent acceptance/accountability | Sections 1, 14-18 | Standalone acceptance before security; separate security qualification; explicit failed-gate reporting |

The C MVP is authorization to build and evaluate this architecture now. It is
not a declaration that the full requirements have already been met.
