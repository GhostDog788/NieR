#include "NativeABIBridge.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"

namespace sela::detail {
namespace {
llvm::Error failure(const llvm::Twine &message) {
  return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), message);
}
llvm::AttributeSet physicalAttributes(llvm::LLVMContext &context, const NativeABIValue &value,
                                     bool result, bool definition) {
  llvm::SmallVector<llvm::Attribute> attributes;
  if (value.kind == NativeABIKind::Indirect) {
    if (result || value.byVal) {
      attributes.push_back(llvm::Attribute::get(context,
          result ? llvm::Attribute::StructRet : llvm::Attribute::ByVal, value.storageType));
      attributes.push_back(llvm::Attribute::getWithAlignment(context, value.abiAlignment));
    }
    if (result) {
      attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::DeadOnUnwind));
      attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::Writable));
      if (definition) attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::NoAlias));
    } else attributes.push_back(llvm::Attribute::get(context, llvm::Attribute::NoUndef));
  }
  if (!result && value.stackAlignment)
    attributes.push_back(llvm::Attribute::getWithStackAlignment(context, *value.stackAlignment));
  return llvm::AttributeSet::get(context, attributes);
}
llvm::Expected<llvm::AttributeList> physicalAttributeList(llvm::LLVMContext &context,
    llvm::AttributeList body, llvm::FunctionType *logical, const NativeABISignature &native, bool definition) {
  bool result = logical->getReturnType()->isAggregateType();
  unsigned offset = result ? 1 : 0;
  if (result && (body.getRetAttrs().hasAttributes() || body.getParamAttrs(0).hasAttributes()))
    return failure("owned aggregate result argument cannot carry scalar native ABI attributes");
  llvm::SmallVector<llvm::AttributeSet> parameters(native.nativeType->getNumParams());
  if (native.sretIndex) parameters[*native.sretIndex] = physicalAttributes(context, native.result, true, definition);
  for (unsigned index = 0; index < native.parameters.size(); ++index) {
    const auto &value = native.parameters[index];
    auto attributes = body.getParamAttrs(index + offset);
    if (value.storageType->isAggregateType()) {
      if (attributes.hasAttributes()) return failure("owned aggregate input cannot carry scalar native ABI attributes");
      attributes = physicalAttributes(context, value, false, definition);
    }
    for (unsigned piece = 0; piece < value.nativeCount; ++piece) parameters[value.nativeBegin + piece] = attributes;
  }
  return llvm::AttributeList::get(context, body.getFnAttrs(), result ? llvm::AttributeSet{} : body.getRetAttrs(), parameters);
}
}

llvm::FunctionType *nativeStorageBodyType(llvm::FunctionType *logical) {
  auto &context = logical->getContext();
  bool aggregateResult = logical->getReturnType()->isAggregateType();
  llvm::SmallVector<llvm::Type *> parameters;
  if (aggregateResult) parameters.push_back(llvm::PointerType::get(context, 0));
  for (auto *parameter : logical->params())
    parameters.push_back(parameter->isAggregateType() ? llvm::PointerType::get(context, 0) : parameter);
  return llvm::FunctionType::get(aggregateResult ? llvm::Type::getVoidTy(context) : logical->getReturnType(),
                                 parameters, logical->isVarArg());
}

