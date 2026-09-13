#include "AggregateABI.h"
#include "NativeTargets.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>

namespace sela::detail {
namespace {
llvm::Error failure(const llvm::Twine &message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
      "native aggregate ABI: " + message);
}

bool scalar(llvm::Type *type, bool field = false) {
  if (auto *pointer = llvm::dyn_cast<llvm::PointerType>(type))
    return pointer->getAddressSpace() == 0;
  return type->isFloatTy() || type->isDoubleTy() ||
         (!field && type->isIntegerTy(1)) || type->isIntegerTy(8) ||
         type->isIntegerTy(16) || type->isIntegerTy(32) || type->isIntegerTy(64);
}

bool nativePieceScalar(llvm::Type *type) {
  if (scalar(type)) return true;
  auto *integer = llvm::dyn_cast<llvm::IntegerType>(type);
  return integer && integer->getBitWidth() <= 64 && integer->getBitWidth() % 8 == 0;
}

llvm::Error validateAccess(llvm::IRBuilderBase &builder, const NativeABIValue &value,
                          llvm::Value *storage, llvm::Align alignment) {
  auto *block = builder.GetInsertBlock();
  if (!block || !block->getModule() || !storage ||
      !storage->getType()->isPointerTy() || storage->getType()->getPointerAddressSpace() != 0)
    return failure("piece access requires an insertion module and ordinary storage pointer");
  if (&storage->getContext() != &builder.getContext())
    return failure("piece storage belongs to another LLVM context");
  if (!value.storageType || &value.storageType->getContext() != &builder.getContext() ||
      !value.storageType->isSized() || value.kind == NativeABIKind::Indirect ||
      value.pieces.empty() || alignment.value() > llvm::Value::MaximumAlignment)
    return failure("invalid direct piece descriptor or storage alignment");
  auto &layout = block->getModule()->getDataLayout();
  if (layout.isDefault() || layout.getTypeAllocSize(value.storageType) != value.storageSize)
    return failure("piece storage disagrees with the insertion module data layout");
  uint64_t end = 0;
  for (auto piece : value.pieces) {
    bool vector = piece.type && llvm::isa<llvm::FixedVectorType>(piece.type) &&
        llvm::cast<llvm::FixedVectorType>(piece.type)->getNumElements() == 2 &&
        llvm::cast<llvm::FixedVectorType>(piece.type)->getElementType()->isFloatTy();
    if (!piece.type || &piece.type->getContext() != &builder.getContext() ||
        (!nativePieceScalar(piece.type) && !vector))
      return failure("invalid native scalar piece type");
    uint64_t size = layout.getTypeStoreSize(piece.type);
    if (piece.offset < end || piece.offset > value.storageSize || size > value.storageSize - piece.offset)
      return failure("overlapping or out-of-bounds native pieces");
    end = piece.offset + size;
  }
  return llvm::Error::success();
}

llvm::Value *address(llvm::IRBuilderBase &builder, llvm::Value *storage, uint64_t offset) {
  if (!offset) return storage;
  auto &layout = builder.GetInsertBlock()->getModule()->getDataLayout();
  auto *indexType = layout.getIndexType(storage->getType());
  return builder.CreateInBoundsGEP(builder.getInt8Ty(), storage,
      llvm::ConstantInt::get(indexType, offset), "abi.piece");
}
} // namespace

llvm::Expected<llvm::DataLayout> nativeABIDataLayout(bool x64) {
  auto *target = findNativeTarget(x64 ? "x86_64" : "i686");
  if (!target) return failure("requested native target is unavailable in this Sela library");
  return llvm::DataLayout(target->layout);
}

llvm::Expected<NativeABISignature> classifyNativeABI(
    llvm::FunctionType *logical, bool x64,
    llvm::ArrayRef<llvm::StructType *> orderedRecords) {
  auto *target = findNativeTarget(x64 ? "x86_64" : "i686");
  if (!target) return failure("requested native target is unavailable in this Sela library");
  return target->classify(logical, orderedRecords);
}

llvm::Expected<NativeABISignature> classifyNativeLayoutABI(
    llvm::FunctionType *logical, bool x64,
    llvm::ArrayRef<NativeABIRecordLayout> records) {
  auto *target = findNativeTarget(x64 ? "x86_64" : "i686");
  if (!target) return failure("requested native target is unavailable in this Sela library");
  return target->classifyLayout(logical, records);
}

llvm::Expected<llvm::SmallVector<llvm::Value *, 2>> loadNativeABIPieces(
    llvm::IRBuilderBase &builder, const NativeABIValue &value,
    llvm::Value *storage, llvm::Align baseAlignment) {
  if (auto error = validateAccess(builder, value, storage, baseAlignment)) return std::move(error);
  llvm::SmallVector<llvm::Value *, 2> result;
  for (auto piece : value.pieces)
    result.push_back(builder.CreateAlignedLoad(piece.type, address(builder, storage, piece.offset),
        llvm::commonAlignment(baseAlignment, piece.offset), "abi.load"));
  return result;
}

llvm::Error storeNativeABIPieces(
    llvm::IRBuilderBase &builder, const NativeABIValue &value,
    llvm::Value *storage, llvm::Align baseAlignment,
    llvm::ArrayRef<llvm::Value *> pieces) {
  if (auto error = validateAccess(builder, value, storage, baseAlignment)) return error;
  if (pieces.size() != value.pieces.size()) return failure("wrong native piece count");
  for (unsigned i = 0; i < pieces.size(); ++i)
    if (!pieces[i] || pieces[i]->getType() != value.pieces[i].type)
      return failure("wrong native piece value type");
  for (unsigned i = 0; i < pieces.size(); ++i) {
    auto offset = value.pieces[i].offset;
    builder.CreateAlignedStore(pieces[i], address(builder, storage, offset),
        llvm::commonAlignment(baseAlignment, offset));
  }
  return llvm::Error::success();
}
} // namespace sela::detail
