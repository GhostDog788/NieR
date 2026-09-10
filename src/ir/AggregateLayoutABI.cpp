#include "AggregateABI.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>

namespace nier::detail {
namespace {
constexpr uint64_t SizeLimit = 1ULL << 30;
llvm::Error failure(const llvm::Twine &message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
      "native semantic-layout ABI: " + message);
}
bool scalar(llvm::Type *type) {
  return type && (type->isIntegerTy(8) || type->isIntegerTy(16) ||
      type->isIntegerTy(32) || type->isIntegerTy(64) || type->isFloatTy() || type->isDoubleTy() ||
      (type->isPointerTy() && type->getPointerAddressSpace() == 0));
}
struct TypeInfo { uint64_t size; llvm::Align alignment; };
struct Span { llvm::Type *type; uint64_t begin, end; bool bitfield; };

class LayoutClassifier {
public:
  bool x64;
  llvm::DataLayout layout;
  llvm::DenseMap<llvm::StructType *, const NativeABIRecordLayout *> records;
  llvm::DenseMap<llvm::Type *, TypeInfo> semanticInfo;
  llvm::SmallPtrSet<llvm::Type *, 32> active;
  llvm::SmallPtrSet<llvm::Type *, 32> checkedStorage;

  explicit LayoutClassifier(bool wide) : x64(wide), layout(nativeABIDataLayout(wide)) {}

  llvm::Error storage(llvm::Type *type, unsigned depth = 0) {
    if (!type || depth > 32 || !type->isSized()) return failure("unsized or deep native storage carrier");
    if (checkedStorage.contains(type)) return llvm::Error::success();
    if (scalar(type) || (type->isIntegerTy() && type->getIntegerBitWidth() <= 128)) {
      checkedStorage.insert(type); return llvm::Error::success();
    }
    if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      if (auto error = storage(array->getElementType(), depth + 1)) return error;
      uint64_t bytes = layout.getTypeAllocSize(array->getElementType());
      if (!bytes || array->getNumElements() > SizeLimit / bytes) return failure("oversized native storage array");
    } else if (auto *record = llvm::dyn_cast<llvm::StructType>(type)) {
      uint64_t bytes = 0;
      for (auto *field : record->elements()) {
        if (auto error = storage(field, depth + 1)) return error;
        bytes = llvm::alignTo(bytes, record->isPacked() ? llvm::Align(1) : layout.getABITypeAlign(field));
        uint64_t fieldSize = layout.getTypeAllocSize(field);
        if (bytes > SizeLimit || fieldSize > SizeLimit - bytes) return failure("oversized native storage record");
        bytes += fieldSize;
      }
    } else return failure("unsupported native storage carrier");
    if (layout.getTypeAllocSize(type) > SizeLimit) return failure("oversized native storage carrier");
    checkedStorage.insert(type);
    return llvm::Error::success();
  }

