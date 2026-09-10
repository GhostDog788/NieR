#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include <memory>

namespace nier::detail {
struct NativeABIInverseHints;
// Shared target lowering, deliberately independent of LLVM capture/merger inputs.
llvm::Expected<std::unique_ptr<llvm::Module>> lowerModule(
    mlir::ModuleOp source, llvm::LLVMContext &context, bool x64,
    NativeABIInverseHints *inverseHints = nullptr);
} // namespace nier::detail
