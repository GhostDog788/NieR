#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/Error.h"

namespace nier::detail {
// Select semantic word-domain CFG fragments on a private NieR copy, before
// target LLVM emission. No source-language or LLVM-capture inputs are used.
llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>> specializeConditionalCFG(
    mlir::ModuleOp source, bool word64);
}
