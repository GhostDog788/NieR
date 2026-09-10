# On-Target AOT: Implementation Plan

## 1. Authority, scope, and current status

[01-architecture-design.md](01-architecture-design.md) remains the approved
requirements contract. This document specifies the selected implementation
direction, the C MVP, and the subsequent work required to satisfy all of 01.
It does not change those requirements.

**Status on 2026-09-10:** the first real Hello World pipeline works, including
two-profile merging, native-width controls, direct native execution, and basic
Make/CMake integration. The implemented compiler is still a deliberately small
single-block scalar/direct-call slice. Broad C, the upstream corpus, general
control-flow/ABI merging, wider portability, native performance parity, and
reverse-engineering parity are not claimed as achieved. Section 17 records
the verified results separately from planned capabilities.

The next deliverable is a working C MVP, not the complete product:

1. Establish the real pipeline with Hello World and native-width probes.
2. Expand it into the agreed broad-C functional MVP, including normal build
   integration and the configured cJSON/zlib corpus.
3. Complete the remaining C facilities, languages, targets, platform services,
   release lifecycle, and full standalone acceptance gates.
4. Only after complete standalone acceptance, implement and qualify security.

The first C work does **not** wait for Rust, Go, all four runtime targets, or
full A1/A2 acceptance. Conversely, completing Hello World does **not** complete
the C MVP; completing the C MVP does **not** complete T1-T9.

All compiler and linker infrastructure is used unmodified. Do not fork or
patch existing compilers. Reuse supported tools, plugins, extension interfaces,
and libraries. Writing replacement source-language compilers would defeat the
chosen reuse strategy. The security platform is not a dependency of any
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

### 2.2 Common consumer, reusable native producers

The external publication contract is source-to-common-IR. Its implementation
may compose existing native compilers, build tools, and profile-specific
configuration. The central LLVM-to-common-IR integration is shared.

The initial C producer is stock Clang with a shared LLVM capture plugin.
Additional languages must feed the same boundary using permitted existing
software. Compiler-specific command-line/configuration integration is not
permission to invent a new source-language compiler or add a separate
language-specific code generator to the device.

Keep source interpretation, preprocessing, macro expansion, build scripts,
generated bindings, language-specific optimizations, and ordinary runtime
lowering in existing publisher-side components. Where those components cannot
supply adequate capture inputs under the selected constraints, record a
feasibility failure; do not hide it behind a new importer.

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
| Ordinary C scalar arithmetic and control flow | Threads, atomics, and TLS |
| Native pointers, fixed arrays, structs, and unions | Dynamic loading and plugin coverage |
| float and double | long double and complex arithmetic |
| Aggregates passed and returned by value | Advanced floating-point environment behavior |
| Function pointers, indirect calls, and native callbacks | Variable-length arrays |
| Variadic calls and supported variadic function bodies | setjmp/longjmp and nonlocal jumps |
| Target-conditional common-IR sharing | Inline assembly and architecture intrinsics |
| Multiple translation units and normal object/archive/link behavior | The remaining general-C and full-product matrix |
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
| Publication CLI | Publish, inspect, and manually compile an experimental artifact |
| SDK/corpus/test support | Pinned inputs, native references, fixtures, and reproducible checks |

These are responsibilities, not a claim that every component exists or that
every path/API is frozen. The compiler, publisher, and CLI may initially share
one small codebase; separate their input contracts even when doing so.

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

The first checkpoint may use an explicit source-list recipe. This is a
bootstrap stepping stone, not completion of build integration.

The C MVP must support the configured Make and CMake projects through normal
compiler/toolchain configuration and wrappers. The wrappers run the actual
unmodified compiler and collect capture products; they must not fake every
object file with a new format that breaks native build-time linking.

Run target-sensitive configure and code-generation steps privately for each
profile. Keep host tools distinct from target outputs. Do not run private
project generators on the destination or distribute their source.

Compare native and publication builds from the same original source and
configuration. Developer-facing source/build adaptation must be packaging
configuration, not rewrites of application logic.

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

Start by requiring corresponding common graphs. Add target-conditional graph
sharing as part of the C MVP, with fixtures whose preprocessing/configuration
produces meaningful differences. A same-graph-only compiler does not complete
that MVP requirement.

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

Use per-translation-unit common MLIR bytecode in a standard tar archive with
a JSON manifest. Use libarchive and LLVM JSON support rather than inventing
archive or JSON parsers.

The envelope is versioned and explicitly experimental. Pin the compatible
dialect/compiler/LLVM/MLIR contract. Do not claim that arbitrary compiler-
internal bytecode is a stable long-term distribution ABI.

A representative logical layout is:

~~~text
manifest.json
modules/<opaque-id>.mlirbc
resources/<declared-relative-path>
native/<declared-payload-id>       # full-product extension where applicable
~~~

The manifest describes artifact identity, format/dialect version, module
digests, profile coverage, entry points, recorded compile/link semantics, and
the declared native SDK/dependency contract. Preserve archive membership and
link ordering separately from portable code modules.

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

### 7.3 Initial commands

The selected initial interface is:

~~~text
aot publish --recipe privatebuild.json -o application.aotpkg
aot inspect application.aotpkg
aot compile application.aotpkg --output-dir native
./native/<entrypoint>
~~~

Command spelling and schema details must match the implementation and its
tests; the first source-list recipe is not a substitute for the Make/CMake
recipe support required at C MVP exit.

Publish runs in the private workspace. Inspect reports the public contract,
coverage, modules, and dependencies without requiring source. Compile runs
in the separate destination workspace and consumes only approved input
classes for that stage.

