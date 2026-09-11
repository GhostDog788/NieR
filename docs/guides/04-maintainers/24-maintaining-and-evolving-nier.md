# 24. Maintaining and evolving Nier

[Series](../README.md) · [Previous: SDK and compiler distribution](23-sdk-and-compiler-distribution.md) · [Next: Series index](../README.md)

## Objective and prerequisites

This final chapter turns the implementation model into a maintenance method.
You should now be able to follow source publication, shared IR, native specialization, build selection, validation and distribution.
The objective is to decide what evidence a change needs, which layer should own it, and what claims remain unjustified after a green test run.

Nier is pre-alpha. The user explicitly permits breaking changes everywhere without backward compatibility until that policy changes.
That freedom removes compatibility engineering from the immediate design, but it does not remove semantic contracts, reproducibility obligations, or the need to explain what was and was not validated.

## Begin with a claim, not a patch

Suppose a captured program fails because the two native profiles express an operation differently.
The first question is what shared meaning is missing.
Is the issue a producer's inability to recognize already-representable semantics?
Is a public operation or type missing? Is target lowering wrong?
Or did the native builds select genuinely different programs?

These questions lead to different changes.
A more complete private idiom proof may need no public format change.
A missing logical operation needs public schema and lowering work, including an independent producer test.
A new native target needs its own data layout and ABI behavior.
A build-selection mismatch cannot be repaired by pretending unrelated source inventories are the same LLVM module.

Write down the input assumptions and promised output before changing the matcher.
State which instructions, attributes, identities and effects the rule will consume, and what must remain untouched.
A phrase such as “recognize this common pattern” is insufficient if no one can tell where the pattern ends.

## Five forms of evidence

A **positive unit test** demonstrates an admitted behavior.
For example, an aggregate descriptor must classify into the expected full native signature, and a common operation must lower to verified native LLVM for each declared target.
Such tests are small enough to localize defects, but they can share the same mistaken assumption as the implementation.

A **negative test** places a similar-looking input just outside the admitted contract.
An extra use, volatile access, unmatched attribute, invalid domain or conflicting storage identity should cause rejection.
For normalization, also check that rejection does not mutate the original module.
For file operations, check that failure does not overwrite an earlier valid output.
“It returns an error” is only part of the expected behavior.

A **native differential test** uses stock Clang as a reference for actual target signatures, layouts and execution.
Inverse checks then specialize common Nier back to both private native profiles and compare justified normalized contracts.
This is strong evidence within the admitted rules, not a general theorem prover for arbitrary program equivalence.

An **end-to-end test** crosses the public boundaries:
stock Clang produces an independent artifact, `nierc` consumes it, and the resulting native program executes.
A native caller of a Nier-built DSO checks an ABI boundary that two pieces of identically mistaken generated code might otherwise conceal.
A consumer-only independent-producer test checks that the device did not quietly acquire a Clang dependency.

A **corpus qualification** runs unchanged configured upstream projects, their native references and their original tests.
The cJSON and zlib gates exercise interactions which small fixtures miss: real configure results, native archive selection, variadics, versioned DSOs and substantial control flow.
Corpus success does not make the earlier tests redundant, and it does not establish all C semantics or every product requirement.

## Worked case: admitting byte reversal correctly

The existing `nier.bswap` work illustrates a complete maintenance path.
The native evidence included a byte-reversal idiom whose carrier width differed from the actual bit domain.
Treating every native-word expression as “reverse all bits in this word” would be wrong for a wider carrier holding a narrower value.

First, the producer-side rule establishes the exact integer operation being performed.
It checks the participating shifts, masks and uses, rather than matching an application function name.
The rule belongs in private recognition code because these instructions are evidence for a logical operation, not the operation's public definition.

Second, the public operation defines its admitted domain: reversing bytes of a 16-, 32-, or 64-bit integer.
Its arity, result type and permitted attributes belong in core verification.
An independent producer must be able to express the same operation without knowing the native C idiom which originally motivated it.

Third, consumer lowering constructs LLVM's target-independent `bswap` intrinsic for the specialized integer type.
It does not invoke the producer or replay a captured native instruction sequence.
The ordinary LLVM backend remains responsible for instruction selection on the selected target.

Fourth, negative cases protect the distinction: unsupported widths, altered arithmetic semantics, extra effects and incorrect intrinsic call properties cannot be silently accepted.
The paired native inverse checks both captured profiles, while `tests/byteswap.cpp` and the real zlib qualification exercise different levels of the feature.

This example is not a recipe to turn every failed graph into a new public operation.
Sometimes the right result is a bounded private proof; sometimes the public semantic model really must grow.
The central constraint is that the common artifact contains the operation's meaning, not opaque native implementations for each profile.

## Corpus qualification versus retained replay

`corpus/qualify.sh` is the full source-to-native gate.
It downloads the hash-locked releases, runs original native builds and tests on both private profiles, publishes every selected output and executes the original tests against source-free destination binaries.
The configured matrix has 42 cJSON artifacts across static/shared configurations and eight zlib artifacts.
cJSON runs all 19 registered tests in each destination configuration.
The zlib destination runs the original static, shared and 64-bit Make recipes.

Fresh native builds repeat for each selected publication.
Repeated configure messages and repeated test names are therefore expected; they are not by themselves evidence of an infinite loop.
The runner is deliberately fail-fast, retains private evidence and does not count unsupported imports as successful qualification.
Machine suspension can also inflate elapsed wall time, so distinguish process progress from a clock that continued overnight.

Retained replay answers a narrower question.
The test-only `tests/corpus-replay.cpp` revalidates previously saved native objects, immutable journals, dependencies and final native witnesses,
then invokes the current stock-Clang producer and linker into new scratch outputs.
It requires each unit artifact and final publication to match the previous artifact bytes.
It checks the retained selection again afterward and must not repair missing evidence.

