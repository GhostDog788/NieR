#pragma once
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Intrinsics.h"

namespace sela::ir {
enum class IntrinsicOverload { None, Result, FirstOperand };
enum class IntrinsicEffects { Pure, Assumption, Prefetch };
struct IntrinsicSpec {
  llvm::StringRef name;
  llvm::Intrinsic::ID llvmID;
  IntrinsicOverload overload;
  unsigned operands;
  IntrinsicEffects effects;
  llvm::StringRef backend;
};
llvm::ArrayRef<IntrinsicSpec> intrinsicRegistry();
const IntrinsicSpec *findIntrinsic(llvm::StringRef name);
const IntrinsicSpec *findIntrinsic(llvm::Intrinsic::ID id);
} // namespace sela::ir
