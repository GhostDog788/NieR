#include "AggregateNormalize.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/ValueTracking.h"

namespace nier::detail {
namespace {
llvm::Error failure(const llvm::Twine &message) {
  return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), message);
}
const llvm::DIType *unqualified(const llvm::DIType *type) {
  for (unsigned depth = 0; type && depth < 64; ++depth) {
    auto *derived = llvm::dyn_cast<llvm::DIDerivedType>(type);
    if (!derived || (derived->getTag() != llvm::dwarf::DW_TAG_typedef &&
                    derived->getTag() != llvm::dwarf::DW_TAG_const_type &&
                    derived->getTag() != llvm::dwarf::DW_TAG_volatile_type &&
                    derived->getTag() != llvm::dwarf::DW_TAG_restrict_type)) return type;
    type = derived->getBaseType();
  }
  return nullptr;
}
bool aggregate(const llvm::DIType *type) {
  auto *record = llvm::dyn_cast_or_null<llvm::DICompositeType>(unqualified(type));
  return record && record->getTag() != llvm::dwarf::DW_TAG_enumeration_type;
}

class LayoutProof {
  llvm::Module &module;
  llvm::DataLayout layout;
  llvm::DenseMap<const llvm::DIType *, llvm::Type *> resolved;
  llvm::DenseSet<const llvm::DIType *> active;
public:
  llvm::SmallVector<llvm::StructType *, 8> ordered;
  explicit LayoutProof(llvm::Module &module) : module(module), layout(module.getDataLayout()) {}

  llvm::Expected<llvm::Type *> resolve(const llvm::DIType *hint, bool parameter = false) {
    hint = unqualified(hint);
    if (!hint) return llvm::Type::getVoidTy(module.getContext());
    if (auto found = resolved.find(hint); found != resolved.end()) return found->second;
    if (active.size() >= 64 || !active.insert(hint).second)
      return failure("recursive or excessive aggregate ABI debug layout");
    struct Pop { llvm::DenseSet<const llvm::DIType *> &set; const llvm::DIType *value;
      ~Pop() { set.erase(value); } } pop{active, hint};
    auto &context = module.getContext();
    if (auto *basic = llvm::dyn_cast<llvm::DIBasicType>(hint)) {
      unsigned bits = basic->getSizeInBits();
      unsigned encoding = basic->getEncoding();
      if (encoding == llvm::dwarf::DW_ATE_float) {
        if (bits == 32) return llvm::Type::getFloatTy(context);
        if (bits == 64) return llvm::Type::getDoubleTy(context);
      } else if (encoding == llvm::dwarf::DW_ATE_signed || encoding == llvm::dwarf::DW_ATE_unsigned ||
                 encoding == llvm::dwarf::DW_ATE_signed_char || encoding == llvm::dwarf::DW_ATE_unsigned_char ||
                 encoding == llvm::dwarf::DW_ATE_boolean) {
        if (parameter && encoding == llvm::dwarf::DW_ATE_boolean) bits = 1;
        if (bits == 1 || bits == 8 || bits == 16 || bits == 32 || bits == 64)
          return llvm::IntegerType::get(context, bits);
      }
      return failure("unqualified scalar encoding in aggregate ABI layout");
    }
    if (auto *derived = llvm::dyn_cast<llvm::DIDerivedType>(hint)) {
      if (derived->getTag() == llvm::dwarf::DW_TAG_pointer_type &&
          derived->getSizeInBits() == layout.getPointerSizeInBits(0) && !derived->getDWARFAddressSpace())
        return llvm::PointerType::get(context, 0);
      return failure("unsupported derived type in native aggregate ABI");
    }
    auto *record = llvm::dyn_cast<llvm::DICompositeType>(hint);
    if (!record) return failure("missing aggregate ABI layout hint");
    if (record->getTag() == llvm::dwarf::DW_TAG_enumeration_type) return resolve(record->getBaseType(), parameter);
    if (record->getTag() == llvm::dwarf::DW_TAG_array_type) {
      auto element = resolve(record->getBaseType());
      if (!element) return element.takeError();
      llvm::Type *result = *element;
      auto dimensions = record->getElements();
      if (dimensions.empty() || dimensions.size() > 16) return failure("unsupported ABI array rank");
      for (unsigned i = dimensions.size(); i > 0; --i) {
        auto entry = dimensions[i - 1];
        auto *range = llvm::dyn_cast<llvm::DISubrange>(entry);
        auto *count = range ? range->getCount().dyn_cast<llvm::ConstantInt *>() : nullptr;
        if (!count || count->isNegative() || count->getValue().getActiveBits() > 20 || count->isZero())
          return failure("aggregate ABI arrays need bounded constant positive extents");
        result = llvm::ArrayType::get(result, count->getZExtValue());
      }
      if (layout.getTypeAllocSizeInBits(result) != record->getSizeInBits())
        return failure("debug array size does not prove native aggregate storage");
      resolved[hint] = result;
      return result;
    }
    if (record->getTag() != llvm::dwarf::DW_TAG_structure_type || record->isForwardDecl())
      return failure("union/class/opaque aggregate ABI needs an explicit additional contract");
    llvm::SmallVector<llvm::Type *> fields;
    llvm::SmallVector<uint64_t> offsets;
    for (auto entry : record->getElements()) {
      auto *member = llvm::dyn_cast<llvm::DIDerivedType>(entry);
      if (!member || member->getTag() != llvm::dwarf::DW_TAG_member || member->isBitField())
        return failure("native aggregate ABI needs ordinary non-bitfield members");
      auto field = resolve(member->getBaseType());
      if (!field) return field.takeError();
      if (!(*field)->isSized() || member->getOffsetInBits() % 8 ||
          layout.getTypeAllocSizeInBits(*field) != member->getSizeInBits())
        return failure("debug member does not prove native aggregate storage");
      fields.push_back(*field); offsets.push_back(member->getOffsetInBits());
    }
    if (fields.empty()) return failure("empty native aggregate ABI record is not qualified");
    llvm::StructType *selected = nullptr;
    for (auto *candidate : module.getIdentifiedStructTypes()) {
      if (candidate->isOpaque() || candidate->isPacked() || candidate->elements() != llvm::ArrayRef<llvm::Type *>(fields)) continue;
      auto *native = layout.getStructLayout(candidate);
      if (native->getSizeInBits() != record->getSizeInBits() ||
          (record->getAlignInBits() && record->getAlignInBits() != layout.getABITypeAlign(candidate).value() * 8)) continue;
      bool matches = true;
      for (unsigned i = 0; i < offsets.size(); ++i) matches &= native->getElementOffsetInBits(i) == offsets[i];
      if (!matches) continue;
      if (selected && selected != candidate) return failure("ambiguous native aggregate storage identity needs an anchor proof");
      selected = candidate;
    }
    if (!selected) return failure("debug record has no matching ordered native storage layout");
    resolved[hint] = selected;
    ordered.push_back(selected);
    return selected;
  }
};
}

