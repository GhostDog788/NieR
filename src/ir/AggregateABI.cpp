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
  return integer && integer->getBitWidth() <= 128 && integer->getBitWidth() % 8 == 0;
}

bool validPieceType(llvm::Type *type, unsigned depth = 0) {
  if (!type || depth > 32 || !type->isSized()) return false;
  if (nativePieceScalar(type)) return true;
  if (auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(type))
    return vector->getNumElements() == 2 && vector->getElementType()->isFloatTy();
  if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type))
    return array->getNumElements() > 0 && array->getNumElements() <= 64 && validPieceType(array->getElementType(), depth + 1);
  if (auto *record = llvm::dyn_cast<llvm::StructType>(type))
    return record->getNumElements() > 0 && record->getNumElements() <= 64 &&
        llvm::all_of(record->elements(), [&](llvm::Type *field) { return validPieceType(field, depth + 1); });
  return false;
}

bool validPieceExtent(llvm::Type *type, uint64_t offset, uint64_t extent, const llvm::DataLayout &layout) {
  if (offset >= extent) return false;
  if (layout.getTypeStoreSize(type) <= extent - offset) return true;
  // AAPCS integer coercions round the last register up. Its unused bits are
  // not permission to access bytes beyond the logical object.
  if (type->isIntegerTy()) return true;
  if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
    uint64_t stride = layout.getTypeAllocSize(array->getElementType());
    for (uint64_t i = 0; i < array->getNumElements(); ++i)
      if (!validPieceExtent(array->getElementType(), offset + i * stride, extent, layout)) return false;
    return true;
  }
  if (auto *record = llvm::dyn_cast<llvm::StructType>(type)) {
    const auto *positions = layout.getStructLayout(record);
    for (unsigned i = 0; i < record->getNumElements(); ++i)
      if (!validPieceExtent(record->getElementType(i), offset + positions->getElementOffset(i), extent, layout)) return false;
    return true;
  }
  return false;
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
    if (!piece.type || &piece.type->getContext() != &builder.getContext() ||
        !validPieceType(piece.type))
      return failure("invalid native piece type");
    uint64_t size = layout.getTypeStoreSize(piece.type);
    if (piece.offset < end || !validPieceExtent(piece.type, piece.offset, value.storageSize, layout))
      return failure("overlapping or out-of-bounds native pieces");
    end = piece.offset + std::min(size, value.storageSize - piece.offset);
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

llvm::Value *loadPiece(llvm::IRBuilderBase &builder, llvm::Type *type, llvm::Value *storage,
                      uint64_t offset, uint64_t extent, llvm::Align alignment) {
  const auto &layout = builder.GetInsertBlock()->getModule()->getDataLayout();
  auto at = llvm::commonAlignment(alignment, offset);
  if (layout.getTypeStoreSize(type) <= extent - offset)
    return builder.CreateAlignedLoad(type, address(builder, storage, offset), at, "abi.load");
  if (type->isIntegerTy()) {
    auto *memoryType = llvm::IntegerType::get(builder.getContext(), (extent - offset) * 8);
    auto *part = builder.CreateAlignedLoad(memoryType, address(builder, storage, offset), at, "abi.load");
    return builder.CreateZExt(part, type, "abi.extend");
  }
  llvm::Value *result = llvm::UndefValue::get(type);
  if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
    uint64_t stride = layout.getTypeAllocSize(array->getElementType());
    for (unsigned i = 0; i < array->getNumElements(); ++i)
      result = builder.CreateInsertValue(result, loadPiece(builder, array->getElementType(), storage,
          offset + i * stride, extent, alignment), i);
  } else if (auto *record = llvm::dyn_cast<llvm::StructType>(type)) {
    const auto *positions = layout.getStructLayout(record);
    for (unsigned i = 0; i < record->getNumElements(); ++i)
      result = builder.CreateInsertValue(result, loadPiece(builder, record->getElementType(i), storage,
          offset + positions->getElementOffset(i), extent, alignment), i);
  }
  return result;
}

void storePiece(llvm::IRBuilderBase &builder, llvm::Value *value, llvm::Value *storage,
                uint64_t offset, uint64_t extent, llvm::Align alignment) {
  const auto &layout = builder.GetInsertBlock()->getModule()->getDataLayout();
  auto *type = value->getType();
  auto at = llvm::commonAlignment(alignment, offset);
  if (layout.getTypeStoreSize(type) <= extent - offset) {
    builder.CreateAlignedStore(value, address(builder, storage, offset), at);
  } else if (type->isIntegerTy()) {
    auto *memoryType = llvm::IntegerType::get(builder.getContext(), (extent - offset) * 8);
    builder.CreateAlignedStore(builder.CreateTrunc(value, memoryType, "abi.truncate"), address(builder, storage, offset), at);
  } else if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
    uint64_t stride = layout.getTypeAllocSize(array->getElementType());
    for (unsigned i = 0; i < array->getNumElements(); ++i)
      storePiece(builder, builder.CreateExtractValue(value, i), storage, offset + i * stride, extent, alignment);
  } else if (auto *record = llvm::dyn_cast<llvm::StructType>(type)) {
    const auto *positions = layout.getStructLayout(record);
    for (unsigned i = 0; i < record->getNumElements(); ++i)
      storePiece(builder, builder.CreateExtractValue(value, i), storage, offset + positions->getElementOffset(i), extent, alignment);
  }
}
} // namespace

llvm::Expected<llvm::DataLayout> nativeABIDataLayout(llvm::StringRef targetID) {
  auto *target = findNativeTarget(targetID);
  if (!target) return failure("requested native target is unavailable in this Sela library");
  return llvm::DataLayout(target->layout);
}

llvm::Expected<NativeABISignature> classifyNativeABI(
    llvm::FunctionType *logical, llvm::StringRef targetID,
    llvm::ArrayRef<llvm::StructType *> orderedRecords) {
  auto *target = findNativeTarget(targetID);
  if (!target) return failure("requested native target is unavailable in this Sela library");
  return target->classify(logical, orderedRecords);
}

llvm::Expected<NativeABISignature> classifyNativeLayoutABI(
    llvm::FunctionType *logical, llvm::StringRef targetID,
    llvm::ArrayRef<NativeABIRecordLayout> records) {
  auto *target = findNativeTarget(targetID);
  if (!target) return failure("requested native target is unavailable in this Sela library");
  return target->classifyLayout(logical, records);
}

llvm::Expected<llvm::SmallVector<llvm::Value *, 2>> loadNativeABIPieces(
    llvm::IRBuilderBase &builder, const NativeABIValue &value,
    llvm::Value *storage, llvm::Align baseAlignment) {
  if (auto error = validateAccess(builder, value, storage, baseAlignment)) return std::move(error);
  llvm::SmallVector<llvm::Value *, 2> result;
  for (auto piece : value.pieces)
    result.push_back(loadPiece(builder, piece.type, storage, piece.offset, value.storageSize, baseAlignment));
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
    storePiece(builder, pieces[i], storage, offset, value.storageSize, baseAlignment);
  }
  return llvm::Error::success();
}
} // namespace sela::detail
