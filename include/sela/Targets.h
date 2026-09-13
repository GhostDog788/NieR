#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace sela::targets {
// Metadata is generated from sdk/targets.json. Build-host identity is separate
// from this device contract; equal word sizes do not imply equal architectures.
struct TargetInfo {
  llvm::StringRef id, triple, sysrootTriple, layout, multiarch, gccTriple;
  llvm::StringRef llvmBackend, abi, cpu, features, floatABI, byteOrder;
  llvm::StringRef loader, compilerRTArch, lldEmulation, packageArch, qemuUser;
  unsigned wordBits, elfClass, elfMachine;
  bool plainCharSigned;
};

llvm::ArrayRef<TargetInfo> all();
const TargetInfo *find(llvm::StringRef id);
// Target policy flags only; callers also pass --target=<triple> and their
// target sysroot. Never infer flags from pointer width or the build host.
llvm::ArrayRef<llvm::StringRef> clangArgs(const TargetInfo &target);
// Application publication policy, additional to the ISA/ABI flags above.
// It is not a build policy for the stock LLVM tool implementation itself.
llvm::ArrayRef<llvm::StringRef> publicationArgs(const TargetInfo &target);
} // namespace sela::targets
