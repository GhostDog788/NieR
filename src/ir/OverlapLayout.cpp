#include "OverlapLayout.h"
#include "llvm/IR/DerivedTypes.h"

namespace sela::detail {
llvm::Expected<llvm::Type *> selectOverlapCarrier(llvm::ArrayRef<llvm::Type *> alternatives,
                                                const llvm::DataLayout &layout) {
  auto fail = [](llvm::StringRef message) -> llvm::Error {
    return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), message);
  };
  if (alternatives.empty() || alternatives.size() > 64)
    return fail("overlapping storage needs 1 to 64 qualified alternatives");
  llvm::Type *carrier = nullptr;
  uint64_t maximumSize = 0, carrierSize = 0;
  llvm::Align maximumAlignment(1);
  for (auto *type : alternatives) {
    auto *pointer = llvm::dyn_cast_or_null<llvm::PointerType>(type);
    if (!type || !(type->isIntegerTy(8) || type->isIntegerTy(16) ||
        type->isIntegerTy(32) || type->isIntegerTy(64) || type->isFloatTy() ||
        type->isDoubleTy() || (pointer && pointer->getAddressSpace() == 0)))
      return fail("unqualified overlapping-storage alternative");
    auto alignment = layout.getABITypeAlign(type);
    uint64_t size = layout.getTypeAllocSize(type);
    maximumSize = std::max(maximumSize, size);
    if (!carrier || alignment > maximumAlignment || (alignment == maximumAlignment && size > carrierSize)) {
      carrier = type;
      carrierSize = size;
      maximumAlignment = alignment;
    }
  }
  // General padding/packed layouts require explicit additional proof. Do not
  // quietly change the maximum extent to the selected alignment carrier size.
  if (carrierSize != maximumSize)
    return fail("overlapping storage needs unqualified tail padding");
  return carrier;
}
}
