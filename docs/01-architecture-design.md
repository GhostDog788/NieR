# On-Target AOT: Project Requirements

This document defines what the product must achieve. Its companion,
`02-implementation-plan.md`, proposes how to achieve it and must demonstrate
compliance with these requirements. Existing choices in 02 do not constrain 01.
These are acceptance requirements; feasibility and compliance remain to be
demonstrated by the implementation proposal and the resulting product.

## 1. Purpose and product boundary

Developers should be able to write ordinary native-language applications,
publish one Linux package without disclosing their source, and have each
supported destination install an ordinary native application. Its behavior and
performance should closely match a build made directly for that device with
the language's normal native toolchain.

The publication method must relieve application developers of maintaining
separate builds for host distributions, host libc versions, and CPU
architectures. The platform owns that compatibility work. Architecture-specific
native components are an explicit exception, defined in T3.

The product has two separately scoped and independently testable subsystems:

- **The standalone publication toolchain** produces, distributes, installs, and
  executes native applications. The complete flow must work without a store,
  permission enforcement, or the security subsystem.
- **The security platform** adds admission rules, a closed application contract,
  executable integrity, and resource permissions around that toolchain.

**Development priority: complete the working standalone toolchain first, then
add and refine security.** Adding security must preserve the toolchain's
independent usability. Security restrictions must not become hidden
prerequisites or limitations of the standalone toolchain. The standalone
toolchain remains subject to the ordinary rules of its host OS.

## 2. Standalone toolchain requirements

### T1. Languages, applications, and developer experience

The first complete toolchain must support **C, Go, and Rust**. Kotlin/Native is a
candidate for additional support, not a requirement of the initial language set.

Supported applications include command-line tools, services, and desktop
applications. Developers must retain their ordinary source languages, standard
libraries, native interoperability, and familiar development workflows.
Publication configuration and packaging work must be kept as transparent as
possible, without requiring application logic to be rewritten around a new
execution framework.

The toolchain must preserve each language's normal semantics and runtime
facilities. This includes memory management, concurrency, error handling, and
reflection where the language normally provides it. Go's collector and
goroutine scheduler, for example, are part of the native behavior to preserve.

### T2. Supported CPUs and Linux environments

The initial supported CPU families must include:

- 64-bit x86 (x86-64).
- 64-bit ARM (ARM64).
- 32-bit x86.
- 32-bit ARM (ARMv7).

Linux is the initial OS scope. The toolchain must cover both glibc and musl host
environments that meet the declared native requirements of the application,
supported language and runtime versions, CPU, and kernel facilities.

The supported baseline must be documented. The publication method must not
introduce accidental host-distribution or host-libc-version barriers beyond
that baseline. Host library versions must not determine application dependency
resolution, as specified in T4. Compatibility guarantees apply within the
declared supported contracts.

### T3. One publication package and the native-code exception

The developer must be able to publish one application package for the supported
Linux targets. Ordinary application code must be architecture-neutral when
published, with its native compilation performed on the destination before
application execution.

The package may also contain architecture-specific native **libraries and
executables**, including executable helpers, for code that needs a native
implementation for a particular CPU. Those components carry explicit target
compatibility requirements and form an exception to ordinary code's portable
publication path.

The standalone toolchain must support these native payloads without requiring
store approval or limiting their use through security policy. The justification
and approval requirements in S3 apply only when security is enforced.

### T4. Platform-managed native dependencies

The application dependency contract and automatic dependency resolution must
use **platform-managed native library packages**, installed as required. The
host distribution's libraries are not the application's dependency contract.
Supplying platform dependencies is a required property of the product, not
merely a fallback when host libraries are unsuitable.

Application developers and users must not have to repair host library
mismatches manually. The platform must provide the dependency environment
needed by the published application across qualifying hosts.

This requirement must preserve the language's normal linking and runtime
behavior. Components normally compiled into an application may remain part of
that application; the product does not require every language runtime or
dependency to become a shared library. T4 also imposes no security restriction
on ordinary dynamic loading in the standalone toolchain.

### T5. Native execution and OS transparency

Installed applications must be ordinary native executables and libraries,
loaded and executed through normal OS mechanisms. Their process behavior,
threading, signals, files, networking, and native-library interactions must
remain consistent with a direct native build.

The publication method must not introduce an interpreter, JIT, or execution
engine that the application needs at run time. Execution must not require a
compiler to remain resident or the distributed representation to be interpreted
or compiled during application execution. There must be no compilation warmup
introduced by this publication method.