llvm::Expected<llvm::SmallVector<LogicalAggregateABI, 8>> discoverAggregateABIs(llvm::Module &module, bool x64) {
  llvm::SmallVector<LogicalAggregateABI, 8> plans;
  for (auto &function : module) {
    if (function.isDeclaration()) continue;
    auto *subprogram = function.getSubprogram();
    auto *signature = subprogram ? subprogram->getType() : nullptr;
    if (!signature) continue;
    auto arguments = signature->getTypeArray();
    if (!arguments.size() || !llvm::any_of(arguments, [](const llvm::DIType *type) { return aggregate(type); })) continue;
    LayoutProof proof(module);
    auto result = proof.resolve(arguments[0], true);
    if (!result) return failure(function.getName() + ": " + llvm::toString(result.takeError()));
    llvm::SmallVector<llvm::Type *> parameters;
    for (unsigned i = 1; i < arguments.size(); ++i) {
      if (!arguments[i] && function.isVarArg() && i + 1 == arguments.size()) continue;
      if (!arguments[i]) return failure("missing fixed logical ABI parameter");
      auto parameter = proof.resolve(arguments[i], true);
      if (!parameter) return failure(function.getName() + ": " + llvm::toString(parameter.takeError()));
      parameters.push_back(*parameter);
    }
    auto *logical = llvm::FunctionType::get(*result, parameters, function.isVarArg());
    auto classification = classifyNativeABI(logical, x64, proof.ordered);
    if (!classification) return failure(function.getName() + ": " + llvm::toString(classification.takeError()));
    if (classification->nativeType != function.getFunctionType())
      return failure(function.getName() + ": logical aggregate ABI classification disagrees with captured native signature");
    LogicalAggregateABI plan;
    plan.function = &function; plan.logicalType = logical;
    plan.native = std::move(*classification); plan.orderedRecords = std::move(proof.ordered);
    for (auto &instruction : llvm::instructions(function)) {
      llvm::DILocalVariable *variable = nullptr;
      llvm::Value *address = nullptr;
      if (auto *declare = llvm::dyn_cast<llvm::DbgDeclareInst>(&instruction)) {
        variable = declare->getVariable();
        if (declare->getExpression()->getNumElements() == 0) address = declare->getAddress();
      } else if (auto *assign = llvm::dyn_cast<llvm::DbgAssignIntrinsic>(&instruction)) {
        variable = assign->getVariable();
        if (assign->getExpression()->getNumElements() == 0 && assign->getAddressExpression()->getNumElements() == 0)
          address = assign->getAddress();
      }
      if (!variable || !address || !variable->isParameter() || !aggregate(variable->getType())) continue;
      unsigned index = variable->getArg() - 1;
      if (index >= parameters.size()) return failure("aggregate debug parameter ordinal is outside logical signature");
      auto inserted = plan.argumentStorage.try_emplace(index, address);
      if (!inserted.second && inserted.first->second != address)
        return failure("aggregate debug parameter has ambiguous owned storage anchors");
    }
    plans.push_back(std::move(plan));
  }
  return plans;
}

namespace {
bool ordinaryMetadata(const llvm::Instruction &instruction) {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>> metadata;
  instruction.getAllMetadataOtherThanDebugLoc(metadata);
  return llvm::all_of(metadata, [](const auto &entry) {
    return entry.first == llvm::LLVMContext::MD_DIAssignID ||
        entry.first == llvm::LLVMContext::MD_tbaa || entry.first == llvm::LLVMContext::MD_tbaa_struct;
  });
}
bool ordinaryAlloca(const llvm::AllocaInst *allocation, llvm::Type *storage) {
  auto *count = allocation ? llvm::dyn_cast<llvm::ConstantInt>(allocation->getArraySize()) : nullptr;
  return allocation && allocation->isStaticAlloca() && allocation->getAllocatedType() == storage &&
      !allocation->getAddressSpace() && !allocation->isUsedWithInAlloca() && !allocation->isSwiftError() &&
      count && count->isOne() && ordinaryMetadata(*allocation);
}
bool atStorageOffset(llvm::Value *pointer, llvm::Value *storage, uint64_t offset,
                     const llvm::DataLayout &layout, llvm::SmallVectorImpl<llvm::Instruction *> &addressShims) {
  if (pointer == storage) return offset == 0;
  auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
  if (!gep || !gep->isInBounds() || !gep->hasAllConstantIndices() || !gep->hasOneUse() ||
      gep->getPointerOperand() != storage || !ordinaryMetadata(*gep)) return false;
  llvm::APInt actual(layout.getIndexSizeInBits(0), 0);
  if (!gep->accumulateConstantOffset(layout, actual) || actual.isNegative() ||
      actual.getLimitedValue() != offset) return false;
  addressShims.push_back(gep);
  return true;
}
}

