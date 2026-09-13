#pragma once

#include "AggregateABI.h"
#include "Internal.h"

namespace sela::detail {
struct NativeTargetBackend {
  llvm::StringRef id;
  llvm::StringRef layout;
  llvm::Expected<std::unique_ptr<llvm::Module>> (*lower)(
      mlir::ModuleOp, llvm::LLVMContext &, NativeABIInverseHints *);
  llvm::Expected<NativeABISignature> (*classify)(
      llvm::FunctionType *, llvm::ArrayRef<llvm::StructType *>);
  llvm::Expected<NativeABISignature> (*classifyLayout)(
      llvm::FunctionType *, llvm::ArrayRef<NativeABIRecordLayout>);
};

// Null means unavailable in this linked library, not an invalid public domain.
const NativeTargetBackend *findNativeTarget(llvm::StringRef id);

} // namespace sela::detail
