# Implementation Plan: On-Target AOT on Linux via a Compile-Only WebAssembly Profile

*Final, decision-complete plan. Supersedes the original GraalVM/JVM-bytecode plan
and all intermediate drafts. Companion to `01-architecture-design.md`.*

---

## What this platform is, in one paragraph

The platform ships **one distribution artifact per operating system**. That artifact
is architecture-neutral bytecode — a compile-only profile of WebAssembly — plus a
signed manifest. On the target device, a trusted compiler lowers the bytecode to a
**pure native ELF binary for that device's exact CPU architecture**, once, at install
time, and discards the bytecode. What runs afterward is ordinary native code:
**no interpreter, no JIT, no resident bytecode, strict W^X**, inside a **per-app OS
sandbox** whose permission set was fixed and proven complete before the app shipped.
The only steady-state cost over a hand-compiled native binary is that one install
compile. Distribution is portable; execution is native; the platform is driven, above
all, by **security**.

Two disciplines govern every decision below and should be read first, because they
are the reason many later choices went the way they did.

- **Fidelity, not improvement.** We reproduce each language's *native* toolchain
  faithfully — including its linking and memory model — and never "fix" its tradeoffs.
  Native Go static-links its runtime, so we static-link it. Native C dynamically links
  a shared libc, so we do too. We add no sharing a language did not already use, and we
  do not make a language behave differently than its own compiler would.
- **Depend, don't fork.** We consume upstream software through its *already-public*
  extension points and keep 100% of our custom logic in our own tree, so upstream
  releases arrive as near-free updates. We require **zero core changes to any
  upstream**, and — this being a **private project** — we never *rely* on upstreaming
  to carry our code. Where a capability is not reachable through a public API, we
  implement it *above* the dependency, never as a patch to it.

---

## Part 0 — Scope of this version

**v1 supports thin-runtime languages: C, Rust, Zig** — any language whose backend
targets our architecture and whose runtime is a static crt plus libc, with no garbage
collector, no green threads, and no runtime code generation.

**Rich-runtime languages — Go first, later NativeAOT .NET and native-image Java — are
post-v1.** They are explicitly designed-for, but not shipped in v1, because they carry
the only two genuinely hard pieces of engineering in the whole system (precise-GC
rooting and green-thread stack switching). Deferring them removes essentially all of
v1's hard-problem budget.

