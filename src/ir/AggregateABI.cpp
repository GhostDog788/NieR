#include "AggregateABI.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>

namespace nier::detail {
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

struct Leaf { llvm::Type *type; uint64_t offset, size; };
struct Classifier {
  bool x64;
  llvm::DataLayout layout;
  llvm::SmallPtrSet<llvm::StructType *, 16> records;
  llvm::DenseSet<llvm::Type *> validated;

  Classifier(bool wide, llvm::ArrayRef<llvm::StructType *> ordered)
      : x64(wide), layout(nativeABIDataLayout(wide)),
        records(ordered.begin(), ordered.end()) {}

  llvm::Error validate(llvm::Type *type, unsigned depth = 0) {
    if (depth > 32 || !type->isSized())
      return failure("unsized or excessively nested logical storage");
    if (validated.contains(type)) return llvm::Error::success();
    if (scalar(type, true)) return llvm::Error::success();
    if (auto *record = llvm::dyn_cast<llvm::StructType>(type)) {
      if (!records.contains(record))
        return failure("record lacks ordered non-overlapping ABI qualification (union storage is not a logical record)");
      if (record->isPacked() || record->getNumElements() == 0)
        return failure("packed or empty records are not qualified");
      uint64_t size = 0;
      for (auto *field : record->elements()) {
        if (auto error = validate(field, depth + 1)) return error;
        size = llvm::alignTo(size, layout.getABITypeAlign(field));
        uint64_t fieldSize = layout.getTypeAllocSize(field);
        if (size > (1ULL << 30) || fieldSize > (1ULL << 30) - size)
          return failure("logical storage exceeds qualification size limit");
        size += fieldSize;
      }
      if (layout.getTypeAllocSize(type) > (1ULL << 30))
        return failure("logical storage exceeds qualification size limit");
      validated.insert(type);
      return llvm::Error::success();
    }
    if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      if (!array->getNumElements())
        return failure("empty or flexible array storage is not qualified");
      if (auto error = validate(array->getElementType(), depth + 1)) return error;
      uint64_t elementSize = layout.getTypeAllocSize(array->getElementType());
      if (!elementSize || array->getNumElements() > (1ULL << 30) / elementSize)
        return failure("logical storage exceeds qualification size limit");
      validated.insert(type);
      return llvm::Error::success();
    }
    return failure("unsupported logical record field (vectors, bitfields and nonstandard scalars need a separate contract)");
  }

  void leaves(llvm::Type *type, uint64_t offset,
              llvm::SmallVectorImpl<Leaf> &result) {
    if (auto *record = llvm::dyn_cast<llvm::StructType>(type)) {
      auto *recordLayout = layout.getStructLayout(record);
      for (unsigned i = 0; i < record->getNumElements(); ++i)
        leaves(record->getElementType(i), offset + recordLayout->getElementOffset(i), result);
    } else if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      uint64_t stride = layout.getTypeAllocSize(array->getElementType());
      for (uint64_t i = 0; i < array->getNumElements(); ++i)
        leaves(array->getElementType(), offset + i * stride, result);
    } else result.push_back({type, offset, layout.getTypeStoreSize(type)});
  }

  NativeABIValue value(llvm::Type *type) {
    NativeABIValue result;
    result.storageType = type;
    if (!type->isVoidTy()) {
      result.storageSize = layout.getTypeAllocSize(type);
      result.storageAlignment = layout.getABITypeAlign(type);
      result.abiAlignment = result.storageAlignment;
    }
    return result;
  }

  NativeABIValue indirect(llvm::Type *type, bool returns) {
    auto result = value(type);
    result.kind = NativeABIKind::Indirect;
    result.sRet = returns;
    result.byVal = !returns;
    if (!returns)
      result.abiAlignment = x64 ? std::max(llvm::Align(8), result.storageAlignment)
                                : llvm::Align(4);
    result.nativeCount = 1;
    return result;
  }

  // Pinned reference rules: llvmorg-18.1.3 clang/lib/CodeGen/Targets/X86.cpp,
  // X86_32ABIInfo::{canExpandIndirectArgument,classifyArgumentType}, and
  // X86_64ABIInfo::{classify,GetINTEGERTypeAtOffset,GetSSETypeAtOffset,
  // getIndirectResult,computeInfo}. This is an independent implementation for
  // the explicitly admitted record semantics above, not a Clang dependency.
  llvm::Expected<NativeABIValue> classify(llvm::Type *type, bool returns,
                                         unsigned freeGP, unsigned freeSSE) {
    if (type->isVoidTy()) {
      if (!returns) return failure("void parameter");
      return value(type);
    }
    if (scalar(type)) {
      auto result = value(type);
      result.pieces.push_back({type, 0});
      result.nativeCount = 1;
      return result;
    }
    if (!llvm::isa<llvm::StructType>(type))
      return failure("by-value logical boundary must be a scalar or qualified record");
    if (auto error = validate(type)) return std::move(error);
    auto result = value(type);
    if (!x64) {
      if (returns || result.storageSize > 16) return indirect(type, returns);
      auto *record = llvm::cast<llvm::StructType>(type);
      auto *recordLayout = layout.getStructLayout(record);
      uint64_t bytes = 0;
      for (unsigned i = 0; i < record->getNumElements(); ++i) {
        auto *field = record->getElementType(i);
        uint64_t size = layout.getTypeAllocSize(field);
        if (!scalar(field, true) || (size != 4 && size != 8) ||
            recordLayout->getElementOffset(i) != bytes)
          return indirect(type, false);
        result.pieces.push_back({field, bytes});
        bytes += size;
      }
      if (bytes != result.storageSize) return indirect(type, false);
      result.kind = NativeABIKind::Expand;
      result.nativeCount = result.pieces.size();
      return result;
    }
    if (result.storageSize > 16) return indirect(type, returns);

    llvm::SmallVector<Leaf, 16> fields;
    leaves(type, 0, fields);
    unsigned neededGP = 0, neededSSE = 0;
    for (uint64_t offset = 0; offset < result.storageSize; offset += 8) {
      llvm::SmallVector<Leaf, 8> chunk;
      bool integer = false;
      for (auto field : fields) {
        if (field.offset >= offset + 8 || field.offset + field.size <= offset) continue;
        if (field.offset < offset || field.offset + field.size > offset + 8)
          return failure("unaligned or straddling scalar field is not qualified");
        integer |= field.type->isIntegerTy() || field.type->isPointerTy();
        chunk.push_back(field);
      }
      if (chunk.empty()) return failure("empty eightbyte record span is not qualified");
      llvm::Type *piece = nullptr;
      if (integer) {
        ++neededGP;
        auto first = chunk.front();
        // A single leading integer/pointer may omit only proven tail padding.
        if (chunk.size() == 1 && first.offset == offset &&
            (first.type->isIntegerTy() || first.type->isPointerTy()))
          piece = first.type;
        else piece = llvm::IntegerType::get(type->getContext(),
            8 * std::min<uint64_t>(8, result.storageSize - offset));
      } else {
        ++neededSSE;
        if (chunk.front().offset != offset)
          return failure("leading padding in SSE eightbyte is not qualified");
        if (chunk.size() == 1) piece = chunk.front().type;
        else if (chunk.size() == 2 && chunk[0].type->isFloatTy() &&
                 chunk[1].type->isFloatTy() && chunk[1].offset == offset + 4)
          piece = llvm::FixedVectorType::get(chunk[0].type, 2);
        else return failure("unsupported SSE eightbyte record shape");
      }
      result.pieces.push_back({piece, offset});
    }
    if (!returns && (neededGP > freeGP || neededSSE > freeSSE)) {
      // Clang's scalar-stack optimization is specifically conditional on no
      // GP registers remaining. It applies even to an exhausted SSE record.
      if (freeGP == 0 && result.storageSize <= 8 && result.storageAlignment <= llvm::Align(8)) {
        result.pieces.clear();
        result.pieces.push_back({llvm::IntegerType::get(type->getContext(), result.storageSize * 8), 0});
      } else return indirect(type, false);
    }
    if (result.pieces.size() == 2) {
      auto *low = result.pieces[0].type;
      auto *high = result.pieces[1].type;
      if (llvm::alignTo(layout.getTypeAllocSize(low), layout.getABITypeAlign(high)) != 8)
        result.pieces[0].type = low->isFloatTy() ? llvm::Type::getDoubleTy(type->getContext())
                                               : llvm::Type::getInt64Ty(type->getContext());
    }
    result.kind = result.pieces.size() == 1 ? NativeABIKind::Coerce : NativeABIKind::Expand;
    result.nativeCount = result.pieces.size();
    return result;
  }
};

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

