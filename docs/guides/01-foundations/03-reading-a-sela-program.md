# 03. Reading a Sela Program

[Series](../README.md) · [Previous](02-compiler-foundations.md) · [Next](04-values-control-flow-and-memory.md)

## What you will understand

You will read an actual small Sela module rather than a made-up assembly notation.
You will distinguish operation names, runtime values, types, attributes, and function metadata.
Read chapters 01–02 first; you do not need an SDK to follow the example.

## Begin with the meaning

Consider this C function:

```c
int fixed_eight(void) {
    return 8;
}
```

Its interesting contract is small: no parameters, a 32-bit result in our current native profiles, and a returned value of eight.
We can express that contract directly in Sela without asking a C frontend to produce it.

The following is a **complete textual Sela module** adapted from the helper module in the independent-producer test.
It is a compiler representation, not a complete `.sela` package or an executable.
A programmatically parsed module also needs its source locations normalized before publication, as chapter 11 explains.

```mlir
module attributes {sela.schema = 1 : i32, sela.targets = ["x86_64", "i686", "armv7", "aarch64"]} {
  "sela.func"() ({
    %eight = "sela.constant"() {value = 8 : i64} : () -> i32
    "sela.return"(%eight) : (i32) -> ()
  }) {id = "fixed_eight", type = () -> i32,
      declaration = false, variadic = false,
      internal = false, dso_local = true,
      attributes = [[], []]} : () -> ()
}
```

Do not try to memorize the punctuation on the first reading.
Read inward: there is a module, it contains a function, the function creates a value, and it returns that value.
The remaining syntax makes these relationships precise.

## Operations and runtime values

Start with the body:

```mlir
%eight = "sela.constant"() {value = 8 : i64} : () -> i32
"sela.return"(%eight) : (i32) -> ()
```

`"sela.constant"` is an **operation name**.
`sela` identifies our dialect; `constant` identifies an operation in that vocabulary.
Quotation marks are part of MLIR's generic operation syntax.
They do not turn the instruction into a runtime string that the application interprets.

`%eight` names the result of the operation.
Later operations can refer to that value.
The printer might choose `%0` instead; that would not change the program's meaning.
These labels help a reader identify edges in the program's data-flow graph.
They are not an obligation to retain C local-variable names.

The parentheses immediately after the operation name list its **operands**: values it takes as input.
A constant has none, so it uses `()`.
A return takes the value to return, so it uses `(%eight)`.

The trailing type expression describes the operation's operand and result types.
For the constant, `() -> i32` means zero operands and one 32-bit integer result.
For the return, `(i32) -> ()` means one 32-bit operand and no produced SSA result.
A return transfers control; it does not need to create another value inside the function being left.

## Attributes are not extra runtime operands

The braces `{value = 8 : i64}` contain an **attribute**.
An attribute is information attached to the operation itself.
The constant's value is part of the program description, not another computed argument passed to it.

There are two widths on this line, and they answer different questions:

| Text | Meaning |
| --- | --- |
| `value = 8 : i64` | The integer attribute used to encode the literal in the IR has a 64-bit representation. |
| `() -> i32` | The program value produced by this operation is 32 bits wide. |

This is not a hidden widening conversion in the C program.
The operation's implementation validates and interprets the literal for its result type.
The distinction becomes even clearer with a symbolic constant:

```mlir
%size = "sela.constant"() {value = "pointer_bytes"} : () -> !sela.word
```

This real **operation excerpt** encodes a target property instead of an integer literal.
`!sela.word` is a Sela type whose width is selected for the target.
Chapter 05 explains that selection.
The attribute is descriptive information; `%size` is the resulting program value.

Other attributes describe which arithmetic operation to perform, whether a memory access is volatile, or which symbol a call addresses.
Calling all attributes “debug metadata” would be a serious mistake: many directly affect program behavior.
Conversely, arbitrary debug annotations are not accepted just because MLIR can store attributes.

## Reading the function wrapper

`"sela.func"() (...)` is an operation that **contains a region**, the body enclosed by the inner braces.
A region holds blocks of operations.
Our example has one unnamed entry block.
Later examples give blocks explicit labels.

The function operation itself has no runtime operands or SSA results, which explains its final `: () -> ()`.
That is not its callable signature.
Its callable signature is the `type = () -> i32` attribute: no arguments and an `i32` return value.
Distinguishing an operation's results from the function it describes avoids a common first-reading confusion.

