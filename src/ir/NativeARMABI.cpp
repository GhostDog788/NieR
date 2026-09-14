#include "NativeTargetConfig.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/MathExtras.h"

namespace sela::detail::SELA_NATIVE_NAMESPACE {
namespace {
llvm::Error failure(const llvm::Twine &message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), "native ARM ABI: " + message);
}

bool scalar(llvm::Type *type) {
  if (auto *pointer = llvm::dyn_cast<llvm::PointerType>(type)) return pointer->getAddressSpace() == 0;
  if (auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(type)) {
    auto bits = vector->getPrimitiveSizeInBits().getFixedValue();
    return (bits == 64 || bits == 128) &&
        (vector->getElementType()->isIntegerTy() || vector->getElementType()->isFloatingPointTy());
  }
  return type->isFloatTy() || type->isDoubleTy() || type->isIntegerTy(1) ||
      type->isIntegerTy(8) || type->isIntegerTy(16) || type->isIntegerTy(32) || type->isIntegerTy(64) ||
      (aapcs64 && type->isIntegerTy(128));
}

struct Information {
  uint64_t size = 0;
  llvm::Align alignment{1};
  llvm::Type *homogeneousBase = nullptr;
  uint64_t members = 0;
};

// Pinned Clang 18.1.3 ARM.cpp/AArch64.cpp ABI rules, expressed over explicit
// language-neutral record layouts. No AST, source-language mode or frontend
// library is needed on the device.
struct Classifier {
  llvm::DataLayout layout{TargetLayout};
  llvm::DenseMap<llvm::StructType *, const NativeABIRecordLayout *> records;
  llvm::SmallPtrSet<llvm::Type *, 16> active;
  unsigned remainingNodes = 4096;
  unsigned remainingStorageNodes = 16384;