  llvm::Expected<TypeInfo> info(llvm::Type *type, unsigned depth = 0) {
    if (!type || depth > 32) return failure("missing or deep semantic field type");
    if (auto found = semanticInfo.find(type); found != semanticInfo.end()) return found->second;
    if (scalar(type)) return TypeInfo{layout.getTypeAllocSize(type), layout.getABITypeAlign(type)};
    if (!active.insert(type).second) return failure("recursive by-value semantic layout");
    if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      auto element = info(array->getElementType(), depth + 1);
      if (!element) return element.takeError();
      if (!array->getNumElements() || !element->size || array->getNumElements() > SizeLimit / element->size)
        return failure("empty, flexible or oversized semantic array");
      if (auto error = storage(type)) return std::move(error);
      TypeInfo result{element->size * array->getNumElements(), element->alignment};
      if (layout.getTypeAllocSize(type) != result.size) return failure("semantic array stride disagrees with storage carrier");
      active.erase(type); semanticInfo[type] = result; return result;
    }
    auto *native = llvm::dyn_cast<llvm::StructType>(type);
    auto found = native ? records.find(native) : records.end();
    if (found == records.end()) return failure("record lacks an explicit semantic layout");
    auto &record = *found->second;
    if (record.kind != NativeABIRecordKind::Ordered && record.kind != NativeABIRecordKind::Union)
      return failure("unknown semantic record kind");
    if (!record.sizeBytes || record.sizeBytes > SizeLimit ||
        record.alignment.value() > SizeLimit || record.sizeBytes % record.alignment.value() ||
        record.fields.empty() || record.fields.size() > 1024)
      return failure("invalid semantic record size/alignment/field count");
    if (auto error = storage(type)) return std::move(error);
    if (layout.getTypeAllocSize(type) != record.sizeBytes)
      return failure("semantic record size disagrees with storage carrier");
    uint64_t end = 0;
    bool data = false;
    for (auto &field : record.fields) {
      if (!field.type || &field.type->getContext() != &type->getContext())
        return failure("missing or foreign-context semantic field");
      uint64_t bits;
      if (field.bitWidth) {
        if (!field.type->isIntegerTy() || !scalar(field.type) ||
            *field.bitWidth > field.type->getIntegerBitWidth() || (!*field.bitWidth && !field.padding))
          return failure("invalid integer bitfield width");
        bits = *field.bitWidth;
      } else {
        if (field.padding || field.bitOffset % 8) return failure("non-bitfield has invalid padding/bit offset");
        auto fieldInfo = info(field.type, depth + 1);
        if (!fieldInfo) return fieldInfo.takeError();
        bits = fieldInfo->size * 8;
      }
      if (field.bitOffset > record.sizeBytes * 8 || bits > record.sizeBytes * 8 - field.bitOffset)
        return failure("semantic field exceeds native storage");
      if (record.kind == NativeABIRecordKind::Union && field.bitOffset != 0)
        return failure("union alternatives must overlap at offset zero");
      if (record.kind == NativeABIRecordKind::Ordered) {
        if (field.bitOffset < end) return failure("ordered semantic fields overlap or are out of order");
        end = field.bitOffset + bits;
      }
      data |= !field.padding && bits != 0;
    }
    if (!data) return failure("empty/all-padding ABI values need an explicit ignored-value contract");
    TypeInfo result{record.sizeBytes, record.alignment};
    active.erase(type); semanticInfo[type] = result; return result;
  }

  llvm::Error initialize(llvm::ArrayRef<NativeABIRecordLayout> descriptors) {
    for (auto &record : descriptors) {
      if (!record.storageType || !records.try_emplace(record.storageType, &record).second)
        return failure("missing or duplicate semantic storage identity");
    }
    for (auto &record : descriptors) {
      auto result = info(record.storageType);
      if (!result) return result.takeError();
    }
    return llvm::Error::success();
  }

  bool gather(llvm::Type *type, uint64_t base, llvm::SmallVectorImpl<Span> &spans, bool &unaligned) {
    if (spans.size() >= 4096) return false;
    if (scalar(type)) {
      spans.push_back({type, base, base + layout.getTypeStoreSize(type) * 8, false});
    } else if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      uint64_t stride = layout.getTypeAllocSize(array->getElementType()) * 8;
      for (uint64_t i = 0; i < array->getNumElements(); ++i)
        if (!gather(array->getElementType(), base + i * stride, spans, unaligned)) return false;
    } else {
      auto &record = *records.at(llvm::cast<llvm::StructType>(type));
      for (auto &field : record.fields) {
        if (spans.size() >= 4096) return false;
        if (field.padding) continue;
        if (field.bitWidth) spans.push_back({field.type, base + field.bitOffset, base + field.bitOffset + *field.bitWidth, true});
        else {
          auto alignment = scalar(field.type) ? layout.getABITypeAlign(field.type) : semanticInfo.at(field.type).alignment;
          unaligned |= (base + field.bitOffset) % (alignment.value() * 8) != 0;
          if (!gather(field.type, base + field.bitOffset, spans, unaligned)) return false;
        }
      }
    }
    return true;
  }

  llvm::Type *preferred(llvm::Type *type, uint64_t offset) {
    if (offset >= layout.getTypeAllocSize(type)) return nullptr;
    if (auto *record = llvm::dyn_cast<llvm::StructType>(type)) {
      if (!record->getNumElements()) return nullptr;
      auto *recordLayout = layout.getStructLayout(record);
      unsigned index = recordLayout->getElementContainingOffset(offset);
      return preferred(record->getElementType(index), offset - recordLayout->getElementOffset(index));
    }
    if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      uint64_t stride = layout.getTypeAllocSize(array->getElementType());
      return stride ? preferred(array->getElementType(), offset % stride) : nullptr;
    }
    return offset == 0 ? type : nullptr;
  }

  NativeABIValue value(llvm::Type *type, TypeInfo information) {
    NativeABIValue result;
    result.storageType = type;
    result.storageSize = information.size;
    result.storageAlignment = information.alignment;
    result.abiAlignment = information.alignment;
    return result;
  }
  NativeABIValue memory(llvm::Type *type, TypeInfo information, bool returns, unsigned freeGP) {
    auto result = value(type, information);
    if (x64 && !returns && freeGP == 0 && information.size <= 8 && information.alignment <= llvm::Align(8)) {
      result.kind = NativeABIKind::Coerce;
      result.pieces.push_back({llvm::IntegerType::get(type->getContext(), information.size * 8), 0});
    } else {
      result.kind = NativeABIKind::Indirect;
      result.byVal = !returns; result.sRet = returns;
      if (!returns) result.abiAlignment = x64 ? std::max(llvm::Align(8), information.alignment) : llvm::Align(4);
    }
    result.nativeCount = 1;
    return result;
  }

  llvm::Expected<NativeABIValue> classify(llvm::Type *type, bool returns, unsigned freeGP, unsigned freeSSE) {
    if (type->isVoidTy() && !returns) return failure("void parameter");
    if (type->isVoidTy() || scalar(type) || type->isIntegerTy(1)) {
      auto result = value(type, type->isVoidTy() ? TypeInfo{0, llvm::Align(1)} :
          TypeInfo{layout.getTypeAllocSize(type), layout.getABITypeAlign(type)});
      if (!type->isVoidTy()) { result.pieces.push_back({type, 0}); result.nativeCount = 1; }
      return result;
    }
    auto *recordType = llvm::dyn_cast<llvm::StructType>(type);
    if (!recordType || !records.count(recordType)) return failure("by-value boundary lacks a semantic record descriptor");
    auto information = semanticInfo.at(type);
    auto &record = *records.at(recordType);
    auto result = value(type, information);
    if (!x64) {
      if (returns || information.size > 16) return memory(type, information, returns, freeGP);
      uint64_t bytes = 0;
      for (auto &field : record.fields) {
        if (field.bitWidth || field.padding || !scalar(field.type)) return memory(type, information, false, freeGP);
        uint64_t size = layout.getTypeAllocSize(field.type);
        if ((size != 4 && size != 8) || field.bitOffset != bytes * 8) return memory(type, information, false, freeGP);
        result.pieces.push_back({field.type, bytes}); bytes += size;
      }
      if (bytes != information.size) return memory(type, information, false, freeGP);
      result.kind = NativeABIKind::Expand; result.nativeCount = result.pieces.size();
      return result;
    }
    if (information.size > 16) return memory(type, information, returns, freeGP);
    llvm::SmallVector<Span, 16> spans;
    bool unaligned = false;
    if (!gather(type, 0, spans, unaligned)) return failure("semantic layout exceeds bounded ABI decomposition");
    if (unaligned) return memory(type, information, returns, freeGP);
    unsigned gp = 0, sse = 0;
    for (uint64_t offset = 0; offset < information.size; offset += 8) {
      bool any = false, integer = false;
      for (auto span : spans) {
        if (span.begin >= (offset + 8) * 8 || span.end <= offset * 8) continue;
        any = true;
        integer |= span.bitfield || span.type->isIntegerTy() || span.type->isPointerTy();
      }
      if (!any) continue; // Proven source padding takes no native register.
      auto *preferredType = preferred(type, offset);
      llvm::Type *piece = nullptr;
      if (integer) {
        ++gp;
        if (preferredType && (preferredType->isIntegerTy(64) || preferredType->isPointerTy())) piece = preferredType;
        else if (preferredType && (preferredType->isIntegerTy(8) || preferredType->isIntegerTy(16) || preferredType->isIntegerTy(32))) {
          uint64_t end = offset * 8 + preferredType->getIntegerBitWidth();
          bool laterData = false;
          for (auto span : spans) laterData |= span.begin < (offset + 8) * 8 && span.end > end;
          if (!laterData) piece = preferredType;
        }
        if (!piece) piece = llvm::IntegerType::get(type->getContext(), std::min<uint64_t>(8, information.size - offset) * 8);
      } else {
        ++sse;
        if (preferredType && preferredType->isFloatTy()) {
          auto *next = preferred(type, offset + 4);
          piece = next && next->isFloatTy() ? llvm::FixedVectorType::get(preferredType, 2) : preferredType;
        } else piece = llvm::Type::getDoubleTy(type->getContext());
      }
      if (layout.getTypeStoreSize(piece) > information.size - offset)
        return failure("preferred native coercion exceeds record storage");
      result.pieces.push_back({piece, offset});
    }
    if (result.pieces.empty()) return failure("record has no admitted native ABI data");
    if (!returns && (gp > freeGP || sse > freeSSE)) return memory(type, information, false, freeGP);
    if (result.pieces.size() == 2) {
      auto *low = result.pieces[0].type, *high = result.pieces[1].type;
      if (llvm::alignTo(layout.getTypeAllocSize(low), layout.getABITypeAlign(high)) != 8)
        result.pieces[0].type = low->isFloatTy() ? llvm::Type::getDoubleTy(type->getContext()) : llvm::Type::getInt64Ty(type->getContext());
    }
    result.kind = result.pieces.size() == 1 ? NativeABIKind::Coerce : NativeABIKind::Expand;
    result.nativeCount = result.pieces.size();
    return result;
  }
};
} // namespace

