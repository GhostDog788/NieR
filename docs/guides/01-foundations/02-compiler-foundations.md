# 02. The Compiler Foundations

[Series](../README.md) · [Previous](01-meet-sela-code.md) · [Next](03-reading-a-sela-program.md)

## What you will understand

You will distinguish a compiler frontend, intermediate representation, optimizer, backend, linker, and loader, and place Clang, LLVM, MLIR, and Sela in that picture.
Read [chapter 01](01-meet-sela-code.md) first.
No compiler build or C++ knowledge is needed.

## The command you know hides several jobs

When you run `clang main.c helper.c -o app`, one command coordinates several activities.
A **compiler driver** chooses tools, arguments, and jobs.
A **frontend** understands the source language: preprocessing, parsing, types, and language rules.
A **backend** chooses target instructions and emits native code.
A **linker** combines selected objects and libraries into an output.

These roles do not require separate operating-system processes for every stage.
Some are libraries inside the same program.
The distinction is about responsibility and input/output, not counting executable filenames.

For our ordinary C example:

```c
int main(void) {
    hello();
    return 0;
}
```

the frontend needs the declaration of `hello` and the C rules for a call and return.
Later code generation needs a callable symbol and a native signature, not the text of the header that originally declared it.
This is an early example of converting language-specific knowledge into a lower-level contract.

The fragment above omits the header deliberately.
It illustrates the body, not a complete C translation unit that can be compiled without its declarations.

## What an intermediate representation buys us

An **intermediate representation**, or IR, expresses a program in a form useful to compilers.
It is not necessarily assembly with different punctuation.
Depending on its purpose, an IR may retain high-level constructs or make control flow, memory, and arithmetic explicit.

An optimizer can work on that representation instead of having a separate implementation for every source-language syntax.
A backend can accept a common representation instead of parsing every source language.
Sharing those components is how a compiler ecosystem avoids rebuilding the whole pipeline for every language/CPU pair.

For example, the following is **pseudocode**, not Sela syntax:

```text
value0 = constant 2
value1 = constant 3
value2 = add value0, value1
return value2
```

An optimizer may replace this calculation with a constant five if the precise arithmetic semantics permit it.
For variables, floating-point behavior, overflow conditions, or memory accesses, the proof is more involved.
An optimization preserves the program's contract; it does not merely make the instruction list shorter.

Changing representation toward native details is often called **lowering**.
Resolving a symbolic property for one target is **specialization**.
These can enable optimization, but they are different activities.
Replacing a symbolic pointer-size operation with eight for an admitted 64-bit target is specialization even if the function remains otherwise unchanged.

## LLVM IR is not automatically a portable publication

LLVM supplies an IR, optimization infrastructure, and target backends.
Clang can translate C into LLVM IR.
However, by that time important native decisions can already have been made: type widths, struct layouts, argument coercions, target-specific attributes, and constants derived from `sizeof`.

The same textual instruction family can occur in target-specific modules.
“Both languages emit LLVM IR” means their outputs use the same representation system, not that those outputs have forgotten architecture and ABI decisions.

Here is the critical C example again:

```c
size_t pointer_size(void) { return sizeof(void *); }
```

In our current native profiles, a frontend can already express the result as eight in one capture and four in another.
Removing a target triple does not turn either constant back into a symbolic pointer size.
The triple is a label for part of the contract, not the only place native assumptions appear.

Sela is our explicit representation for the common meaning we can preserve and validate.
The reference producer recovers that meaning from private native evidence within a bounded target domain.
An independent producer may construct valid Sela directly.
Chapter 16 explains why the reference producer needs more than a mechanical LLVM-opcode-to-Sela-opcode rename.

## MLIR is infrastructure; Sela is a particular contract