llvm::Expected<AggregateDefinitionProof> proveAggregateDefinition(LogicalAggregateABI abi) {
  auto &function = *abi.function;
  if (function.isDeclaration()) return failure("aggregate definition proof requires a body");
  auto &context = function.getContext();
  auto &layout = function.getParent()->getDataLayout();
  llvm::DominatorTree dominance(function);
  AggregateDefinitionProof proof;
  proof.abi = std::move(abi);
  bool aggregateResult = proof.abi.logicalType->getReturnType()->isAggregateType();
  if (function.getCallingConv() || function.hasPersonalityFn() || function.hasPrefixData() ||
      function.hasPrologueData() || (aggregateResult && function.getAttributes().getRetAttrs().hasAttributes()))
    return failure(function.getName() + ": aggregate function has an unqualified native return ABI");
  auto qualifiedAttributes = [&](const NativeABIValue &value, bool result) {
    llvm::SmallVector<llvm::Attribute> attributes;
    if (value.kind == NativeABIKind::Indirect) {
      attributes.push_back(llvm::Attribute::get(context, result ? llvm::Attribute::StructRet : llvm::Attribute::ByVal,
                                               value.storageType));
      attributes.push_back(llvm::Attribute::getWithAlignment(context, value.abiAlignment));
      if (result) {
        attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::DeadOnUnwind));
        attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::NoAlias));
        attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::Writable));
      } else attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::NoUndef));
    }
    return llvm::AttributeSet::get(context, attributes);
  };
  if (proof.abi.native.sretIndex &&
      function.getAttributes().getParamAttrs(*proof.abi.native.sretIndex) !=
          qualifiedAttributes(proof.abi.native.result, true))
    return failure(function.getName() + ": sret definition attributes need an explicit additional contract");
  for (const auto &value : proof.abi.native.parameters) {
    if (!value.storageType->isAggregateType()) continue;
    for (unsigned piece = 0; piece < value.nativeCount; ++piece)
      if (function.getAttributes().getParamAttrs(value.nativeBegin + piece) != qualifiedAttributes(value, false))
        return failure(function.getName() + ": aggregate definition attributes differ from the qualified native contract");
  }
  llvm::SmallVector<llvm::Type *> bodyParameters;
  if (aggregateResult) bodyParameters.push_back(llvm::PointerType::get(context, 0));
  for (auto *parameter : proof.abi.logicalType->params())
    bodyParameters.push_back(parameter->isAggregateType() ? llvm::PointerType::get(context, 0) : parameter);
  proof.bodyType = llvm::FunctionType::get(aggregateResult ? llvm::Type::getVoidTy(context) :
      proof.abi.logicalType->getReturnType(), bodyParameters, function.isVarArg());

  for (unsigned index = 0; index < proof.abi.native.parameters.size(); ++index) {
    const auto &value = proof.abi.native.parameters[index];
    if (!value.storageType->isAggregateType()) continue;
    auto *storage = proof.abi.argumentStorage.lookup(index);
    if (!storage) return failure(function.getName() + ": aggregate parameter has no owned storage anchor");
    if (value.kind == NativeABIKind::Indirect) {
      auto *argument = function.getArg(value.nativeBegin);
      if (storage != argument || !argument->hasByValAttr() || argument->getParamByValType() != value.storageType ||
          argument->getParamAlign() != value.abiAlignment)
        return failure(function.getName() + ": aggregate byval anchor disagrees with classified native ownership");
      continue;
    }
    auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(storage);
    if (!ordinaryAlloca(allocation, value.storageType) || allocation->getAlign() != value.storageAlignment)
      return failure(function.getName() + ": aggregate parameter storage is not the qualified entry allocation");
    llvm::SmallVector<llvm::StoreInst *> stores;
    llvm::SmallVector<llvm::Instruction *> localShims;
    for (unsigned piece = 0; piece < value.pieces.size(); ++piece) {
      auto *argument = function.getArg(value.nativeBegin + piece);
      llvm::StoreInst *store = nullptr;
      for (auto *user : argument->users()) {
        if (llvm::isa<llvm::DbgInfoIntrinsic>(user)) continue;
        auto *candidate = llvm::dyn_cast<llvm::StoreInst>(user);
        if (store || !candidate || candidate->getValueOperand() != argument || candidate->isVolatile() ||
            candidate->isAtomic() || candidate->getParent() != &function.getEntryBlock() ||
            !ordinaryMetadata(*candidate) ||
            !atStorageOffset(candidate->getPointerOperand(), storage, value.pieces[piece].offset, layout, localShims) ||
            candidate->getAlign() > llvm::commonAlignment(allocation->getAlign(), value.pieces[piece].offset))
          return failure(function.getName() + ": native aggregate argument has an unmatched piece use");
        store = candidate;
      }
      if (!store || store->getValueOperand()->getType() != value.pieces[piece].type)
        return failure(function.getName() + ": aggregate native entry piece is missing");
      stores.push_back(store); localShims.push_back(store);
    }
    // No body observation or escape may occur until every native entry piece
    // has been materialized. This proves replacing the entire closed shim with
    // an owned-storage body argument does not reorder an observable access.
    for (auto *user : storage->users()) {
      if (llvm::isa<llvm::DbgInfoIntrinsic>(user) || llvm::is_contained(localShims, user)) continue;
      auto *instruction = llvm::dyn_cast<llvm::Instruction>(user);
      if (!instruction || llvm::any_of(stores, [&](auto *store) { return !dominance.dominates(store, instruction); }))
        return failure(function.getName() + ": aggregate storage is observed before its complete native entry shim");
    }
    proof.entryShims.append(localShims.begin(), localShims.end());
    proof.entryShims.push_back(allocation);
  }

  if (aggregateResult && proof.abi.native.sretIndex) {
    auto *argument = function.getArg(*proof.abi.native.sretIndex);
    if (!argument->hasStructRetAttr() || argument->getParamStructRetType() != proof.abi.native.result.storageType ||
        argument->getParamAlign() != proof.abi.native.result.abiAlignment)
      return failure(function.getName() + ": native sret pointer does not prove aggregate result storage");
    proof.resultStorage = argument;
    // Pinned i686 Clang may preserve a dead debug-only sret pointer spill.
    // Remove only a closed single-store allocation, never an observed spill.
    for (auto *user : argument->users()) {
      auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
      auto *allocation = store ? llvm::dyn_cast<llvm::AllocaInst>(store->getPointerOperand()) : nullptr;
      if (!store || store->getValueOperand() != argument || !allocation ||
          !ordinaryAlloca(allocation, argument->getType())) continue;
      if (store->isVolatile() || store->isAtomic() || !ordinaryMetadata(*store) ||
          llvm::any_of(allocation->users(), [&](auto *spillUser) {
            return spillUser != store && !llvm::isa<llvm::DbgInfoIntrinsic>(spillUser);
          })) continue;
      proof.entryShims.push_back(store); proof.entryShims.push_back(allocation);
    }
  }
  for (auto &block : function) {
    auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
    if (!ret) continue;
    proof.returns.push_back(ret);
    if (!aggregateResult || proof.abi.native.sretIndex) continue;
    auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(ret->getReturnValue());
    auto *allocation = load ? llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand()) : nullptr;
    if (!load || !load->hasOneUse() || load->isVolatile() || load->isAtomic() ||
        !ordinaryMetadata(*load) || load->getNextNonDebugInstruction() != ret ||
        !ordinaryAlloca(allocation, proof.abi.native.result.storageType) ||
        allocation->getAlign() != proof.abi.native.result.storageAlignment ||
        load->getAlign() > allocation->getAlign() ||
        (proof.resultStorage && proof.resultStorage != allocation))
      return failure(function.getName() + ": native aggregate return is not a complete qualified storage load");
    proof.resultStorage = allocation;
    proof.resultShims.push_back(load);
  }
  if (aggregateResult && (!proof.resultStorage || proof.returns.empty()))
    return failure(function.getName() + ": aggregate result storage cannot be established");
  if (aggregateResult && !proof.abi.native.sretIndex)
    proof.resultShims.push_back(llvm::cast<llvm::Instruction>(proof.resultStorage));
  return proof;
}

