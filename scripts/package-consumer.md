# Nier compiler-only distribution prototype

This pre-alpha bundle runs on **x86-64 Ubuntu 24.04**. It includes independent
`bin/nierc`, unmodified LLVM 18 `opt`, `llc`, `ld.lld`, and `llvm-ar`, their
runtime library closure, and the current managed x86-64 native link/runtime
SDK and compiler-rt CRT/builtins. Clang, frontend plugins, publication tools,
headers, CMake and the i686 sysroot are not included.

To assemble it from a current Release build and matching extracted SDK:

```sh
bash scripts/package-consumer.sh build/prealpha /absolute/new/bundle .sdk
```

The output directory must not already exist. Assembly requires the development
SDK plus ordinary Bash/coreutils, `rg`, and `readelf`; those assembly utilities
are not required to run the installed compiler. The relocation test additionally
uses `strace`, `nm`, and `ar` from the development host.

Move the entire directory anywhere before compiling:

```sh
./bin/nierc application.nier -o application
./application
```

No environment setup, compiler wrapper, patched LLVM binary, `patchelf`, or
native application ELF rewrite is required. `nierc` locates its sibling `sdk`
directory. An explicit `--sdk` or `NIER_SDK_ROOT` overrides that discovery.
Only its LLVM subprocesses receive the bundle's runtime library search path;
the native application runs normally without that environment.

The compiler/tool host glibc and dynamic loader remain the Ubuntu 24.04
baseline. Other compiler dependencies come from the bundled SDK libraries.
Native application output uses the bundle's managed glibc/runtime paths as
they existed when it was compiled. This prototype does not relocate already
compiled output when its runtime directory moves. Keep that directory in place;
production runtime installation and lifecycle management remain future work.

`sdk/packages.lock` and `sdk/sdk-lock.sha256` record the development SDK
contract; `payload.sha256` records copied file contents. These receipts are
not signatures or tamper-resistant security enforcement. Ubuntu package
copyright notices for included components are retained under `licenses`.
Release license/source-offer review remains a distribution-release task.

This is not a minimum-footprint SDK: the stock monolithic LLVM library retains
unused backends, and the native x86-64 library set is intentionally preserved.
Removing publication components does not impose an additional language subset
on Nier input. The compiler's existing semantic and native-target qualification
limits still apply; this prototype does not claim every C ABI construct is
already supported.
