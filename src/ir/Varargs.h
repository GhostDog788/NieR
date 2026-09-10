#pragma once

#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Error.h"

namespace llvm { class Module; class AllocaInst; }
namespace nier::detail {
struct NativeVarargs {
  llvm::DenseSet<const llvm::AllocaInst *> states;
  unsigned scalarExtractions = 0;
};

// Producer-only recognition of the pinned native ABI expansion. Each rewrite
// proves an entire bounded state transition, including all uses and effects.
llvm::Expected<NativeVarargs> normalizeNativeVarargs(llvm::Module &, bool word64);
}
