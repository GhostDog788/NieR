#pragma once

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Error.h"
#include "llvm/ADT/StringRef.h"

namespace llvm { class Module; class AllocaInst; class Argument; class CallBase; class Function; class Instruction; }
namespace sela::detail {
struct NativeVarargs {
  llvm::DenseSet<const llvm::AllocaInst *> states;
  unsigned scalarExtractions = 0;
  // Each entry proves this particular native call operand is the ABI's
  // forwarding representation of a known cursor, not merely pointer-shaped.
  llvm::DenseMap<const llvm::CallBase *, llvm::DenseMap<unsigned, const llvm::AllocaInst *>> forwardedArguments;
  // Incoming cursors forward by identity except AAPCS64, where this entry
  // requires a proved implicit ABI copy, recreated by native va_forward.
  llvm::DenseSet<const llvm::Argument *> incomingArguments;
  llvm::DenseMap<const llvm::CallBase *, llvm::DenseMap<unsigned, const llvm::Argument *>> forwardedValues;
  llvm::DenseMap<const llvm::Function *, llvm::DenseSet<unsigned>> forwardedParameters;
  llvm::DenseSet<const llvm::Instruction *> forwardingScaffolding;
};

// Producer-only recognition of the pinned native ABI expansion. Each rewrite
// proves an entire bounded state transition, including all uses and effects.
llvm::Expected<NativeVarargs> normalizeNativeVarargs(llvm::Module &, llvm::StringRef targetID);
}