Native fidelity concerns observable behavior and the performance limits in A1;
it does not require byte-for-byte identical executables. Target-dependent
behavior, including native word and pointer sizes, must match the corresponding
native target. A 32-bit application need not behave as though it had been built
for a 64-bit target.

### T6. Normal native capabilities without security enforcement

The standalone toolchain must not impose a closed-world application model.
Ordinary native capabilities, including `dlopen` and dynamic loading selected
at run time, must remain available under the host OS's normal rules.

The absence of a publication-specific JIT is not a toolchain policy banning an
application's own native facilities or code-generation behavior. Restrictions
on generating or admitting executable code belong to the security platform.
The toolchain must remain usable independently of those restrictions.

### T7. Installation priorities

Native output quality takes priority over installation speed. Substantial
one-time compilation work is acceptable within the target device's resources.
Installation duration and resource use must be measured, but there is no
universal installation-time limit at this stage.

Compilation of the portable application code must finish before that installed
application is run. Installation work must not be shifted into application
execution to conceal its cost or introduce a compilation-dependent startup
phase.

### T8. Release lifecycle and compatible updates

An already-installed application release must not be recompiled. A change that
requires rebuilding that application's native code must arrive as a **new
application release**. This excludes background retuning or maintenance
recompilation of the installed release.

Compatible platform and shared-dependency updates must be possible without
modifying or recompiling the application. They must preserve its declared
interface contract. With security applied, they must also satisfy the verified
capability and approval requirements in S2.

The implementation must explain how compatibility is maintained within the
supported contracts. It must not silently assume that an installed application
can be rebuilt when an update breaks those contracts.

### T9. Source privacy and reverse-engineering resistance

Application source must remain private. The publication process must not
require distributing source code or expose private build inputs through the
published package.

The distributed application must expose as little additional information as
possible compared with the corresponding fully stripped native release and
must meet the requirement of being **no easier to reverse-engineer**. The
comparison must use the same application and language, including information
that its normal stripped native build necessarily retains.

This requirement covers the entire distributed package, including code,
metadata, and auxiliary artifacts. It applies independently of security
enforcement. Deleting installation inputs after use does not substitute for
protecting the distributed artifact. The evidence required to assess compliance
is defined in A2.

## 3. Security platform requirements

The requirements in this section apply when security is enforced. They must
not restrict the independently usable toolchain defined in section 2.

### S1. Deployment and trust scope

There is one product with two deployment scopes:

- **Integration into an existing OS:** enforce security over applications
  managed by the platform. The platform is a guest and does not govern the
  entire host system.
- **The supplied Linux OS:** use the same platform as a core component to
  enforce security system-wide.

The store is trusted to attest developer identity and authorize releases. The
product does not promise independent protection against a dishonest or
compromised store impersonating a developer. Guarantees also depend on the
trusted platform and OS enforcement functioning correctly.

### S2. Closed applications and release acceptance

A secure application must be closed: its code dependencies and complete
capability requirements must be known and validated at build time. Native
exceptions must be declared parts of that application contract.

Before accepting an application release, the store must confirm that its
dependency and permission declarations are complete and that all required
checks and tests pass. Installation must not repair an incomplete release by
discovering and granting additional capabilities.

Secure applications must not acquire undeclared executable extensions at run
time. Dynamic linking or loading of an already-declared, approved dependency
may still satisfy the closed application contract. A compatible dependency
release may be adopted without an application rebuild only when preservation
of the application's interface and capability contract is verified and the
dependency is approved.

Changes that expand the application's declared capability requirements require
a new validated application release.

### S3. Admission of published native exceptions

For every submitted native payload, the developer must justify its technical
need, store developers must approve it manually, and the store must sign it.
This applies to native libraries and executable programs alike.

The exception must be reserved for justified needs rather than become the
ordinary secure publication path. Approval must not grant a native component
exemption from executable integrity or the application's resource permissions.

### S4. Executable integrity and application permissions

All application executable mappings must be backed by approved, signed native
files and protected against unauthorized modification. This must cover the
main executable, libraries, and executable helpers throughout their use.

Applications must not manufacture executable memory, introduce undeclared
executable code, or modify executable code. Both simultaneous writable and
executable access and application-controlled conversion of writable data into
executable code must be prevented. Native outputs produced during installation
must be authenticated as originating from the approved publication and
installation path.

