#pragma once
#include "sela/IR/Compiler.h"

namespace sela {
// Reconstruct one declared native translation unit from shared Sela fragments.
// No optimization runs here; the caller runs its one original per-TU pipeline
// only after this final Sela lowering has assembled the complete native unit.
llvm::Error lowerCompilationUnit(llvm::ArrayRef<llvm::StringRef> fragments,
                                llvm::StringRef profile,
                                llvm::StringRef llvmIROutput);
}
