#pragma once
#include "mlir/IR/Operation.h"
#include "llvm/Support/Error.h"

namespace sela::detail {
// Target-independent validation, including foreign alternatives. Run before
// native LLVM APIs (some assume their input signatures are already valid).
llvm::Error validateExtendedInstruction(mlir::Operation &operation);
llvm::Error validateLoopOption(mlir::Attribute option);
}