The `id` identifies the function for calls and native symbol handling.
`declaration = false` means there is a body.
A declaration can name a function implemented elsewhere.
`variadic = false` rules out a variable argument tail.
`internal` and `dso_local` express different native linkage/resolution facts; they are not source-language tags.
Later linking chapters unpack them.

The `attributes = [[], []]` field is the Sela function's encoded native attribute list:
function attributes followed by return attributes, then parameter slots when parameters exist.
Here the slots are empty, not omitted by guesswork.
Do not remove bookkeeping from a real module merely because the function body seems obvious.
Validation checks the whole contract.

Finally, `sela.schema = 1 : i32` identifies the current module schema.
It is not a promise to accept historical formats indefinitely.
This project is pre-alpha and can change that contract together with its producers and consumers.

## A second body: calculation rather than a literal

The following is a **body excerpt** from the shape used in the IR tests:

```mlir
%a = "sela.constant"() {value = 1 : i64} : () -> i32
%b = "sela.constant"() {value = 2 : i64} : () -> i32
%sum = "sela.binary"(%a, %b) {opcode = "add", flags = 0 : i32} : (i32, i32) -> i32
"sela.return"(%sum) : (i32) -> ()
```

The data-flow chain is now explicit.
`%sum` depends on `%a` and `%b`; the return depends on `%sum`.
`sela.binary` is a family of operations selected by its `opcode` attribute.
The two operands and result have fixed-width types.
The `flags` field supplies additional arithmetic constraints; zero here does not assert extra overflow/exactness flags.
Chapter 16 explains why those flags must be preserved rather than treated as optimization hints that can be invented freely.

An optimizer may eventually return three directly.
Sela is still a useful representation before that optimization: it records enough meaning to make the transformation legitimate.
A printed instruction count is not the same as the final machine instruction count.

## How to read a larger dump

First locate function `id` and `type` attributes.
Find the body and its returns.
Then follow the values feeding those returns or observable stores/calls.
Only then expand the attributes and layout details relevant to that path.

For Hello World, the useful first question is “where is the call to `hello`?”
You do not need to master every function attribute before answering it.
However, ignoring an attribute while learning is different from deleting it while implementing a transformation.
The latter needs a semantic argument.

Textual IR is a view of a structured compiler object.
Our publication stores modules as bytecode.
A tool that can print the structure with unregistered dialects is useful for inspection, but it does not thereby know whether Sela's specific rules hold.
Chapter 08 separates viewing from validation explicitly.

## Optional paper exercise

In the calculation excerpt, rename `%a` to `%first` everywhere it appears.
Then change only the return operand from `%sum` to `%b`.
Which edit changes behavior?
You can answer without installing any tools.
The first changes a display label; the second changes the returned value from three to two.

## Recap

An operation has operands, results, types, and attributes; some operations also contain regions.
A function's callable signature is distinct from the results of the operation representing that function.
SSA names label values, while attributes describe parts of the contract.
Always distinguish complete inputs, body excerpts, and pseudocode when experimenting.

## Check your understanding

1. Why is the constant's `i64` attribute compatible with an `i32` result?
2. Does the function operation's final `() -> ()` mean `fixed_eight` returns nothing?
3. If an operation has a `sela.` name, must every `selac` understand it?

> [!faq]- Answers
>
> 1. They describe the attribute encoding and runtime result respectively. They are different layers, not two contradictory declarations of one runtime value.
> 2. No. The callable signature is the function's `type` attribute. The operation representing the function creates no SSA result in its surrounding module.
> 3. No. The current compiler has a closed admitted vocabulary and schema. New operations require implemented semantics and validation/lowering support.

## Source and evidence trail

- Independent producer (`tests/independent.cpp`): `helperModule` contains the real fixed-value and native-property examples.
- IR tests (`tests/ir.cpp`): `valid` contains the calculation shape, with a complete function wrapper.
- Dialect definitions (`include/sela/IR/Dialect.h`) name the operations and types; core validation (`src/ir/Compiler.cpp`) and native lowering (`src/ir/NativeLowering.cpp`) give them their admitted behavior.

[Next: Values, Control Flow, and Memory](04-values-control-flow-and-memory.md)