  bool boundedStorage(llvm::Type *type, unsigned depth = 0) {
    if (!remainingStorageNodes || !type || depth > 32 || !type->isSized()) return false;
    --remainingStorageNodes;
    if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      if (!array->getNumElements() || !boundedStorage(array->getElementType(), depth + 1)) return false;
      auto elementSize = layout.getTypeAllocSize(array->getElementType());
      return elementSize && array->getNumElements() <= (1ULL << 30) / elementSize;
    }
    if (auto *record = llvm::dyn_cast<llvm::StructType>(type)) {
      uint64_t maximum = 0;
      for (auto *field : record->elements()) {
        if (!boundedStorage(field, depth + 1)) return false;
        // Include each field's worst-case alignment padding before asking
        // LLVM to calculate a layout; malformed extents must never overflow.
        auto bytes = layout.getTypeAllocSize(field) + layout.getABITypeAlign(field).value();
        if (bytes > (1ULL << 30) - maximum) return false;
        maximum += bytes;
      }
    }
    return layout.getTypeAllocSize(type) <= (1ULL << 30);
  }

  llvm::Expected<Information> information(llvm::Type *type, unsigned depth = 0) {
    if (!remainingNodes || depth > 32 || !boundedStorage(type))
      return failure("unsized, oversized or excessively nested storage");
    --remainingNodes;
    Information result;
    result.size = layout.getTypeAllocSize(type);
    result.alignment = layout.getABITypeAlign(type);
    if (!result.size || result.size > (1ULL << 30)) return failure("empty or oversized ABI storage");
    if (scalar(type)) {
      if (type->isFloatTy() || type->isDoubleTy()) { result.homogeneousBase = type; result.members = 1; }
      return result;
    }
    if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      auto element = information(array->getElementType(), depth + 1);
      if (!element) return element.takeError();
      if (element->homogeneousBase && array->getNumElements() <= 4 / element->members) {
        result.homogeneousBase = element->homogeneousBase;
        result.members = element->members * array->getNumElements();
      }
      return result;
    }
    auto *record = llvm::dyn_cast<llvm::StructType>(type);
    auto found = record ? records.find(record) : records.end();
    if (found == records.end()) return failure("aggregate lacks a qualified semantic record layout");
    if (!active.insert(type).second) return failure("recursive by-value record");
    const auto &description = *found->second;
    if (description.sizeBytes != result.size || description.fields.empty() ||
        description.fields.size() > 4096 || description.sizeBytes % description.alignment.value()) {
      active.erase(type); return failure("semantic layout disagrees with storage extent");
    }
    result.alignment = description.alignment;
    bool homogeneous = true;
    bool hasValue = false;
    uint64_t previousEnd = 0;
    for (const auto &field : description.fields) {
      auto child = information(field.type, depth + 1);
      if (!child) { active.erase(type); return child.takeError(); }
      uint64_t bits = field.bitWidth.value_or(child->size * 8);
      if (field.bitOffset > result.size * 8 || bits > result.size * 8 - field.bitOffset ||
          (field.bitWidth && (!field.type->isIntegerTy() || bits > field.type->getIntegerBitWidth())) ||
          (!field.bitWidth && field.bitOffset % 8) ||
          (description.kind == NativeABIRecordKind::Union && field.bitOffset != 0) ||
          (description.kind == NativeABIRecordKind::Ordered && !field.padding && field.bitOffset < previousEnd)) {
        active.erase(type); return failure("invalid or overlapping semantic field extent");
      }
      if (!field.padding) { previousEnd = field.bitOffset + bits; hasValue = true; }
      if (field.padding || field.bitWidth || !child->homogeneousBase) { homogeneous = false; continue; }
      if (result.homogeneousBase && result.homogeneousBase != child->homogeneousBase) homogeneous = false;
      result.homogeneousBase = child->homogeneousBase;
      result.members = description.kind == NativeABIRecordKind::Union
          ? std::max(result.members, child->members) : result.members + child->members;
    }
    active.erase(type);
    if (!hasValue) return failure("all-padding record requires an explicit ignored-value contract");
    if (!homogeneous || !result.members || result.members > 4 ||
        !result.homogeneousBase || layout.getTypeAllocSize(result.homogeneousBase) * result.members != result.size) {
      result.homogeneousBase = nullptr; result.members = 0;
    }
    return result;
  }

  llvm::Expected<NativeABIValue> classify(llvm::Type *type, bool returns, bool variadic) {
    NativeABIValue result;
    result.storageType = type;
    if (type->isVoidTy()) {
      if (!returns) return failure("void argument");
      return result;
    }
    auto info = information(type);
    if (!info) return info.takeError();
    result.storageSize = info->size;
    result.storageAlignment = info->alignment;
    result.abiAlignment = info->alignment;
    result.nativeCount = 1;
    if (scalar(type)) { result.pieces.push_back({type, 0}); return result; }
    if (!type->isStructTy()) return failure("a by-value aggregate must be an explicitly qualified record");
    auto direct = [&](llvm::Type *physical) {
      result.kind = NativeABIKind::Coerce;
      result.pieces.push_back({physical, 0});
      return result;
    };
    auto indirect = [&](bool byVal) {
      result.kind = NativeABIKind::Indirect;
      result.byVal = byVal;
      result.sRet = returns;
      if (!aapcs64 && !returns) result.abiAlignment = llvm::Align(std::clamp<uint64_t>(info->alignment.value(), 4, 8));
      return result;
    };
    auto &context = type->getContext();
    bool hfa = info->homogeneousBase && (aapcs64 || !variadic);
    if (hfa) {
      if constexpr (aapcs64) {
        if (returns) return direct(type);
        result.stackAlignment = llvm::Align(info->alignment.value() >= 16 ? 16 : 8);
        return direct(llvm::ArrayType::get(info->homogeneousBase, info->members));
      } else {
        auto baseAlignment = layout.getABITypeAlign(info->homogeneousBase);
        if (!returns && info->alignment > baseAlignment && info->alignment.value() >= 8)
          result.stackAlignment = llvm::Align(8);
        return direct(type);
      }
    }
    if constexpr (aapcs64) {
      if (info->size > 16) return indirect(false);
      if (returns && info->size <= 8) return direct(llvm::IntegerType::get(context, info->size * 8));
      unsigned unit = info->alignment.value() >= 16 ? 16 : 8;
      auto *integer = llvm::IntegerType::get(context, unit * 8);
      uint64_t count = llvm::divideCeil(info->size, uint64_t(unit));
      return direct(count == 1 ? static_cast<llvm::Type *>(integer) : llvm::ArrayType::get(integer, count));
    } else {
      if (returns) {
        if (info->size > 4) return indirect(false);
        unsigned bytes = info->size <= 1 ? 1 : info->size <= 2 ? 2 : 4;
        return direct(llvm::IntegerType::get(context, bytes * 8));
      }
      if (info->size > 64) return indirect(true);
      unsigned unit = info->alignment.value() <= 4 ? 4 : 8;
      return direct(llvm::ArrayType::get(llvm::IntegerType::get(context, unit * 8),
                                       llvm::divideCeil(info->size, uint64_t(unit))));
    }
  }

  llvm::Expected<NativeABISignature> signature(llvm::FunctionType *logical) {
    if (!logical) return failure("missing logical signature");
    NativeABISignature result;
    auto returns = classify(logical->getReturnType(), true, logical->isVarArg());
    if (!returns) return returns.takeError();
    result.result = std::move(*returns);
    llvm::SmallVector<llvm::Type *> parameters;
    llvm::Type *returnType = llvm::Type::getVoidTy(logical->getContext());
    if (result.result.sRet) {
      result.sretIndex = 0;
      parameters.push_back(llvm::PointerType::get(logical->getContext(), 0));
    } else if (!result.result.pieces.empty()) returnType = result.result.pieces.front().type;
    for (auto *parameter : logical->params()) {
      auto argument = classify(parameter, false, logical->isVarArg());
      if (!argument) return argument.takeError();
      argument->nativeBegin = parameters.size();
      parameters.push_back(argument->kind == NativeABIKind::Indirect
          ? llvm::PointerType::get(logical->getContext(), 0) : argument->pieces.front().type);
      result.parameters.push_back(std::move(*argument));
    }
    result.nativeType = llvm::FunctionType::get(returnType, parameters, logical->isVarArg());
    return result;
  }
};
} // namespace