static llvm::SmallVector<LogicalAggregateABI, 8> rewriteAggregateDefinitions(
    llvm::Module &module, llvm::SmallVector<AggregateDefinitionProof, 8> proofs) {
  llvm::SmallVector<LogicalAggregateABI, 8> normalized;
  for (auto &proof : proofs) {
    auto *original = proof.abi.function;
    auto *body = llvm::Function::Create(proof.bodyType, original->getLinkage(), "", &module);
    module.getFunctionList().splice(original->getIterator(), module.getFunctionList(), body->getIterator());
    body->copyAttributesFrom(original);
    body->copyMetadata(original, 0);
    body->takeName(original);
    body->splice(body->end(), original);
    bool aggregateResult = proof.abi.logicalType->getReturnType()->isAggregateType();
    unsigned firstParameter = aggregateResult ? 1 : 0;
    llvm::SmallVector<llvm::AttributeSet> bodyParameterAttributes(firstParameter);
    for (unsigned index = 0; index < proof.abi.native.parameters.size(); ++index) {
      const auto &value = proof.abi.native.parameters[index];
      auto *argument = body->getArg(firstParameter + index);
      if (value.storageType->isAggregateType()) {
        proof.abi.argumentStorage.lookup(index)->replaceAllUsesWith(argument);
        bodyParameterAttributes.emplace_back();
      } else {
        original->getArg(value.nativeBegin)->replaceAllUsesWith(argument);
        bodyParameterAttributes.push_back(original->getAttributes().getParamAttrs(value.nativeBegin));
      }
    }
    body->setAttributes(llvm::AttributeList::get(module.getContext(), original->getAttributes().getFnAttrs(),
        aggregateResult ? llvm::AttributeSet{} : original->getAttributes().getRetAttrs(), bodyParameterAttributes));
    if (aggregateResult) {
      proof.resultStorage->replaceAllUsesWith(body->getArg(0));
      for (auto *ret : proof.returns) {
        auto *replacement = llvm::ReturnInst::Create(module.getContext(), nullptr, ret);
        replacement->setDebugLoc(ret->getDebugLoc());
        ret->eraseFromParent();
      }
    }
    llvm::DenseSet<llvm::Instruction *> shims;
    for (auto *instruction : proof.entryShims) shims.insert(instruction);
    for (auto *instruction : proof.resultShims) shims.insert(instruction);
    for (auto *instruction : shims) instruction->dropAllReferences();
    for (auto *instruction : shims) instruction->eraseFromParent();
    original->replaceAllUsesWith(body);
    original->eraseFromParent();
    proof.abi.function = body;
    proof.abi.argumentStorage.clear();
    for (unsigned index = 0; index < proof.abi.native.parameters.size(); ++index)
      if (proof.abi.native.parameters[index].storageType->isAggregateType())
        proof.abi.argumentStorage[index] = body->getArg(firstParameter + index);
    normalized.push_back(std::move(proof.abi));
  }
  return normalized;
}

