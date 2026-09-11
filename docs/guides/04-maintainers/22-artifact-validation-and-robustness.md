# 22. Artifact validation and robustness

[Series](../README.md) · [Previous: Build and link semantics](21-build-and-link-semantics.md) · [Next: SDK and compiler distribution](23-sdk-and-compiler-distribution.md)

## Objective and prerequisites

This chapter explains how bytes become an admitted Nier artifact and how a
failed compilation avoids damaging existing files. You should understand the
artifact's shared modules and compilation-unit plans, basic Unix file types,
and the difference between a parser accepting syntax and a compiler accepting
semantics.

The maintainer objective is to reason about boundaries in their actual order:
open a bounded input safely, decode its container, validate its declared
contents, verify its IR, perform compilation privately, and publish only a
successful result. Each step has a different failure mode. None should be
confused with the future security platform's executable authorization policy.

## The archive is a closed envelope

[Artifact.cpp](../../../src/artifact/Artifact.cpp) reads an uncompressed tar
container into a `PackageFiles` mapping. It does not extract arbitrary archive
paths into a directory. The admitted contents are a canonical `manifest.json`,
indexed `modules/N.nierbc` members, and an optional declared
`link/version.script`.

The manifest describes the experimental contract, artifact kind, supported
target domain, managed runtime, native library declarations, qualified link
options, module digests, and native compilation-unit plans. Supported kinds
currently include object, executable, shared, and static. An object artifact
requires publication linking before normal native output; it is not a command
for the consumer to guess the final executable's dependencies.

The envelope is deliberately closed. Unknown manifest fields, unknown module
fields, and undeclared archive members reject even if their names look
optional. An extra field could express a semantic requirement the consumer
does not understand, or carry private source information which a superficial
reader ignores. “Ignore what you do not recognize” is not this format's
extension strategy.

The same principle applies inside Nier IR. The core validates operation and
attribute names, admitted types and relationships, source-location policy and
target specialization. The archive validator does not claim to understand
every bytecode instruction. [package.cpp](../../../tests/package.cpp) deliberately
uses opaque placeholder module bytes for some envelope tests; those tests are
not evidence that the core would accept those bytes as Nier code.

## Bound the input before decoding it

The current envelope limits are concrete implementation limits, not universal
properties of tar: at most 512 entries, at most 64 MiB of total member data,
an outer-file bound of 65 MiB, and a manifest bound of 1 MiB. Entry names have
a bounded length, and the reader rejects duplicate entries, absolute paths,
noncanonical paths, and names containing `..`. Members must be regular files;
archive directories, symbolic links, hard links and special files are not
accepted. Compression filters are not enabled.

Those checks need a safe opening strategy. A preliminary pathname `stat`,
followed by a library reopening that pathname, can race with replacement of
the file. Worse, opening a FIFO in ordinary blocking mode can hang before a
later “regular files only” check runs.

[Support.cpp](../../../src/support/Support.cpp) opens with `O_NONBLOCK` and checks
the resulting descriptor using `fstat`. It reads from that same descriptor,
requires a regular file, enforces its caller's bound, detects early EOF and
unexpected extra bytes, and checks the final size. Ordinary symlinks are useful
for SDK inputs, so the rule is about the opened target rather than a blanket
ban on every input pathname containing a symlink.

The archive reader then parses that bounded in-memory buffer instead of asking
libarchive to reopen the path. This closes the specific reopening race. It
does not establish an authenticated snapshot against every same-size concurrent
write or provide a sandbox around a native parser. Claims about those stronger
threats would need a different design and tests.

## Worked case: one changed module byte

Suppose a valid artifact contains `modules/0.nierbc`, and one byte changes
during copying while `manifest.json` remains unchanged.

First, the bounded file reader accepts the input only if it remains a regular
file within the outer-size limit. Second, libarchive must decode valid headers
and complete member bodies. These steps might succeed: a corrupted module
payload need not corrupt tar's structure.

Third, manifest parsing checks the experimental format and runtime contract,
the allowed field set, target identifiers and artifact kind. The current
canonical JSON requirement compares the parsed value's deterministic Nier
encoding with the original manifest text. LLVM's JSON object parser otherwise
retains only one value for duplicate keys; canonical encoding prevents a
discarded duplicate from hiding a competing declaration.

Fourth, the validator looks up the module at its required indexed path and
compares its SHA-256 digest with the declared digest. The changed byte causes
rejection here, before core decoding or native compilation. If somebody also
updates the digest, this particular check will pass—but the module must still
pass the IR verifier. A digest proves consistency with a declaration, not that
the declaration came from a trusted publisher.

Finally, compilation-unit validation checks how the shared fragments are
consumed. Every declared target must have a matching plan. Every fragment must
appear exactly once per target, within range, in a nonempty unit with an
admitted optimization setting. Static outputs additionally need bounded native
member identities. A plan cannot drop a module on one target, duplicate its
effects, or attach a private side payload to a unit.

This ordering makes diagnostics meaningful. A missing member is an envelope
error; a bad digest is a consistency error; an unsupported Nier operation is
an IR contract error. Suppressing all three under “invalid file” makes future
maintenance harder and can conceal which boundary was actually exercised by
a negative test.

