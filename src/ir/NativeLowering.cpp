#include "sela/IR/Compiler.h"
#include "Internal.h"
#include "NativeTargets.h"
#include "NativeTargetConfig.h"
#include "NativeABIBridge.h"
#include "OverlapLayout.h"
#include "ConditionalSpecialization.h"
#include "sela/IR/Dialect.h"
#include "sela/IR/Domains.h"

#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/Bytecode/BytecodeReader.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sela::detail::SELA_NATIVE_NAMESPACE {
namespace {
using mlir::Attribute;
using mlir::Operation;
using llvm::StringRef;

llvm::Error failure(const llvm::Twine &message) {
  return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), message);
}

bool configurationAttribute(StringRef name) {
  return name == "target-cpu" || name == "target-features" ||
         name == "tune-cpu" || name == "min-legal-vector-width";
}

bool validArithmeticFlags(unsigned opcode, unsigned flags) {
  bool overflowing = opcode == llvm::Instruction::Add || opcode == llvm::Instruction::Sub ||
                     opcode == llvm::Instruction::Mul || opcode == llvm::Instruction::Shl;
  return (!(flags & 3) || overflowing) &&
         (!(flags & 4) || llvm::PossiblyExactOperator::isPossiblyExactOpcode(opcode));
}

void configureModule(llvm::Module &module) {
  module.setModuleIdentifier("sela");
  module.setSourceFileName("sela");
  module.setTargetTriple(TargetTriple);
  module.setDataLayout(TargetLayout);
}

class Lowerer {
public:
  llvm::LLVMContext &context;
  std::unique_ptr<llvm::Module> module;
  llvm::IRBuilder<llvm::NoFolder> builder;
  std::string error;
  std::map<std::string, llvm::GlobalValue *> symbols;
  llvm::DenseMap<mlir::Value, llvm::Value *> values;
  llvm::DenseMap<mlir::Block *, llvm::BasicBlock *> blocks;
  llvm::DenseMap<mlir::Type, llvm::Type *> aggregateTypes;
  std::map<std::string, mlir::Type> recordIdentities;
  std::map<std::string, std::pair<mlir::Attribute, llvm::MDNode *>> nativeLoops;
  struct AggregateABI {
    llvm::FunctionType *logical = nullptr;
    detail::NativeABISignature native;
    llvm::SmallVector<llvm::StructType *, 8> orderedRecords;
  };
  llvm::DenseMap<llvm::Function *, AggregateABI> aggregateFunctions;
  llvm::DenseMap<llvm::CallInst *, AggregateABI> aggregateCalls;
  detail::NativeABIInverseHints *inverseHints;

  Lowerer(llvm::LLVMContext &context, detail::NativeABIInverseHints *inverseHints = nullptr)
      : context(context), module(std::make_unique<llvm::Module>("sela", context)), builder(context), inverseHints(inverseHints) {
    configureModule(*module);
  }

  void fail(const llvm::Twine &message) {
    if (error.empty())
      error = message.str();
  }

  llvm::Type *type(mlir::Type input) {
    if (mlir::isa<ir::PointerType>(input))
      return llvm::PointerType::get(context, 0);
    if (mlir::isa<ir::WordType>(input))
      return llvm::IntegerType::get(context, targetInfo().wordBits);
    if (mlir::isa<ir::VaListArgumentType>(input))
      return nativeVaListArgumentType(context);
    if (mlir::isa<ir::VaListType>(input)) {
      if (auto found = aggregateTypes.find(input); found != aggregateTypes.end()) return found->second;
      auto *native = nativeVaListType(context);
      aggregateTypes[input] = native;
      return native;
    }
    if (input.isF32()) return llvm::Type::getFloatTy(context);
    if (input.isF64()) return llvm::Type::getDoubleTy(context);
    if (auto array = mlir::dyn_cast<ir::ArrayType>(input)) {
      auto *element = type(array.getElementType());
      if (!element || array.getNumElements() > (1ULL << 30)) {
        fail("unsupported or oversized native array type"); return nullptr;
      }
      return llvm::ArrayType::get(element, array.getNumElements());
    }
    if (auto overlap = mlir::dyn_cast<ir::OverlapType>(input)) {
      if (auto found = aggregateTypes.find(input); found != aggregateTypes.end()) return found->second;
      auto identity = overlap.getIdentity();
      if (identity.size() < 2 || identity.size() > 64 || !identity.starts_with("r") ||
          !llvm::all_of(identity.drop_front(), [](char c) { return c >= '0' && c <= '9'; })) {
        fail("overlap identities must be opaque r-prefixed integers"); return nullptr;
      }
      auto inserted = recordIdentities.emplace(identity.str(), input);
      if (!inserted.second && inserted.first->second != input) {
        fail("conflicting definitions of a Sela storage identity"); return nullptr;
      }
      auto alternatives = overlap.getAlternatives();
      auto domains = overlap.getDomains();
      if (alternatives.empty() || alternatives.size() > 64 || alternatives.size() != domains.size()) {
        fail("invalid overlap alternative inventory"); return nullptr;
      }
      llvm::SmallVector<llvm::Type *> selected;
      for (unsigned i = 0; i < alternatives.size(); ++i) {
        if (!ir::containsTarget(domains[i], TargetID)) { fail("unspecialized overlap alternative domain"); return nullptr; }
        auto *native = type(alternatives[i]);
        if (!native) return nullptr;
        auto qualified = detail::selectOverlapCarrier({native}, module->getDataLayout());
        if (!qualified) { fail(llvm::toString(qualified.takeError())); return nullptr; }
        selected.push_back(native);
      }
      auto carrier = detail::selectOverlapCarrier(selected, module->getDataLayout());
      if (!carrier) { fail(llvm::toString(carrier.takeError())); return nullptr; }
      auto *storage = llvm::StructType::create(context, {*carrier}, identity);
      aggregateTypes[input] = storage;
      return storage;
    }
    if (auto record = mlir::dyn_cast<ir::RecordType>(input)) {
      if (auto found = aggregateTypes.find(input); found != aggregateTypes.end()) return found->second;
      auto identity = record.getIdentity();
      if (!identity.empty() && (identity.size() > 64 || !identity.starts_with("r") ||
          identity.size() < 2 || !llvm::all_of(identity.drop_front(), [](char c) { return c >= '0' && c <= '9'; }))) {
        fail("record identities must be opaque r-prefixed integers"); return nullptr;
      }
      if (!identity.empty()) {
        auto inserted = recordIdentities.emplace(identity.str(), input);
        if (!inserted.second && inserted.first->second != input) {
          fail("conflicting definitions of a Sela record identity"); return nullptr;
        }
      }
      llvm::SmallVector<llvm::Type *> fields;
      for (auto field : record.getFields()) {
        auto *native = type(field);
        if (!native || !native->isSized()) { fail("record field must have a native storage size"); return nullptr; }
        fields.push_back(native);
      }
      auto *native = identity.empty() ? llvm::StructType::get(context, fields, record.isPacked())
          : llvm::StructType::create(context, fields, identity, record.isPacked());
      aggregateTypes[input] = native;
      return native;
    }
    if (auto integer = mlir::dyn_cast<mlir::IntegerType>(input)) {
      unsigned width = integer.getWidth();
      if (width == 1 || width == 8 || width == 16 || width == 32 || width == 64)
        return llvm::IntegerType::get(context, width);
    }
    fail("artifact contains an unsupported type");
    return nullptr;
  }