llvm::Expected<NativeABISignature> classifyNativeLayoutABI(
    llvm::FunctionType *logical, bool x64, llvm::ArrayRef<NativeABIRecordLayout> records) {
  if (!logical) return failure("missing logical signature");
  LayoutClassifier classifier(x64);
  if (auto error = classifier.initialize(records)) return std::move(error);
  NativeABISignature signature;
  unsigned freeGP = x64 ? 6 : 0, freeSSE = x64 ? 8 : 0;
  auto returns = classifier.classify(logical->getReturnType(), true, freeGP, freeSSE);
  if (!returns) return returns.takeError();
  signature.result = std::move(*returns);
  llvm::SmallVector<llvm::Type *, 16> parameters;
  llvm::Type *resultType = llvm::Type::getVoidTy(logical->getContext());
  if (signature.result.sRet) {
    signature.sretIndex = 0;
    parameters.push_back(llvm::PointerType::get(logical->getContext(), 0));
    if (x64) --freeGP;
  } else if (signature.result.pieces.size() == 1) resultType = signature.result.pieces[0].type;
  else if (!signature.result.pieces.empty()) {
    llvm::SmallVector<llvm::Type *, 2> pieces;
    for (auto piece : signature.result.pieces) pieces.push_back(piece.type);
    resultType = llvm::StructType::get(logical->getContext(), pieces);
  }
  for (auto *parameter : logical->params()) {
    auto native = classifier.classify(parameter, false, freeGP, freeSSE);
    if (!native) return native.takeError();
    native->nativeBegin = parameters.size();
    if (native->byVal) parameters.push_back(llvm::PointerType::get(logical->getContext(), 0));
    else {
      unsigned gp = 0, sse = 0;
      for (auto piece : native->pieces) {
        parameters.push_back(piece.type);
        if (piece.type->isIntegerTy() || piece.type->isPointerTy()) ++gp; else ++sse;
      }
      if (x64 && gp <= freeGP && sse <= freeSSE) { freeGP -= gp; freeSSE -= sse; }
    }
    signature.parameters.push_back(std::move(*native));
  }
  signature.nativeType = llvm::FunctionType::get(resultType, parameters, logical->isVarArg());
  signature.remainingGP = freeGP; signature.remainingSSE = freeSSE;
  return signature;
}
} // namespace nier::detail
