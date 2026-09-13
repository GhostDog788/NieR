#pragma once

#include "AggregateABI.h"
#include "Internal.h"

namespace nier::detail {
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

#define NIER_DECLARE_NATIVE_BACKEND(NS) \
namespace NS { \
const NativeTargetBackend &backend(); \
llvm::Expected<std::unique_ptr<llvm::Module>> lowerModule( \
    mlir::ModuleOp, llvm::LLVMContext &, NativeABIInverseHints *); \
llvm::Expected<NativeABISignature> classifyNativeABI( \
    llvm::FunctionType *, llvm::ArrayRef<llvm::StructType *>); \
llvm::Expected<NativeABISignature> classifyNativeLayoutABI( \
    llvm::FunctionType *, llvm::ArrayRef<NativeABIRecordLayout>); \
}
NIER_DECLARE_NATIVE_BACKEND(native64)
NIER_DECLARE_NATIVE_BACKEND(native32)
#undef NIER_DECLARE_NATIVE_BACKEND
} // namespace nier::detail
