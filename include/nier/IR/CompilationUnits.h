#pragma once
#include "nier/IR/Compiler.h"

namespace nier {
// Reconstruct one declared native translation unit from shared Nier fragments.
// No optimization runs here; the caller runs its one original per-TU pipeline
// only after this final Nier lowering has assembled the complete native unit.
llvm::Error lowerCompilationUnit(llvm::ArrayRef<llvm::StringRef> fragments,
                                llvm::StringRef profile,
                                llvm::StringRef llvmIROutput);
}