llvm::Expected<llvm::SmallVector<LogicalAggregateABI, 8>> normalizeAggregateDefinitions(llvm::Module &module, bool x64) {
  auto discovered = discoverAggregateABIs(module, x64);
  if (!discovered) return discovered.takeError();
  llvm::SmallVector<AggregateDefinitionProof, 8> proofs;
  for (auto &abi : *discovered) {
    auto proof = proveAggregateDefinition(std::move(abi));
    if (!proof) return proof.takeError();
    proofs.push_back(std::move(*proof));
  }
  return rewriteAggregateDefinitions(module, std::move(proofs));
}

namespace {
llvm::StructType *anchoredRecord(llvm::Value *storage, llvm::ArrayRef<llvm::StructType *> ordered) {
  llvm::Type *type = nullptr;
  if (auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(storage)) type = allocation->getAllocatedType();
  if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(storage)) type = global->getValueType();
  if (auto *argument = llvm::dyn_cast<llvm::Argument>(storage)) {
    if (argument->hasByValAttr()) type = argument->getParamByValType();
    if (argument->hasStructRetAttr()) type = argument->getParamStructRetType();
  }
  auto *record = llvm::dyn_cast_or_null<llvm::StructType>(type);
  return record && llvm::is_contained(ordered, record) ? record : nullptr;
}
llvm::Value *storageBase(llvm::Value *address) {
  auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(address);
  return gep && gep->isInBounds() && gep->hasAllConstantIndices() ? gep->getPointerOperand() : address;
}
llvm::MaybeAlign anchorAlignment(llvm::Value *storage, const llvm::DataLayout &layout) {
  if (auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(storage)) {
    if (!ordinaryAlloca(allocation, allocation->getAllocatedType())) return {};
    return allocation->getAlign();
  }
  if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(storage))
    return global->getAlign().valueOrOne();
  if (auto *argument = llvm::dyn_cast<llvm::Argument>(storage)) return argument->getParamAlign();
  return {};
}
llvm::AttributeSet callABIAttributes(llvm::LLVMContext &context, const NativeABIValue &value, bool result) {
  llvm::SmallVector<llvm::Attribute> attributes;
  if (value.kind == NativeABIKind::Indirect) {
    attributes.push_back(llvm::Attribute::get(context, result ? llvm::Attribute::StructRet : llvm::Attribute::ByVal,
                                             value.storageType));
    attributes.push_back(llvm::Attribute::getWithAlignment(context, value.abiAlignment));
    if (result) {
      attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::DeadOnUnwind));
      attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::Writable));
    } else attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::NoUndef));
  }
  return llvm::AttributeSet::get(context, attributes);
}
}