MLIR provides a framework for representing operations, values, types, and regions.
A **dialect** defines a vocabulary within that framework.
Sela uses its own registered dialect rather than accepting arbitrary operations from every MLIR dialect.
The [MLIR language reference](https://mlir.llvm.org/docs/LangRef/) is useful later for the framework's syntax and terminology.

In the current repository, Sela functions are represented by `sela.func`, not by a promise that every upstream function dialect is interchangeable.
Sela pointers use `!sela.ptr`.
Our implementation decides how to validate and lower those operations and types.
MLIR does not infer their meaning from their names.

There are therefore three distinct things to avoid collapsing:

| Name | Its role in this project |
| --- | --- |
| LLVM IR | Private producer inputs and target-specific consumer output before native code generation. |
| MLIR | Infrastructure used to define, hold, parse, and serialize our common representation. |
| Sela Code | Our admitted operation/type/semantic contract implemented using that infrastructure. |

There is no required step where `selac` asks a C frontend to reinterpret Sela.
The public representation has already crossed the source-language boundary.

## Linking and loading are not the same thing

You know that `main.o` alone may not be an executable.
A native object contains code and data plus information the linker needs, such as unresolved symbol references and relocations.
The linker resolves and arranges the selected inputs according to the output kind and link options.

A static archive can hold many object members; a normal link may select only some of them.
A shared library can remain a runtime dependency rather than being copied into the executable.
These choices affect program behavior and which code belongs to the application.
Sela must preserve them rather than flattening every available object into one universal program.

The **loader** acts when the native program starts.
It maps the executable and required native libraries and performs the platform's startup work.
The prototype uses an ordinary native loader with supplied runtime paths; it does not introduce a Sela application loader.
Producing a native ELF still does not mean that all machines have its requested interpreter and libraries installed.

## The complete route in this repository

Read this as a responsibility map, not a literal list of subprocesses:

```text
Source language
    → stock-Clang frontend and private LLVM captures
    → reference producer: common Sela semantics
    → independent .sela publication
    → selac: validated, target-specialized LLVM IR
    → LLVM optimization and object generation
    → native linking
    → ordinary native loading and execution
```

For target compilation, the current consumer coordinates the SDK's `opt` (optimization), `llc` (native object generation), and LLD (native linking).
Static archive outputs use `llvm-ar`.
These are implementation choices behind the public consumer boundary, not extra source-language compilers that an application must run at execution time.

The developer side is deliberately different: normal C publication is entered through stock Clang configured with the Sela integration.
More complex native builds have SDK coordination helpers, but those do not redefine the public format or make the consumer language-aware.

## Recap

Compilation is a chain of contracts.
A frontend understands the source language;
IR makes semantics available to later compiler stages;
optimization preserves those semantics;
lowering realizes them with more native detail;
linking chooses and combines native inputs;
loading starts the finished native program.
Sela adds an independent publication contract between the producer and target compiler, reusing LLVM and MLIR without treating them as synonyms.

## Check your understanding

1. Why is deleting an LLVM module's target triple insufficient to make it neutral?
2. Is lowering always an optimization?
3. If `selac` invokes `llc`, does the generated application depend on `llc` at runtime?

> [!faq]- Answers
>
> 1. Native decisions may already be embedded in widths, layouts, constants, signatures, and attributes. A label change does not recover their original meaning.
> 2. No. It translates representation toward a target. It may enable later optimization, but correctness does not depend on reducing the operation count.
> 3. No. `llc` is a compilation tool. The resulting application can depend on ordinary native libraries, but it does not call `llc` to execute its instructions.

## Source and evidence trail

- Consumer entry point (`src/consumer/Main.cpp`): find `lowerCompilationUnit`, `opt`, and `llc` to see the stages coordinated by `selac`.
- Dialect registration (`src/ir/Dialect.cpp`): `SelaDialect` registers our vocabulary; it does not register every upstream dialect.
- [LLVM 18 language reference](https://releases.llvm.org/18.1.8/docs/LangRef.html): consult when later chapters discuss target data layouts and instruction semantics. The repository SDK is pinned separately; upstream documentation is supporting reference, not a replacement for the checked-in contract.

[Next: Reading a Sela Program](03-reading-a-sela-program.md)