**The one non-negotiable that keeps the door open:** the IR is *specified* at full
scope now — reserving the mechanisms the rich-runtime floor will need — even though the
v1 toolchain leaves those mechanisms *unimplemented*. Reserving costs specification and
encoding space, not code. The genuine cost is a **design-time analysis of Go's lowering,
performed now**, so we reserve *correctly* (the right conventions and metadata, sized to
what Go's runtime actually requires) rather than blindly. Skipping this analysis is what
would turn "add Go later" into "redesign the format later," which would invalidate every
already-compiled artifact. This is paper work, done once, up front.

---

# Part I — The Architecture

## 1. Component map

| Abstract element (design doc) | Concrete implementation |
|---|---|
| Architecture-neutral bytecode | A compile-only **profile of WebAssembly** (+ reserved conventions) |
| Author language | Any AOT / closed-world language, via its own compiler backend |
| Optional ahead-of-time profile | Standard per-language PGO, carried in the bundle |
| Thin on-target compiler | A **re-hosting** translator (ours) driving **`libLLVM` + `lld`**, in the TCB |
| Native execution artifact | ELF `ET_DYN` (PIE), strict W^X, device-signed |
| Base platform | **Real Linux** — kernel, syscalls, ELF, process/thread/signal model |
| Cross-library references | SONAME-versioned native shared libraries via `ld.so` |
| Arch-committed hot code | Developer-built, store-signed native `.so` slices (the escape hatch) |
| Store-side per-arch delivery | **None** for bytecode — architecture is deferred to the device |
| Sandbox | OS-enforced: per-app UID, LSM domain, seccomp compiled from the manifest |

Note the near-empty per-arch row: because architecture is resolved on-device, the store
ships **no** per-architecture native content *for bytecode apps*. The only per-arch
content that ever crosses the wire is native hatch slices (§7), which exist only if an
app opts into them.

## 2. The distribution IR: a compile-only WebAssembly profile

The IR is **standard WebAssembly, used as a compile-only representation** — not a
from-scratch format, not an extended dialect, and not an execution target. Getting this
exactly right is central, so in layers.

**Why WebAssembly.** It is the thinnest representation that is genuinely *neutral across
instruction sets*, and choosing it means reusing its entire toolchain. LLVM IR is not
neutral — a frontend bakes in `datalayout` and per-target ABI lowering, which fragments
"one artifact" into a per-target matrix; both serious attempts to ship it as a
distribution format, PNaCl and Apple Bitcode, were retired. JVM bytecode sits too high —
its verifier requires complete nominal type information for every value, which is
un-strippable and *more* reverse-engineering-informative than the native binary it
produces. WebAssembly is the sweet spot.

**The commitment (this was a deliberate decision, not a default).** The IR is **a strict
profile of standard WebAssembly with no new opcodes.** Everything non-standard we need
lives *outside the instruction stream*: in **custom sections** (metadata the spec says
engines ignore), in **import conventions** (ordinary imported functions we assign special
meaning at lowering), and in **on-device lowering choices** (how we translate standard
instructions to native). We adopt only *shipped* WebAssembly features — the MVP plus
**threads/atomics**, **tail calls**, and **Memory64**. We reject owning a separate format
outright. We hold a single, sharply-bounded escape valve — a *per-instruction*
non-standard extension — in reserve *only* for a capability the future Go work proves
cannot be expressed as a convention, a section, or a lowering (stack switching is the sole
candidate; see §14). The point is not whether we ever emit one non-standard byte; it is
that non-standard is a **proven, documented exception**, never the rule — which is what
keeps the whole toolchain (LLVM's wasm backend, `wasm-ld`, standard validators, `wamrc`
as a blueprint) permanently on the free-update path.

**Why this is safe: we never execute the IR.** Nothing interprets it; it is only a form
to be compiled again into a real architecture. So the IR owes a *lowering* contract to
our own compiler, not an *execution* contract to an engine. It happens to be
standard-wasm-shaped, so standard tooling can read it — but *we* assign it relaxed,
native-faithful semantics a stock engine would not, and the syscall/rich-runtime
conventions mean a stock engine could not fully run it anyway. Concretely, this is what
lets us shed WebAssembly's sandbox character without leaving its format:

- **WebAssembly's safety machinery does no work for us.** Bounds-checked linear memory,
  reducible-only control flow, and a safe abstract machine exist so an engine can execute
  *untrusted* modules. Nothing executes ours, and the native output is caged by the OS
  sandbox (Part III). So we relax it — but note *where*: **relaxation is overwhelmingly an
  on-device lowering choice, not a format change.** When we lower a memory access to
  native, we simply do not emit the bounds check an engine would. The frontend emits
  ordinary wasm; the relaxation happens where we control it.
- **We are a single internal consumer, versioned with our own compiler.** So we are
  partly free of WebAssembly's standardization timeline — but honestly only *partly*: we
  can lean on *shipped* features and route around unshipped proposals via conventions and
  lowering, or wait. This is not the unbounded freedom an owned format would give; it is
  the freedom to under-use and re-interpret a standard, which is enough.

**The capability floor** — the operations the IR must be able to name so a real runtime's
architecture-assembly lowers *faithfully* rather than as a sandbox-safe approximation — is
specified in full now, implemented in stages:

| Floor capability | In v1? | Realized as (all standard-wasm-compatible) |
|---|---|---|
| Syscall | **yes** | Import convention → real trap (`svc`/`syscall`) on device |
| Threads + atomics + memory model | **yes** | Shipped wasm threads feature |
| Thread pointer / TLS | **yes** | Native TLS at lowering |
| Tail calls | yes, as needed | Shipped wasm tail-call feature |
| 64-bit flat pointers | **yes** | Memory64, base 0, unchecked lowering (see §18) |
| Safepoint / stackmap channel | reserved | Post-v1; carried in custom sections, finalized on device |
| Stack switching (raw SP swap) | reserved | Post-v1; import convention if possible, else the one opcode exception |

## 3. Platform: a virtual architecture over real Linux

**We implement an architecture, not an operating system.** The target triple is
`<ourarch>-linux-<abi>`: a new architecture, with the OS held at **Linux**. The ISA is
virtual (our IR); the platform — kernel, syscall numbers and semantics, ELF, the
process/threading/signal model — is **real Linux, reused wholesale**.

This is the load-bearing platform decision and the reason output is native-like. The
degradation in a naive language-to-wasm port comes from the **OS**, not the architecture:
the standard wasm platforms have no real threads and no readiness-based I/O, so
concurrency collapses to a single core and sockets are crippled. By holding the OS at
Linux we inherit each language's entire mature Linux layer — `clone`/futex threads, the
epoll netpoller, signal-based preemption — and only the *architecture backend* is new.

The **syscall operation** is the one thing our virtual architecture must carry that a
silicon architecture gets for free, because the platform is real and runtimes talk to it
directly. The IR *names* a Linux syscall (via the import convention), and the on-device
compiler lowers it to the real trap. Our neutrality is therefore **architecture-neutrality,
not OS-neutrality**: the artifact is neutral across ISAs but is `…-linux`.

## 4. Portability scope

**Architecture — neutral within an OS, for free.** All real ISAs are the same abstract
machine in different encodings, so one virtual ISA lowers faithfully to every real one.
This eliminates the entire architecture dimension of the traditional build matrix for
bytecode apps.

**Operating system — a deliberate per-artifact axis.** Operating systems share no
substructure; the sharpest proof is the netpoller — Linux `epoll` and BSD/macOS `kqueue`
are readiness-based, Windows `IOCP` is completion-based, incompatible philosophies that
force separate implementations. One-artifact-across-all-OSes would require a portability
abstraction (a PAL) with per-OS device shims, at which point output "behaves like our
PAL," not like native anything — forfeiting the core invariant (the JVM/.NET/libuv
lesson). So we ship **roughly four artifacts** — `linux`, `windows`, `darwin`, `bsd` —
each architecture-neutral, each native-faithful for its OS. Off-Linux obstacles are
concrete and known (no stable raw-syscall ABI on Windows/macOS/OpenBSD → the syscall
convention lowers to a library call there; preemption differs in *mechanism*; executable
format and unwind metadata diverge → the metadata channels must be format-neutral).

**iOS is a hard policy blocker**, independent of engineering: our model installs a
compiler that emits native executable code, which iOS forbids at the app level (JIT
entitlements are Apple-only). Ironically our *runtime* model — strict W^X, no JIT — is
exactly what iOS wants; it is the *install-time compile* that collides with policy.
macOS is fine; iOS likely needs Apple's cooperation.

**This plan's concrete instance is the Linux target.** Everything below is Linux-specific
where it needs to be.

---

# Part II — Execution

## 5. The author toolchain and the artifact

The language frontend runs and stops before machine code, emitting our IR. "Linking" on
the author's machine is **IR-level**: combine translation units into one pre-linked,
pre-optimized module, resolve internal symbols, and leave external shared-library
references as SONAME imports. **All native code generation and native linking happen only
on-device.** The author's toolchain also runs the capability-reconciliation pass (§9) so
the developer sees any permission mismatch *at build time*, and signs the result (§11).

