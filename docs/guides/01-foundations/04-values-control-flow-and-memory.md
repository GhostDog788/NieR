# 04. Values, Control Flow, and Memory

[Series](../README.md) · [Previous](03-reading-a-nier-program.md) · [Next](05-architecture-neutral-meaning.md)

## What you will understand

You will follow a Nier program across branches and loops, distinguish SSA
values from mutable memory, and understand why valid control flow is a
compiler obligation. Read [chapter 03](03-reading-a-nier-program.md) first.
All examples are readable without a build; fragments are labeled explicitly.

## A C variable can hide several values

Consider this C fragment:

```c
unsigned x = 1;
x = x + 2;
return x;
```

The name `x` refers to different values at different points. A compiler can
represent this calculation using two distinct values: the original one and
the sum. In **static single assignment**, or SSA, each value has one defining
operation or is a block argument. Uses refer to that definition explicitly.

This is a representation of data flow, not a restriction preventing C
assignments. The compiler translates the source's changing variables into
explicit relationships. It can then ask whether a definition is used, what
depends on it, or whether a replacement preserves all uses.

Nier uses this value model. It does not require each value to occupy a unique
machine register. Register allocation happens later; the backend may combine,
spill, or eliminate values while preserving behavior.

## Why branches need blocks

A **basic block** is a sequence of operations with a defined control-flow exit.
A branch transfers control to another block. The blocks and their possible
transfers form a **control-flow graph**, or CFG.

Here is a **real Nier function-body excerpt**, following `validCFG` in the IR
tests. The containing module and function attributes are omitted:

```mlir
%condition = "nier.constant"() {value = 1 : i64} : () -> i1
"nier.cond_br"(%condition)[^yes, ^no] {true_count = 0 : i32} : (i1) -> ()
^yes:
  %left = "nier.constant"() {value = 42 : i64} : () -> i32
  "nier.br"(%left)[^join] : (i32) -> ()
^no:
  %right = "nier.constant"() {value = 7 : i64} : () -> i32
  "nier.br"(%right)[^join] : (i32) -> ()
^join(%result : i32):
  "nier.return"(%result) : (i32) -> ()
```

`i1` is a one-bit condition type. `^yes`, `^no`, and `^join` are block labels.
The square brackets identify branch destinations. The first branch chooses
between `^yes` and `^no`. Each of those blocks passes a different value to the
same joining block:

```text
               condition
              /         \
       yes: value 42   no: value 7
              \         /
              join(result)
                   ↓
              return result
```

`%result` is a **block argument**. Entering the block binds it to the value
supplied by the selected predecessor edge. It is not a variable written by
both branches in shared memory. In LLVM IR you will encounter **PHI nodes**
for the analogous incoming-edge selection; MLIR block arguments make that
relationship part of the destination block's interface.

The condition is constant in this teaching fixture, so this execution returns
42. Keeping the other block in an unoptimized example makes the graph visible.
An optimizer may later remove the unused path. The representation still has
to be structurally and semantically valid before that optimization.

## The branch interface is a type contract

In `nier.cond_br`, operands after the condition can carry values to the two
destinations. `true_count` says how many belong to the true edge; the rest
belong to the false edge. In the example it is zero because neither `^yes`
nor `^no` takes arguments. The unconditional branches pass one `i32` to
`^join`, matching its one `i32` argument.

A compiler cannot redirect an edge to a block with incompatible arguments and
hope code generation repairs it. The graph includes both control transfer and
data transfer. A change to either must preserve the other.

This matters when a target-specific branch disappears during specialization.
Removing a block is not just deleting its label and instructions. Its uses,
successors, argument positions, and the paths that make values available all
need to remain coherent.

## Dominance: a definition must be available on the path

Suppose the join block above returned `%left` directly. On the `^no` path,
`%left` was never defined. A value does not become usable merely because its
definition appears earlier in the printed file.

A block **dominates** another block when every path from the entry to the
second passes through the first. Within a block, an ordinary definition must
also precede its use. These ideas make availability precise. `%left` does not
dominate the join, whereas the join's own `%result` argument is available
there by construction.

For the concrete constant-condition fixture, one could prove the other edge
unreachable and rewrite the whole graph. That is different from accepting an
invalid graph on the assumption that an optimizer will eventually fix it.
Maintainer chapter 17 returns to this distinction for target-conditioned IR.