llvm::Expected<llvm::SmallVector<AggregateCallProof, 8>> proveAggregateCalls(
    llvm::Module &module, bool x64, llvm::ArrayRef<llvm::StructType *> ordered) {
  llvm::SmallVector<AggregateCallProof, 8> proofs;
  const auto &layout = module.getDataLayout();
  for (auto &function : module) for (auto &instruction : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
    if (!call || call->getIntrinsicID() || call->getFunctionType()->isVarArg()) continue;
    if (call->getCallingConv() || call->hasOperandBundles() || call->isTailCall()) continue;
    AggregateCallProof proof;
    proof.call = call; proof.orderedRecords.append(ordered.begin(), ordered.end());
    llvm::Type *resultType = call->getType();
    unsigned nativeIndex = 0;
    if (call->arg_size() && call->paramHasAttr(0, llvm::Attribute::StructRet)) {
      proof.resultStorage = call->getArgOperand(0);
      resultType = call->getAttributes().getParamStructRetType(0);
      if (anchoredRecord(proof.resultStorage, ordered) != resultType)
        return failure("native aggregate call sret has no qualified storage anchor");
      nativeIndex = 1;
    } else if (!call->getType()->isVoidTy()) {
      // A complete native return is immediately stored into one qualified
      // object, either as one coercion or as all fields of a native pair.
      llvm::SmallVector<llvm::StoreInst *> resultStores;
      llvm::SmallVector<llvm::Instruction *> resultShims;
      llvm::Value *storage = nullptr;
      bool matched = true;
      for (auto *user : call->users()) {
        if (llvm::isa<llvm::DbgInfoIntrinsic>(user)) continue;
        llvm::Value *piece = call;
        if (call->getType()->isStructTy()) {
          auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(user);
          if (!extract || extract->getNumIndices() != 1 || !extract->hasOneUse() || !ordinaryMetadata(*extract)) { matched = false; break; }
          piece = extract; resultShims.push_back(extract);
          user = *extract->user_begin();
        }
        auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
        if (!store || store->getValueOperand() != piece || store->isVolatile() || store->isAtomic() ||
            !ordinaryMetadata(*store) || store->getParent() != call->getParent()) { matched = false; break; }
        auto *base = storageBase(store->getPointerOperand());
        // The pinned scalar-coercion return template writes the entire owned
        // object directly. A field GEP does not establish that template: an
        // ordinary scalar assignment can use it even when its byte offset is
        // zero and the record has only that field.
        // Split native results have explicit extractvalue pieces and retain
        // their separately checked field-store template below.
        if (!call->getType()->isStructTy() && store->getPointerOperand() != base) { matched = false; break; }
        if (!anchoredRecord(base, ordered) || (storage && storage != base)) { matched = false; break; }
        storage = base;
        resultStores.push_back(store); resultShims.push_back(store);
      }
      if (matched && storage && !resultStores.empty()) {
        auto *candidateType = anchoredRecord(storage, ordered);
        auto candidate = classifyNativeABI(llvm::FunctionType::get(candidateType, false), x64, ordered);
        // A scalar result assigned to one field of a record is not an
        // aggregate return. Establish the complete physical result form
        // before proposing any result-storage normalization. This check is
        // independent of parameter register pressure; the later full-signature
        // and closed-use proofs remain mandatory for an admitted candidate.
        if (!candidate) llvm::consumeError(candidate.takeError());
        else if (!candidate->sretIndex && candidate->nativeType->getReturnType() == call->getType() &&
                 candidate->result.pieces.size() == resultStores.size()) {
          resultType = candidateType;
          proof.resultStorage = storage;
          proof.shims.append(resultShims.begin(), resultShims.end());
        }
      }
    }
    llvm::SmallVector<llvm::Type *> parameters;
    bool anyAggregate = resultType->isAggregateType();
    while (nativeIndex < call->arg_size()) {
      auto *argument = call->getArgOperand(nativeIndex);
      if (call->paramHasAttr(nativeIndex, llvm::Attribute::ByVal)) {
        auto *storageType = call->getAttributes().getParamByValType(nativeIndex);
        if (anchoredRecord(argument, ordered) != storageType)
          return failure("native aggregate call byval has no qualified storage anchor");
        parameters.push_back(storageType); proof.arguments.push_back(argument);
        ++nativeIndex; anyAggregate = true; continue;
      }
      auto *load = llvm::dyn_cast<llvm::LoadInst>(argument);
      auto *storage = load ? storageBase(load->getPointerOperand()) : nullptr;
      auto *record = storage ? anchoredRecord(storage, ordered) : nullptr;
      bool groupMatches = false;
      if (record) {
        auto candidateParameters = parameters;
        candidateParameters.push_back(record);
        auto *candidateType = llvm::FunctionType::get(resultType, candidateParameters, false);
        auto candidate = classifyNativeABI(candidateType, x64, ordered);
        if (!candidate) llvm::consumeError(candidate.takeError());
        else {
          const auto &value = candidate->parameters.back();
          if (value.kind != NativeABIKind::Indirect && value.nativeBegin == nativeIndex &&
              nativeIndex + value.nativeCount <= call->arg_size()) {
            groupMatches = true;
            for (unsigned piece = 0; piece < value.nativeCount; ++piece) {
              auto *part = llvm::dyn_cast<llvm::LoadInst>(call->getArgOperand(nativeIndex + piece));
              llvm::SmallVector<llvm::Instruction *> addresses;
              if (!part || !part->hasOneUse() || part->isVolatile() || part->isAtomic() ||
                  part->getType() != value.pieces[piece].type || !ordinaryMetadata(*part) ||
                  part->getParent() != call->getParent() ||
                  call->getAttributes().getParamAttrs(nativeIndex + piece).hasAttributes() ||
                  !atStorageOffset(part->getPointerOperand(), storage, value.pieces[piece].offset, layout, addresses)) {
                groupMatches = false; break;
              }
            }
            if (groupMatches) {
              parameters.push_back(record); proof.arguments.push_back(storage);
              nativeIndex += value.nativeCount; anyAggregate = true;
            }
          }
        }
      }
      if (!groupMatches) {
        parameters.push_back(argument->getType()); proof.arguments.push_back(argument); ++nativeIndex;
      }
    }
    if (!anyAggregate) continue;
    proof.logicalType = llvm::FunctionType::get(resultType, parameters, false);
    auto native = classifyNativeABI(proof.logicalType, x64, ordered);
    if (!native) return native.takeError();
    if (native->nativeType != call->getFunctionType())
      return failure(function.getName() + ": recovered aggregate call storage does not reproduce its complete native signature for " +
                     (call->getCalledFunction() ? call->getCalledFunction()->getName() : llvm::StringRef("indirect call")));
    proof.native = std::move(*native);
    if (proof.resultStorage && call->getAttributes().getRetAttrs().hasAttributes())
      return failure("aggregate call result has unsupported native attributes");
    if (proof.native.sretIndex &&
        call->getAttributes().getParamAttrs(*proof.native.sretIndex) !=
            callABIAttributes(module.getContext(), proof.native.result, true))
      return failure("native aggregate call sret attributes are not the qualified contract");
    // Discovery above only proposes groups. Recheck every complete descriptor
    // and closed packing graph, including absence of intervening effects.
    llvm::DenseSet<llvm::Instruction *> inputShims, outputShims;
    for (unsigned index = 0; index < proof.native.parameters.size(); ++index) {
      const auto &value = proof.native.parameters[index];
      if (!value.storageType->isAggregateType()) continue;
      auto alignment = anchorAlignment(proof.arguments[index], layout);
      if (!alignment || (value.kind == NativeABIKind::Indirect && *alignment < value.abiAlignment))
        return failure("aggregate call argument storage alignment is unproved");
      for (unsigned piece = 0; piece < value.nativeCount; ++piece)
        if (call->getAttributes().getParamAttrs(value.nativeBegin + piece) !=
            callABIAttributes(module.getContext(), value, false))
          return failure("aggregate call argument has unsupported native attributes");
      if (value.kind == NativeABIKind::Indirect) continue;
      for (unsigned piece = 0; piece < value.nativeCount; ++piece) {
        auto *load = llvm::cast<llvm::LoadInst>(call->getArgOperand(value.nativeBegin + piece));
        if (load->getAlign() > llvm::commonAlignment(*alignment, value.pieces[piece].offset))
          return failure("aggregate native input piece alignment exceeds its proved storage");
        llvm::SmallVector<llvm::Instruction *> addresses;
        if (!atStorageOffset(load->getPointerOperand(), proof.arguments[index], value.pieces[piece].offset, layout, addresses))
          return failure("aggregate call piece address changed during proof");
        inputShims.insert(load);
        for (auto *address : addresses) inputShims.insert(address);
      }
    }
    for (auto *instruction : proof.shims) outputShims.insert(instruction);
    if (proof.resultStorage && !proof.native.sretIndex) {
      auto &value = proof.native.result;
      auto alignment = anchorAlignment(proof.resultStorage, layout);
      if (!alignment) return failure("aggregate result storage alignment is unproved");
      llvm::SmallVector<bool> seen(value.pieces.size(), false);
      for (auto *instruction : proof.shims) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(instruction);
        if (!store) continue;
        unsigned piece = 0;
        if (auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(store->getValueOperand())) piece = extract->getIndices()[0];
        if (piece >= value.pieces.size() || seen[piece] || store->getValueOperand()->getType() != value.pieces[piece].type)
          return failure("aggregate result has missing or duplicate native pieces");
        if (store->getAlign() > llvm::commonAlignment(*alignment, value.pieces[piece].offset))
          return failure("aggregate native result piece alignment exceeds its proved storage");
        seen[piece] = true;
        llvm::SmallVector<llvm::Instruction *> addresses;
        if (!atStorageOffset(store->getPointerOperand(), proof.resultStorage, value.pieces[piece].offset, layout, addresses))
          return failure("aggregate result piece has an incorrect storage offset");
        for (auto *address : addresses) outputShims.insert(address);
      }
      if (!llvm::all_of(seen, [](bool value) { return value; }))
        return failure("aggregate result storage does not receive every native piece");
    }
    for (auto *shim : inputShims) {
      for (auto *next = shim->getNextNode(); next != call; next = next ? next->getNextNode() : nullptr) {
        if (!next || (!inputShims.contains(next) && !llvm::isa<llvm::DbgInfoIntrinsic>(next) &&
                      next->mayHaveSideEffects()))
          return failure("native aggregate call input pack crosses an observable effect");
      }
    }
    for (auto *shim : outputShims) {
      for (auto *next = call->getNextNode(); next != shim; next = next ? next->getNextNode() : nullptr) {
        if (!next || (!outputShims.contains(next) && !llvm::isa<llvm::DbgInfoIntrinsic>(next)))
          return failure("native aggregate call result unpack is not one closed contiguous graph");
      }
    }
    proof.shims.clear();
    for (auto *shim : inputShims) proof.shims.push_back(shim);
    for (auto *shim : outputShims) proof.shims.push_back(shim);
    proofs.push_back(std::move(proof));
  }
  return proofs;
}