- **C** — `clang --target=<ourarch>-linux-<abi> echo.c` emits IR objects; `wasm-ld` links
  them into one module importing libc symbols by name, recording `DT_NEEDED: libc.so.N`.
  Artifact = IR module + signed manifest. Dynamically imports a shared libc — faithful to
  native C.
- **Rust / Zig** — the same shape via their own LLVM-backed toolchains.

The **manifest** declares the app's capability set (Part III) and its SONAME dependencies.
The **IR module and manifest are signed by the developer** (§11).

**The "virtual ABI meets real ABI" seam is a non-issue**: libc itself ships as an IR
package, lowered on-device by the *same* compiler, so both sides were compiled against the
virtual ABI and one authority lowers both to the device's real ABI identically. Because we
use Memory64 with base-0 unchecked lowering (§18), a bytecode pointer *is* a native
pointer, so calls across this and every C-ABI boundary carry no marshaling.

## 6. The store

On download, the store ships **IR + manifest** (plus native hatch slices for the device's
architecture, if any). Because architecture is deferred, **no per-architecture native
bytecode content ever crosses the wire** — one artifact serves every architecture on the
OS. The store also performs the authoritative build-time security checks and countersigns
(§9, §11). Dependency resolution runs over SONAMEs: missing shared-library packages are
scheduled, refcounted, and kept side-by-side by major version — a Linux-distribution
package model applied to an app store.

## 7. The on-device compiler, and the native hatch

### 7.1 The re-hosting compiler (the core new component, in the TCB)

It occupies the slot ART's `dex2oat` occupies, but its job is **re-hosting**, not
managed-runtime compilation. At install it: verifies the signatures (Part III) → lowers
IR → native ELF (`ET_DYN`, PIE) for the device's real architecture → links by SONAME (or
self-contains) → finalizes any GC stackmaps against native frames → applies the mandatory
hardened codegen baseline → **signs the output with the device compiler key and records
its hash in the device allowlist** (§13) → registers it for launch → and **discards the
IR**.

**"Re-host, not re-optimize" — stated precisely, because it is easy to misread.** It does
*not* mean "no optimization." It means: **run LLVM's *backend* pipeline** (instruction
selection, register allocation, scheduling, target-specific lowering, hardening) — which
is exactly where native-quality codegen for the real architecture comes from — while
**skipping a re-run of the target-independent *middle-end*** (the interprocedural and loop
optimizations, the vectorizer), because the language frontend already ran those, and
re-running them against reconstructed-from-wasm IR is where the neutral-waist information
loss (§18) would bite anyway. The device does the arch-specific work that must happen on
the arch; it does not redo the arch-neutral work that was already done well upstream.

**In the TCB.** Trust chain: developer signs → store verifies and countersigns (attesting
the IR passed the security checks) → the trusted on-device compiler lowers the verified IR
→ the compiler signs and lists its own output. The device trusts the native binary because
it came from the device's own TCB compiler, from store-attested IR. The honest cost: the
compiler ingests **relaxed, low-level, possibly-adversarial IR** (unchecked memory, and —
later — irreducible control flow), so hardening it against malformed input is a real job;
the signature is the first defense, not the only one.

### 7.2 The native escape hatch (Form A)

The neutral IR pays a tax on **architecture-committed code** — wide SIMD, `-march=native`
tuning, hardware intrinsics, inline assembly (§18). To keep the platform from having a glass
ceiling on that code, an app may carry, for the arch-committed *fraction* of itself (a
codec, a crypto core, a DSP inner loop), **developer-built, store-signed native `ET_DYN`
shared libraries, one per target architecture, presenting the C ABI**, linked by the
bytecode app via `DT_NEEDED` and resolved by the real `ld.so`. This is the NDK model, and
it reuses machinery we already have: the C-ABI interface is our cross-language floor (§10),
and the store already splits bundles by architecture (§6). This is *not* a native
replacement for the app — it is a per-arch native library the bytecode app links.

Its two accepted concessions are named in §18: the single-artifact property gains an
"unless you ship native slices" caveat (and only for the slice, only for apps that opt in),
and the native slice does **not** receive the on-device compiler's mandatory hardened
baseline (though it is still store-signed, runs under the same OS sandbox, and is W^X at
load — see §13). Two code-provenance classes coexist in one process, cleanly partitioned:
the compiler signs and lists what it *generates*; the store signs what developers *publish*
natively.

---

# Part III — Security

Security is the platform's primary driver. Its guarantees rest on a single structural
property that recurs in every mechanism below: **the legitimate path is made the *only*
path by construction, so every violation is unambiguously hostile** — which is what earns
each mechanism the right to treat a violation as an alarm rather than a bug.

## 8. The two boundaries

Two boundaries must never be conflated.

- **Process → system** (the sandbox): what the app, as a whole, may do to the OS.
- **App → its own libraries** (in-process memory isolation): we do **not** provide this,
  and native operating systems do not either — a linked library shares the app's address
  space and permissions.

So **the process is the unit of trust**: everything compiled into one process is mutually
trusting, because after the compile there is no intra-process boundary to enforce. Two
things in different trust domains need a real process boundary, not the vestige of a wasm
sandbox we compiled away (Memory64 base-0 unchecked lowering means bytecode code can
address the whole process, exactly like native C — the accepted consequence of §18's
in-process position). **Enforcement is the OS cage, never a wasm sandbox:** per-app UID, an
LSM/SELinux domain, and a seccomp allowlist compiled from the manifest, enforced at the
syscall boundary.

## 9. The permission model — two layers, verified at build