There is no initial installer registry, updater, activation daemon, store,
or application launcher. These manual compilation jobs are development
artifacts, not production installed releases or an implementation of T8.
A later production installer must enforce the no-recompile lifecycle.

## 8. Destination compilation, linking, and execution

### 8.1 Input and transformation boundary

The destination selects a supported profile from the publication and a
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
application permissions. Those are security-platform obligations, not implicit
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
indirect calls, callbacks, and variadics. Include calls crossing translation
units and boundaries with stock-native reference components.

ABI tests must expose wrong registers, hidden result pointers, alignment,
promotion rules, and return classifications. A program that merely prints the
right pointer size does not establish any of these.

### C3 — Target-conditional sharing and build integration

Implement differing target-conditioned regions in a shared graph. Add
multi-translation-unit projects, native object/archive/link preservation, and
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

This section is a later implementation proposal for S1-S5, not part of the
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

Use one security model and product in two qualified deployment scopes:

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

Do not implement the production security/store stack before P3 acceptance.
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

As of 2026-09-10, C0 has passed on the development machine. Source-list and
basic Make/CMake publication use the same shared merger and artifact/device
path. The native-width portion of C1 has passed; its general control-flow
coverage has not. Some C3 build plumbing is implemented, but C2-C4 are not
accepted. No requirement in 01 has been reduced to match the current slice.

| Item | Status recorded by this revision |
|---|---|
| Requirements in 01 | Approved and unchanged |
| Selected architecture and C-first delivery | Documented here |
| Pinned SDK/bootstrap | Verified local Ubuntu 24.04 SDK, matching LLVM/MLIR/LLD 18.1.3 and both glibc 2.39 sysroots; no system installation |
| Initial capture/merger/common artifact/device slice | Implemented: registered MLIR dialect, actual bytecode, matching scalar graphs, mandatory reconstruction checks for both profiles, stock LLVM/LLD |
| C0 execution | Passed: ordinary C printf source produces a directly executed, stripped native x86-64 PIE using supplied glibc/loader |
| C1 specialization evidence | Native word/pointer sizes, fixed literals, fixed 64-bit integers, noinline and cross-TU calls pass; general CFG support remains missing |
| Existing-build portion of C3 | Direct-object Make/CMake builds, generated headers, native generators and native configure probes work in private profile trees; archives/divergent graphs remain unqualified |
| Parser and input boundaries | 73 package checks, 13 IR cases, compiler/native input-access tracing; not a security sandbox or full parser-hardening proof |
| Broad-C C2-C4 and corpus | Not completed; cJSON/zlib have not been accepted through this compiler |
| Full standalone A1/A2 and platform/lifecycle | Later mandatory gates |
| Security product | Later phase after standalone acceptance |

Reproduction from the repository root:

~~~sh
bash scripts/bootstrap-sdk.sh
source sdk/env.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
mkdir -p artifacts
build/aot publish --recipe examples/hello/hello.build.json -o artifacts/hello.aotpkg
build/aot compile artifacts/hello.aotpkg --output-dir artifacts/hello-native
env -u LD_LIBRARY_PATH artifacts/hello-native/bin/hello
~~~

The recorded demo outputs already exist in this workspace. Reuse the executable
or select new output paths when repeating compilation; no overwrite is allowed.
Observed stdout is `Hello world` followed by a newline, with exit status 0.
The original capture calls printf; normal device LLVM optimization changes
this case to a native `puts@GLIBC_2.2.5` import. The final ELF also uses the
SDK's `__libc_start_main@GLIBC_2.34`, not a publication launcher.

The release build passes all seven CTest groups: `ir_validation`,
`package_validation`, `capture_hook`, `hello_pipeline`, `scalar_pipeline`,
`build_integration`, and `device_boundary`. Capture is tested at O0/O1/O2/O3/Os/Oz
for both profiles. Build integration tests use unchanged application sources,
separate private native generator executions for both profiles, and a real
CMake runtime configure probe. Only selected application modules are published.
General Make/CMake compatibility and archive extraction are not thereby proved.

Recorded SHA-256 identities:

| Input/output | SHA-256 |
|---|---|
| Approved 01 | `fd0078c01339158f798d83a4efd4d639e97069113a8394f2d8a7e280afca644f` |
| SDK package lock | `f731dcf4284a77f01eeadaa809c963e23987e8ca6cb2580393b614b2ed0b1b36` |
| Hello C source | `c88dc247112f06475cfe75a61f60b3c50cafc59e4e6e1bd06494a04a54c5b79d` |
| Hello publication | `51f4d712bb3ff870fa55090f68207313bea2b50497c10f8e43636f6d1bd55869` |
| Hello native executable | `4972bdb161cb01d759a5d1df2c37de2aedd535ca8e3d8f007bb46a107e761249` |

Two local publications produced identical package bytes. The package is 4,096
bytes and the stripped native executable 4,856 bytes. A single warm-cache
measurement observed publication at 0.05 s / 96,840 KiB peak RSS and device
compilation at 0.04 s / 62,176 KiB. These are plumbing measurements, not
controlled performance acceptance or reproducible-build certification. The
native executable identity includes its local SDK interpreter/search paths.

The current reader enforces a closed public schema and canonical manifest JSON,
and rejects unsupported semantic IR. The publisher strips private debug data,
paths, and unnecessary private symbol names; needed native linkage symbols and
program string data remain. Full TAR auxiliary-metadata analysis and comparative
RE evaluation remain open. The environment is a qualified development host,
not a hermetic publisher image. See the repository README and SDK README for
current usage and host assumptions.

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