llvm::Expected<NativeABISignature> classifyNativeLayoutABI(
    llvm::FunctionType *logical, llvm::ArrayRef<NativeABIRecordLayout> records) {
  Classifier classifier;
  for (const auto &record : records)
    if (!record.storageType || !classifier.records.try_emplace(record.storageType, &record).second)
      return failure("missing or duplicate semantic record");
  for (const auto &record : records) {
    auto checked = classifier.information(record.storageType);
    if (!checked) return checked.takeError();
  }
  return classifier.signature(logical);
}

llvm::Expected<NativeABISignature> classifyNativeABI(
    llvm::FunctionType *logical, llvm::ArrayRef<llvm::StructType *> records) {
  llvm::DataLayout layout(TargetLayout);
  Classifier bounds;
  llvm::SmallVector<NativeABIRecordLayout> descriptions;
  for (auto *record : records) {
    if (!record || !bounds.boundedStorage(record) || record->isPacked() || record->getNumElements() == 0)
      return failure("ordered ABI records must be nonempty, sized and naturally aligned");
    NativeABIRecordLayout description;
    description.storageType = record;
    description.sizeBytes = layout.getTypeAllocSize(record);
    description.alignment = layout.getABITypeAlign(record);
    auto *positions = layout.getStructLayout(record);
    for (unsigned i = 0; i < record->getNumElements(); ++i)
      description.fields.push_back({record->getElementType(i), positions->getElementOffset(i) * 8, std::nullopt, false});
    descriptions.push_back(std::move(description));
  }
  return classifyNativeLayoutABI(logical, descriptions);
}
} // namespace sela::detail::SELA_NATIVE_NAMESPACE
