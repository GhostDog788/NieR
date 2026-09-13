#include "OverlapEvidence.h"
#include "sela/IR/Domains.h"
#include "OverlapLayout.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InstIterator.h"
#include <set>

namespace sela::detail {
namespace {
llvm::Error fail(llvm::StringRef message) {
  return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), message);
}
llvm::DIType *baseType(llvm::DIType *type) {
  unsigned depth = 0;
  while (auto *derived = llvm::dyn_cast_or_null<llvm::DIDerivedType>(type)) {
    if (++depth > 64) return nullptr;
    auto tag = derived->getTag();
    if (tag != llvm::dwarf::DW_TAG_typedef && tag != llvm::dwarf::DW_TAG_const_type &&
        tag != llvm::dwarf::DW_TAG_volatile_type && tag != llvm::dwarf::DW_TAG_restrict_type)
      break;
    type = derived->getBaseType();
  }
  return type;
}
llvm::Type *scalarType(llvm::DIType *input, llvm::LLVMContext &context, const llvm::DataLayout &layout) {
  auto *type = baseType(input);
  if (auto *basic = llvm::dyn_cast_or_null<llvm::DIBasicType>(type)) {
    auto bits = basic->getSizeInBits();
    if (basic->getEncoding() == llvm::dwarf::DW_ATE_float)
      return bits == 32 ? llvm::Type::getFloatTy(context) : bits == 64 ? llvm::Type::getDoubleTy(context) : nullptr;
    switch (basic->getEncoding()) {
    case llvm::dwarf::DW_ATE_signed: case llvm::dwarf::DW_ATE_unsigned:
    case llvm::dwarf::DW_ATE_signed_char: case llvm::dwarf::DW_ATE_unsigned_char:
    case llvm::dwarf::DW_ATE_boolean:
      if (bits == 8 || bits == 16 || bits == 32 || bits == 64)
        return llvm::IntegerType::get(context, bits);
      break;
    default: break;
    }
  }
  if (auto *pointer = llvm::dyn_cast_or_null<llvm::DIDerivedType>(type))
    if (pointer->getTag() == llvm::dwarf::DW_TAG_pointer_type &&
        pointer->getSizeInBits() == layout.getPointerSizeInBits())
      return llvm::PointerType::get(context, 0);
  return nullptr;
}
llvm::Expected<NativeOverlap> prove(llvm::StructType *storage, llvm::DICompositeType *evidence,
                                  const llvm::DataLayout &layout) {
  if (storage->isOpaque() || storage->isPacked() || storage->getNumElements() != 1 ||
      evidence->getElements().empty() || evidence->getElements().size() > 64)
    return fail("unqualified overlapping-storage layout");
  NativeOverlap result;
  llvm::SmallVector<llvm::Type *> alternatives;
  std::set<std::string> names;
  for (auto *item : evidence->getElements()) {
    auto *member = llvm::dyn_cast<llvm::DIDerivedType>(item);
    auto *type = member ? scalarType(member->getBaseType(), storage->getContext(), layout) : nullptr;
    if (!member || member->getTag() != llvm::dwarf::DW_TAG_member || member->isBitField() ||
        member->getOffsetInBits() || !type || member->getName().empty() ||
        !names.insert(member->getName().str()).second ||
        member->getSizeInBits() != layout.getTypeSizeInBits(type) ||
        (member->getAlignInBits() && member->getAlignInBits() != layout.getABITypeAlign(type).value() * 8))
      return fail("unqualified or contradictory overlap member evidence");
    result.alternatives.push_back({member->getName().str(), type});
    alternatives.push_back(type);
  }
  auto carrier = selectOverlapCarrier(alternatives, layout);
  if (!carrier) return carrier.takeError();
  if (*carrier != storage->getElementType(0) ||
      evidence->getSizeInBits() != layout.getTypeAllocSize(storage) * 8 ||
      layout.getABITypeAlign(*carrier) != layout.getABITypeAlign(storage) ||
      (evidence->getAlignInBits() && evidence->getAlignInBits() != layout.getABITypeAlign(storage).value() * 8))
    return fail("native overlap carrier does not match its semantic layout evidence");
  return result;
}
}

