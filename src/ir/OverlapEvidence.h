#pragma once

#include "nier/IR/Overlap.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include <string>
#include <vector>

namespace nier::detail {
struct OverlapAlternative {
  std::string privateName;
  llvm::Type *type;
};
struct NativeOverlap {
  std::vector<OverlapAlternative> alternatives;
};
using NativeOverlaps = llvm::DenseMap<llvm::StructType *, NativeOverlap>;

// Publisher-only debug association. Debug names never enter Nier code.
llvm::Expected<NativeOverlaps> discoverNativeOverlaps(llvm::Module &module);
llvm::Expected<ir::OverlapType> mergeNativeOverlap(
    const NativeOverlap &left, const NativeOverlap &right,
    mlir::MLIRContext &context, llvm::StringRef identity,
    llvm::function_ref<mlir::Type(llvm::Type *, llvm::Type *)> mergeType);
}