## Loops carry values around edges

SSA does not forbid loops. A loop header can take a block argument on its
initial entry and a new value on each back edge. This **pseudocode** shows the
shape of the actual `validLoop` fixture without its attribute syntax:

```text
entry:
    branch loop(0)
loop(index):
    next = index + 1
    if next < 5:
        branch loop(next)
    else:
        branch done(next)
done(result):
    return result
```

The initial edge supplies zero. The first iteration computes one, and its
back edge supplies that value to the next iteration. The loop returns five.
There is one static definition for the loop argument and one for `next`, even
though execution visits them repeatedly with different dynamic values.

Loop-related compiler metadata can also carry relationships and identity.
Preserving the same integer trip count is not automatically sufficient to
preserve every optimization-relevant loop fact. The repository has a separate
loop-identity test for this reason; chapter 17 explains the implementation.

## Mutable memory still exists

SSA values do not make C memory immutable. A pointer can identify storage,
and a store can change the bytes in that storage. The pointer value itself
does not have to change.

This is a **Nier body excerpt** showing the distinction:

```mlir
%slot = "nier.alloca"() {element = i32, alignment = 4 : i64} : () -> !nier.ptr
%value = "nier.constant"() {value = 42 : i64} : () -> i32
"nier.store"(%value, %slot) {alignment = 4 : i64, volatile = false} : (i32, !nier.ptr) -> ()
%loaded = "nier.load"(%slot) {alignment = 4 : i64, volatile = false} : (!nier.ptr) -> i32
"nier.return"(%loaded) : (i32) -> ()
```

The allocation produces `%slot`, a pointer to local storage for an `i32`.
The store writes 42. The load reads an `i32` from that address. `%value` and
`%loaded` are distinct SSA definitions even though this example reads back
the same number. `%slot` was defined once; the pointed-to memory changed.

`!nier.ptr` is a pointer type, not an integer that you may freely replace with
a fixed-width address. The allocation's `element` describes its storage.
The load/store value types describe the access. The alignment attributes are
part of the access contract, not decorative formatting.

`nier.gep` computes an address using an element/layout description and indices.
It does not load the object at that address. This is the same conceptual
difference you know between `&array[i]` and `array[i]`, but the IR exposes the
addressing calculation as its own operation. Chapter 18 expands the layout
rules and why a fixed byte offset is not always portable.

## Observable effects constrain transformations

A compiler may eliminate local storage in the simple example if it can prove
the replacement preserves behavior. It cannot apply that rule blindly when
addresses escape, other accesses can alias the same memory, or volatility
changes what must remain observable. A native function call may also read or
write memory and affect control flow.

In IR, “these operations calculate the same integer” is therefore weaker than
“these program fragments are interchangeable.” Correctness includes memory,
effects, ordering requirements, and whether every value is available on the
paths where it is used. Native-specific annotations that communicate these
facts must survive the appropriate transformations.

## Recap

SSA makes definitions and uses explicit. Block arguments make incoming
edge-dependent values explicit. CFGs make possible execution paths explicit.
Dominance ties data availability to those paths. None of this removes mutable
memory or licenses a compiler to ignore its effects.

## Check your understanding

1. Why can the join return `%result` but not generally `%left`?
2. Does a loop violate single assignment when its body executes many times?
3. Does `nier.gep` read memory? Does `nier.store` redefine its pointer operand?

<details>
<summary>Answers</summary>

1. Every incoming edge supplies the block argument. `%left` is defined only along the yes path and is not available on the other path.
2. No. SSA refers to static definitions in the representation. Dynamic executions can revisit the same definition with different incoming values.
3. Address calculation does not itself load the addressed value. A store changes memory; it does not create a second definition of the existing pointer value.

</details>

## Source and evidence trail

- [IR tests](../../../tests/ir.cpp): read `validCFG`, `validLoop`, and storage examples before the associated implementation.
- [Core lowering](../../../src/ir/Compiler.cpp): the value/block maps translate graph relationships, not just printed opcode names.
- [Conditional specialization](../../../src/ir/ConditionalSpecialization.cpp) is the later worked example of removing target-inactive control flow while preserving the graph.

[Next: Architecture-Neutral Meaning](05-architecture-neutral-meaning.md)
