# 10 — Reading the repository as a C developer

[Series](../README.md) · [Previous: Beyond Hello World](../02-toolchain-users/09-beyond-hello-world.md) · [Next: Implementing the NieR contract](11-implementing-the-nier-contract.md)

## Objective and prerequisites

This chapter builds the bridge from ordinary C development to reading NieR's C++17 and LLVM/MLIR implementation.
You should understand pointers, structs, functions, and separate compilation in C. You do not need to learn all of C++ before making a useful contribution.
Focus first on ownership, checked errors, and the boundaries between components.

The aim is not to read the largest source file from top to bottom.
It is to follow one observable behavior from its public entry point to its test, while recognizing the C++ idioms that make the implementation safe to change.

## Choose a boundary before choosing a file

The directory layout encodes a real architectural separation:

| Question | Start here |
| --- | --- |
| What does `nierc` accept and run? | `src/consumer/Main.cpp` |
| What belongs in a standalone archive? | `src/artifact/Artifact.cpp` |
| What does valid NieR Code mean? | `include/nier/IR` and `src/ir/Compiler.cpp` |
| How does stock Clang emit NieR? | `src/publisher/ClangPlugin.cpp` |
| How are native LLVM profiles merged? | `src/ir/Producer.cpp` |
| How are existing builds coordinated? | `src/cli/Build.cpp` and SDK integrations |
| Which libraries may depend on which others? | `CMakeLists.txt` |

Do not infer architecture solely from the shared `src/ir` directory.
CMake puts consumer-safe files into `nier_ir` and the optional LLVM producer into `nier_llvm_producer`.
Producer files can inspect private LLVM captures; consumer files must not require those captures or Clang frontend libraries.
The build target graph is therefore part of the design, not incidental wiring.

Public headers under `include/nier/IR` and `include/nier/Artifact` expose the independent contract.
`include/nier/Producer` exposes the optional reference producer. A new language producer may use the former without using the latter.
That distinction is more important than memorizing every class name.

## C++ syntax you will repeatedly encounter

`namespace nier::driver` groups names without a C-style prefix on every function. A declaration such as `llvm::Expected<Options>` uses a **template**: a type parameter creates a particular form of a reusable container or wrapper.
Here, `Options` is the value type returned on success.

`auto` asks the compiler to infer a static type from an initializer. It does not make a value dynamically typed.
In `auto files = readPackage(path)`, the type is still exactly the return type declared by `readPackage`.
`const auto &entry` means a non-owning, read-only reference to an existing entry.
Omitting `&` may copy a container element; adding it does not extend the element's lifetime indefinitely.

Lambdas are small callable objects written near their use. For example:

```cpp
module.walk([&](mlir::Operation *operation) {
  operation->setLoc(mlir::UnknownLoc::get(&context));
});
```

`[&]` captures surrounding variables by reference.
`walk` visits operations in the module. The callback borrows both `operation` and `context`; it does not own them.
This example is appropriate for a synchronous walk.
Saving such a callback and invoking it after its captured variables die would be unsafe.

## Ownership: who destroys what?

C often pairs allocation and cleanup explicitly: `malloc/free`, `open/close`, or an initialization function and a matching destroy function.
C++ commonly uses **RAII**: resource acquisition is initialization. An object's destructor releases its resource when the object leaves scope, including on an early return.

NieR's `Scratch` type is a small example.
`Scratch::create()` returns an owned temporary workspace. Its destructor normally removes that workspace; a `keep` flag retains it for diagnostics.
Copying is disabled, and moving transfers ownership. Otherwise two objects might both try to clean up the same directory.

`std::unique_ptr<llvm::Module>` similarly owns one LLVM module. An ordinary `llvm::Function *` usually borrows a function owned by a module.
The pointer does not make an independent copy, and it does not keep the module alive.
Erasing the function or destroying its module invalidates the pointer.

MLIR has its own ownership vocabulary.
`mlir::OwningOpRef<mlir::ModuleOp>` owns an operation tree.
`mlir::ModuleOp`, `mlir::Value`, and `mlir::Type` are typically small handles, not deep copies of that tree.
Passing a `ModuleOp` by value therefore does not give the callee an independent module to modify freely.

A **context** owns shared compiler infrastructure such as uniqued types and attributes.
Objects that refer to those types must not outlive the context. You will see this natural order:

```cpp
mlir::MLIRContext context;
context.getOrLoadDialect<nier::ir::NIERDialect>();
auto module = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
```

Local variables are destroyed in reverse construction order, so the module is destroyed before the context.
LLVM IR uses a separate `llvm::LLVMContext`; an MLIR context and an LLVM context are not interchangeable because both have “context” in their names.

## Checked errors: read the condition carefully

LLVM uses explicit error values extensively. `llvm::Error` represents success or a diagnostic.
`llvm::Expected<T>` represents either a successful `T` or an error. These are not integer status codes and should not be ignored.

The consumer contains this characteristic sequence:

```cpp
auto files = readPackage(options.input);
if (!files)
  return files.takeError();

auto manifest = validatePackage(*files);
if (!manifest)
  return manifest.takeError();
```