llvm::Expected<llvm::SmallVector<AggregateCallProof, 8>> proveAggregateCalls(llvm::Module &module, bool x64) {
  llvm::DebugInfoFinder debug;
  debug.processModule(module);
  LayoutProof layouts(module);
  for (auto *type : debug.types()) {
    // Qualified void (for example the const void * operands of qsort
    // callbacks) has no underlying DIType. It is unrelated to an aggregate
    // boundary and must not be dereferenced while scanning debug hints.
    auto *record = llvm::dyn_cast_or_null<llvm::DICompositeType>(unqualified(type));
    if (!record || record->getTag() != llvm::dwarf::DW_TAG_structure_type) continue;
    auto resolved = layouts.resolve(record);
    if (!resolved) llvm::consumeError(resolved.takeError());
  }
  return proveAggregateCalls(module, x64, layouts.ordered);
}

namespace {
llvm::Expected<llvm::AttributeList> storageAttributes(llvm::LLVMContext &context,
    llvm::AttributeList attributes, llvm::FunctionType *logical, const NativeABISignature &native,
    bool definition) {
  bool result = logical->getReturnType()->isAggregateType();
  llvm::SmallVector<llvm::AttributeSet> parameters(result ? 1 : 0);
  if (result && attributes.getRetAttrs().hasAttributes()) return failure("aggregate result has unqualified scalar attributes");
  if (native.sretIndex) {
    auto expected = callABIAttributes(context, native.result, true);
    if (definition) expected = expected.addAttribute(context, llvm::Attribute::NoAlias);
    if (attributes.getParamAttrs(*native.sretIndex) != expected)
      return failure("sret attributes do not match the exact native boundary contract");
  }
  for (const auto &value : native.parameters) {
    if (value.storageType->isAggregateType()) {
      for (unsigned piece = 0; piece < value.nativeCount; ++piece)
        if (attributes.getParamAttrs(value.nativeBegin + piece) != callABIAttributes(context, value, false))
          return failure("aggregate argument attributes do not match the exact native boundary contract");
      parameters.emplace_back();
    } else parameters.push_back(attributes.getParamAttrs(value.nativeBegin));
  }
  return llvm::AttributeList::get(context, attributes.getFnAttrs(),
      result ? llvm::AttributeSet{} : attributes.getRetAttrs(), parameters);
}
}