llvm::Expected<NativeOverlaps> discoverNativeOverlaps(llvm::Module &module) {
  NativeOverlaps result;
  auto associate = [&](llvm::Type *native, llvm::DIType *debug) -> llvm::Error {
    auto *composite = llvm::dyn_cast_or_null<llvm::DICompositeType>(baseType(debug));
    if (!composite || composite->getTag() != llvm::dwarf::DW_TAG_union_type)
      return llvm::Error::success();
    auto *storage = llvm::dyn_cast<llvm::StructType>(native);
    if (!storage) return fail("overlap evidence does not describe LLVM record storage");
    auto proven = prove(storage, composite, module.getDataLayout());
    if (!proven) return proven.takeError();
    if (auto previous = result.find(storage); previous != result.end()) {
      auto &a = previous->second.alternatives;
      auto &b = proven->alternatives;
      if (a.size() != b.size()) return fail("conflicting overlap evidence for native storage");
      for (size_t i = 0; i < a.size(); ++i)
        if (a[i].privateName != b[i].privateName || a[i].type != b[i].type)
          return fail("conflicting overlap evidence for native storage");
    } else result[storage] = std::move(*proven);
    return llvm::Error::success();
  };
  for (auto &global : module.globals()) {
    llvm::SmallVector<llvm::DIGlobalVariableExpression *> expressions;
    global.getDebugInfo(expressions);
    for (auto *expression : expressions)
      if (expression->getExpression()->getNumElements() == 0)
        if (auto error = associate(global.getValueType(), expression->getVariable()->getType())) return error;
  }
  for (auto &function : module)
    for (auto &instruction : llvm::instructions(function)) {
      auto *debug = llvm::dyn_cast<llvm::DbgDeclareInst>(&instruction);
      if (!debug || !debug->getAddress() || debug->getExpression()->getNumElements()) continue;
      auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(debug->getAddress());
      if (allocation)
        if (auto error = associate(allocation->getAllocatedType(), debug->getVariable()->getType())) return error;
    }
  return result;
}

llvm::Expected<ir::OverlapType> mergeNativeOverlap(
    const NativeOverlap &left, const NativeOverlap &right,
    mlir::MLIRContext &context, llvm::StringRef identity,
    llvm::StringRef leftTarget, llvm::StringRef rightTarget,
    llvm::function_ref<mlir::Type(llvm::Type *, llvm::Type *)> mergeType) {
  llvm::SmallVector<mlir::Type> alternatives;
  llvm::SmallVector<mlir::Attribute> domains;
  size_t i = 0, j = 0;
  while (i < left.alternatives.size() || j < right.alternatives.size()) {
    auto *a = i < left.alternatives.size() ? &left.alternatives[i] : nullptr;
    auto *b = j < right.alternatives.size() ? &right.alternatives[j] : nullptr;
    bool common = a && b && a->privateName == b->privateName;
    auto appears = [](const auto &entries, size_t from, const std::string &name) {
      for (size_t k = from; k < entries.size(); ++k) if (entries[k].privateName == name) return true;
      return false;
    };
    bool onlyLeft = a && !appears(right.alternatives, j, a->privateName);
    bool onlyRight = b && !appears(left.alternatives, i, b->privateName);
    if (!common && !onlyLeft && !onlyRight)
      return fail("overlap alternatives have conflicting semantic order");
    mlir::Type type = common ? mergeType(a->type, b->type)
        : onlyLeft ? mergeType(a->type, a->type) : mergeType(b->type, b->type);
    if (!type) return fail("unsupported overlap alternative type correspondence");
    alternatives.push_back(type);
    llvm::SmallVector<llvm::StringRef> ids;
    if (common || onlyLeft) ids.push_back(leftTarget);
    if ((common || !onlyLeft) && !llvm::is_contained(ids, rightTarget)) ids.push_back(rightTarget);
    domains.push_back(ir::targetSet(&context, ids));
    if (common || onlyLeft) ++i;
    if (common || (!common && !onlyLeft)) ++j;
  }
  return ir::OverlapType::get(&context, identity, alternatives, mlir::ArrayAttr::get(&context, domains));
}
}