The platform must also enforce declared application permissions for resources
such as files, networking, devices, and other applications' data, including
when the application is correctly signed.

Denied resource access must return an error the application can handle.
Completeness of a release's declared capabilities does not guarantee that
every resource request will succeed. A resource denial alone must not be
treated as proof of compromise. Unauthorized executable-code admission must
be blocked as an integrity violation.

### S5. Offline operation and revocation

Securely installed applications must be able to launch and run offline using
locally known approval state. Connectivity must not be required for their
execution.

Once a device learns that an application or code dependency has been revoked
as unsafe, it must stop affected running applications and prevent further
execution until an approved replacement is installed. Availability or unsaved
work must not delay enforcement of a known revocation. This does not promise
that an offline device learns of revocations immediately.

Revocation must not trigger recompilation of an installed application release
as a repair mechanism. Replacements must follow the release and compatible
dependency-update rules in T8 and S2.

## 4. Acceptance criteria and evidence

### A1. Native reference and performance limits

The reference must be the same application and dependency versions, including
platform-managed libraries, built directly for the same device using the
language's normal native compiler and documented release optimizations.
Compiler and runtime versions, target settings, workload, and measurement
conditions must be recorded so that the comparison can be reproduced.

The toolchain may introduce at most **5% regression for each agreed acceptance
test and metric** in:

- Execution speed or throughput.
- Latency.
- Application startup time.
- Memory use.

Measurement uncertainty must be controlled. Passing averages must not conceal
an individual failing acceptance test or metric. Comparing the same dependency
versions must separate publication effects from differences between library
implementations.

Installation costs and costs added by security must be measured separately
from these toolchain performance gates. Producing native instructions alone
does not establish compliance with the performance requirements.

### A2. Information-exposure evidence

Acceptance requires a documented analysis of information exposed by the entire
distributed package and comparative reverse-engineering testing against the
corresponding stripped native release.

The assessment must identify the artifacts and metadata available to an
analyst, the native reference, comparison methods, findings, and limitations.
It must assess the obtainable distribution itself, independently of later
deletion or execution restrictions.

Analysis and testing are the agreed evidence standard; a universal
mathematical proof is not required. The requirement in T9 remains the
acceptance target, and claims of compliance must state the scope and limits of
the evidence. Reduced persistence of a more revealing representation is not
evidence of equivalent resistance.

### A3. Required functional and portability coverage

The acceptance suite must cover:

- **Language behavior:** normal source and development workflows; native
  arithmetic, pointer-size-sensitive behavior, allocation, error handling,
  runtime facilities, and native interoperability for C, Go, and Rust.
- **Concurrency:** threads, synchronization, atomics, thread-local state,
  concurrent I/O, and shutdown, including Go's collector and scheduler.
- **Linux integration:** files, sockets, timers, signals, subprocesses, pipes,
  arguments, environment, and representative desktop and native-library use.
- **Native components:** library calls, callbacks across language boundaries,
  native executable helpers, and normal dynamic loading in the standalone
  toolchain.
- **Publication and installation:** the same published application package
  installed across the required CPU families and qualifying glibc and musl
  hosts, with the appropriate declared native payloads where used.
- **Dependency independence:** platform-managed dependency installation and
  resolution despite differing host libraries, followed by compatible shared
  dependency updates without application modification or recompilation.
- **Release lifecycle:** new application releases can be installed while an
  already-installed release is never recompiled in place.
- **Privacy and performance:** A1 and A2 applied to representative compute,
  allocation, concurrency, I/O, and startup workloads.

Each target must be compared with its own normal native behavior. Cross-target
tests must account for legitimate differences between 32-bit and 64-bit
programs rather than require one target's behavior on every CPU.

### A4. Independent acceptance and implementation accountability

The complete standalone publication-to-native-execution flow must pass its
acceptance criteria without the security subsystem. Security must then have
separate acceptance coverage for release admission, native exceptions,
executable integrity, permissions and recoverable denials, compatible
dependency updates, offline execution, and revocation in both deployment
scopes. Adding security must preserve the standalone flow's usability.

02 must map its implementation proposal to the requirements in this document
and define the concrete fixtures, measurement procedures, and verification
methods. These procedures must be established before using results to claim
acceptance.

A proposed scope reduction, unsupported required feature, weakened guarantee,
or changed acceptance limit must be reconciled explicitly with 01. Implementation
choices cannot silently redefine the requirements. Formats, compiler choices,
library mechanisms, and security enforcement mechanisms belong in 02.
