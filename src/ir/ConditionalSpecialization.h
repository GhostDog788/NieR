#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/Error.h"

namespace sela::detail {
// Select semantic target-domain CFG fragments on a private Sela copy, before
// target LLVM emission. No source-language or LLVM-capture inputs are used.
llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>> specializeConditionalCFG(
    mlir::ModuleOp source, llvm::StringRef target);
}