Read it in three steps. `files` first owns an unchecked result.
Testing it tells us whether it contains a value. Only on success may `*files` expose the package.
On failure, `takeError()` transfers its error to the caller instead of losing the explanation.

An `Error` condition has the opposite everyday reading:

```cpp
if (auto error = nier::verifyModule(module, nier::supportedNativeTargets()))
  return error;
return llvm::Error::success();
```

Here a truthy `error` means failure. A truthy `Expected<T>` means success.
Confusing those conventions can turn a validation path upside down.

At the command-line boundary, `llvm::logAllUnhandledErrors` turns the error into a diagnostic and consumes it.
`std::move(error)` allows ownership to transfer; it is not itself a function that copies or relocates memory.
After transferring an owned result, do not keep using it as though it still held the original resource.

For a contributor, the rule is simple: check each result before dereferencing it, propagate the actual error, and do not replace a rejected transformation with a convenient success value.
A malformed artifact is expected input to a validator, not a reason to crash or assert.

## Recognizing IR objects without unsafe casts

LLVM's instruction hierarchy contains many specific instruction kinds.
`llvm::isa<T>(value)` asks whether an object belongs to a kind; `llvm::dyn_cast<T>(value)` returns the requested view if it does, or a null result if it does not.
For example:

```cpp
if (auto *overflow =
        llvm::dyn_cast<llvm::OverflowingBinaryOperator>(&operation)) {
  bool signedNoWrap = overflow->hasNoSignedWrap();
  // Preserve this semantic promise when representing the operation.
}
```

This uses LLVM's type inquiry machinery, not an unchecked C pointer cast.
`llvm::cast<T>` is for a type already established by a valid invariant; using it on an unvalidated external input can turn a diagnostic into an assertion.
MLIR has corresponding `isa` and `dyn_cast` helpers for handle types.

The example's no-wrap flag is not decorative metadata. It records an optimizer promise about arithmetic.
Preserving the opcode while dropping or inventing such flags can change semantics.
Learning the meaning of the inspected field matters just as much as learning the C++ spelling.

## Containers and borrowed views

`std::vector<T>` owns elements. `llvm::SmallVector<T, N>` also owns elements, but can keep a small number inline before allocating more storage.
`N` is an inline-capacity optimization, not a hard input bound. Actual bounds need explicit validation.

`llvm::ArrayRef<T>` is a borrowed pointer-and-length view over elements; `llvm::StringRef` is a borrowed character range.
Neither extends the backing storage's lifetime. A `StringRef` made from a temporary `std::string` can dangle as soon as the statement ends.
Use an owning string when a name must survive the source buffer or an iteration.

`llvm::DenseMap` provides efficient keyed lookup, often from an LLVM value to its corresponding MLIR value. A pointer key is useful for identity **within** one live IR graph.
Pointer addresses or temporary SSA names cannot establish semantic correspondence between two separately parsed target modules.
The merger needs explicit matching rules for that job.

## A practical reading exercise

Read `execute` in `src/consumer/Main.cpp` and draw a short list of its stages:
read archive, validate manifest, check requested target, materialize private modules, inspect or lower, compile native units, and commit output.
For each stage, identify one checked `Error` or `Expected` and its owner.

Then read `tests/independent.cpp`. It is small enough to see MLIR context lifetime, parsed-module ownership, unknown locations, bytecode serialization, and artifact construction in one place.
Finally read its CMake target: the libraries it links are evidence that this producer does not require Clang.

This is a useful first contributor exercise without changing a line of code.
For a first patch, a focused rejection test and a better diagnostic are often more tractable than modifying graph matching.
For maintainer work, follow the dependency boundary too: a fix that pulls Clang into `nier_ir` may solve a local test while breaking the independent-consumer product.

## Recap and check your understanding

Read C++ here as explicit ownership and checked transformations.
Determine who owns each object, how long borrowed handles remain valid, and which failures must reach the caller.
Then follow one behavior through its test and link dependencies rather than attempting to memorize the repository.

> [!faq]- Does copying an MLIR ModuleOp handle copy the whole module?
>
> No. It is a handle to an existing operation.
> Ownership is represented separately, for example by `OwningOpRef`; modifying the handle's operation changes that tree.

> [!faq]- Why can SmallVector&lt;T, 8&gt; still require an explicit input limit?
>
> Eight is inline capacity, not maximum size. The container can allocate more storage.
> A validator must enforce its own resource and semantic limits.

> [!faq]- What should happen to a failed Expected&lt;T&gt;?
>
> Check it, then transfer its error with `takeError()` or report it at the proper boundary.
> Do not dereference it or silently discard its error.

## Guided reading

Start with `include/nier/Support.h`, `src/consumer/Main.cpp`, and `tests/independent.cpp`.
Then compare `include/nier/IR/Compiler.h` with the optional `include/nier/Producer/LLVM.h`.
`src/ir/Producer.cpp` provides real examples of casting, borrowed views,
and semantic flags; it is a second reading, not the entry point.
`CMakeLists.txt` shows which of these dependencies survive a consumer-only build.