**(i) What a permission is, mechanically — two layers.** Developers declare **capabilities**
(intent: `network.listen`, `fs.read:/data`, `threads`). A **platform-owned
capability→syscall mapping, in the TCB**, expands each capability to its concrete syscall
set and compiles the union to a **seccomp-bpf filter**. The capability is the contract and
the audit surface a reviewer and the user see; the syscall filter is the enforcement floor.
This layering is not optional: seccomp is syscall-level regardless, so *something* must
produce a syscall filter, and a raw syscall list is unstable across libc changes and
meaningless to a reviewer. The accepted cost: **the capability→syscall mapping joins the
TCB** (a wrong mapping over-grants for every app), so it is security-reviewed, versioned,
and — because it is central — fixed once rather than per-app.

**(ii) Where the set comes from — reachability ceiling, narrowed to intent, reconciled at
build.** Closed-world analysis computes the set of capabilities the code *can* exercise, as
a **sound over-approximation** (indirect calls are handled conservatively; a dependency
that reaches `socket` is caught whether or not the developer knew). This is the ceiling.
The developer supplies the *policy* the analysis cannot infer — which hosts, which paths,
whether a reachable-but-unwanted capability should be granted at all — narrowing the ceiling
toward intent. The two are **reconciled at build time**, fail-closed: any capability the
analysis proves reachable that the manifest does not cover is a **hard build error**, and
the toolchain reports the reachable call path that demands it. The honest calibration:
because indirect calls are over-approximated, the result is **sound and free of
runtime-discovered holes**, and developer-narrowed toward intent — it is *not* claimed to be
theoretically minimal.

**(iii) The consequence — a runtime denial is a security breach, not an error.** Because the
world is closed and compilation is on-device from verified IR, the set of capabilities the
app can possibly exercise is **fully known and fully enforced before it ships**. There
exists no legitimate execution that reaches a denied syscall — that case was eliminated at
build. Therefore a seccomp denial in production can *only* mean the running code is not the
signed program (tampering, injected/ROP'd behavior), or an exploit is reaching for a
primitive the legitimate program never uses, or the trust chain failed. None is
recoverable. The seccomp action is **`KILL`, and the event is routed as a security alert.**
This is a property most sandboxes cannot claim: on an open-world platform a denial is
ambiguous (missing declaration vs. attack), so it cannot be treated as an alarm; here it is
unambiguous. The reconciliation pass is thus **security-critical, not merely a developer
convenience** — the entire justification for treating denials as attacks rests on it, so it
must be exhaustive, must run authoritatively store-side (attested by the countersignature,
§11), and must fail closed.

## 10. Packages and cross-language interfaces

Dependencies are **SONAME-versioned native shared libraries** (the ELF model), refcounted
with side-by-side majors. A package's exported interface is the platform's stable,
**C-compatible native ABI** — the universal FFI floor every language already speaks.
Per-language bindings over an interface are generated at **build time** (a WIT-style IDL is
a natural authoring form, reused, not required) and compile to **direct native calls
resolved by GOT/PLT at load** — no runtime marshaling, consistent with native performance.
Cross-library references resolve by symbol at load, so a library's internal layout may
change freely within a compatible version without invalidating a single compiled dependent.
A native hatch slice (§7.2) is just such a package, with hand-tuned native contents.

## 11. The trust chain — two signatures

Two independent signatures, each proving a different thing.

- **The developer signature — proof of sole authorship.** The developer's private key is a
  secret held only by the developer; on first upload the client generates the keypair and
  sends **only the public key** to the store. From then on, **every** update must carry a
  signature verifiable by that enrolled key. The store *verifies* this but never possesses
  the private key, so **the store cannot forge authorship** — it can only refuse to serve,
  never counterfeit. This is stronger than a typical store model: authorship is independent
  of the store's honesty.
- **The store signature — proof of trusted provenance and of checks-run.** The store
  verifies the developer signature, runs the authoritative security checks (§9's
  reconciliation, malware/policy scanning on the IR), and **countersigns** — attesting both
  "this arrived through a trusted store" and "the build-time checks passed." The device
  requires **both** signatures over the same bytes.
- **The device pins only the trusted store(s).** Adding a trusted store is a **privileged
  device operation**. The device does not track developer keys — it trusts the store to have
  verified the developer, and trusts the developer transitively through the store's
  attestation. The device's trust anchor is thus **small, singular, and long-lived**, which
  is what a TCB wants; developer-key management (enrollment, rotation) lives store-side and
  is invisible to the device.

Two consequences are load-bearing and are written in as obligations, not footnotes:

- **First-upload enrollment is the trust root of the entire developer half.** Every
  subsequent update authenticates against the key registered on first upload, but that
  registration is what binds the key to the real party. So enrollment must bind the first
  public key to a verified developer identity — after enrollment, the key *is* the identity
  to the platform. The security of "no one can impersonate the author" reduces to the
  integrity of enrollment.
- **Developer-key loss/rotation is a privileged, identity-verified, audited store
  operation.** Because the private key is client-only and is the *sole* proof of authorship,
  there is deliberately no backdoor (a store that could re-key freely could impersonate). So
  re-binding a new developer key to an existing app is gated exactly like adding a trusted
  store — strong identity proof, audited — and it is the one moment the platform *can* change
  who controls an app, so it is treated as such.

## 12. Executable integrity — signing, on write and on execute

Signature verification is not a one-time install gate; it is a property the running system
enforces continuously, so that **only code produced by a trusted, verifying pipeline ever
executes**, for the whole lifetime of the binary.

Because the native binary is **produced on the device**, the store cannot have signed it
(the store never sees native bytes). So the anchor is the **on-device compiler**, in the
TCB: it signs every native binary it emits with a **hardware-protected, device-held key**.
The **kernel/loader enforces signatures on both write-to-disk and execute** — an executable
file appearing on disk unsigned, or an attempt to execute an unsigned or bad-signature
binary, is refused and alerted. Provenance is split cleanly and exhaustively: the **store
attests the IR** it admitted; the **compiler attests the native binary** it emitted from
that IR; published native hatch slices (§7.2), which the compiler did not produce, remain
**store-signature-enforced**. Every executable that can run is thus signed by whichever
trusted party actually produced it.

We reject the alternative of verifying native bytes against a store-signed reproducible-build
hash: it would demand bit-for-bit deterministic on-device codegen across LLVM versions and
architectures, re-coupling us to reproducibility as a correctness requirement, to buy a
property (the store, specifically, vouches for native bytes) that transitive trust already
provides.

## 13. The generated-executable allowlist

On top of signing, the device holds a **hash allowlist of every executable the on-device
compiler has generated**, and to execute, a generated binary must be **both validly signed
and present in the list**. Appending to the list is **restricted to the compiler's measured
identity** — nothing else may write it, including root inside the app sandbox — and the list
is **kernel/LSM-protected with rollback/tamper-evidence**. Both the list and the compiler
key are **device-local state**, not global: each device's compiler signs and lists its own
device's output; there is no cross-device hash comparison. Published native hatch slices are
**not** in this list (the compiler did not make them; their signature is the store's).

**What this buys, precisely, stated without overclaim.** A signature says "some holder of
the key vouched for this"; the list says "this *specific* binary was *actually produced* by
the compiler and recorded at production time." That converts an open-ended capability (sign
anything) into a closed, enumerated record (these exact artifacts, and no others). It
specifically defeats **signing-key misuse short of full compiler compromise** — a leaked key,
a signing bug, a signing oracle — because a validly-signed-but-unlisted binary still fails to
run. It also gives immediate, trivial **revocation** (drop the hash; the old binary is
instantly non-runnable though its signature is still valid) and sharp **anomaly detection**
(the runnable set is finite and known). The honest limit, stated plainly: the list is a
**second gate behind the same door**, not an independent second door — both the signature and
the list fall to the *same* event, a compromise of the TCB compiler and its enforcement path.
It hardens everything *up to* TCB compromise and nothing beyond it — which is the correct and
expected boundary for on-device integrity, exactly where dm-verity, AVB, and IMA also live.

---

# Part IV — Languages

## 14. Adding languages, the scope, and the reserved rich-runtime floor

**Mechanism.** A language runs iff its backend can target our architecture triple. You do
not add languages; you add **one architecture**, and languages arrive through existing
machinery. A stock JVM (C2 JIT) or a JS engine is excluded by the W^X/closed-world
constraint — and not by accident: the surviving languages are exactly those whose runtimes
never generate code at run time, which is what keeps W^X and executable-integrity intact.

**Cost tiers.**

- *Thin-runtime* (C, Rust, Zig) — **v1**: essentially free, because one shared LLVM
  architecture backend covers them all and there is no runtime to preserve. Native fidelity
  for nothing.
- *Rich-runtime* (Go; later NativeAOT .NET / native-image Java) — **post-v1**: a bounded
  per-language runtime port, plus the two hard items below.

**Go, and why the hatch does not rescue it.** When Go is added, the change is a **new Go
port** (`GOARCH=<ourarch>`, `GOOS=linux`): reimplement Go's ~dozen runtime assembly stubs
in terms of the IR's floor conventions, and carry GC stackmap/safepoint metadata through the
IR for on-device finalization. Because the floor exposes the real operations, the stubs
**lower to native** rather than being emulated — the entire difference between native-Go
behavior and the degraded single-threaded wasm port. Go static-links its runtime and makes
raw syscalls, both faithful to native Go, both unchanged by us; all of Go's `linux/*` runtime
is reused because we hold the OS at Linux.

The native hatch (§7.2) gives Go's *arch-committed performance* the same escape it gives C —
Go's own hand-tuned asm kernels can be Form-A slices linked over the C ABI. But the hatch
**cannot** absorb Go's runtime *substrate*, and it is worth seeing why: stack switching,
GC rooting, and scheduling are not hot *leaves* — they are the connective tissue woven
through *all* of the app's own bytecode (every goroutine switch, every safepoint, every `go`
statement). You cannot factor connective tissue into an arch-specific corner library. So the
substrate must be expressed in the IR itself and lowered across the whole program — which is
exactly the **reserved floor**. This is the deep reason the v1/post-v1 line sits where it
does: the thing we deferred (the floor for stack-switch + statepoint rooting) is precisely
the thing no hatch can absorb, while the thing one might have feared (Go being slow on
arch-committed kernels) is precisely the thing the hatch *does* absorb.