llvm::Expected<NormalizedAggregateModule> normalizeNativeAggregates(
    llvm::Module &module, bool x64, const NativeABIInverseHints *inverseHints) {
  llvm::SmallVector<LogicalAggregateABI, 8> discovered;
  if (inverseHints) discovered = inverseHints->definitions;
  else {
    auto definitions = discoverAggregateABIs(module, x64);
    if (!definitions) return definitions.takeError();
    discovered = std::move(*definitions);
  }
  llvm::SmallVector<AggregateDefinitionProof, 8> definitions;
  llvm::DenseMap<llvm::Function *, LogicalAggregateABI> boundaries;
  for (auto &abi : discovered) {
    boundaries[abi.function] = abi;
    if (abi.function->isDeclaration()) continue;
    auto proof = proveAggregateDefinition(std::move(abi));
    if (!proof) return proof.takeError();
    definitions.push_back(std::move(*proof));
  }
  auto calls = inverseHints ? proveAggregateCalls(module, x64, inverseHints->orderedRecords) : proveAggregateCalls(module, x64);
  if (!calls) return calls.takeError();
  llvm::DenseMap<llvm::CallInst *, llvm::AttributeList> callAttributes;
  for (const auto &proof : *calls) {
    auto attributes = storageAttributes(module.getContext(), proof.call->getAttributes(), proof.logicalType, proof.native, false);
    if (!attributes) return attributes.takeError();
    callAttributes[proof.call] = *attributes;
    auto *callee = proof.call->getCalledFunction();
    if (!callee) continue;
    if (auto known = boundaries.find(callee); known != boundaries.end()) {
      if (known->second.logicalType != proof.logicalType)
        return failure("native call sites disagree about their recovered logical aggregate boundary");
    } else {
      if (!callee->isDeclaration())
        return failure("native aggregate callee definition has no complete logical storage proof");
      LogicalAggregateABI abi;
      abi.function = callee; abi.logicalType = proof.logicalType; abi.native = proof.native;
      abi.orderedRecords = proof.orderedRecords;
      boundaries[callee] = std::move(abi);
    }
  }
  llvm::DenseMap<llvm::Function *, llvm::AttributeList> declarationAttributes;
  for (const auto &entry : boundaries) {
    auto *function = entry.first;
    for (auto *user : function->users()) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(user);
      if (call && call->getCalledFunction() == function && !callAttributes.count(call))
        return failure("aggregate function has a native call without a complete storage-boundary proof");
    }
    if (!function->isDeclaration()) continue;
    auto attributes = storageAttributes(module.getContext(), function->getAttributes(), entry.second.logicalType,
                                         entry.second.native, false);
    if (!attributes) return attributes.takeError();
    declarationAttributes[function] = *attributes;
  }
  // Every graph and signature has now been proved. Mutation below contains no
  // speculative matching and cannot turn a failed candidate into a partial IR.
  NormalizedAggregateModule result;
  for (auto &proof : *calls) {
    auto *original = proof.call;
    llvm::SmallVector<llvm::Value *> arguments;
    if (proof.resultStorage) arguments.push_back(proof.resultStorage);
    arguments.append(proof.arguments.begin(), proof.arguments.end());
    auto *call = llvm::CallInst::Create(nativeStorageBodyType(proof.logicalType), original->getCalledOperand(), arguments, "", original);
    call->setAttributes(callAttributes.lookup(original));
    call->setDebugLoc(original->getDebugLoc());
    if (!proof.resultStorage) original->replaceAllUsesWith(call);
    for (auto *shim : proof.shims) shim->dropAllReferences();
    for (auto *shim : proof.shims) shim->eraseFromParent();
    original->eraseFromParent();
    result.calls[call] = proof.logicalType;
  }
  for (auto &entry : boundaries) {
    auto *original = entry.first;
    if (!original->isDeclaration()) continue;
    auto *body = llvm::Function::Create(nativeStorageBodyType(entry.second.logicalType), original->getLinkage(), "", &module);
    module.getFunctionList().splice(original->getIterator(), module.getFunctionList(), body->getIterator());
    body->copyAttributesFrom(original);
    body->copyMetadata(original, 0);
    body->setAttributes(declarationAttributes.lookup(original));
    body->takeName(original);
    original->replaceAllUsesWith(body);
    original->eraseFromParent();
    entry.second.function = body;
    result.functions.push_back(std::move(entry.second));
  }
  auto normalizedDefinitions = rewriteAggregateDefinitions(module, std::move(definitions));
  result.functions.append(std::make_move_iterator(normalizedDefinitions.begin()), std::make_move_iterator(normalizedDefinitions.end()));
  return result;
}
}