  uint64_t expression(Attribute value) {
    auto evaluated = ir::evaluateInteger(value, targetInfo());
    if (!evaluated) { fail(llvm::toString(evaluated.takeError())); return 0; }
    return *evaluated;
  }

  llvm::MaybeAlign alignment(Operation &operation) {
    uint64_t n = expression(operation.getAttr("alignment"));
    if (n == 0 || n > (1ULL << 29) || !llvm::isPowerOf2_64(n)) {
      fail("invalid memory alignment");
      return llvm::MaybeAlign();
    }
    return llvm::Align(n);
  }

  bool pureLiteralInitializer(Attribute value) {
    if (mlir::isa<mlir::IntegerAttr>(value)) return true;
    if (auto number = mlir::dyn_cast<mlir::FloatAttr>(value))
      return number.getType().isF32() || number.getType().isF64();
    if (auto text = mlir::dyn_cast<mlir::StringAttr>(value))
      return text.getValue() == "zero" || text.getValue() == "null";
    if (auto elements = mlir::dyn_cast<mlir::ArrayAttr>(value))
      return elements.size() <= 1024 * 1024 && llvm::all_of(elements, [&](Attribute element) { return pureLiteralInitializer(element); });
    return false;
  }

  llvm::Constant *initializer(llvm::Type *expected, Attribute value) {
    if (auto text = mlir::dyn_cast<mlir::StringAttr>(value)) {
      if (text.getValue() == "zero") return llvm::Constant::getNullValue(expected);
      if (text.getValue() == "undef") return llvm::UndefValue::get(expected);
      if (text.getValue() == "poison") return llvm::PoisonValue::get(expected);
      if (text.getValue() == "null" && expected->isPointerTy())
        return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(expected));
    }
    if (expected->isIntegerTy()) {
      auto number = expression(value);
      if (!error.empty()) return nullptr;
      return llvm::ConstantInt::get(expected, number);
    }
    if (expected->isFloatingPointTy()) {
      auto number = mlir::dyn_cast<mlir::FloatAttr>(value);
      if (!number || type(number.getType()) != expected) { fail("invalid floating initializer"); return nullptr; }
      return llvm::ConstantFP::get(context, number.getValue());
    }
    if (auto reference = mlir::dyn_cast<mlir::DictionaryAttr>(value)) {
      if (auto array = reference.getAs<mlir::ArrayAttr>("array")) {
        auto *nativeType = llvm::dyn_cast<llvm::ArrayType>(expected);
        auto count = reference.get("count");
        uint64_t selected = expression(count);
        if (!error.empty() || reference.size() != 2 || !nativeType ||
            selected != nativeType->getNumElements() || selected != array.size() ||
            array.size() > 1024 * 1024) {
          fail("invalid native-index-domain array initializer"); return nullptr;
        }
        llvm::SmallVector<llvm::Constant *> elements;
        for (unsigned i = 0; i < selected; ++i) {
          auto *element = initializer(nativeType->getElementType(), array[i]);
          if (!element) return nullptr;
          elements.push_back(element);
        }
        return llvm::ConstantArray::get(nativeType, elements);
      }
      if (auto opcode = reference.getAs<mlir::StringAttr>("op")) {
        auto element = reference.getAs<mlir::TypeAttr>("element");
        auto indices = reference.getAs<mlir::ArrayAttr>("indices");
        auto inbounds = reference.getAs<mlir::BoolAttr>("inbounds");
        if (opcode.getValue() != "gep" || reference.size() != 5 || !element || !indices ||
            !inbounds || !reference.get("base") || !expected->isPointerTy()) {
          fail("invalid constant address expression"); return nullptr;
        }
        auto *sourceType = type(element.getValue());
        auto *base = initializer(llvm::PointerType::get(context, 0), reference.get("base"));
        llvm::SmallVector<llvm::Constant *> nativeIndices;
        for (auto entry : indices) {
          auto record = mlir::dyn_cast<mlir::DictionaryAttr>(entry);
          auto kind = record ? record.getAs<mlir::TypeAttr>("type") : mlir::TypeAttr();
          auto *indexType = kind ? type(kind.getValue()) : nullptr;
          if (!record || record.size() != 2 || !indexType || !indexType->isIntegerTy() || !record.get("value")) {
            fail("invalid constant address index"); return nullptr;
          }
          auto *index = initializer(indexType, record.get("value"));
          if (!llvm::isa_and_nonnull<llvm::ConstantInt>(index)) { fail("address index is not a defined integer constant"); return nullptr; }
          nativeIndices.push_back(index);
        }
        llvm::SmallVector<llvm::Value *> checked(nativeIndices.begin(), nativeIndices.end());
        if (!sourceType || !base || nativeIndices.empty() ||
            !llvm::GetElementPtrInst::getIndexedType(sourceType, checked)) {
          fail("invalid constant address index path"); return nullptr;
        }
        return llvm::ConstantExpr::getGetElementPtr(sourceType, base, nativeIndices, inbounds.getValue());
      }
      auto name = reference.getAs<mlir::StringAttr>("symbol");
      auto found = name ? symbols.find(name.getValue().str()) : symbols.end();
      if (reference.size() == 1 && expected->isPointerTy() && found != symbols.end())
        return found->second;
      fail("invalid initializer symbol reference"); return nullptr;
    }
    if (auto elements = mlir::dyn_cast<mlir::ArrayAttr>(value)) {
      uint64_t count = expected->isArrayTy() ? llvm::cast<llvm::ArrayType>(expected)->getNumElements()
          : expected->isStructTy() ? llvm::cast<llvm::StructType>(expected)->getNumElements() : UINT64_MAX;
      if (elements.size() != count || count > 1024 * 1024) { fail("aggregate initializer extent mismatch"); return nullptr; }
      llvm::SmallVector<llvm::Constant *> native;
      for (unsigned i = 0; i < count; ++i) {
        auto *elementType = expected->isArrayTy() ? llvm::cast<llvm::ArrayType>(expected)->getElementType()
            : llvm::cast<llvm::StructType>(expected)->getElementType(i);
        auto *element = initializer(elementType, elements[i]);
        if (!element) return nullptr;
        native.push_back(element);
      }
      return expected->isArrayTy() ? static_cast<llvm::Constant *>(llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(expected), native))
          : llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(expected), native);
    }
    fail("unsupported typed global initializer"); return nullptr;
  }

  llvm::AttributeSet attributes(Attribute value) {
    auto array = mlir::dyn_cast_or_null<mlir::ArrayAttr>(value);
    if (!array) {
      fail("missing ABI attribute array");
      return {};
    }
    llvm::SmallVector<llvm::Attribute> result;
    for (Attribute entry : array) {
      auto dictionary = mlir::dyn_cast<mlir::DictionaryAttr>(entry);
      auto name = dictionary ? dictionary.getAs<mlir::StringAttr>("name") : mlir::StringAttr();
      if (!name) {
        fail("invalid ABI attribute record");
        return {};
      }
      for (auto field : dictionary)
        if (field.getName() != "name" && field.getName() != "string" && field.getName() != "integer") {
          fail("unknown ABI attribute record field"); return {};
        }
      if (dictionary.get("string") && dictionary.get("integer")) {
        fail("ambiguous ABI attribute value"); return {};
      }
      if (auto text = dictionary.getAs<mlir::StringAttr>("string")) {
        if (configurationAttribute(name.getValue())) {
          fail("target-specific compiler attributes are forbidden in the common artifact");
          return {};
        }
        if (name.getValue() != "no-trapping-math" && name.getValue() != "stack-protector-buffer-size" &&
            name.getValue() != "frame-pointer") {
          fail("unsupported string code-generation attribute"); return {};
        }
        result.push_back(llvm::Attribute::get(context, name.getValue(), text.getValue()));
        continue;
      }
      auto kind = llvm::Attribute::getAttrKindFromName(name.getValue());
      if (kind == llvm::Attribute::None || llvm::Attribute::isTypeAttrKind(kind)) {
        fail("unsupported ABI attribute kind");
        return {};
      }
      if (auto integer = dictionary.get("integer")) {
        if (!llvm::Attribute::isIntAttrKind(kind)) {
          fail("ABI integer attribute has the wrong kind");
          return {};
        }
        uint64_t number = expression(integer);
        if (!error.empty()) return {};
        if (!((kind == llvm::Attribute::UWTable && number >= 1 && number <= 2) ||
              (kind == llvm::Attribute::Memory && number <= 63) ||
              kind == llvm::Attribute::AllocSize ||
              ((kind == llvm::Attribute::Alignment || kind == llvm::Attribute::StackAlignment) &&
               number && number <= (1ULL << 29) && llvm::isPowerOf2_64(number)) ||
              kind == llvm::Attribute::Dereferenceable || kind == llvm::Attribute::DereferenceableOrNull)) {
          fail("unsupported integer code-generation attribute or value: " + name.getValue()); return {};
        }
        result.push_back(llvm::Attribute::get(context, kind, number));
      } else {
        if (!llvm::Attribute::isEnumAttrKind(kind)) {
          fail("ABI enum attribute has the wrong kind");
          return {};
        }
        result.push_back(llvm::Attribute::get(context, kind));
      }
    }
    return llvm::AttributeSet::get(context, result);
  }

  llvm::AttributeList attributeList(Operation &operation, unsigned arguments) {
    auto array = operation.getAttrOfType<mlir::ArrayAttr>("attributes");
    if (!array || array.size() != arguments + 2) {
      fail("invalid function/call ABI attribute list");
      return {};
    }
    auto fn = attributes(array[0]), ret = attributes(array[1]);
    if (auto allocation = fn.getAttribute(llvm::Attribute::AllocSize); allocation.isValid()) {
      auto indices = allocation.getAllocSizeArgs();
      if (indices.first >= arguments || (indices.second && *indices.second >= arguments)) {
        fail("allocation-size attribute refers to an unknown argument"); return {};
      }
    }
    llvm::SmallVector<llvm::AttributeSet> params;
    for (unsigned i = 0; i < arguments; ++i)
      params.push_back(attributes(array[i + 2]));
    return llvm::AttributeList::get(context, fn, ret, params);
  }

  llvm::Value *operand(mlir::Value value) {
    auto found = values.find(value);
    if (found == values.end()) {
      fail("unknown or non-dominating common IR value");
      return nullptr;
    }
    return found->second;
  }

  bool visibility(Operation &operation, llvm::GlobalValue &value) {
    auto attribute = operation.getAttr("visibility");
    if (!attribute) return true;
    auto name = mlir::dyn_cast<mlir::StringAttr>(attribute);
    if (!name || (name.getValue() != "default" && name.getValue() != "hidden" &&
                  name.getValue() != "protected") ||
        (value.hasLocalLinkage() && name.getValue() != "default")) {
      fail("invalid native symbol visibility"); return false;
    }
    value.setVisibility(name.getValue() == "hidden" ? llvm::GlobalValue::HiddenVisibility :
        name.getValue() == "protected" ? llvm::GlobalValue::ProtectedVisibility : llvm::GlobalValue::DefaultVisibility);
    return true;
  }

  bool qualifyABIType(mlir::Type input, llvm::SmallVectorImpl<llvm::StructType *> &ordered, unsigned depth = 0) {
    if (depth > 64) { fail("native ABI record nesting exceeds the qualified bound"); return false; }
    if (auto record = mlir::dyn_cast<ir::RecordType>(input)) {
      if (record.isPacked() || record.getFields().empty()) { fail("packed/empty native ABI records require an additional layout contract"); return false; }
      for (auto field : record.getFields()) if (!qualifyABIType(field, ordered, depth + 1)) return false;
      auto *native = llvm::dyn_cast_or_null<llvm::StructType>(type(record));
      if (!native) return false;
      if (!llvm::is_contained(ordered, native)) ordered.push_back(native);
      return true;
    }
    if (auto array = mlir::dyn_cast<ir::ArrayType>(input)) return qualifyABIType(array.getElementType(), ordered, depth + 1);
    auto *native = type(input);
    if (!native || (!native->isIntegerTy() && !native->isPointerTy() && !native->isFloatingPointTy())) {
      fail("native ABI requires explicit ordinary record semantics; overlapping/special storage is not an ordered record"); return false;
    }
    return true;
  }

  bool aggregateABI(Operation &operation, llvm::FunctionType *body, AggregateABI &abi) {
    auto attribute = operation.getAttrOfType<mlir::TypeAttr>("native_abi");
    auto signature = attribute ? mlir::dyn_cast<mlir::FunctionType>(attribute.getValue()) : mlir::FunctionType();
    if (!signature || signature.getNumResults() > 1 || body->isVarArg() || operation.getAttr("intrinsic")) {
      fail("invalid or unsupported aggregate native ABI signature"); return false;
    }
    llvm::SmallVector<llvm::Type *> parameters;
    bool aggregate = false;
    for (auto input : signature.getInputs()) {
      if (!qualifyABIType(input, abi.orderedRecords)) return false;
      auto *native = type(input);
      parameters.push_back(native); aggregate |= native->isAggregateType();
    }
    llvm::Type *returns = llvm::Type::getVoidTy(context);
    if (signature.getNumResults()) {
      auto result = signature.getResult(0);
      if (!qualifyABIType(result, abi.orderedRecords)) return false;
      returns = type(result); aggregate |= returns->isAggregateType();
    }
    abi.logical = llvm::FunctionType::get(returns, parameters, false);
    if (!aggregate || detail::nativeStorageBodyType(abi.logical) != body) {
      fail("native ABI signature disagrees with its owned-storage body arguments/result"); return false;
    }
    auto classified = classifyNativeABI(abi.logical, abi.orderedRecords);
    if (!classified) { fail(llvm::toString(classified.takeError())); return false; }
    abi.native = std::move(*classified);
    return true;
  }

  llvm::Function *function(Operation &operation) {
    auto id = operation.getAttrOfType<mlir::StringAttr>("id");
    auto ft = operation.getAttrOfType<mlir::TypeAttr>("type");
    auto variadic = operation.getAttrOfType<mlir::BoolAttr>("variadic");
    auto internal = operation.getAttrOfType<mlir::BoolAttr>("internal");
    auto weak = operation.getAttrOfType<mlir::BoolAttr>("weak");
    auto available = operation.getAttrOfType<mlir::BoolAttr>("available_externally");
    auto local = operation.getAttrOfType<mlir::BoolAttr>("dso_local");
    auto declaration = operation.getAttrOfType<mlir::BoolAttr>("declaration");
    auto signature = ft ? mlir::dyn_cast<mlir::FunctionType>(ft.getValue()) : mlir::FunctionType();
    if (!id || id.getValue().empty() || !signature || !variadic || !internal ||
        !local || !declaration || signature.getNumResults() > 1 ||
        symbols.count(id.getValue().str()) ||
        (operation.getAttr("weak") && !weak) ||
        (operation.getAttr("available_externally") && !available) ||
        (unsigned(internal.getValue()) + unsigned(weak && weak.getValue()) + unsigned(available && available.getValue()) > 1) ||
        (available && available.getValue() && declaration.getValue())) {
      fail("invalid or duplicate function declaration");
      return nullptr;
    }
    llvm::SmallVector<llvm::Type *> inputs;
    for (auto t : signature.getInputs()) {
      auto *native = type(t);
      if (!native)
        return nullptr;
      if (!native->isIntegerTy() && !native->isFloatingPointTy() && !native->isPointerTy() &&
          !mlir::isa<ir::VaListArgumentType>(t)) {
        fail("aggregate by-value ABI parameters are not qualified yet"); return nullptr;
      }
      inputs.push_back(native);
    }
    llvm::Type *returns = signature.getNumResults() ? type(signature.getResult(0))
                                                   : llvm::Type::getVoidTy(context);
    if (!returns)
      return nullptr;
    if (!returns->isVoidTy() && !returns->isIntegerTy() &&
        !returns->isFloatingPointTy() && !returns->isPointerTy()) {
      fail("aggregate by-value ABI results are not qualified yet"); return nullptr;
    }
    auto *nativeType = llvm::FunctionType::get(returns, inputs, variadic.getValue());
    std::string nativeName = id.getValue().str();
    if (auto attribute = operation.getAttr("intrinsic")) {
      auto name = mlir::dyn_cast<mlir::StringAttr>(attribute);
      llvm::Intrinsic::ID intrinsic = name && name.getValue() == "memcpy" ? llvm::Intrinsic::memcpy :
          name && name.getValue() == "memmove" ? llvm::Intrinsic::memmove :
          name && name.getValue() == "memset" ? llvm::Intrinsic::memset : llvm::Intrinsic::not_intrinsic;
      if (!intrinsic || !declaration.getValue() || variadic.getValue() || internal.getValue() ||
          (weak && weak.getValue()) || inputs.size() != 4 || !inputs[0]->isPointerTy() ||
          !inputs[2]->isIntegerTy() ||
          (intrinsic != llvm::Intrinsic::memset && !inputs[1]->isPointerTy())) {
        fail("invalid native memory intrinsic declaration"); return nullptr;
      }
      llvm::SmallVector<llvm::Type *> overloads{inputs[0]};
      if (intrinsic != llvm::Intrinsic::memset) overloads.push_back(inputs[1]);
      overloads.push_back(inputs[2]);
      if (llvm::Intrinsic::getType(context, intrinsic, overloads) != nativeType) {
        fail("native memory intrinsic signature mismatch"); return nullptr;
      }
      nativeName = llvm::Intrinsic::getName(intrinsic, overloads, module.get(), nativeType);
      if (module->getNamedValue(nativeName)) {
        fail("duplicate native intrinsic specialization"); return nullptr;
      }
    }
    auto *result = llvm::Function::Create(nativeType,
        internal.getValue() ? llvm::GlobalValue::InternalLinkage :
        weak && weak.getValue() ? llvm::GlobalValue::WeakAnyLinkage :
        available && available.getValue() ? llvm::GlobalValue::AvailableExternallyLinkage : llvm::GlobalValue::ExternalLinkage,
        nativeName, module.get());
    result->setDSOLocal(local.getValue());
    if (!visibility(operation, *result)) return nullptr;
    result->setAttributes(attributeList(operation, inputs.size()));
    if (operation.getAttr("native_abi")) {
      AggregateABI abi;
      if (!aggregateABI(operation, nativeType, abi)) return nullptr;
      aggregateFunctions[result] = std::move(abi);
    }
    symbols[id.getValue().str()] = result;
    return result;
  }

  bool shape(Operation &operation, unsigned inputs, unsigned outputs) {
    if (operation.getNumOperands() != inputs || operation.getNumResults() != outputs ||
        operation.getNumRegions()) {
      fail("invalid operation arity: " + operation.getName().getStringRef());
      return false;
    }
    return true;
  }

  llvm::BasicBlock *edge(Operation &operation, unsigned successor,
                         mlir::ValueRange arguments) {
    auto *target = operation.getSuccessor(successor);
    auto found = blocks.find(target);
    if (found == blocks.end() || target->isEntryBlock() ||
        target->getNumArguments() != arguments.size()) {
      fail("invalid branch target or block argument count");
      return nullptr;
    }
    for (unsigned i = 0; i < arguments.size(); ++i) {
      auto *value = operand(arguments[i]);
      auto *phi = llvm::dyn_cast_or_null<llvm::PHINode>(
          values.lookup(target->getArgument(i)));
      if (!value || !phi || phi->getType() != value->getType()) {
        fail("branch block argument type mismatch");
        return nullptr;
      }
      phi->addIncoming(value, builder.GetInsertBlock());
    }
    return found->second;
  }

  void instruction(Operation &operation) {
    StringRef name = operation.getName().getStringRef();
    llvm::Value *result = nullptr;
    if (name == "sela.constant") {
      if (!shape(operation, 0, 1)) return;
      auto *t = type(operation.getResult(0).getType());
      if (!t) return;
      result = initializer(t, operation.getAttr("value"));
      if (!result) return;
    } else if (name == "sela.address") {
      if (!shape(operation, 0, 1)) return;
      auto id = operation.getAttrOfType<mlir::StringAttr>("global");
      auto found = id ? symbols.find(id.getValue().str()) : symbols.end();
      if (found == symbols.end()) {
        fail("address refers to an unknown global or function"); return;
      }
      result = found->second;
    } else if (name == "sela.alloca") {
      if (!shape(operation, 0, 1)) return;
      auto element = operation.getAttrOfType<mlir::TypeAttr>("element");
      auto *t = element ? type(element.getValue()) : nullptr;
      auto align = alignment(operation);
      if (!t || !align) { fail("invalid scalar allocation"); return; }
      auto *allocation = builder.CreateAlloca(t, 0, nullptr);
      allocation->setAlignment(*align);
      result = allocation;
    } else if (name == "sela.gep") {
      auto element = operation.getAttrOfType<mlir::TypeAttr>("element");
      auto inbounds = operation.getAttrOfType<mlir::BoolAttr>("inbounds");
      if (operation.getNumOperands() < 2 || operation.getNumResults() != 1 ||
          operation.getNumRegions() || !element || !inbounds) {
        fail("invalid native address calculation shape"); return;
      }
      auto *sourceType = type(element.getValue());
      auto *pointer = operand(operation.getOperand(0));
      llvm::SmallVector<llvm::Value *> indices;
      for (auto index : operation.getOperands().drop_front()) {
        auto *value = operand(index);
        if (!value || !value->getType()->isIntegerTy()) {
          fail("native address index must be an integer"); return;
        }
        indices.push_back(value);
      }
      if (!sourceType || !pointer || !pointer->getType()->isPointerTy() ||
          !llvm::GetElementPtrInst::getIndexedType(sourceType, indices)) {
        fail("invalid native address element or index path"); return;
      }
      result = builder.CreateGEP(sourceType, pointer, indices, "", inbounds.getValue());
    } else if (name == "sela.bswap") {
      if (!shape(operation, 1, 1)) return;
      auto *value = operand(operation.getOperand(0));
      if (!value || (!value->getType()->isIntegerTy(16) && !value->getType()->isIntegerTy(32) && !value->getType()->isIntegerTy(64))) {
        fail("byte reversal requires a qualified 16/32/64-bit integer"); return;
      }
      result = builder.CreateIntrinsic(llvm::Intrinsic::bswap, {value->getType()}, {value});
    } else if (name == "sela.va_forward") {
      if (!shape(operation, 1, 1)) return;
      auto *state = operand(operation.getOperand(0));
      bool incoming = mlir::isa<ir::VaListArgumentType>(operation.getOperand(0).getType());
      if (!state || (incoming ? state->getType() != nativeVaListArgumentType(context) : !state->getType()->isPointerTy()) ||
          !mlir::isa<ir::VaListArgumentType>(operation.getResult(0).getType())) {
        fail("native va_list forwarding requires a state address or incoming cursor and native argument result"); return;
      }
      result = nativeVaForward(builder, state, incoming);
    } else if (name == "sela.va_arg") {
      if (!shape(operation, 1, 1)) return;
      auto *state = operand(operation.getOperand(0));
      auto *element = type(operation.getResult(0).getType());
      if (!state || !state->getType()->isPointerTy() || !element ||
          (!element->isIntegerTy(32) && !element->isIntegerTy(64) &&
           !element->isPointerTy() && !element->isDoubleTy())) {
        fail("native va_arg currently requires a promoted scalar or pointer result"); return;
      }
      result = nativeVaArg(builder, state, element);
    } else if (name == "sela.load") {
      if (!shape(operation, 1, 1)) return;
      auto *pointer = operand(operation.getOperand(0));
      auto *t = type(operation.getResult(0).getType());
      auto align = alignment(operation);
      auto isVolatile = operation.getAttrOfType<mlir::BoolAttr>("volatile");
      if (!pointer || !t || !align || !pointer->getType()->isPointerTy() ||
          (operation.getAttr("volatile") && !isVolatile)) {
        fail("invalid scalar load"); return;
      }
      result = builder.CreateAlignedLoad(t, pointer, *align, isVolatile && isVolatile.getValue());
    } else if (name == "sela.store") {
      if (!shape(operation, 2, 0)) return;
      auto *value = operand(operation.getOperand(0));
      auto *pointer = operand(operation.getOperand(1));
      auto align = alignment(operation);
      auto isVolatile = operation.getAttrOfType<mlir::BoolAttr>("volatile");
      if (!value || !pointer || !align || !pointer->getType()->isPointerTy() ||
          (operation.getAttr("volatile") && !isVolatile)) {
        fail("invalid scalar store"); return;
      }
      builder.CreateAlignedStore(value, pointer, *align, isVolatile && isVolatile.getValue());
    } else if (name == "sela.call_indirect") {
      auto signature = operation.getAttrOfType<mlir::TypeAttr>("type");
      auto functionType = signature ? mlir::dyn_cast<mlir::FunctionType>(signature.getValue()) : mlir::FunctionType();
      auto variadic = operation.getAttrOfType<mlir::BoolAttr>("variadic");
      auto tail = operation.getAttrOfType<mlir::IntegerAttr>("tail");
      if (operation.getNumRegions() || operation.getNumResults() > 1 || operation.getNumOperands() < 1 ||
          !functionType || functionType.getNumResults() > 1 || !variadic || !tail || tail.getInt() < 0 || tail.getInt() > 3) {
        fail("invalid indirect call signature"); return;
      }
      auto *callee = operand(operation.getOperand(0));
      if (!callee || !callee->getType()->isPointerTy()) { fail("indirect callee is not a native pointer"); return; }
      llvm::SmallVector<llvm::Type *> parameters;
      for (auto parameter : functionType.getInputs()) {
        auto *native = type(parameter);
        if (!native || (!native->isIntegerTy() && !native->isFloatingPointTy() && !native->isPointerTy())) {
          fail("indirect aggregate by-value ABI parameters are not qualified yet"); return;
        }
        parameters.push_back(native);
      }
      auto *returns = functionType.getNumResults() ? type(functionType.getResult(0)) : llvm::Type::getVoidTy(context);
      if (!returns || (!returns->isVoidTy() && !returns->isIntegerTy() && !returns->isFloatingPointTy() && !returns->isPointerTy())) {
        fail("indirect aggregate by-value ABI results are not qualified yet"); return;
      }
      llvm::SmallVector<llvm::Value *> arguments;
      for (auto argument : operation.getOperands().drop_front()) {
        auto *value = operand(argument);
        if (!value) return;
        arguments.push_back(value);
      }
      if (arguments.size() < parameters.size() || (!variadic.getValue() && arguments.size() != parameters.size())) {
        fail("indirect argument count mismatch"); return;
      }
      for (unsigned i = 0; i < parameters.size(); ++i)
        if (arguments[i]->getType() != parameters[i]) { fail("indirect argument type mismatch"); return; }
      if (returns->isVoidTy() != (operation.getNumResults() == 0)) { fail("indirect return arity mismatch"); return; }
      auto *nativeType = llvm::FunctionType::get(returns, parameters, variadic.getValue());
      auto *call = builder.CreateCall(nativeType, callee, arguments);
      call->setAttributes(attributeList(operation, arguments.size()));
      call->setTailCallKind(llvm::CallInst::TailCallKind(tail.getInt()));
      if (operation.getAttr("native_abi")) {
        AggregateABI abi;
        if (!aggregateABI(operation, nativeType, abi)) return;
        aggregateCalls[call] = std::move(abi);
      }
      result = call;
    } else if (name == "sela.call") {
      if (operation.getNumRegions() || operation.getNumResults() > 1) {
        fail("invalid call shape"); return;
      }
      auto callee = operation.getAttrOfType<mlir::StringAttr>("callee");
      auto tail = operation.getAttrOfType<mlir::IntegerAttr>("tail");
      auto found = callee ? symbols.find(callee.getValue().str()) : symbols.end();
      auto *function = found == symbols.end() ? nullptr : llvm::dyn_cast<llvm::Function>(found->second);
      if (!function || !tail || tail.getInt() < 0 || tail.getInt() > 3) {
        fail("invalid direct callee or tail-call kind"); return;
      }
      llvm::SmallVector<llvm::Value *> arguments;
      for (auto argument : operation.getOperands()) {
        auto *value = operand(argument);
        if (!value) return;
        arguments.push_back(value);
      }
      if (arguments.size() < function->arg_size() ||
          (!function->isVarArg() && arguments.size() != function->arg_size())) {
        fail("direct call argument count mismatch"); return;
      }
      for (unsigned i = 0; i < function->arg_size(); ++i)
        if (arguments[i]->getType() != function->getFunctionType()->getParamType(i)) {
          fail("direct call argument type mismatch"); return;
        }
      auto *call = builder.CreateCall(function, arguments);
      call->setAttributes(attributeList(operation, arguments.size()));
      call->setTailCallKind(llvm::CallInst::TailCallKind(tail.getInt()));
      if (function->getReturnType()->isVoidTy() != (operation.getNumResults() == 0)) {
        fail("direct call return arity mismatch"); return;
      }
      auto abi = aggregateFunctions.find(function);
      if (operation.getAttr("native_abi")) {
        AggregateABI explicitABI;
        if (!aggregateABI(operation, function->getFunctionType(), explicitABI)) return;
        if (abi == aggregateFunctions.end() || abi->second.logical != explicitABI.logical) {
          fail("direct call native ABI disagrees with its function declaration"); return;
        }
      }
      if (abi != aggregateFunctions.end()) aggregateCalls[call] = abi->second;
      result = call;
    } else if (name == "sela.br") {
      if (operation.getNumResults() || operation.getNumRegions() ||
          operation.getNumSuccessors() != 1) {
        fail("invalid unconditional branch shape"); return;
      }
      auto *target = edge(operation, 0, operation.getOperands());
      if (!target) return;
      builder.CreateBr(target);
    } else if (name == "sela.cond_br") {
      auto count = operation.getAttrOfType<mlir::IntegerAttr>("true_count");
      if (operation.getNumResults() || operation.getNumRegions() ||
          operation.getNumSuccessors() != 2 || !count || count.getInt() < 0 ||
          operation.getNumOperands() < 1 ||
          uint64_t(count.getInt()) > operation.getNumOperands() - 1) {
        fail("invalid conditional branch shape"); return;
      }
      auto *condition = operand(operation.getOperand(0));
      if (!condition || !condition->getType()->isIntegerTy(1)) {
        fail("conditional branch requires an i1 condition"); return;
      }
      auto arguments = operation.getOperands().drop_front();
      auto *yes = edge(operation, 0, arguments.take_front(count.getInt()));
      auto *no = edge(operation, 1, arguments.drop_front(count.getInt()));
      if (!yes || !no) return;
      builder.CreateCondBr(condition, yes, no);
    } else if (name == "sela.switch") {
      auto cases = operation.getAttrOfType<mlir::ArrayAttr>("cases");
      auto counts = operation.getAttrOfType<mlir::DenseI32ArrayAttr>("argument_counts");
      if (!cases || !counts || operation.getNumOperands() < 1 ||
          operation.getNumResults() || operation.getNumRegions() ||
          operation.getNumSuccessors() != cases.size() + 1 ||
          counts.size() != operation.getNumSuccessors()) {
        fail("invalid switch shape"); return;
      }
      auto *condition = operand(operation.getOperand(0));
      if (!condition || !condition->getType()->isIntegerTy()) {
        fail("switch condition must be an integer"); return;
      }
      auto arguments = operation.getOperands().drop_front();
      llvm::SmallVector<llvm::BasicBlock *> targets;
      for (unsigned i = 0; i < counts.size(); ++i) {
        auto count = counts[i];
        if (count < 0 || unsigned(count) > arguments.size()) {
          fail("invalid switch block argument counts"); return;
        }
        auto *target = edge(operation, i, arguments.take_front(count));
        if (!target) return;
        targets.push_back(target);
        arguments = arguments.drop_front(count);
      }
      if (!arguments.empty()) { fail("undeclared switch operands"); return; }
      auto *result = builder.CreateSwitch(condition, targets.front(), cases.size());
      std::set<uint64_t> unique;
      for (unsigned i = 0; i < cases.size(); ++i) {
        auto number = expression(cases[i]);
        if (!error.empty()) return;
        auto *value = llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(condition->getType()), number);
        if (!unique.insert(value->getZExtValue()).second) {
          fail("duplicate switch case after native specialization"); return;
        }
        result->addCase(value, targets[i + 1]);
      }
    } else if (name == "sela.unreachable") {
      if (!shape(operation, 0, 0)) return;
      builder.CreateUnreachable();
    } else if (name == "sela.return") {
      if (operation.getNumOperands() > 1 || operation.getNumResults() || operation.getNumRegions()) {
        fail("invalid return shape"); return;
      }
      if (operation.getNumOperands()) {
        auto *value = operand(operation.getOperand(0));
        if (!value) return;
        builder.CreateRet(value);
      } else {
        builder.CreateRetVoid();
      }
    } else if (name == "sela.binary") {
      if (!shape(operation, 2, 1)) return;
      auto opcode = operation.getAttrOfType<mlir::StringAttr>("opcode");
      auto flagExpression = operation.getAttr("flags");
      auto flags = expression(flagExpression);
      unsigned code = 0;
      if (opcode)
        for (unsigned i = llvm::Instruction::BinaryOpsBegin; i < llvm::Instruction::BinaryOpsEnd; ++i)
          if (opcode.getValue() == llvm::Instruction::getOpcodeName(i)) code = i;
      auto *left = operand(operation.getOperand(0));
      auto *right = operand(operation.getOperand(1));
      bool floatingOpcode = code == llvm::Instruction::FAdd || code == llvm::Instruction::FSub ||
          code == llvm::Instruction::FMul || code == llvm::Instruction::FDiv ||
          code == llvm::Instruction::FRem;
      if (!code || !error.empty() || flags > 7 ||
          !validArithmeticFlags(code, unsigned(flags)) ||
          !left || !right || left->getType() != right->getType() ||
          (floatingOpcode ? !left->getType()->isFloatingPointTy() : !left->getType()->isIntegerTy())) {
        fail("invalid scalar binary operation"); return;
      }
      auto *binary = llvm::cast<llvm::BinaryOperator>(
          builder.CreateBinOp(llvm::Instruction::BinaryOps(code), left, right));
      if (flags & 1) binary->setHasNoUnsignedWrap();
      if (flags & 2) binary->setHasNoSignedWrap();
      if (flags & 4) binary->setIsExact();
      result = binary;
    } else if (name == "sela.select") {
      if (!shape(operation, 3, 1)) return;
      auto *condition = operand(operation.getOperand(0));
      auto *yes = operand(operation.getOperand(1));
      auto *no = operand(operation.getOperand(2));
      if (!condition || !yes || !no || !condition->getType()->isIntegerTy(1) ||
          yes->getType() != no->getType()) {
        fail("invalid scalar select"); return;
      }
      result = builder.CreateSelect(condition, yes, no);
    } else if (name == "sela.fneg") {
      if (!shape(operation, 1, 1)) return;
      auto *value = operand(operation.getOperand(0));
      if (!value || !value->getType()->isFloatingPointTy()) {
        fail("floating negate requires float or double"); return;
      }
      result = builder.CreateFNeg(value);
    } else if (name == "sela.cast") {
      if (!shape(operation, 1, 1)) return;
      auto opcode = operation.getAttrOfType<mlir::StringAttr>("opcode");
      unsigned code = 0;
      StringRef spelling = opcode ? opcode.getValue() : StringRef();
      bool nativeIdentity = spelling.consume_front("native_");
      if (nativeIdentity && spelling != "zext" && spelling != "sext" && spelling != "trunc") {
        fail("unsupported native cast policy"); return;
      }
      if (opcode)
        for (unsigned i = llvm::Instruction::CastOpsBegin; i < llvm::Instruction::CastOpsEnd; ++i)
          if (spelling == llvm::Instruction::getOpcodeName(i)) code = i;
      auto *input = operand(operation.getOperand(0));
      auto *output = type(operation.getResult(0).getType());
      bool identity = nativeIdentity && input && output && input->getType() == output && output->isIntegerTy();
      if (!code || !input || !output ||
          (!identity && !llvm::CastInst::castIsValid(llvm::Instruction::CastOps(code), input, output))) {
        fail("invalid scalar cast"); return;
      }
      result = identity ? input : builder.CreateCast(llvm::Instruction::CastOps(code), input, output);
    } else if (name == "sela.compare") {
      if (!shape(operation, 2, 1)) return;
      auto predicate = operation.getAttrOfType<mlir::IntegerAttr>("predicate");
      auto *left = operand(operation.getOperand(0));
      auto *right = operand(operation.getOperand(1));
      bool floating = left && left->getType()->isFloatingPointTy();
      int first = floating ? llvm::CmpInst::FIRST_FCMP_PREDICATE : llvm::CmpInst::FIRST_ICMP_PREDICATE;
      int last = floating ? llvm::CmpInst::LAST_FCMP_PREDICATE : llvm::CmpInst::LAST_ICMP_PREDICATE;
      if (!predicate || predicate.getInt() < first || predicate.getInt() > last ||
          !left || !right || left->getType() != right->getType() ||
          (!floating && !left->getType()->isIntegerTy() && !left->getType()->isPointerTy())) {
        fail("invalid scalar comparison"); return;
      }
      result = floating
          ? builder.CreateFCmp(llvm::CmpInst::Predicate(predicate.getInt()), left, right)
          : builder.CreateICmp(llvm::CmpInst::Predicate(predicate.getInt()), left, right);
    } else {
      fail("unknown required common operation: " + name); return;
    }
    if (operation.getAttr("loop_id") && !operation.getAttr("loop")) {
      fail("native loop identity requires loop options"); return;
    }
    if (auto loop = operation.getAttr("loop")) {
      auto options = mlir::dyn_cast<mlir::ArrayAttr>(loop);
      auto identity = operation.getAttrOfType<mlir::StringAttr>("loop_id");
      auto id = identity ? identity.getValue() : StringRef();
      auto *terminator = builder.GetInsertBlock()->getTerminator();
      if (!options || options.empty() || !llvm::isa_and_nonnull<llvm::BranchInst>(terminator) ||
          id.size() < 2 || id.size() > 64 || !id.starts_with("l") ||
          !llvm::all_of(id.drop_front(), [](char c) { return c >= '0' && c <= '9'; })) {
        fail("loop options require a branch terminator and opaque loop identity"); return;
      }
      llvm::SmallVector<llvm::Metadata *> metadata{nullptr};
      for (auto option : options) {
        auto name = mlir::dyn_cast<mlir::StringAttr>(option);
        if (!name || (name.getValue() != "llvm.loop.mustprogress" &&
                      name.getValue() != "llvm.loop.unroll.disable" &&
                      name.getValue() != "llvm.loop.unroll.enable")) {
          fail("unknown loop semantic option"); return;
        }
        metadata.push_back(llvm::MDNode::get(context,
            llvm::MDString::get(context, name.getValue())));
      }
      llvm::MDNode *node;
      if (auto previous = nativeLoops.find(id.str()); previous != nativeLoops.end()) {
        if (previous->second.first != loop) { fail("conflicting options for one native loop identity"); return; }
        node = previous->second.second;
      } else {
        node = llvm::MDNode::getDistinct(context, metadata);
        node->replaceOperandWith(0, node);
        nativeLoops.emplace(id.str(), std::make_pair(loop, node));
      }
      terminator->setMetadata(llvm::LLVMContext::MD_loop, node);
    }
    if (operation.getNumResults()) {
      auto *expected = type(operation.getResult(0).getType());
      if (!result || result->getType() != expected) {
        fail("operation result type mismatch after specialization"); return;
      }
      values[operation.getResult(0)] = result;
    }
  }

  void lower(mlir::ModuleOp source) {
    auto selectedCFG = ir::specializeDomains(source, targetInfo());
    if (!selectedCFG) { fail(llvm::toString(selectedCFG.takeError())); return; }
    source = **selectedCFG;
    auto schema = source->getAttrOfType<mlir::IntegerAttr>("sela.schema");
    if (!schema || schema.getInt() != 1) {
      fail("unsupported common IR schema or profile domain"); return;
    }
    auto flags = source->getAttrOfType<mlir::ArrayAttr>("sela.module_flags");
    if (!flags) flags = mlir::ArrayAttr::get(source.getContext(), {});
    for (Attribute entry : flags) {
      auto record = mlir::dyn_cast<mlir::DictionaryAttr>(entry);
      auto name = record ? record.getAs<mlir::StringAttr>("name") : mlir::StringAttr();
      auto behavior = record ? record.getAs<mlir::IntegerAttr>("behavior") : mlir::IntegerAttr();
      auto value = record ? record.getAs<mlir::IntegerAttr>("value") : mlir::IntegerAttr();
      auto profiles = record ? record.getAs<mlir::ArrayAttr>("targets") : mlir::ArrayAttr();
      if (!name || !behavior || !value || !profiles || behavior.getInt() < 1 || behavior.getInt() > 8 ||
          !ir::containsTarget(profiles, TargetID)) {
        fail("invalid module compilation flag"); return;
      }
      for (auto field : record)
        if (field.getName() != "name" && field.getName() != "behavior" &&
            field.getName() != "value" && field.getName() != "targets") {
          fail("unknown module flag record field"); return;
        }
      bool ordinary = (name.getValue() == "wchar_size" && value.getInt() == 4) ||
                      (name.getValue() == "frame-pointer" && value.getInt() >= 0 && value.getInt() <= 2) ||
                      ((name.getValue() == "PIC Level" || name.getValue() == "PIE Level" ||
                        name.getValue() == "uwtable") && value.getInt() == 2) ||
                      (name.getValue() == "uwtable" && value.getInt() == 1);
      bool abiFlag = nativeModuleFlag(name.getValue(), value.getInt());
      if ((!ordinary && !abiFlag) ||
          module->getModuleFlag(name.getValue())) {
        fail("unsupported or duplicate native module compilation flag"); return;
      }
      module->addModuleFlag(llvm::Module::ModFlagBehavior(behavior.getInt()),
                            name.getValue(), uint32_t(value.getInt()));
    }
    for (auto &operation : source.getBody()->getOperations()) {
      StringRef name = operation.getName().getStringRef();
      if (name == "sela.func") {
        function(operation);
      } else if (name == "sela.global") {
        if (!shape(operation, 0, 0)) return;
        auto id = operation.getAttrOfType<mlir::StringAttr>("id");
        auto bytes = operation.getAttrOfType<mlir::StringAttr>("bytes");
        auto unnamed = operation.getAttrOfType<mlir::IntegerAttr>("unnamed");
        if (!bytes) {
          auto element = operation.getAttrOfType<mlir::TypeAttr>("element");
          auto constant = operation.getAttrOfType<mlir::BoolAttr>("constant");
          auto declaration = operation.getAttrOfType<mlir::BoolAttr>("declaration");
          auto local = operation.getAttrOfType<mlir::BoolAttr>("dso_local");
          auto linkage = operation.getAttrOfType<mlir::StringAttr>("linkage");
          auto *nativeType = element ? type(element.getValue()) : nullptr;
          uint64_t align = expression(operation.getAttr("alignment"));
          std::map<std::string, llvm::GlobalValue::LinkageTypes> linkages = {
              {"external", llvm::GlobalValue::ExternalLinkage}, {"internal", llvm::GlobalValue::InternalLinkage},
              {"private", llvm::GlobalValue::PrivateLinkage}, {"common", llvm::GlobalValue::CommonLinkage},
              {"weak", llvm::GlobalValue::WeakAnyLinkage}};
          auto selected = linkage ? linkages.find(linkage.getValue().str()) : linkages.end();
          if (!error.empty() || !id || id.getValue().empty() || symbols.count(id.getValue().str()) ||
              !nativeType || !nativeType->isSized() || !constant || !declaration || !local || selected == linkages.end() ||
              !operation.getAttr("initializer") || !unnamed || unnamed.getInt() < 0 || unnamed.getInt() > 2 ||
              (align && (align > (1ULL << 29) || !llvm::isPowerOf2_64(align))) || operation.getAttr("bytes")) {
            fail("invalid typed global declaration"); return;
          }
          auto *global = new llvm::GlobalVariable(*module, nativeType, constant.getValue(), selected->second, nullptr, id.getValue());
          global->setDSOLocal(local.getValue());
          if (!visibility(operation, *global)) return;
          if (align) global->setAlignment(llvm::Align(align));
          global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr(unnamed.getInt()));
          symbols[id.getValue().str()] = global;
          continue;
        }
        for (StringRef field : {"element", "initializer", "constant", "declaration", "linkage", "dso_local", "visibility"})
          if (operation.getAttr(field)) { fail("byte global cannot also carry a typed definition"); return; }
        auto align = alignment(operation);
        if (!id || id.getValue().empty() || !bytes || !unnamed ||
            unnamed.getInt() < 0 || unnamed.getInt() > 2 || !align ||
            symbols.count(id.getValue().str())) {
          fail("invalid byte-string global"); return;
        }
        auto *initializer = llvm::ConstantDataArray::getString(context, bytes.getValue(), false);
        auto *global = new llvm::GlobalVariable(*module, initializer->getType(), true,
            llvm::GlobalValue::PrivateLinkage, initializer, id.getValue());
        global->setAlignment(*align);
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr(unnamed.getInt()));
        symbols[id.getValue().str()] = global;
      } else {
        fail("unknown common module operation: " + name); return;
      }
      if (!error.empty()) return;
    }
    for (auto &operation : source.getBody()->getOperations()) {
      if (operation.getName().getStringRef() != "sela.global" || operation.getAttr("bytes")) continue;
      auto id = operation.getAttrOfType<mlir::StringAttr>("id");
      auto declaration = operation.getAttrOfType<mlir::BoolAttr>("declaration");
      auto value = operation.getAttr("initializer");
      if (declaration.getValue()) {
        if (!mlir::isa<mlir::UnitAttr>(value)) { fail("external global declaration has an initializer"); return; }
        continue;
      }
      auto *global = llvm::cast<llvm::GlobalVariable>(symbols.at(id.getValue().str()));
      auto *native = initializer(global->getValueType(), value);
      if (!native) return;
      global->setInitializer(native);
    }
    for (auto &operation : source.getBody()->getOperations()) {
      if (operation.getName().getStringRef() != "sela.func") continue;
      if (operation.getNumRegions() != 1) { fail("invalid function region count"); return; }
      auto declaration = operation.getAttrOfType<mlir::BoolAttr>("declaration");
      auto &region = operation.getRegion(0);
      if (declaration.getValue()) {
        if (!region.empty()) fail("declaration unexpectedly contains code");
        if (!error.empty()) return;
        continue;
      }
      if (region.empty()) { fail("function definition has no entry block"); return; }
      auto id = operation.getAttrOfType<mlir::StringAttr>("id");
      auto *function = llvm::cast<llvm::Function>(symbols[id.getValue().str()]);
      auto &block = region.front();
      if (block.getNumArguments() != function->arg_size() || block.empty()) {
        fail("function body parameter count or terminator mismatch"); return;
      }
      values.clear();
      blocks.clear();
      for (unsigned i = 0; i < block.getNumArguments(); ++i) {
        if (type(block.getArgument(i).getType()) != function->getArg(i)->getType()) {
          fail("function body parameter type mismatch"); return;
        }
        values[block.getArgument(i)] = function->getArg(i);
      }
      for (auto &sourceBlock : region)
        blocks[&sourceBlock] = llvm::BasicBlock::Create(context, "", function);
      for (auto &sourceBlock : region) {
        if (sourceBlock.isEntryBlock()) continue;
        builder.SetInsertPoint(blocks.lookup(&sourceBlock));
        for (auto argument : sourceBlock.getArguments()) {
          auto *nativeType = type(argument.getType());
          if (!nativeType) return;
          values[argument] = builder.CreatePHI(nativeType, 0);
        }
      }
      for (auto &sourceBlock : region) {
        builder.SetInsertPoint(blocks.lookup(&sourceBlock));
        for (auto &child : sourceBlock) {
          if (builder.GetInsertBlock()->getTerminator()) {
            fail("operation follows a block terminator"); return;
          }
          instruction(child);
          if (!error.empty()) return;
        }
        if (!builder.GetInsertBlock()->getTerminator()) {
          fail("block is missing its terminator"); return;
        }
      }
    }
    for (auto &entry : aggregateCalls) {
      auto materialized = detail::materializeNativeAggregateCall(*entry.first, entry.second.logical, entry.second.native);
      if (!materialized) { fail(llvm::toString(materialized.takeError())); return; }
      if (inverseHints)
        for (auto *record : entry.second.orderedRecords)
          if (!llvm::is_contained(inverseHints->orderedRecords, record)) inverseHints->orderedRecords.push_back(record);
    }
    for (auto &entry : aggregateFunctions) {
      auto materialized = detail::materializeNativeAggregateDefinition(*entry.first, entry.second.logical, entry.second.native,
          entry.second.orderedRecords, inverseHints);
      if (!materialized) { fail(llvm::toString(materialized.takeError())); return; }
    }
    // Optimization and code generation must use the declared baseline, not
    // LLVM's implicit defaults or the build machine's capabilities. These
    // attributes are native compiler configuration, not public Sela payload.
    for (auto &function : *module) {
      if (function.isIntrinsic()) continue;
      function.addFnAttr("target-cpu", targetInfo().cpu);
      function.addFnAttr("target-features", targetInfo().features);
    }
    std::string diagnostics;
    llvm::raw_string_ostream stream(diagnostics);
    if (llvm::verifyModule(*module, &stream))
      fail("specialized LLVM verification failed: " + stream.str());
  }
};

} // namespace

llvm::Expected<std::unique_ptr<llvm::Module>> lowerModule(
    mlir::ModuleOp source, llvm::LLVMContext &context, NativeABIInverseHints *inverseHints) {
  Lowerer lowerer(context, inverseHints);
  lowerer.lower(source);
  if (!lowerer.error.empty()) return failure(lowerer.error);
  return std::move(lowerer.module);
}

const NativeTargetBackend &backend() {
  static const NativeTargetBackend value{TargetID, TargetLayout, &lowerModule,
      &classifyNativeABI, &classifyNativeLayoutABI};
  return value;
}
} // namespace sela::detail::SELA_NATIVE_NAMESPACE