llvm::Expected<llvm::Function *> materializeNativeAggregateDefinition(llvm::Function &body,
    llvm::FunctionType *logical, const NativeABISignature &native,
    llvm::ArrayRef<llvm::StructType *> orderedRecords, NativeABIInverseHints *hints) {
  if (body.getFunctionType() != nativeStorageBodyType(logical) || body.getCallingConv() ||
      native.parameters.size() != logical->getNumParams())
    return failure("aggregate native definition does not match its owned-storage body signature");
  auto attributes = physicalAttributeList(body.getContext(), body.getAttributes(), logical, native, !body.isDeclaration());
  if (!attributes) return attributes.takeError();
  auto &module = *body.getParent();
  auto *function = llvm::Function::Create(native.nativeType, body.getLinkage(), "", &module);
  module.getFunctionList().splice(body.getIterator(), module.getFunctionList(), function->getIterator());
  function->copyAttributesFrom(&body);
  function->copyMetadata(&body, 0);
  function->takeName(&body);
  function->setAttributes(*attributes);
  function->splice(function->end(), &body);
  NativeAggregateDefinition inverse;
  inverse.function = function; inverse.logicalType = logical; inverse.native = native;
  inverse.orderedRecords.append(orderedRecords.begin(), orderedRecords.end());
  if (!function->isDeclaration()) {
    llvm::IRBuilder<llvm::NoFolder> builder(&function->getEntryBlock(), function->getEntryBlock().begin());
    bool aggregateResult = logical->getReturnType()->isAggregateType();
    unsigned offset = aggregateResult ? 1 : 0;
    llvm::Value *resultStorage = nullptr;
    if (aggregateResult) {
      if (native.sretIndex) resultStorage = function->getArg(*native.sretIndex);
      else {
        auto *allocation = builder.CreateAlloca(native.result.storageType);
        allocation->setAlignment(native.result.storageAlignment);
        resultStorage = allocation;
      }
      body.getArg(0)->replaceAllUsesWith(resultStorage);
    }
    for (unsigned index = 0; index < native.parameters.size(); ++index) {
      const auto &value = native.parameters[index];
      llvm::Value *replacement = nullptr;
      if (!value.storageType->isAggregateType() || value.kind == NativeABIKind::Indirect)
        replacement = function->getArg(value.nativeBegin);
      else {
        auto *allocation = builder.CreateAlloca(value.storageType);
        allocation->setAlignment(value.storageAlignment);
        llvm::SmallVector<llvm::Value *> pieces;
        for (unsigned piece = 0; piece < value.nativeCount; ++piece)
          pieces.push_back(function->getArg(value.nativeBegin + piece));
        if (auto error = storeNativeABIPieces(builder, value, allocation, value.storageAlignment, pieces)) return std::move(error);
        replacement = allocation;
      }
      body.getArg(offset + index)->replaceAllUsesWith(replacement);
      if (value.storageType->isAggregateType()) inverse.argumentStorage[index] = replacement;
    }
    if (aggregateResult && !native.sretIndex) {
      llvm::SmallVector<llvm::ReturnInst *> returns;
      for (auto &block : *function)
        if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) returns.push_back(ret);
      for (auto *ret : returns) {
        builder.SetInsertPoint(ret);
        if (module.getDataLayout().getTypeStoreSize(native.nativeType->getReturnType()) <= native.result.storageSize) {
          auto *load = builder.CreateAlignedLoad(native.nativeType->getReturnType(), resultStorage, native.result.storageAlignment);
          builder.CreateRet(load);
        } else {
          auto pieces = loadNativeABIPieces(builder, native.result, resultStorage, native.result.storageAlignment);
          if (!pieces) return pieces.takeError();
          if (pieces->size() != 1) return failure("oversized native return requires one bounded coercion");
          builder.CreateRet(pieces->front());
        }
        ret->eraseFromParent();
      }
    }
  }
  body.replaceAllUsesWith(function);
  body.eraseFromParent();
  if (hints) {
    hints->definitions.push_back(std::move(inverse));
    for (auto *record : orderedRecords)
      if (!llvm::is_contained(hints->orderedRecords, record)) hints->orderedRecords.push_back(record);
  }
  return function;
}

llvm::Expected<llvm::CallInst *> materializeNativeAggregateCall(llvm::CallInst &bodyCall,
    llvm::FunctionType *logical, const NativeABISignature &native) {
  if (bodyCall.getFunctionType() != nativeStorageBodyType(logical) || bodyCall.getCallingConv() ||
      bodyCall.hasOperandBundles() || bodyCall.isTailCall() || logical->isVarArg())
    return failure("aggregate native call does not match its qualified owned-storage signature");
  auto attributes = physicalAttributeList(bodyCall.getContext(), bodyCall.getAttributes(), logical, native, false);
  if (!attributes) return attributes.takeError();
  llvm::IRBuilder<llvm::NoFolder> builder(&bodyCall);
  bool aggregateResult = logical->getReturnType()->isAggregateType();
  unsigned offset = aggregateResult ? 1 : 0;
  llvm::SmallVector<llvm::Value *> arguments;
  if (native.sretIndex) arguments.push_back(bodyCall.getArgOperand(0));
  for (unsigned index = 0; index < native.parameters.size(); ++index) {
    const auto &value = native.parameters[index];
    auto *argument = bodyCall.getArgOperand(index + offset);
    if (value.storageType->isAggregateType() && value.kind == NativeABIKind::Indirect && !value.byVal) {
      // AAPCS64's ordinary indirect pointer does not ask LLVM to make the
      // by-value copy. Preserve the logical argument's owned-value semantics
      // explicitly, including when the caller forwards its own argument.
      auto *function = bodyCall.getFunction();
      llvm::IRBuilder<llvm::NoFolder> allocationBuilder(&function->getEntryBlock(), function->getEntryBlock().begin());
      auto *copy = allocationBuilder.CreateAlloca(value.storageType);
      copy->setAlignment(value.storageAlignment);
      builder.CreateMemCpy(copy, value.storageAlignment, argument, value.storageAlignment,
          builder.getInt64(value.storageSize));
      arguments.push_back(copy);
    } else if (!value.storageType->isAggregateType() || value.kind == NativeABIKind::Indirect) arguments.push_back(argument);
    else {
      auto pieces = loadNativeABIPieces(builder, value, argument, value.storageAlignment);
      if (!pieces) return pieces.takeError();
      arguments.append(pieces->begin(), pieces->end());
    }
  }
  auto *call = builder.CreateCall(native.nativeType, bodyCall.getCalledOperand(), arguments);
  call->setAttributes(*attributes);
  call->setDebugLoc(bodyCall.getDebugLoc());
  if (aggregateResult && !native.sretIndex) {
    llvm::SmallVector<llvm::Value *> pieces;
    if (native.result.pieces.size() > 1)
      for (unsigned index = 0; index < native.result.pieces.size(); ++index) pieces.push_back(builder.CreateExtractValue(call, index));
    else pieces.push_back(call);
    if (auto error = storeNativeABIPieces(builder, native.result, bodyCall.getArgOperand(0), native.result.storageAlignment, pieces))
      return std::move(error);
  } else if (!aggregateResult) bodyCall.replaceAllUsesWith(call);
  bodyCall.eraseFromParent();
  return call;
}
}
