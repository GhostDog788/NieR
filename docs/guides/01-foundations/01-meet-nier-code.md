# 01. Meet Nier Code

[Series](../README.md) · [Next: The Compiler Foundations](02-compiler-foundations.md)

## What you will understand

By the end of this chapter, you should be able to explain why Nier code exists, which two programs exchange it, and what runs after compilation finishes.
You only need your existing experience writing and building C applications.
There is no installation exercise yet.

## Start with the boundary, not the compiler implementation

Imagine that you have written a C application.
You want to publish it without shipping the source,
but you also want the receiving machine to produce an ordinary native executable suitable for its supported environment.
You need a way to describe the application between those two events.

Nier code is that description.
It is a language for compilers to exchange a program, rather than a language application developers normally write by hand.
The program is expressed using operations such as addition, function calls, branches, and memory access.
Some decisions remain symbolic until a target is selected.
Others, such as a deliberately fixed 32-bit value, are already exact.

The most important picture in the project is:

```text
Developer's machine                       Target machine

C source → producer → application.nier → nierc → native executable
                                                    ↓
                                             ordinary OS execution
```

The producer understands its source language and emits the agreed format.
`nierc` understands that format and the target's native rules.
The executable does not need either compiler to stay alive while it runs.

In our current C workflow, the producer is integrated with **stock Clang**.
The developer invokes Clang configured for Nier publication.
`nierc` is a separate executable, not another mode of a combined source-language compiler.
This separation is a product boundary, not just a convenient directory layout.

## Why source portability does not imply binary portability

You may already have built the same C source on several Linux machines.
The source works because each build supplies the local compiler, headers, library interfaces, and target conventions.
Those inputs influence the resulting binary, even when the `.c` files never change.

A native binary has made choices that source has not.
Its instructions target a CPU architecture.
Its external calls follow a calling convention.
Its loader and libraries must be available with compatible interfaces.
A binary linked against a newer glibc interface may require symbols an older machine does not provide.
Linux is an OS family, not one universal native binary contract covering every CPU and userspace.

Nier moves the final native compilation decision to the destination.
That does not remove native contracts.
Instead, the producer must preserve enough meaning for the destination compiler to make the right decisions later.
Native dependencies must still be supplied in a compatible environment.

Consider two descriptions:

```text
Description A: return the integer eight
Description B: return the number of bytes in a native pointer
```

They happen to agree on our 64-bit target.
They do not mean the same thing.
A portable representation needs to distinguish them.
This is why a Nier producer does more than rename a native object file or remove its CPU label.

## Three activities, three different outputs

**Publication** happens on the developer's side.
It transforms the application into Nier code and packages that code with the information the consumer needs.
Publication may use private source files, headers, debug evidence, and native reference builds.
Those private inputs are not automatically public payload.

**Target compilation** happens when `nierc` receives the artifact.
It validates the input, resolves admitted target-dependent properties, generates native code, and links the required output.
An executable is one possible output; the implementation also supports qualified shared-library and static-archive outputs.

**Execution** happens afterward.
The OS loads ordinary native output using the native runtime arrangement selected during linking.
There is no Nier virtual machine interpreting operations on each call, and no requirement to retain a resident compiler.
Ordinary libraries such as libc can still be necessary.

This distinction also explains the name “on-device compiler.”
The device is where the final compiler can run, not where every application instruction is handed to a compiler during execution.
Our development exercises often perform both stages on one machine so you can observe the boundary without a second computer.
The inputs and responsibilities are still separate.

## Nier is not defined by C

Suppose another compiler produces valid Nier operations directly.
It does not have to imitate our C producer's private work.
It has to obey the same public meaning: valid types, calls, memory operations, target constraints, and package rules.
The consumer can then compile those operations without a switch saying “this was written in C” or “this was written in language X.”

The repository already tests this distinction.
An independent producer builds Nier modules through the public libraries without Clang captures.
That proves the boundary is usable independently; it does not prove that every language's semantics have already been implemented.

A language with exceptions, garbage collection, or another runtime model needs an appropriate representation and dependencies.
If the existing Nier contract cannot express a required behavior, somebody must extend the contract and its consumer support.
“MLIR is extensible” does not mean `nierc` can compile an operation it has never been taught to understand.

## What the current implementation does—and does not—establish

The code you will study is a working pre-alpha implementation, not the complete future platform.
It has a bounded C producer and a language-blind consumer.
Its semantic validation currently recognizes x86-64 and i686 profiles; product native execution is initially qualified for x86-64.
Other architectures and languages remain work, not automatic consequences of the file extension.

The publication is not original source or a package of the original LLVM captures.
It still contains executable meaning, required external names, and program data.
Omitting source is not the same as proving that reverse engineering is as difficult as it is for a native binary.
Bytecode is not an encryption mechanism.

Security is a separate axis.
The standalone toolchain can operate without signing policy, a trusted store, or executable-memory enforcement.
Those are important platform requirements, but this course will not describe them as features already provided by ordinary artifact validation.

## A worked thought experiment

Take the two-file Hello application used later in the course.
`main` calls `hello`, and `hello` prints a message.
During publication, Clang knows about C declarations and includes.
In Nier, the important surviving facts are functions, calls, values, data, and the native contracts needed to reproduce behavior.

During target compilation, `nierc` does not reopen `hello.h` to check a C prototype.
The producer has already expressed the function contract in Nier.
During execution, the native program does not reopen `application.nier` to interpret `nier.call`.
That operation has already become native code.

Ask “who needs this information, and at which stage?” whenever the repository seems confusing.
Source headers belong to publication.
Native code generation belongs to target compilation.
A shared library used by the final executable belongs to its native execution environment.
Putting all three in one SDK directory for development does not merge their roles.

## Recap

Nier code is the independent program contract exchanged between a producer and `nierc`.
The producer knows how to express a source program; the consumer knows how to realize that meaning for a supported target.
The eventual program runs natively.
Portability, native dependencies, source privacy, and security remain different questions with different evidence requirements.

## Check your understanding

1. If both compilers run on your laptop during a demonstration, has the independent boundary disappeared?
2. Does a language-independent consumer need to understand native calling conventions?
3. Can a successful Nier validation establish that a publisher is trusted?

<details>
<summary>Answers</summary>

1. No. The artifact is still the producer's output and consumer's independent input. Separate machines make the deployment separation visible but do not create it.
2. Yes. Language independence removes the source frontend requirement, not target-specific code-generation and ABI responsibilities.
3. No. Validation establishes admitted structure and semantics. Publisher identity, signatures, trust policy, and enforcement are separate concerns.

</details>

## Source and evidence trail

- [Requirements, purpose and product boundary](../../01-architecture-design.md) states the intended product; read its standalone and security sections separately.
- Independent producer (`tests/independent.cpp`) is the concrete example of creating Nier without our Clang pipeline. You will read its API use in chapter 11.
- Hello source (`examples/hello/hello/main.c`) is deliberately ordinary C, not a program rewritten for a special runtime.

[Next: The Compiler Foundations](02-compiler-foundations.md)