llvm::DataLayout nativeABIDataLayout(bool x64) {
  return llvm::DataLayout(x64
      ? "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
      : "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128");
}

llvm::Expected<NativeABISignature> classifyNativeABI(
    llvm::FunctionType *logical, bool x64,
    llvm::ArrayRef<llvm::StructType *> orderedRecords) {
  if (!logical) return failure("missing logical function type");
  Classifier classifier(x64, orderedRecords);
  NativeABISignature signature;
  unsigned freeGP = x64 ? 6 : 0, freeSSE = x64 ? 8 : 0;
  auto returns = classifier.classify(logical->getReturnType(), true, freeGP, freeSSE);
  if (!returns) return returns.takeError();
  signature.result = std::move(*returns);
  llvm::SmallVector<llvm::Type *, 16> nativeParameters;
  llvm::Type *nativeResult = llvm::Type::getVoidTy(logical->getContext());
  if (signature.result.sRet) {
    signature.sretIndex = 0;
    nativeParameters.push_back(llvm::PointerType::get(logical->getContext(), 0));
    if (x64) --freeGP;
  } else if (signature.result.pieces.size() == 1) nativeResult = signature.result.pieces[0].type;
  else if (!signature.result.pieces.empty()) {
    llvm::SmallVector<llvm::Type *, 2> resultTypes;
    for (auto piece : signature.result.pieces) resultTypes.push_back(piece.type);
    nativeResult = llvm::StructType::get(logical->getContext(), resultTypes);
  }
  for (auto *parameter : logical->params()) {
    auto native = classifier.classify(parameter, false, freeGP, freeSSE);
    if (!native) return native.takeError();
    native->nativeBegin = nativeParameters.size();
    if (native->byVal) nativeParameters.push_back(llvm::PointerType::get(logical->getContext(), 0));
    else {
      unsigned gp = 0, sse = 0;
      for (auto piece : native->pieces) {
        nativeParameters.push_back(piece.type);
        if (piece.type->isIntegerTy() || piece.type->isPointerTy()) ++gp;
        else ++sse;
      }
      // An argument that cannot fit consumes neither bank; its register
      // assignment is rolled back as one complete aggregate.
      if (x64 && freeGP >= gp && freeSSE >= sse) { freeGP -= gp; freeSSE -= sse; }
    }
    signature.parameters.push_back(std::move(*native));
  }
  signature.nativeType = llvm::FunctionType::get(nativeResult, nativeParameters, logical->isVarArg());
  signature.remainingGP = freeGP;
  signature.remainingSSE = freeSSE;
  return signature;
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
} // namespace nier::detail