The recorded final replay covered all 50 selections and 170 unit-artifact occurrences.
That links a later publisher checkpoint to the earlier complete corpus evidence.
It is not a fresh native configure/build/test run, does not exercise a new source frontend capture, and does not establish that an altered SDK produces identical native references.
Describe it as retained regression replay, not as having rerun the entire corpus.

The distinction has practical value.
A native-ABI normalization change once mistook an ordinary scalar call result stored into a record for an aggregate return candidate.
Retained real cJSON evidence exposed the overbroad rule.
The correct response was to fix the semantic admission rule and rerun the complete selected replay set, not to exempt cJSON by name or drop the failed output from the report.

## Record the actual checkpoint

`qualification.txt` records the replayable command, terminal result, tool and configuration hashes, SDK/recipe identity, publication logs, private lane references, artifact hashes and destination hashes.
Keep the separate private capture directories as well as the outer corpus directory when replay is needed.
A Git revision alone cannot identify binaries built from a dirty pre-alpha worktree, and a later report's creation time is not necessarily the original run's start time.

Do not rebuild shared tools halfway through a qualification run.
Otherwise different outputs may be produced by different plugin or core versions while one report appears to describe a coherent checkpoint.
Parallel work is useful when ownership and build checkpoints are explicit; concurrent relinking of a tool being used as the oracle is not useful parallelism.

## Independent qualification lab

The following is an optional, relatively long Bash lab, not part of merely reading this chapter.
It requires the prepared SDK and coherent pre-alpha build, internet access, and sufficient temporary disk space.
Run from the repository root; use a whitespace-free temporary base for the zlib configure path:

```bash
source sdk/env.sh
evidence_lab=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide-evidence-XXXXXX")
set -o pipefail
TMPDIR="$evidence_lab" bash corpus/qualify.sh \
  "$PWD/build/prealpha/nier-build" "$PWD/build/prealpha/nierc" \
  "$NIER_SDK_ROOT" 2>&1 | tee "$evidence_lab/corpus.log"
printf 'Evidence base: %s\n' "$evidence_lab"
```

The outer `pipefail` matters: otherwise a successful `tee` can conceal the runner's failing status.
Append `--project cjson` or `--project zlib` to run one whole configured project, not a filtered subset of its required tests.
The absence of a selector runs both.
Do not remove test options after a failure and still describe the result as the same qualification.

## Breaking evolution still needs discipline

Pre-alpha compatibility freedom permits changing the schema, opcode set, artifacts, command details and SDK lock without retaining old readers or aliases.
Keep a contract identifier and reject mismatched inputs coherently; do not guess how to interpret an older artifact.
Regenerate fixtures and rebuild dependent tools when an intentional contract change requires it.
Compatibility freedom is not permission for producer and consumer to disagree within one checkout.

A new producer must obey the same public verification and publication rules.
Supporting another language means mapping its semantics and required runtime behavior into admitted Nier operations and native dependencies.
A language which needs garbage collection, exceptions or a runtime does not lose those requirements merely because it emits Nier.
The consumer can remain language-blind while compiling runtime code or linking explicitly qualified native runtime components; the current C implementation does not claim those language integrations already exist.

A new target needs more than a pointer width.
Data layout, endianness, integer and floating representations, native ABI, varargs, attributes, runtime dependencies and build profiles need explicit specialization and tests.
x86-64/i686 agreement cannot prove ARM behavior, including behavior on another 64-bit target.
Update the admitted target domain and exercise independent lowering rather than allowing a new target name to select a nearby old profile.

## Keep product claims separated

[01](../../01-architecture-design.md) owns requirements; [02](../../02-implementation-plan.md) owns the implementation offer and status.
The toolchain's next steps do not silently convert the later security platform into a prerequisite for compiling ordinary programs.
The pure toolchain may produce programs which use ordinary dynamic loading.
Future signed-code, executable-mapping, closure and revocation policies are a separate enforcement axis with their own threat model and qualification.

Similarly, using ordinary LLVM optimization is a sound implementation choice, not a measurement proving native-performance parity.
Runtime performance, on-device compilation cost and security overhead need separate baselines and metrics.
Removing private debug locations and profile payloads does not prove reverse-engineering parity either: required symbols, constants and common IR structure can still convey information.
Both performance and RE remain explicit acceptance work rather than consequences of the word “native.”

## Recap and questions

Maintain Nier by making claims small enough to test, proofs strong enough to reject misleading near-matches, and status reports precise enough to survive a change of compiler, target or developer machine.

1. **Can a negative test complete an unsupported feature?** No. It proves safe rejection; feature completion also requires positive admitted behavior.
2. **What does byte-identical retained replay omit?** Fresh source compilation, configure decisions, native reference rebuilding and upstream test execution.
3. **Does breaking compatibility remove the need for version checks?** No. Explicit rejection is still necessary to prevent mismatched tools from silently interpreting the wrong contract.
4. **Why are performance and security not implied by corpus success?** The corpus measures configured functional behavior, not performance thresholds, RE exposure or operating-system enforcement.

For daily work, start with `include/nier/IR/Compiler.h`, the producer interface (`include/nier/Producer/LLVM.h`), `tests/independent.cpp`, and the tests closest to the changed semantics.
Use [qualification corpus reference](../../reference/qualification-corpus.md) for the larger evidence boundary, and return to [01](../../01-architecture-design.md) whenever an implementation shortcut would redefine the product instead of implementing it.

[Previous: SDK and compiler distribution](23-sdk-and-compiler-distribution.md) · [Next: Series index](../README.md)