**The two genuinely hard items** live here and only here — which is why deferring them empties
v1's hard-problem budget:

1. **Precise-GC rooting.** Go's collector expects Go's own stackmap format and Go does not use
   LLVM, so bridging Go's GC to LLVM-`gc.statepoint`-generated rooting metadata is real
   integration work, not a mapping.
2. **Green-thread stack switching.** LLVM's coroutine intrinsics are shaped for async/await,
   not raw Go-style stack swaps, so this is likely inline-asm — and making stack switching
   cooperate with statepoint GC is the hard knot. This is also the sole candidate that might
   force the per-instruction escape valve of §2 (if it cannot be an import convention).

**The reservation rule (in force for v1):** specify these conventions and metadata channels in
the IR now, from a real analysis of Go's lowering, and leave them unimplemented. Reserve
correctly, not blindly. This is the cheap insurance that keeps Go additive.

---

# Part V — Build Strategy: Maximum Reuse

## 15. The core bet: LLVM is the on-device backend

Make LLVM the on-device backend, so that **LLVM's existing, already-shipped features *are*
our capability floor**, and the on-device compiler becomes a **thin our-IR → LLVM-IR
translator + stock `libLLVM` + `lld`**. Almost everything that sounded exotic already exists
inside LLVM, built for these very languages:

