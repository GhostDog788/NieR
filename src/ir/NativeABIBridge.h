#pragma once

#include "AggregateABI.h"
#include "llvm/ADT/DenseMap.h"

namespace llvm { class Function; class CallInst; class Value; }
namespace sela::detail {
struct NativeAggregateDefinition {
  llvm::Function *function = nullptr;
  llvm::FunctionType *logicalType = nullptr;
  NativeABISignature native;
  llvm::SmallVector<llvm::StructType *, 8> orderedRecords;
  llvm::DenseMap<unsigned, llvm::Value *> argumentStorage;
};
struct NativeABIInverseHints {
  llvm::SmallVector<NativeAggregateDefinition, 8> definitions;
  llvm::SmallVector<llvm::StructType *, 8> orderedRecords;
};

llvm::FunctionType *nativeStorageBodyType(llvm::FunctionType *logical);

// Materialize a qualified native ABI inside the original function/call, using
// existing owned-storage body arguments. There is no thunk, boxed argument,
// language runtime or auxiliary call. The optional hints expose only newly
// generated storage anchors for the producer's independent inverse proof.
llvm::Expected<llvm::Function *> materializeNativeAggregateDefinition(
    llvm::Function &body, llvm::FunctionType *logical,
    const NativeABISignature &, llvm::ArrayRef<llvm::StructType *> orderedRecords,
    NativeABIInverseHints *hints = nullptr);
llvm::Expected<llvm::CallInst *> materializeNativeAggregateCall(
    llvm::CallInst &bodyCall, llvm::FunctionType *logical,
    const NativeABISignature &);
}
