#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/Error.h"

namespace nier::detail {
// Current qualified overlapping storage is natural, scalar and non-bitfield.
// The native LLVM carrier is a layout choice, never an ordered-record ABI
// assertion. Consumers use semantic alternatives when classifying native calls.
llvm::Expected<llvm::Type *> selectOverlapCarrier(llvm::ArrayRef<llvm::Type *> alternatives,
                                                const llvm::DataLayout &layout);
}
