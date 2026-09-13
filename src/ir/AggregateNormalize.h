#pragma once

#include "NativeABIBridge.h"
#include "llvm/ADT/DenseMap.h"

namespace llvm { class Function; class Module; class Value; class Instruction; class ReturnInst; class CallInst; }
namespace sela::detail {

// Source-side debug data proposes these storage anchors; complete instruction
// proofs below are mandatory. Consumer inverse anchors are generated directly
// from generic native ABI semantics and contain no language/debug data.
using LogicalAggregateABI = NativeAggregateDefinition;

llvm::Expected<llvm::SmallVector<LogicalAggregateABI, 8>> discoverAggregateABIs(
    llvm::Module &, bool x64);

struct AggregateDefinitionProof {
  LogicalAggregateABI abi;
  llvm::FunctionType *bodyType = nullptr;
  llvm::Value *resultStorage = nullptr;
  llvm::SmallVector<llvm::Instruction *, 16> entryShims;
  llvm::SmallVector<llvm::Instruction *, 8> resultShims;
  llvm::SmallVector<llvm::ReturnInst *, 4> returns;
};

// Proves the complete native entry/return coercion graph without mutation.
// Addresses proposed by debug information are not accepted until all native
// pieces, offsets, alignments, uses and ordering have been checked.
llvm::Expected<AggregateDefinitionProof> proveAggregateDefinition(
    LogicalAggregateABI abi);

// Private producer normalization. All definitions are discovered/proved before
// any body changes. Returned descriptors retain the native ABI contract while
// function points at its storage-argument body. Native calls are a separate
// proof/rewrite stage and must be normalized before LLVM/public IR validation.
llvm::Expected<llvm::SmallVector<LogicalAggregateABI, 8>> normalizeAggregateDefinitions(
    llvm::Module &, bool x64);

struct AggregateCallProof {
  llvm::CallInst *call = nullptr;
  llvm::FunctionType *logicalType = nullptr;
  NativeABISignature native;
  llvm::SmallVector<llvm::StructType *, 8> orderedRecords;
  llvm::Value *resultStorage = nullptr;
  llvm::SmallVector<llvm::Value *, 8> arguments;
  llvm::SmallVector<llvm::Instruction *, 16> shims;
};
llvm::Expected<llvm::SmallVector<AggregateCallProof, 8>> proveAggregateCalls(
    llvm::Module &, bool x64);
llvm::Expected<llvm::SmallVector<AggregateCallProof, 8>> proveAggregateCalls(
    llvm::Module &, bool x64, llvm::ArrayRef<llvm::StructType *> orderedRecords);
struct NormalizedAggregateModule {
  llvm::SmallVector<LogicalAggregateABI, 8> functions;
  llvm::DenseMap<llvm::CallInst *, llvm::FunctionType *> calls;
};
llvm::Expected<NormalizedAggregateModule> normalizeNativeAggregates(
    llvm::Module &, bool x64, const NativeABIInverseHints *inverseHints = nullptr);
}