| Floor / output need | Existing LLVM feature (invoke, don't build) |
|---|---|
| Precise-GC roots | `gc.statepoint` / stackmaps (`GCStrategy`, `GCMetadataPrinter`) |
| Tail calls | `musttail` |
| Threads / atomics | LLVM atomics |
| TLS | native `thread_local` |
| Syscall | inline asm |
| Every CPU architecture | LLVM targets |
| Hardened baseline | `-mbranch-protection` (BTI/PAC), `-fcf-protection` (CET), stack protectors |
| ELF / PIE / SONAME linking | `lld` |

So the translator mostly **maps floor-convention → existing LLVM construct** — mapping code,
not new compiler algorithms — and "re-host, not re-optimize" becomes literal (§7.1). This is
also why **LLVM, not Cranelift**: Cranelift is compile-speed-oriented (threatening native
codegen quality) and its GC-rooting story is less mature than LLVM statepoints (threatening
Go's GC) — trading *less* capability-reuse for *more* new code to hit the same bar. The price
of LLVM is a large on-device compiler and slower install compile (§18), managed by
configuration and caching, not by fighting codegen quality.

## 16. Depend, don't fork — and the private-project constraints

The governing rule, stated once for every component:

> **Depend on upstream through its already-public extension points; keep 100% of custom logic
> in our own tree; require zero core changes to any upstream. Where a capability is not
> reachable via public API, it is ours to implement *above* the dependency — never a patch to
> it.**

Being a **private project** deletes two escape hatches an open project would have:

- **No "add our need upstream as an option."** We cannot rely on LLVM/WAMR/Go accepting a
  change, so every custom capability must be reachable through *already-shipped* public API.
  Usefully, everything we have identified is — statepoints, `musttail`, inline asm, atomics,
  target/hardening/ELF features are all public surface we link against, exactly as
  Rust/Zig/Swift consume LLVM privately at scale. This makes **confirming that surface** (the
  gating design task, §20) the thing that keeps the "implement it ourselves" bucket empty.
- **The Go rebase burden is permanent.** Go has no out-of-tree backend mechanism, so a
  `GOARCH` port is a patch against the Go tree, re-merged every release, with **no upstreaming
  exit**. This is a second strong reason Go is out of v1, and when added it is budgeted as
  *ongoing maintenance*, kept as a minimal, well-isolated patch set so rebases stay
  mechanical.

## 17. The component reuse map, by how updates reach it, and the irreducible new code

Classified by update-flow mechanism — the cleaner the mechanism, the more freely upstream
improvements arrive.

| Component | Reuse target | Update-flow mechanism | New code (ours) |
|---|---|---|---|
| Kernel, `ld.so`, threads/epoll/signals | **stock Linux** | pinned dependency | none |
| libc | **musl** | pinned dependency (compiled through our frontend) | small syscall-backend port to our convention |
| IR-level linker | **`wasm-ld` / `lld`** | pinned dependency | none |
| Codegen (all arches, statepoints, hardening, ELF/PIE) | **`libLLVM`** | **LLVM public API** (linked lib, no fork) | invoke / register only |
| Signing primitives | **ed25519 / minisign-class** | pinned dependency | thin glue |
| Package resolver | **apk / dpkg-style** | pinned dependency | thin config (no per-arch split) |
| Sandbox primitives | **seccomp-bpf, SELinux/AppArmor, bubblewrap** | pinned dependency | the manifest→seccomp generator |
| Cross-language bindings | **`wit-bindgen` + WIT** | pinned dependency | lower-to-C-ABI config |
| Author frontend (source→IR) | **LLVM wasm backend** | pinned dependency (v1: standard-wasm output) | custom-section injector for syscall/manifest; relaxation is on-device |
| **On-device translator** (IR→LLVM-IR→native) | **`wamrc` as blueprint** | **our own tree**, consumes `libLLVM` | **the translator** — the flagship |
| Executable-integrity enforcement | seccomp/LSM/IMA-style primitives | our own tree + pinned kernel features | loader signing check + the hash-allowlist enforcement |
| Rich-runtime frontend/dialect extensions | — | our own tree | **post-v1** (with Go) |
| Go `GOARCH` port | Go's wasm port as scaffold | **rebased patch (permanent)** | **post-v1** — the one unavoidable rebase cost |

Two entries warrant a note. **The v1 author frontend is not a fork**: because relaxation is an
on-device choice and thin-runtime languages emit ordinary wasm, v1's frontend is stock
LLVM-wasm + `wasm-ld` plus a tiny custom-section injector; backend *extension* is deferred
with the rich-runtime dialect. **`wamrc` is a blueprint, not a fork** — it already is
"wasm → LLVM-IR → native AOT," the exact shape we need, so it quantifies the effort and serves
as the design reference; but its *output model is the opposite of ours* (it targets the WAMR
runtime — linear memory, host imports, instantiation — whereas we emit a standalone native ELF
with `ld.so` linking, real syscalls, and relaxed native memory), so a vendored fork would have
to gut its entire back half and keep only its opcode→LLVM-IR front half, which is exactly the
blueprint surface we reuse anyway. So we **build the translator ourselves against `libLLVM`**,
which holds the permanent-rebase count at exactly one (Go).

**The irreducible new code, after maximal reuse:**

- **v1:** (1) the **on-device translator** (the flagship: relaxed lowering, syscall
  convention, drive `libLLVM` + `lld` to signed hardened ELF, sign + list output); (2) the
  **manifest schema + capability-reconciliation pass + seccomp generator + install-time
  orchestrator + loader-integrity enforcement** (the security core); (3) the **IR spec** — a
  WebAssembly profile plus the *reserved* rich-runtime conventions (mostly a document); (4) a
  **small musl syscall-backend port**.
- **Post-v1 (with Go):** (5) the **Go `GOARCH` port** (permanent rebase), including the
  GC-statepoint rooting bridge and stack-switching-meets-GC; (6) the **rich-runtime
  frontend/dialect extensions** that back them.

Genuinely new *and* hard is a short list: the **translator**, the **security core**, and —
deferred — the **Go port's two knots**. Everything else is reuse-or-configure.

---

# Part VI — Honest Boundaries, Lifecycle, Sequencing, Outcome

## 18. Honest boundaries — the trades, stated as first-class costs

Following the design doc's practice, and with a deliberately critical eye. Several of these
were understated earlier in the project's thinking; they are corrected here.

**The neutral-waist law (the root cost).** A neutral representation in the middle of the
pipeline *must* discard exactly the information needed for architecture-specific
optimization — because what makes an optimization architecture-specific is that it cannot be
expressed neutrally. Portability and source-directed arch optimization are the same quantity
with opposite sign; no representation is both target-agnostic and target-optimal. Compiling
on-device recovers *this-architecture-generically* (the backend sees the real chip) but never
*this-architecture-as-the-source-directed-it*, because the direction (an AVX-512 intrinsic, a
`restrict`, a `MAP_FIXED`) was erased at the waist and cannot be reconstructed — that is the
decompilation problem again. This is the price of the architecture, not a defect. It is **free
for arch-neutral code** (which never issued such direction) and **expensive-to-prohibitive for
arch-committed code**. Every specific cost below is an instance of this one law.

- **Wide SIMD is capped.** Standard wasm's `v128` is fixed at 128 bits (a conforming engine
  must produce identical results everywhere); code that lowers through it is 128-bit-at-a-time,
  and the backend generally cannot soundly re-widen to AVX-256/512. This is the sharpest
  format-imposed gap, and it is relieved only by the native hatch (§7.2), not by the format.
- **`-march=native` tuning and hardware intrinsics/inline-asm.** These are target-specific and
  do not survive a neutral waist; intrinsic/asm-laden code either falls back to scalar or fails
  to compile to the IR — the same boundary `wasm32` draws today. This is the reason the
  language promise is **portable code, not all code**, and the native hatch is the escape for
  the code that pays.
- **The linear-memory constraint is *design-mitigated*, unlike the two above.** By using
  **Memory64 with the linear memory based at 0 and lowered unchecked**, a bytecode pointer
  becomes a real native pointer, `mmap`/`MAP_FIXED` can be the real syscall returning real
  addresses (no emulation), arbitrary pointers work, and the C-ABI boundary to native code is
  zero-cost. The usual Memory64 performance worry — that bounds checks return because guard
  pages cannot cover the 64-bit space — **does not apply to us, because we do not bounds-check
  at all.** So the linear-memory model largely collapses into the native flat model. The honest
  residual is **toolchain maturity**: Memory64 and its LLVM support are newer and less
  battle-tested than wasm32, so this design must be *validated* in implementation rather than
  assumed. It is a strong mitigation with a flagged risk, not a solved problem.
- **The diffuse pipeline tax.** Beyond specific features, the wasm round-trip can lose aliasing
  and overflow facts the original LLVM IR carried, so the on-device backend optimizes with less
  than a straight-through `clang` had. This is the small, workload-dependent "few percent," not
  a missing feature — real in aggregate, near-zero for typical application code.
- **Install-compile latency.** LLVM is a heavy compiler; compiling at install is slower than a
  package copy. "Re-host, not re-optimize" (skip the middle-end) helps materially, caching helps,
  and it is one-time — but it is a real cost, the same one `dex2oat` pays, and install-time code
  quality versus compile speed is a genuine tunable (a fast path trades a little quality for
  speed, with optional background re-optimization). Native runtime performance is unaffected;
  only install time is.
- **In-process, there is no memory isolation.** A bug in linked or hatch code can corrupt the
  process, exactly as in native C. This is the accepted §8 position (process = trust unit); the
  OS sandbox, not in-process isolation, is the security boundary.
- **Every integrity mechanism shares one failure boundary.** The seccomp tripwire (§9), executable
  signing (§12), and the hash allowlist (§13) all harden the system *up to* a compromise of the
  TCB compiler and its kernel enforcement path, and none beyond it. This is the correct and
  expected boundary for on-device integrity; it is stated so no mechanism is mistaken for
  protection against a compromised TCB.
- **Reverse-engineering resistance is near-native, not provably identical.** The shipped bytecode
  is stripped and pre-optimized (so structure is already destroyed) and is **discarded after
  install** (the persistent on-device artifact is native only), so the RE exposure is limited to
  the store and transit. Stripped standard wasm is *close* to a stripped native binary but,
  being more structured (function boundaries, typed locals, reducible control flow), is not
  provably strictly-no-easier to RE. The strong property comes from stripping + pre-optimization
  + delete-after-install, which is the accepted portability residual, not a proof of parity.
- **Two permanent burdens** (everything else rides upstream as free updates): the **Go port's
  rebase** against the Go tree, and **crown-jewel key protection** on three device/store assets —
  the store's signing key, the on-device compiler's signing key, and the integrity of the hash
  allowlist — each of which must be protected to the standard its consequence demands (HSM /
  hardware-backed, non-exportable, rollback-protected).

## 19. End-to-end lifecycle

**v1, real today — a C multithreaded echo server.**

```
Author:  clang --target=<ourarch>-linux-<abi> echo.c  ->  one IR module
         + toolchain runs capability reconciliation: reachable set = { network.listen:7777,
           threads }, plus DT_NEEDED libc.  Any gap -> hard build error with the call path.
         Developer key (client-generated, private never leaves client) signs IR + manifest.
Store:   verify developer signature against the enrolled key (store cannot forge it);
         re-run reconciliation authoritatively; malware/policy scan; countersign.
         Ship IR + manifest.  No native bytecode content on the wire.
Install: device verifies BOTH signatures under the pinned store root -> re-hosting compiler
         lowers IR -> native ELF ET_DYN for this arch (say arm64): unchecked Memory64 lowering
         (pointers native), syscall-convention -> svc, hardened baseline (BTI/PAC, stack
         cookies), W^X; DT_NEEDED: libc.so.N.  Compiler signs the ELF with the device key and
         records its hash in the kernel-protected allowlist.  IR discarded.  Manifest -> seccomp
         allowlist of exactly the reachable syscalls.
Run:     execve -> loader verifies the ELF's device signature AND its presence in the allowlist
         (both required) -> kernel maps PT_LOAD under W^X -> ld.so binds libc by symbol ->
         socket/bind:7777/listen (each permitted; an undeclared syscall is KILLed and alerted,
         never a functional error) -> per connection: pthread_create -> clone, one OS thread
         per connection (1:1), read/write echo.  W^X holds throughout.
```

C **behaves like native C** — 1:1 pthreads, shared libc, PLT-bound, native pointers — because it
is clang's own backend targeting our architecture; we changed nothing about C's memory or linking
model.

**Post-v1, the reserved case — the same server in Go.** Once the Go port and rich-runtime floor
land, `GOOS=linux GOARCH=<ourarch> CGO_ENABLED=0 go build` yields one self-contained IR module
(program + static runtime, runtime stubs in floor conventions). On device it lowers to a
self-contained ELF whose floor conventions become native (stack switch → SP swap, `g` → pinned
register, atomics → LSE, syscall-convention → svc), GC stackmaps finalized against native frames.
At run time: `go func()` schedules goroutines with no syscall, the **epoll netpoller** parks
blocked readers without consuming OS threads, and the scheduler spins up threads via `clone` up to
`GOMAXPROCS` for real multicore parallelism — **M:N**, native-faithful. This is what the reserved
floor exists to make possible, and why reserving it correctly now matters.

## 20. Difficulty and sequencing

| Work item | Difficulty | Notes |
|---|---|---|
| IR spec (wasm profile + reserved floor) | Medium | Mostly a document; the reserved-floor analysis from Go's lowering is the careful part. |
| **On-device translator + `libLLVM` + `lld`** | **Hard** | The flagship. `wamrc` as blueprint; relaxed lowering, conventions, signed hardened ELF, sign + list. |
| **Security core** (reconciliation, seccomp gen, loader integrity, allowlist, signing chain) | **Hard** | The primary driver; multiple integrity-critical, TCB-adjacent pieces. |
| LLVM wasm frontend + `wasm-ld` (v1) | Low | Stock, plus a custom-section injector. |
| musl syscall-backend port | Low–Medium | Small, `wasi-libc`-shaped. |
| Store / resolver / dependency pipeline | Medium | Distro-style resolution + side-by-side majors; no per-arch bytecode split. |
| Native hatch (Form A) integration | Low–Medium | Reuses C-ABI packages + bundle splitting; store-signed slices. |
| Go `GOARCH` port + GC bridge + stack switching | **Hard** | **Post-v1.** The other hard item; permanent rebase burden. |

**Suggested sequence (v1):**

1. **Fix the IR spec at full scope**, reserving the rich-runtime floor from a real Go-lowering
   analysis; implement only the thin-runtime subset.
2. **Confirm the LLVM public-API surface** covers every floor operation with zero core patch —
   the gating design task, since the security model's "no runtime denial ⇒ breach" and the whole
   free-updates story both rest on it.
3. **Build the on-device translator**: IR → signed, hardened, listed native ELF running on one
   real architecture. Establishes the execution model and the executable-integrity spine.
4. **Build the security core**: capability reconciliation (fail-closed, with developer call-path
   feedback), manifest→seccomp, the two-signature chain with device-pins-store, loader signing +
   allowlist enforcement. Establishes the guarantees that are the point of the platform.
5. **C end-to-end**: stock LLVM frontend + `wasm-ld` + musl-on-IR, installed, sandboxed under W^X,
   both signatures enforced. The first real app.
6. **Store + dependency + native-hatch pipeline**: one artifact in, native out, shared libraries
   resolved, arch-committed slices supported.

**Post-v1:** the Go port and rich-runtime floor (7), then additional OS targets (8), each a
separate per-OS effort. iOS remains out by policy.

## 21. What this achieves

- **One architecture-neutral artifact per OS** reaches every architecture on that OS; no native
  bytecode content is ever shipped, and only opt-in native hatch slices are per-arch.
- Each device runs a **pure native ELF** — no interpreter, no JIT, no resident IR — under **strict
  W^X**, **continuously signature- and allowlist-verified on write and execute**, inside a
  **per-app OS sandbox** whose **fixed permission set** was proven complete at build, so that a
  runtime permission denial is a security event and not a bug.
- **Authorship is unforgeable even by the store** (client-held developer key), **provenance and
  checks-run are attested** (store countersignature), and the device trusts a **single pinned
  store root**.
- Each supported language **behaves as its own native build does**, because our IR is simply a
  target architecture its existing backend aims at; we change no language's memory or linking
  model, and the native hatch removes the arch-committed glass ceiling.
- The **only** steady-state cost over a hand-shipped native binary is a **one-time install
  compile**, plus the near-native neutral-waist tax on arch-neutral code (and the hatch for the
  code that would pay it).
- It is **built with maximum reuse**: stock Linux, musl, `lld`, seccomp/LSM, and `wit-bindgen` as
  pinned dependencies; `libLLVM` as a linked library through public API; and — genuinely ours — one
  translator, the security core, and (post-v1) the Go port. Upstream releases arrive as near-free
  updates, with the single deliberate exception of the Go port's rebase.

The platform is native distribution and native execution at once — bought with a closed world, a
security-first OS sandbox with an unforgeable trust chain and continuous executable integrity, an
IR chosen low enough to host a real runtime and re-hosted faithfully to every real machine, a
native escape hatch for the code that must direct the architecture itself, and a build that owns as
little code as the requirements allow.
