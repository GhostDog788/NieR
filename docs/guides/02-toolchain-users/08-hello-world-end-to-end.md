# 08 — Hello World, from C to a native executable

[Series](../README.md) · [Previous: Development environment](07-development-environment.md) · [Next: Beyond Hello World](09-beyond-hello-world.md)

## Objective and prerequisites

This chapter follows one small program across the complete implemented boundary: C source, a standalone Nier artifact, destination compilation, and ordinary native execution.
You should already know how to compile a C program and have a working SDK plus publisher build in `build/prealpha`.
The lab is independent of every earlier lab and writes only to a newly created scratch directory.

We use one machine for convenience. It performs the publisher and destination roles at different steps.
That does not make publication and consumption the same program: `clang` and `nierc` have different inputs and responsibilities.

## Start with a genuinely normal C program

The starter in `examples/hello/hello/` is a normal Make/CMake project.
Its program has two C source files and one header. `examples/hello/hello/main.c` includes `examples/hello/hello/hello.h`, calls `hello()`, and returns zero.
`examples/hello/hello/hello.c` includes `<stdio.h>` and defines:

```c
void hello(void) {
    printf("Hello world\n");
}
```

There are no Nier application APIs, special entry points, or runtime registration calls.
The header declares a function shared between the two C files. It is not compiled independently.
Each C file, together with the headers it includes, forms a **translation unit**: one source unit processed by the C frontend.

That distinction will matter later. A translation unit is an optimization and symbol-visibility boundary, not just an arbitrary packaging choice.
Publishing two units does not authorize combining the program into one large optimization unit with different behavior.

For the hands-on project route, follow the [Hello project walkthrough](hello-project-walkthrough.md).
This chapter instead follows the starter's C files directly to explain what happens at each publication and compilation boundary.

## The three commands that define the workflow

The publication command is actual stock `clang`, configured with the generated `nier.cfg`. The configuration loads the Nier frontend adapter and selects the internal publication linker.
The developer still supplies normal C source paths and a normal `-o` output path.

The output named `hello.nier` is an archive, not an executable. It contains the Nier representation and public metadata needed to compile the output.
Its metadata declares target constraints, modules, optimization settings, and native dependencies. It does not point back to this example's source files.

The second command, `nierc hello.nier -o hello`, specializes Nier code for the destination, optimizes the resulting LLVM IR, generates native objects, and links them.
**Specialization** means resolving an abstract choice—such as native word size—using the destination's supported target contract.
It is not a second C compilation. There is no C source input at this stage.

Finally, executing `hello` asks the operating system to run an ordinary native program. The Nier compiler does not remain resident.
The publication artifact is not interpreted during execution. The application still uses normal native runtime components such as the supplied glibc and its stock loader.

## Optional lab: publish, inspect, compile, run

Run the whole block in Bash from the repository root.
Keep the printed scratch path if you want to inspect the files afterward.

```bash
source sdk/env.sh
guide_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide08-XXXXXX")
guide_config="$PWD/build/prealpha/nier.cfg"

clang --config="$guide_config" -O2 \
  examples/hello/hello/main.c examples/hello/hello/hello.c \
  -o "$guide_work/hello.nier"

build/prealpha/nierc inspect "$guide_work/hello.nier"
tar -tf "$guide_work/hello.nier"
tar -xOf "$guide_work/hello.nier" manifest.json

build/prealpha/nierc "$guide_work/hello.nier" -o "$guide_work/hello"
env -u LD_LIBRARY_PATH "$guide_work/hello"
readelf -h "$guide_work/hello"
readelf -l "$guide_work/hello"

build/prealpha/nierc lower "$guide_work/hello.nier" \
  --target x86_64 --output-dir "$guide_work/lowered"
opt -passes=verify -disable-output "$guide_work/lowered/0.ll"

tar -xOf "$guide_work/hello.nier" modules/0.nierbc \
  | mlir-opt --allow-unregistered-dialect -

mv "$guide_work/hello.nier" "$guide_work/hello.offline.nier"
env -u LD_LIBRARY_PATH "$guide_work/hello"
printf 'Hello World workspace: %s\n' "$guide_work"
```

The application prints `Hello world` on both executions. The manifest and inspection output are diagnostic information, not application output.
The tar listing contains `manifest.json` and two bytecode modules for this example.

`readelf -h` shows an ELF native file for x86-64.
`readelf -l` shows program segments, including the interpreter pathname used by the kernel to start the dynamic loader.
The current development link selects the supplied SDK loader and runtime paths. Those files must remain available when the application runs.