## Schema validation is more than JSON validation

After the envelope is admitted, [Compiler.cpp](../../../src/ir/Compiler.cpp)
checks a closed IR schema. Unknown operation attributes are not simply passed
through to LLVM. Public operations and block arguments must use unknown source
locations; source paths, line information and private debug records are not
required device semantics. Numeric attributes and aggregate structures also
have qualified bounds and shape checks.

The verifier must preserve facts which are semantic even when they are not
machine instructions. Loop identity is an example: two branches sharing one
native loop metadata node differ from two independent nodes with equal-looking
options. Native calling signatures, arithmetic flags and target-domain
constraints are other examples. A validator that only counts operations cannot
establish these relationships.

Version scripts illustrate a controlled exception to “only IR members.” The
artifact can declare a bounded normalized symbol-version script with its own
digest. The implementation removes private comments and permits a qualified
character set. The consumer passes it to LLD's version-script parser, not its
general linker-script parser. The final LLD invocation still validates the
script's syntax. A symbol named `INPUT` is not automatically a file-input
directive in that grammar, so substring blacklists are not a replacement for
understanding the receiving parser.

## Output aliases and transactional publication

Consider `nierc input.nier -o input.nier`. Even if compilation could finish,
replacing the input would destroy the artifact. The consumer rejects this before
working. Canonical path comparison covers spelling differences and parent
directory symlinks; filesystem equivalence catches hard links whose pathnames
are different but whose inode is the same. The check is repeated before final
installation.

For an ordinary distinct output, compilation takes place in a newly allocated
private workspace. Only successful native output reaches `replaceFile`. That
helper verifies the destination is absent or a regular file, stages a copy in
the destination's parent directory, applies the intended file mode, and renames
the staging file over the destination. Creating the staging file on the
destination filesystem makes the final rename the relevant atomic visibility
step.

An existing symlink, FIFO or directory output rejects. A compiler or linker
failure must leave the previous valid output intact. This is not a full
durability protocol: the implementation does not claim a crash-consistent
installation transaction involving dependency directories, journaling and
`fsync` across a production release. Atomic replacement of one completed file
and durable multi-file deployment are different requirements.

There is also a driver boundary. Direct stock Clang retains its ordinary
failed-`-o` cleanup behavior. The SDK coordinator therefore asks Clang to write
to a private staged artifact and replaces the user's prior artifact only after
success. An atomic writer buried inside `nier-ld` cannot stop the outer Clang
driver from unlinking the same user path on failure. The tests must exercise
the actual entry point whose preservation guarantee is being claimed.

## A useful failure lab

This independent Bash lab uses only newly created lab outputs and an alias
attempt which the consumer must reject. Run it from the repository root:

```bash
source sdk/env.sh
validation_lab=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide-validation-XXXXXX")
clang --config="$PWD/build/prealpha/nier.cfg" -O2 \
  examples/hello/main.c examples/hello/hello.c \
  -o "$validation_lab/hello.nier"
build/prealpha/nierc inspect "$validation_lab/hello.nier"
before=$(sha256sum "$validation_lab/hello.nier")
if build/prealpha/nierc "$validation_lab/hello.nier" \
    -o "$validation_lab/hello.nier"; then
  printf 'Unexpected alias acceptance\n' >&2
  exit 1
fi
after=$(sha256sum "$validation_lab/hello.nier")
test "$before" = "$after"
printf 'Input preserved; lab files: %s\n' "$validation_lab"
```

This proves one rejection and preservation case, not all transactional behavior.
Read [read.cpp](../../../tests/read.cpp) for FIFO, descriptor and bounded-input
cases; [package.cpp](../../../tests/package.cpp) for archive and manifest cases;
and [build-rejections.sh](../../../tests/build-rejections.sh) for failed native
build/publication behavior. Those tests construct distinct failures so that a
passing result identifies the boundary being protected.

## Recap and questions

Robustness is layered: bounded descriptor input, closed container, canonical
manifest, declared digests and plans, verified IR, private compilation, and
controlled output replacement. Each layer should reject what it cannot safely
interpret without claiming the responsibilities of the next layer.

1. **Does SHA-256 validation authenticate a publisher?** No. It detects a
   mismatch with the manifest; the manifest itself is not a trusted signature.
2. **Why reject duplicate JSON keys?** Different interpretations can retain
   different values, hiding required semantics or private content.
3. **Why compare file identity as well as path strings?** Hard links can name
   the same input inode with different canonical paths.
4. **Does atomic rename prove the security platform or durable installation?**
   No. It gives a bounded file-replacement property; executable authorization,
   revocation, trusted storage and deployment durability are separate work.

The guiding implementation sequence is [Support.cpp](../../../src/support/Support.cpp),
[Artifact.cpp](../../../src/artifact/Artifact.cpp), then
[consumer/Main.cpp](../../../src/consumer/Main.cpp). Compare these mechanisms with
the independent security requirements in [01](../../01-architecture-design.md)
before describing them as enforcement.

[Previous: Build and link semantics](21-build-and-link-semantics.md) · [Next: SDK and compiler distribution](23-sdk-and-compiler-distribution.md)