We clear `LD_LIBRARY_PATH` for the application because the sourced development environment sets it for compiler tools.
Leaving it set could make the loader search tool-library directories before the application's intended runtime locations.
Clearing it does not make the application static or eliminate its native dependencies.

The `lower` command exposes diagnostic LLVM files without running the native backend.
`opt -passes=verify` checks LLVM's structural rules for one such file. That is a useful additional check, not a replacement for Nier's own validation.

The final pipeline prints the bytecode through stock MLIR tooling. Its `--allow-unregistered-dialect` option is essential because this stock `mlir-opt` does not register Nier's dialect.
It can display the representation; it does **not** validate Nier's semantic contract.
Use `nierc inspect` for that public validation path.
Being able to print an operation is not proof that its meaning is supported.

Moving the artifact before the second execution demonstrates a narrow but important fact: the installed native executable does not reopen that artifact to run.
It does not prove the complete deployment requirements, performance limits, or future security properties.

## The same flow with separate compilation

Many real Makefiles compile one source file at a time.
Nier's stock-Clang mode supports that shape. Here is a separate, independently runnable lab:

```bash
source sdk/env.sh
guide_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-guide08-separate-XXXXXX")
guide_config="$PWD/build/prealpha/nier.cfg"

clang --config="$guide_config" -O2 -c examples/hello/hello/main.c \
  -o "$guide_work/main.o"
clang --config="$guide_config" -O2 -c examples/hello/hello/hello.c \
  -o "$guide_work/hello.o"
tar -tf "$guide_work/main.o"

clang --config="$guide_config" "$guide_work/main.o" "$guide_work/hello.o" \
  -o "$guide_work/separate.nier"
build/prealpha/nierc "$guide_work/separate.nier" \
  -o "$guide_work/separate"
env -u LD_LIBRARY_PATH "$guide_work/separate"
printf 'Separate-compilation workspace: %s\n' "$guide_work"
```

The `.o` suffix here is a build convention. These particular `.o` files contain relocatable **Nier object artifacts**, not native ELF object code.
They are inputs to the publication link step.
`nierc` deliberately rejects an object-kind artifact as a complete native output request: first finish publication linking through Clang.
The final archive embeds the required modules, not references to the intermediate `.o` filenames.

This does not mean every existing build works by globally replacing `CC` with this configuration. A configure probe may immediately execute its compiler output.
A Nier archive cannot serve as that native probe. The next chapter introduces the SDK integration that keeps such build-time programs native.

## What a failure tells you

A source error belongs to Clang. An unsupported neutral transformation belongs to the producer.
A malformed artifact, unsupported target, or invalid Nier operation belongs to the consumer's validation path. A missing declared native library belongs to dependency resolution or linking.
Distinguishing these stages makes a failure actionable.

Never reuse a source or input pathname as `-o`. The internal writer and consumer have alias checks, but a direct stock-Clang driver retains its own failed-job cleanup and may delete its requested output after an error.
Fresh scratch outputs avoid turning an experiment into a data-loss problem. The SDK coordinator adds staging around final publication; it does not change stock Clang's general cleanup semantics.

## Recap and check your understanding

The result is ordinary native execution reached through an independent publication format.
Nothing in the C example knows it is being published. The successful experiment establishes this flow for the current qualified environment, not for every C program or CPU.

<details>
<summary>Why are there two modules although the program uses two C files and a header?</summary>

There are two C translation units. The header contributes declarations to each unit that includes it; it is not a separately compiled unit.

</details>

<details>
<summary>Can the output from clang's publication command be executed directly?</summary>

No. It is a Nier archive.
The separate `nierc` step creates native output, which the OS can execute using the selected native runtime.

</details>

<details>
<summary>Why is stock mlir-opt not the artifact validator?</summary>

It can decode generic MLIR structure with unknown dialects allowed, but it does not know Nier's allowed operations, attributes, privacy rules, or target semantics.
The Nier consumer performs those checks.

</details>

## Guided reading

Read `tests/hello.sh` after this lab: it automates the same flow and adds output equivalence, missing-SDK, unsupported-input, and privacy checks.
The generated configuration comes from `CMakeLists.txt`.
Follow `src/publisher/LinkMain.cpp` for publication linking
and `src/consumer/Main.cpp` for the separate native compiler.
Later chapters explain those files rather than assuming they are already familiar.
