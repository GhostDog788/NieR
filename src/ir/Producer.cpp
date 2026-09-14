#include "sela/Producer/LLVM.h"
#include "Internal.h"
#include "Varargs.h"
#include "ByteSwap.h"
#include "OverlapEvidence.h"
#include "ConditionalCFG.h"
#include "AggregateNormalize.h"
#include "sela/IR/Dialect.h"
#include "sela/IR/Domains.h"
#include "sela/IR/Intrinsics.h"
#include "CommonMerge.h"
#include "InstructionContracts.h"

#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/Bytecode/BytecodeReader.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace sela {
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

unsigned arithmeticFlags(const llvm::BinaryOperator &operation) {
  unsigned flags = 0;
  if (auto *overflow = llvm::dyn_cast<llvm::OverflowingBinaryOperator>(&operation))
    flags |= (overflow->hasNoUnsignedWrap() ? 1 : 0) |
             (overflow->hasNoSignedWrap() ? 2 : 0);
  if (auto *exact = llvm::dyn_cast<llvm::PossiblyExactOperator>(&operation))
    flags |= exact->isExact() ? 4 : 0;
  return flags;
}

bool validArithmeticFlags(unsigned opcode, unsigned flags) {
  bool overflowing = opcode == llvm::Instruction::Add || opcode == llvm::Instruction::Sub ||
                     opcode == llvm::Instruction::Mul || opcode == llvm::Instruction::Shl;
  return (!(flags & 3) || overflowing) &&
         (!(flags & 4) || llvm::PossiblyExactOperator::isPossiblyExactOpcode(opcode));
}

bool correspondingInstructionKind(const llvm::Instruction &left, const llvm::Instruction &right) {
  if (left.getOpcode() == right.getOpcode()) return true;
  // These alternatives have the same explicit operand/result contract. The
  // selected opcode remains public semantics and is independently inverted.
  return (llvm::isa<llvm::BinaryOperator>(left) && llvm::isa<llvm::BinaryOperator>(right)) ||
      (llvm::isa<llvm::CastInst>(left) && llvm::isa<llvm::CastInst>(right));
}

bool debugFunction(const llvm::Function &function) {
  return function.getName().starts_with("llvm.dbg.");
}

bool byteSwapPrimitive(const llvm::Function &function) {
  if (function.getIntrinsicID() != llvm::Intrinsic::bswap || !function.isDeclaration() ||
      function.getAttributes() != llvm::Intrinsic::getAttributes(function.getContext(), llvm::Intrinsic::bswap) ||
      function.getCallingConv() || function.hasSection() || function.hasComdat() ||
      function.getVisibility() != llvm::GlobalValue::DefaultVisibility || function.isDSOLocal() ||
      function.getLinkage() != llvm::GlobalValue::ExternalLinkage) return false;
  auto *type = function.getReturnType();
  return type->isIntegerTy(16) || type->isIntegerTy(32) || type->isIntegerTy(64);
}

bool memoryIntrinsic(const llvm::Function &function) {
  auto id = function.getIntrinsicID();
  return id == llvm::Intrinsic::memcpy || id == llvm::Intrinsic::memmove ||
         id == llvm::Intrinsic::memset;
}

bool debugInstruction(const llvm::Instruction &instruction) {
  return llvm::isa<llvm::DbgInfoIntrinsic>(instruction);
}

bool hasAssembly(const llvm::Module &module) {
  if (!module.getModuleInlineAsm().empty()) return true;
  for (const auto &function : module) for (const auto &instruction : llvm::instructions(function))
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction); call && call->isInlineAsm()) return true;
  return false;
}

std::vector<const llvm::Instruction *> captureInstructions(const llvm::Function &f) {
  std::vector<const llvm::Instruction *> result;
  for (const auto &instruction : llvm::instructions(f))
    if (!debugInstruction(instruction))
      result.push_back(&instruction);
  return result;
}

std::string valueText(const llvm::Value &value) {
  std::string result;
  llvm::raw_string_ostream stream(result);
  value.print(stream);
  return result;
}

// Only debug/TBAA are discarded. TBAA is an optional optimization aid, not a
// replacement for the represented load/store behavior. No performance parity
// claim is made by this first checkpoint.
bool permittedMetadata(const llvm::Instruction &instruction) {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>> metadata;
  instruction.getAllMetadataOtherThanDebugLoc(metadata);
  for (const auto &entry : metadata)
    if (entry.first != llvm::LLVMContext::MD_tbaa &&
        entry.first != llvm::LLVMContext::MD_tbaa_struct &&
        entry.first != llvm::LLVMContext::MD_DIAssignID &&
        entry.first != llvm::LLVMContext::MD_loop &&
        entry.first != instruction.getContext().getMDKindID("srcloc"))
      return false;
  return true;
}

void configureModule(llvm::Module &module, const targets::TargetInfo &target) {
  module.setModuleIdentifier("sela");
  module.setSourceFileName("sela");
  module.setTargetTriple(target.triple);
  module.setDataLayout(target.layout);
}

using ModuleFlags = std::map<std::string, std::pair<unsigned, uint64_t>>;
ModuleFlags moduleFlags(const llvm::Module &module) {
  ModuleFlags result;
  llvm::SmallVector<llvm::Module::ModuleFlagEntry> flags;
  module.getModuleFlagsMetadata(flags);
  for (const auto &flag : flags) {
    StringRef name = flag.Key->getString();
    if (name == "Dwarf Version" || name == "Debug Info Version" ||
        name == "debug-info-assignment-tracking") continue;
    if (auto *value = llvm::mdconst::dyn_extract<llvm::ConstantInt>(flag.Val))
      result[name.str()] = {unsigned(flag.Behavior), value->getZExtValue()};
  }
  return result;
}

void canonicalizeFlags(llvm::Module &module) {
  auto flags = moduleFlags(module);
  if (auto *old = module.getModuleFlagsMetadata()) module.eraseNamedMetadata(old);
  for (const auto &[name, value] : flags)
    module.addModuleFlag(llvm::Module::ModFlagBehavior(value.first), name,
                         uint32_t(value.second));
}

llvm::Error validateCapture(const llvm::Module &module, const targets::TargetInfo &target) {
  llvm::Triple triple(module.getTargetTriple());
  llvm::Triple expectedTriple(target.triple);
  if (!triple.isOSLinux() ||
      triple.getArch() != expectedTriple.getArch() || triple.getSubArch() != expectedTriple.getSubArch() ||
      triple.getEnvironment() != expectedTriple.getEnvironment())
    return failure("capture has the wrong qualified Linux CPU/ABI profile");
  if (module.getDataLayoutStr() != target.layout)
    return failure("capture data layout is not the pinned LLVM 18 profile");
  if (!module.getModuleInlineAsm().empty() || !module.alias_empty() ||
      !module.ifunc_empty())
    return failure("module assembly, aliases and ifuncs are not supported yet");
  for (const auto &function : module)
    for (const auto *name : {"target-cpu", "target-features", "tune-cpu", "min-legal-vector-width"}) {
      auto value = function.getFnAttribute(name);
      if (value.isValid() && !value.isStringAttribute())
        return failure("invalid capture code-generation attribute: " + llvm::Twine(name));
    }
  const std::map<std::string, uint64_t> expected = {
      {"NumRegisterParameters", 0}, {"wchar_size", 4}, {"PIC Level", 2},
      {"PIE Level", 2}, {"uwtable", 2}, {"min_enum_size", 4}};
  llvm::SmallVector<llvm::Module::ModuleFlagEntry> flags;
  module.getModuleFlagsMetadata(flags);
  for (const auto &flag : flags) {
    StringRef key = flag.Key->getString();
    if (key == "Dwarf Version" || key == "Debug Info Version" ||
        key == "debug-info-assignment-tracking")
      continue;
    auto found = expected.find(key.str());
    auto *number = llvm::mdconst::dyn_extract<llvm::ConstantInt>(flag.Val);
    if (key == "frame-pointer" && number && number->getZExtValue() <= 2)
      continue;
    if (key == "uwtable" && number && (number->getZExtValue() == 1 || number->getZExtValue() == 2)) continue;
    if (found == expected.end() || !number ||
        number->getZExtValue() != found->second)
      return failure("unsupported module flag: " + key);
  }
  for (const auto &named : module.named_metadata())
    if (named.getName() != "llvm.module.flags" &&
        named.getName() != "llvm.ident" &&
        named.getName() != "llvm.dbg.cu")
      return failure("unsupported named module metadata: " + named.getName());
  return llvm::Error::success();
}

// Full instruction semantics remain in this comparison; source spellings,
// debug/TBAA and the explicitly pinned target configuration do not.
void canonicalize(llvm::Module &module, const targets::TargetInfo &target) {
  const bool retainSymbolNames = hasAssembly(module);
  llvm::StripDebugInfo(module);
  if (auto *ident = module.getNamedMetadata("llvm.ident"))
    module.eraseNamedMetadata(ident);
  std::vector<llvm::Function *> erase;
  std::vector<llvm::Function *> intrinsicDeclarations;
  unsigned functionIndex = 0;
  for (auto &function : module) {
    if (debugFunction(function)) {
      erase.push_back(&function);
      continue;
    }
    if (function.isDeclaration() && function.isIntrinsic()) intrinsicDeclarations.push_back(&function);
    if (byteSwapPrimitive(function)) continue;
    if (function.hasLocalLinkage() && !retainSymbolNames)
      function.setName("f" + std::to_string(functionIndex));
    if (!function.isDeclaration()) ++functionIndex;
    for (const auto &[name, baseline] : std::vector<std::pair<StringRef, StringRef>>{
           {"target-cpu", target.cpu}, {"target-features", target.features},
           {"tune-cpu", "generic"}, {"min-legal-vector-width", "0"}})
      if (auto value = function.getFnAttribute(name); value.isStringAttribute() && value.getValueAsString() == baseline)
        function.removeFnAttr(name);
    for (auto &argument : function.args())
      argument.setName("");
    llvm::DenseMap<const llvm::BasicBlock *, unsigned> blockOrder;
    unsigned blockIndex = 0;
    for (auto &block : function) blockOrder[&block] = blockIndex++;
    for (auto &block : function) {
      block.setName("");
      for (auto &instruction : block) {
        instruction.setName("");
        instruction.setMetadata(llvm::LLVMContext::MD_tbaa, nullptr);
        instruction.setMetadata(llvm::LLVMContext::MD_tbaa_struct, nullptr);
        instruction.setMetadata("srcloc", nullptr);
        if (auto *loop = instruction.getMetadata(llvm::LLVMContext::MD_loop);
            loop && loop->getNumOperands() == 1 && loop->getOperand(0).get() == loop)
          instruction.setMetadata(llvm::LLVMContext::MD_loop, nullptr);
        // Phi incoming-list order is not semantic. Branch lowering naturally
        // visits predecessors in block order, so compare both sides in that
        // same order without changing any incoming value/predecessor pair.
        if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
          std::vector<std::pair<llvm::BasicBlock *, llvm::Value *>> incoming;
          for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
            incoming.emplace_back(phi->getIncomingBlock(i), phi->getIncomingValue(i));
          llvm::stable_sort(incoming, [&](const auto &a, const auto &b) {
            return blockOrder.lookup(a.first) < blockOrder.lookup(b.first);
          });
          for (unsigned i = 0; i < incoming.size(); ++i) {
            phi->setIncomingBlock(i, incoming[i].first);
            phi->setIncomingValue(i, incoming[i].second);
          }
        }
      }
    }
  }
  for (auto *function : erase)
    function->eraseFromParent();
  // ABI-owned copies can introduce a shared memory intrinsic earlier in one
  // observation. Declaration placement has no executable semantics: retain
  // every signature, attribute and use, but compare existing intrinsic
  // declarations in one order without moving any function definition.
  llvm::sort(intrinsicDeclarations, [](auto *a, auto *b) { return a->getName() < b->getName(); });
  for (auto *function : intrinsicDeclarations)
    module.getFunctionList().splice(module.end(), module.getFunctionList(), function->getIterator());
  unsigned globalIndex = 0;
  for (auto &global : module.globals()) {
    if (global.hasLocalLinkage() && !retainSymbolNames) global.setName("g" + std::to_string(globalIndex));
    ++globalIndex;
  }
  configureModule(module, target);
  canonicalizeFlags(module);
}

std::string moduleText(const llvm::Module &module) {
  std::string result;
  llvm::raw_string_ostream stream(result);
  module.print(stream, nullptr);
  return result;
}

class LLVMImporter {
public:
  mlir::MLIRContext context;
  mlir::OpBuilder builder;
  mlir::OwningOpRef<mlir::ModuleOp> module;
  std::string error;
  llvm::DenseMap<const llvm::Value *, mlir::Value> values;
  llvm::DenseMap<const llvm::Value *, mlir::Value> domainValues;
  std::optional<detail::ConditionalCFG> conditionalCFG;
  unsigned singleDomain = 0;
  llvm::DenseMap<std::pair<const llvm::Value *, const llvm::Value *>, mlir::Value> conditionalValues;
  llvm::DenseMap<const llvm::Value *, const llvm::Value *> pairs;
  llvm::DenseMap<const llvm::GlobalValue *, std::string> symbols;
  llvm::DenseMap<const llvm::BasicBlock *, mlir::Block *> blocks;
  llvm::DenseMap<const llvm::BasicBlock *, const llvm::BasicBlock *> blockPairs;
  llvm::DenseMap<llvm::Type *, mlir::Type> storageTypes;
  llvm::DenseMap<llvm::Type *, llvm::Type *> storageCorrespondences;
  llvm::DenseMap<llvm::StructType *, std::string> leftRecordIDs, rightRecordIDs;
  llvm::DenseMap<const llvm::MDNode *, std::string> loopIDs;
  llvm::DenseMap<const llvm::MDNode *, const llvm::MDNode *> loopPairs, reverseLoopPairs;
  const detail::NativeVarargs &leftVarargs, &rightVarargs;
  const detail::NativeOverlaps &leftOverlaps, &rightOverlaps;
  const detail::NormalizedAggregateModule &leftAggregates, &rightAggregates;
  llvm::DenseMap<const llvm::Function *, llvm::FunctionType *> leftNativeABIs, rightNativeABIs;
  const targets::TargetInfo &leftTarget, &rightTarget;

  LLVMImporter(const detail::NativeVarargs &varargs, const detail::NativeOverlaps &overlaps,
         const detail::NormalizedAggregateModule &aggregates, const targets::TargetInfo &target)
      : builder(&context), leftVarargs(varargs), rightVarargs(varargs),
        leftOverlaps(overlaps), rightOverlaps(overlaps), leftAggregates(aggregates), rightAggregates(aggregates),
        leftTarget(target), rightTarget(target) {
    context.getOrLoadDialect<ir::SelaDialect>();
    module = mlir::ModuleOp::create(builder.getUnknownLoc());
    (*module)->setAttr("sela.schema", builder.getI32IntegerAttr(1));
    (*module)->setAttr("sela.targets", ir::targetSet(&context, leftTarget.id == rightTarget.id
        ? llvm::ArrayRef<StringRef>{leftTarget.id} : llvm::ArrayRef<StringRef>{leftTarget.id, rightTarget.id}));
    builder.setInsertionPointToEnd(module->getBody());
    for (const auto &function : aggregates.functions) {
      leftNativeABIs[function.function] = function.logicalType;
      rightNativeABIs[function.function] = function.logicalType;
    }
  }

  void fail(const llvm::Twine &message) {
    if (error.empty())
      error = message.str();
  }

  Operation *op(StringRef name, mlir::TypeRange results = {},
                mlir::ValueRange operands = {},
                llvm::ArrayRef<mlir::NamedAttribute> attributes = {},
                bool region = false) {
    mlir::OperationState state(builder.getUnknownLoc(), name);
    state.addTypes(results);
    state.addOperands(operands);
    state.addAttributes(attributes);
    if (region)
      state.addRegion();
    return builder.create(state);
  }

  mlir::NamedAttribute attr(StringRef name, Attribute value) {
    return builder.getNamedAttr(name, value);
  }

  void nativeABI(Operation *operation, llvm::FunctionType *left, llvm::FunctionType *right) {
    if (!left && !right) return;
    if (!left || !right || left->getNumParams() != right->getNumParams() || left->isVarArg() || right->isVarArg()) {
      fail("logical native aggregate ABI differs between profiles"); return;
    }
    llvm::SmallVector<mlir::Type> inputs, results;
    for (unsigned index = 0; index < left->getNumParams(); ++index) {
      auto parameter = type(left->getParamType(index), right->getParamType(index));
      if (!parameter) return;
      inputs.push_back(parameter);
    }
    auto result = type(left->getReturnType(), right->getReturnType());
    if (!result) return;
    if (!mlir::isa<mlir::NoneType>(result)) results.push_back(result);
    operation->setAttr("native_abi", mlir::TypeAttr::get(builder.getFunctionType(inputs, results)));
  }

  mlir::Type type(llvm::Type *left, llvm::Type *right) {
    if (singleDomain && left->isStructTy()) {
      if (auto found = storageTypes.find(left); found != storageTypes.end()) return found->second;
      for (const auto &entry : storageCorrespondences)
        if (entry.second == left) return storageTypes.lookup(entry.first);
      fail("conditional-only aggregate storage requires additional semantic layout evidence"); return {};
    }
    if (left->isVoidTy() && right->isVoidTy())
      return builder.getNoneType();
    if (left->isPointerTy() && right->isPointerTy() &&
        left->getPointerAddressSpace() == 0 && right->getPointerAddressSpace() == 0)
      return ir::PointerType::get(&context);
    if (left->isFloatTy() && right->isFloatTy()) return builder.getF32Type();
    if (left->isDoubleTy() && right->isDoubleTy()) return builder.getF64Type();
    if (auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(left)) {
      auto *other = llvm::dyn_cast<llvm::FixedVectorType>(right);
      if (!other || vector->getNumElements() != other->getNumElements() || vector->getNumElements() > 1024) {
        fail("unsupported vector type correspondence"); return {};
      }
      auto element = type(vector->getElementType(), other->getElementType());
      if (!element) return {};
      return mlir::VectorType::get({int64_t(vector->getNumElements())}, element);
    }
    if (auto *array = llvm::dyn_cast<llvm::ArrayType>(left)) {
      auto *other = llvm::dyn_cast<llvm::ArrayType>(right);
      if (!other) { fail("array storage kinds differ between profiles"); return {}; }
      auto element = type(array->getElementType(), other->getElementType());
      if (!element) return {};
      return ir::ArrayType::get(&context, element, expression(array->getNumElements(), other->getNumElements()));
    }
    if (auto *record = llvm::dyn_cast<llvm::StructType>(left)) {
      auto *other = llvm::dyn_cast<llvm::StructType>(right);
      if (!other || record->isOpaque() || other->isOpaque()) {
        fail("aggregate storage shapes need additional layout normalization"); return {};
      }
      if (auto found = storageTypes.find(left); found != storageTypes.end()) {
        if (storageCorrespondences.lookup(left) != right) {
          fail("record type correspondence changes within a module"); return {};
        }
        return found->second;
      }
      auto leftOverlap = leftOverlaps.find(record), rightOverlap = rightOverlaps.find(other);
      if (leftOverlap != leftOverlaps.end() || rightOverlap != rightOverlaps.end()) {
        if (leftOverlap == leftOverlaps.end() || rightOverlap == rightOverlaps.end() ||
            record->isLiteral() || other->isLiteral() || rightRecordIDs.count(other)) {
          fail("overlapping storage requires matching unique semantic layout evidence"); return {};
        }
        auto identity = "r" + std::to_string(leftRecordIDs.size());
        auto overlap = detail::mergeNativeOverlap(leftOverlap->second, rightOverlap->second, context, identity,
            leftTarget.id, rightTarget.id,
            [&](llvm::Type *a, llvm::Type *b) { return type(a, b); });
        if (!overlap) { fail(llvm::toString(overlap.takeError())); return {}; }
        leftRecordIDs[record] = identity;
        rightRecordIDs[other] = identity;
        storageTypes[left] = *overlap;
        storageCorrespondences[left] = right;
        return *overlap;
      }
      if (record->isPacked() != other->isPacked() || record->isLiteral() != other->isLiteral() ||
          record->getNumElements() != other->getNumElements()) {
        fail("aggregate storage shapes need additional layout normalization"); return {};
      }
      std::string identity;
      if (!record->isLiteral()) {
        identity = "r" + std::to_string(leftRecordIDs.size());
        if (rightRecordIDs.count(other)) { fail("record correspondence is not one-to-one"); return {}; }
        leftRecordIDs[record] = identity;
        rightRecordIDs[other] = identity;
      }
      llvm::SmallVector<mlir::Type> fields;
      for (unsigned i = 0; i < record->getNumElements(); ++i) {
        auto field = type(record->getElementType(i), other->getElementType(i));
        if (!field) return {};
        fields.push_back(field);
      }
      auto result = ir::RecordType::get(&context, identity, record->isPacked(), fields);
      storageTypes[left] = result;
      storageCorrespondences[left] = right;
      return result;
    }
    if (left->isIntegerTy() && right->isIntegerTy()) {
      unsigned lw = left->getIntegerBitWidth(), rw = right->getIntegerBitWidth();
      if (lw == rw && lw >= 1 && lw <= 128)
        return builder.getIntegerType(lw);
      if (lw == leftTarget.wordBits && rw == rightTarget.wordBits && lw != rw)
        return ir::WordType::get(&context);
      if ((lw == 1 || lw == 8 || lw == 16 || lw == 32 || lw == 64) &&
          (rw == 1 || rw == 8 || rw == 16 || rw == 32 || rw == 64))
        return ir::ChoiceType::get(&context, choice(mlir::TypeAttr::get(builder.getIntegerType(lw)),
                                                   mlir::TypeAttr::get(builder.getIntegerType(rw))));
    }
    fail("unsupported type correspondence; aggregate layout normalization is required");
    return {};
  }

  Attribute choice(Attribute left, Attribute right) {
    return ir::targetChoice(&context, {{leftTarget.id, left}, {rightTarget.id, right}});
  }

  mlir::ArrayAttr domain(unsigned mask) {
    llvm::SmallVector<StringRef> ids;
    if (mask & 1) ids.push_back(leftTarget.id);
    if ((mask & 2) && !llvm::is_contained(ids, rightTarget.id)) ids.push_back(rightTarget.id);
    return ir::targetSet(&context, ids);
  }

  Attribute expression(uint64_t left, uint64_t right) {
    if (left == right)
      return builder.getI64IntegerAttr(left);
    if (left == leftTarget.wordBits / 8 && right == rightTarget.wordBits / 8)
      return builder.getStringAttr("pointer_bytes");
    return choice(builder.getI64IntegerAttr(left), builder.getI64IntegerAttr(right));
  }

  mlir::ArrayAttr attributes(llvm::AttributeSet input) {
    llvm::SmallVector<Attribute> result;
    for (llvm::Attribute attribute : input) {
      llvm::SmallVector<mlir::NamedAttribute> fields;
      if (attribute.isStringAttribute()) {
        if (configurationAttribute(attribute.getKindAsString()))
          continue;
        fields.push_back(attr("name", builder.getStringAttr(attribute.getKindAsString())));
        fields.push_back(attr("string", builder.getStringAttr(attribute.getValueAsString())));
      } else if (attribute.isEnumAttribute() || attribute.isIntAttribute()) {
        fields.push_back(attr("name", builder.getStringAttr(
            llvm::Attribute::getNameFromAttrKind(attribute.getKindAsEnum()))));
        if (attribute.isIntAttribute())
          fields.push_back(attr("integer", builder.getI64IntegerAttr(attribute.getValueAsInt())));
      } else if (attribute.isTypeAttribute()) {
        auto value = type(attribute.getValueAsType(), attribute.getValueAsType());
        if (!value) return {};
        fields.push_back(attr("name", builder.getStringAttr(llvm::Attribute::getNameFromAttrKind(attribute.getKindAsEnum()))));
        fields.push_back(attr("type", mlir::TypeAttr::get(value)));
      } else {
        fail("type/range ABI attributes are not supported in the first checkpoint");
        return {};
      }
      result.push_back(builder.getDictionaryAttr(fields));
    }
    return builder.getArrayAttr(result);
  }

  Attribute attributeList(llvm::AttributeList left,
                               llvm::AttributeList right, unsigned count) {
    llvm::SmallVector<Attribute> result;
    auto add = [&](llvm::AttributeSet a, llvm::AttributeSet b) {
      auto x = attributes(a), y = attributes(b);
      if (!x || !y)
        return;
      result.push_back(choice(x, y));
    };
    add(left.getFnAttrs(), right.getFnAttrs());
    add(left.getRetAttrs(), right.getRetAttrs());
    for (unsigned index = 0; index < count; ++index)
      add(left.getParamAttrs(index), right.getParamAttrs(index));
    return builder.getArrayAttr(result);
  }

  Attribute literalInitializer(const llvm::Constant *value) {
    if (auto *integer = llvm::dyn_cast<llvm::ConstantInt>(value)) {
      if (integer->getBitWidth() > 64) { fail("oversized one-domain integer initializer"); return {}; }
      return builder.getIntegerAttr(builder.getI64Type(), integer->getValue().sextOrTrunc(64));
    }
    if (auto *number = llvm::dyn_cast<llvm::ConstantFP>(value)) {
      auto kind = number->getType()->isFloatTy() ? builder.getF32Type() :
          number->getType()->isDoubleTy() ? builder.getF64Type() : mlir::FloatType();
      if (!kind) { fail("unsupported one-domain floating initializer"); return {}; }
      return builder.getFloatAttr(kind, number->getValueAPF());
    }
    if (llvm::isa<llvm::ConstantAggregateZero>(value)) return builder.getStringAttr("zero");
    if (llvm::isa<llvm::ConstantPointerNull>(value)) return builder.getStringAttr("null");
    if (value->getType()->isAggregateType()) {
      uint64_t count = value->getType()->isArrayTy() ? llvm::cast<llvm::ArrayType>(value->getType())->getNumElements()
          : llvm::cast<llvm::StructType>(value->getType())->getNumElements();
      if (count > 1024 * 1024) { fail("oversized one-domain aggregate initializer"); return {}; }
      llvm::SmallVector<Attribute> elements;
      for (unsigned i = 0; i < count; ++i) {
        auto *element = value->getAggregateElement(i);
        if (!element) { fail("invalid one-domain aggregate element"); return {}; }
        auto literal = literalInitializer(element);
        if (!literal) return {};
        elements.push_back(literal);
      }
      return builder.getArrayAttr(elements);
    }
    fail("one-domain array tails currently require pure literal data, not symbol/address expressions");
    return {};
  }

  Attribute initializer(const llvm::Constant *left, const llvm::Constant *right) {
    if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(left)) {
      auto *other = llvm::dyn_cast<llvm::GEPOperator>(right);
      if (!other || gep->getNumIndices() != other->getNumIndices() ||
          gep->isInBounds() != other->isInBounds() || gep->getInRangeIndex() || other->getInRangeIndex()) {
        fail("constant address expression needs additional normalization"); return {};
      }
      auto element = type(gep->getSourceElementType(), other->getSourceElementType());
      auto base = initializer(llvm::cast<llvm::Constant>(gep->getPointerOperand()),
                              llvm::cast<llvm::Constant>(other->getPointerOperand()));
      if (!element || !base) return {};
      llvm::SmallVector<Attribute> indices;
      for (unsigned i = 1; i < gep->getNumOperands(); ++i) {
        auto *a = llvm::cast<llvm::Constant>(gep->getOperand(i));
        auto *b = llvm::cast<llvm::Constant>(other->getOperand(i));
        auto indexType = type(a->getType(), b->getType());
        auto value = initializer(a, b);
        if (!indexType || !value) return {};
        indices.push_back(builder.getDictionaryAttr({attr("type", mlir::TypeAttr::get(indexType)), attr("value", value)}));
      }
      return builder.getDictionaryAttr({attr("op", builder.getStringAttr("gep")), attr("element", mlir::TypeAttr::get(element)),
          attr("base", base), attr("indices", builder.getArrayAttr(indices)), attr("inbounds", builder.getBoolAttr(gep->isInBounds()))});
    }
    if (auto *integer = llvm::dyn_cast<llvm::ConstantInt>(left)) {
      auto *other = llvm::dyn_cast<llvm::ConstantInt>(right);
      if (!other) { fail("initializer kinds differ"); return {}; }
      if (integer->getBitWidth() > 64 || other->getBitWidth() > 64) {
        auto encode = [&](const llvm::ConstantInt *value) {
          return builder.getIntegerAttr(builder.getIntegerType(value->getBitWidth()), value->getValue());
        };
        return choice(encode(integer), encode(other));
      }
      auto a = integer->getValue().sextOrTrunc(64), b = other->getValue().sextOrTrunc(64);
      if (a == b) return builder.getIntegerAttr(builder.getI64Type(), a);
      return expression(integer->getZExtValue(), other->getZExtValue());
    }
    if (auto *number = llvm::dyn_cast<llvm::ConstantFP>(left)) {
      auto *other = llvm::dyn_cast<llvm::ConstantFP>(right);
      auto commonType = type(left->getType(), right->getType());
      if (!other || !commonType || !number->getValueAPF().bitwiseIsEqual(other->getValueAPF())) {
        fail("floating initializers differ"); return {};
      }
      return builder.getFloatAttr(commonType, number->getValueAPF());
    }
    if (llvm::isa<llvm::ConstantAggregateZero>(left) && llvm::isa<llvm::ConstantAggregateZero>(right))
      return builder.getStringAttr("zero");
    if (llvm::isa<llvm::ConstantPointerNull>(left) && llvm::isa<llvm::ConstantPointerNull>(right))
      return builder.getStringAttr("null");
    if (llvm::isa<llvm::PoisonValue>(left) && llvm::isa<llvm::PoisonValue>(right))
      return builder.getStringAttr("poison");
    if (llvm::isa<llvm::UndefValue>(left) && llvm::isa<llvm::UndefValue>(right) &&
        !llvm::isa<llvm::PoisonValue>(left) && !llvm::isa<llvm::PoisonValue>(right))
      return builder.getStringAttr("undef");
    if (auto *global = llvm::dyn_cast<llvm::GlobalValue>(left)) {
      if ((!singleDomain && pairs.lookup(global) != right) || !symbols.count(global)) {
        fail("initializer address has no shared symbol correspondence"); return {};
      }
      return builder.getDictionaryAttr({attr("symbol", builder.getStringAttr(symbols.lookup(global)))});
    }
    if ((left->getType()->isAggregateType() && right->getType()->isAggregateType()) ||
        (left->getType()->isVectorTy() && right->getType()->isVectorTy())) {
      uint64_t count = left->getType()->isArrayTy()
          ? llvm::cast<llvm::ArrayType>(left->getType())->getNumElements()
          : left->getType()->isVectorTy() ? llvm::cast<llvm::FixedVectorType>(left->getType())->getNumElements()
          : llvm::cast<llvm::StructType>(left->getType())->getNumElements();
      uint64_t otherCount = right->getType()->isArrayTy()
          ? llvm::cast<llvm::ArrayType>(right->getType())->getNumElements()
          : right->getType()->isVectorTy() ? llvm::cast<llvm::FixedVectorType>(right->getType())->getNumElements()
          : llvm::cast<llvm::StructType>(right->getType())->getNumElements();
      if (std::max(count, otherCount) > 1024 * 1024 ||
          (count != otherCount && (!left->getType()->isArrayTy() || !right->getType()->isArrayTy()))) {
        fail("explicit aggregate initializers require bounded compatible element domains"); return {};
      }
      llvm::SmallVector<Attribute> elements;
      for (unsigned i = 0; i < std::max(count, otherCount); ++i) {
        auto *a = i < count ? left->getAggregateElement(i) : nullptr;
        auto *b = i < otherCount ? right->getAggregateElement(i) : nullptr;
        if ((!a && i < count) || (!b && i < otherCount)) { fail("invalid aggregate initializer element"); return {}; }
        auto value = a && b ? initializer(a, b) : literalInitializer(a ? a : b);
        if (!value) return {};
        elements.push_back(value);
      }
      auto array = builder.getArrayAttr(elements);
      if (count != otherCount)
        return builder.getDictionaryAttr({attr("array", array), attr("count", expression(count, otherCount))});
      return array;
    }
    fail("constant initializer expression requires unsupported normalization");
    return {};
  }

  mlir::Value operand(const llvm::Value *left, const llvm::Value *right) {
    if (!error.empty())
      return {};
    if (singleDomain) {
      if (auto found = domainValues.find(left); found != domainValues.end()) return found->second;
      if (!llvm::isa<llvm::Constant>(left)) {
        fail("conditional arm uses an unproved or forward native SSA value"); return {};
      }
    }
    if (auto found = conditionalValues.find({left, right}); found != conditionalValues.end())
      return found->second;
    if (auto found = values.find(left); found != values.end()) {
      if (pairs.lookup(left) != right) {
        fail("SSA operands do not have the same profile correspondence");
        return {};
      }
      return found->second;
    }
    mlir::Type mergedType = type(left->getType(), right->getType());
    if (!mergedType)
      return {};
    if (const auto *a = llvm::dyn_cast<llvm::ConstantInt>(left)) {
      const auto *b = llvm::dyn_cast<llvm::ConstantInt>(right);
      if (!b) {
        fail("constant kind differs between profiles");
        return {};
      }
      Attribute value;
      const llvm::APInt &av = a->getValue(), &bv = b->getValue();
      if (av.getBitWidth() > 64 || bv.getBitWidth() > 64)
        value = choice(builder.getIntegerAttr(builder.getIntegerType(av.getBitWidth()), av),
                       builder.getIntegerAttr(builder.getIntegerType(bv.getBitWidth()), bv));
      else if (av.sextOrTrunc(64) == bv.sextOrTrunc(64))
        value = builder.getIntegerAttr(builder.getI64Type(), av.sextOrTrunc(64));
      else if (av.zextOrTrunc(64) == bv.zextOrTrunc(64))
        value = builder.getIntegerAttr(builder.getI64Type(), av.zextOrTrunc(64));
      else
        value = expression(av.getZExtValue(), bv.getZExtValue());
      if (!value)
        return {};
      return op("sela.constant", mergedType, {}, {attr("value", value)})->getResult(0);
    }
    if (const auto *a = llvm::dyn_cast<llvm::ConstantFP>(left)) {
      const auto *b = llvm::dyn_cast<llvm::ConstantFP>(right);
      if (!b || !a->getValueAPF().bitwiseIsEqual(b->getValueAPF())) {
        fail("floating constants differ between profiles"); return {};
      }
      return op("sela.constant", mergedType, {},
          {attr("value", builder.getFloatAttr(mergedType, a->getValueAPF()))})->getResult(0);
    }
    if (llvm::isa<llvm::ConstantPointerNull>(left) &&
        llvm::isa<llvm::ConstantPointerNull>(right))
      return op("sela.constant", mergedType, {},
                {attr("value", builder.getStringAttr("null"))})->getResult(0);
    const auto *global = llvm::dyn_cast<llvm::GlobalValue>(left);
    if (global && symbols.count(global) && (singleDomain || pairs.lookup(left) == right)) {
      return op("sela.address", mergedType, {},
                {attr("global", builder.getStringAttr(symbols.lookup(global)))})
          ->getResult(0);
    }
    if (auto *constant = llvm::dyn_cast<llvm::Constant>(left)) {
      auto *other = llvm::dyn_cast<llvm::Constant>(right);
      auto value = other ? initializer(constant, other) : Attribute();
      if (!value) return {};
      return op("sela.constant", mergedType, {}, {attr("value", value)})->getResult(0);
    }
    fail("unsupported constant, forward SSA reference or function address");
    return {};
  }

  bool normalizeOneSidedCast(const llvm::Instruction *leftInstruction,
                             const llvm::Instruction *rightInstruction, bool leftSide) {
    const auto *cast = llvm::dyn_cast_or_null<llvm::CastInst>(leftSide ? leftInstruction : rightInstruction);
    if (!cast || !cast->getSrcTy()->isIntegerTy() || !cast->getDestTy()->isIntegerTy() ||
        (cast->getOpcode() != llvm::Instruction::ZExt && cast->getOpcode() != llvm::Instruction::SExt &&
         cast->getOpcode() != llvm::Instruction::Trunc) || !permittedMetadata(*cast)) return false;
    const llvm::Value *leftSource = nullptr, *rightSource = nullptr;
    if (leftSide) {
      leftSource = cast->getOperand(0);
      rightSource = pairs.lookup(leftSource);
      if (!rightSource || !values.count(leftSource)) {
        rightSource = nullptr;
        for (const auto &entry : conditionalValues)
          if (entry.first.first == leftSource) {
            if (rightSource && rightSource != entry.first.second) return false;
            rightSource = entry.first.second;
          }
        if (!rightSource) return false;
      }
    } else {
      rightSource = cast->getOperand(0);
      for (const auto &entry : values)
        if (pairs.lookup(entry.first) == rightSource) {
          if (leftSource) return false; // No guessed correspondence among ambiguous candidates.
          leftSource = entry.first;
        }
      if (!leftSource)
        for (const auto &entry : conditionalValues)
          if (entry.first.second == rightSource) {
            if (leftSource && leftSource != entry.first.first) return false;
            leftSource = entry.first.first;
          }
      if (!leftSource) return false;
    }
    const llvm::Value *leftResult = leftSide ? cast : leftSource;
    const llvm::Value *rightResult = leftSide ? rightSource : cast;
    if (!leftResult->getType()->isIntegerTy() || !rightResult->getType()->isIntegerTy()) return false;
    unsigned a = leftResult->getType()->getIntegerBitWidth(), b = rightResult->getType()->getIntegerBitWidth();
    if (a != b && !(a == leftTarget.wordBits && b == rightTarget.wordBits)) return false;
    auto input = operand(leftSource, rightSource);
    auto resultType = type(leftResult->getType(), rightResult->getType());
    if (!input || !resultType) return false;
    auto *result = op("sela.cast", resultType, input,
        {attr("opcode", builder.getStringAttr("native_" + std::string(cast->getOpcodeName())))});
    conditionalValues[{leftResult, rightResult}] = result->getResult(0);
    domainValues[cast] = result->getResult(0);
    return true;
  }

  bool edgeArguments(const llvm::BasicBlock *leftTarget,
                     const llvm::BasicBlock *rightTarget,
                     const llvm::BasicBlock *leftSource,
                     const llvm::BasicBlock *rightSource,
                     llvm::SmallVectorImpl<mlir::Value> &arguments) {
    if (singleDomain) {
      if (!blocks.count(leftTarget) || !leftTarget->phis().empty()) {
        fail("conditional arm requires a proved target without phi arguments"); return false;
      }
      return true;
    }
    if (blockPairs.lookup(leftTarget) != rightTarget || !blocks.count(leftTarget)) {
      fail("branch targets do not have the same CFG correspondence");
      return false;
    }
    for (const auto &phi : leftTarget->phis()) {
      const auto *other = llvm::dyn_cast_or_null<llvm::PHINode>(pairs.lookup(&phi));
      int leftIndex = phi.getBasicBlockIndex(leftSource);
      int rightIndex = other ? other->getBasicBlockIndex(rightSource) : -1;
      if (leftIndex < 0 || rightIndex < 0) {
        fail("branch predecessor is missing from its phi node"); return false;
      }
      auto value = operand(phi.getIncomingValue(leftIndex), other->getIncomingValue(rightIndex));
      if (!value) return false;
      arguments.push_back(value);
    }
    return true;
  }

  mlir::Value callArgument(const llvm::CallBase *left, const llvm::CallBase *right, unsigned index) {
    auto local = [&](const detail::NativeVarargs &proof, const llvm::CallBase *call) {
      auto entry = proof.forwardedArguments.find(call);
      return entry == proof.forwardedArguments.end() ? nullptr : entry->second.lookup(index);
    };
    auto incoming = [&](const detail::NativeVarargs &proof, const llvm::CallBase *call) {
      auto entry = proof.forwardedValues.find(call);
      return entry == proof.forwardedValues.end() ? nullptr : entry->second.lookup(index);
    };
    const auto &leftProof = singleDomain == 2 ? rightVarargs : leftVarargs;
    const auto &rightProof = singleDomain == 1 ? leftVarargs : rightVarargs;
    auto *a = local(leftProof, left), *b = local(rightProof, right);
    auto *ai = incoming(leftProof, left), *bi = incoming(rightProof, right);
    if (ai || bi) {
      if (!ai || !bi || a || b) { fail("native cursor argument provenance differs"); return {}; }
      auto source = operand(ai, bi);
      if (!source) return {};
      return op("sela.va_forward", ir::VaListArgumentType::get(&context), source)->getResult(0);
    }
    if (a || b) {
      if (!a || !b) { fail("native cursor forwarding lacks paired proof"); return {}; }
      auto storage = operand(a, b);
      if (!storage) return {};
      // The recognizer proves forwarding loads are single-use and immediately
      // adjacent to this call. Emitting here cannot move the cursor read across
      // effects. The ABI-owned AArch64 transfer copy is also proved closed and
      // regenerated by this operation, independently from an explicit va_copy.
      return op("sela.va_forward", ir::VaListArgumentType::get(&context), storage)->getResult(0);
    }
    return operand(left->getArgOperand(index), right->getArgOperand(index));
  }

  Operation *inlineAssembly(const llvm::CallBase &call) {
    auto *assembly = llvm::dyn_cast<llvm::InlineAsm>(call.getCalledOperand());
    auto *branch = llvm::dyn_cast<llvm::CallBrInst>(&call);
    if (!assembly || call.hasOperandBundles() || call.getCallingConv() || assembly->getFunctionType()->isVarArg()) {
      fail("inline assembly requires a fixed single-target call signature"); return nullptr;
    }
    llvm::SmallVector<mlir::Value> arguments;
    for (auto &argument : call.args()) {
      auto value = operand(argument, argument);
      if (!value) return nullptr;
      arguments.push_back(value);
    }
    mlir::OperationState state(builder.getUnknownLoc(), branch ? "sela.inline_asm_br" : "sela.inline_asm");
    if (!call.getType()->isVoidTy()) {
      auto resultType = type(call.getType(), call.getType());
      if (!resultType) return nullptr;
      state.addTypes(resultType);
    }
    state.addAttributes({
      attr("template", builder.getStringAttr(assembly->getAsmString())),
      attr("constraints", builder.getStringAttr(assembly->getConstraintString())),
      attr("backend", builder.getStringAttr(leftTarget.llvmBackend)),
      attr("side_effects", builder.getBoolAttr(assembly->hasSideEffects())),
      attr("align_stack", builder.getBoolAttr(assembly->isAlignStack())),
      attr("can_throw", builder.getBoolAttr(assembly->canThrow())),
      attr("dialect", builder.getStringAttr(assembly->getDialect() == llvm::InlineAsm::AD_ATT ? "att" : "intel")),
      attr("attributes", attributeList(call.getAttributes(), call.getAttributes(), call.arg_size())),
      attr("tail", builder.getI32IntegerAttr(branch ? 0 : llvm::cast<llvm::CallInst>(call).getTailCallKind()))});
    if (branch) {
      state.addAttribute("argument_count", builder.getI32IntegerAttr(arguments.size()));
      llvm::SmallVector<int32_t> counts;
      for (unsigned index = 0; index < branch->getNumSuccessors(); ++index) {
        auto *destination = branch->getSuccessor(index);
        size_t before = arguments.size();
        if (!edgeArguments(destination, destination, branch->getParent(), branch->getParent(), arguments)) return nullptr;
        counts.push_back(arguments.size() - before);
        state.addSuccessors(blocks.lookup(destination));
      }
      state.addAttribute("argument_counts", builder.getDenseI32ArrayAttr(counts));
    }
    if (!error.empty()) return nullptr;
    state.addOperands(arguments);
    return builder.create(state);
  }

  void mergeInstruction(const llvm::Instruction &left,
                         const llvm::Instruction &right) {
    if (!error.empty())
      return;
    if (!correspondingInstructionKind(left, right) || !permittedMetadata(left) ||
        !permittedMetadata(right)) {
      fail("unsupported instruction correspondence or semantic metadata");
      return;
    }
    if (singleDomain && left.getMetadata(llvm::LLVMContext::MD_loop)) {
      fail("conditional arm loop identities require additional correspondence proof"); return;
    }
    Operation *result = nullptr;
    if (auto *a = llvm::dyn_cast<llvm::AllocaInst>(&left)) {
      auto *b = llvm::cast<llvm::AllocaInst>(&right);
      auto *ac = llvm::dyn_cast<llvm::ConstantInt>(a->getArraySize());
      auto *bc = llvm::dyn_cast<llvm::ConstantInt>(b->getArraySize());
      if (!ac || !bc || !ac->isOne() || !bc->isOne() ||
          a->getAddressSpace() || b->getAddressSpace() ||
          a->isUsedWithInAlloca() || b->isUsedWithInAlloca() ||
          a->isSwiftError() || b->isSwiftError()) {
        fail("only single native scalar stack allocations are supported yet");
        return;
      }
      mlir::Type element;
      if (leftVarargs.states.contains(a) || rightVarargs.states.contains(b)) {
        if (!leftVarargs.states.contains(a) || !rightVarargs.states.contains(b)) {
          fail("native variadic cursor storage correspondence differs"); return;
        }
        auto recordOf = [](llvm::Type *type) {
          if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) type = array->getElementType();
          return llvm::dyn_cast<llvm::StructType>(type);
        };
        auto *record = recordOf(a->getAllocatedType());
        if (record && !leftRecordIDs.empty() && llvm::any_of(leftRecordIDs, [&](const auto &entry) {
              return entry.second == "v0" && entry.first != record;
            })) {
          fail("multiple incompatible native variadic cursor types"); return;
        }
        if (record) leftRecordIDs[record] = "v0";
        if (auto *other = recordOf(b->getAllocatedType())) rightRecordIDs[other] = "v0";
        element = ir::VaListType::get(&context);
      } else element = type(a->getAllocatedType(), b->getAllocatedType());
      auto alignment = expression(a->getAlign().value(), b->getAlign().value());
      if (!element || !alignment)
        return;
      result = op("sela.alloca", ir::PointerType::get(&context), {},
                  {attr("element", mlir::TypeAttr::get(element)),
                   attr("alignment", alignment)});
    } else if (auto *a = llvm::dyn_cast<llvm::VAArgInst>(&left)) {
      auto *b = llvm::cast<llvm::VAArgInst>(&right);
      auto state = operand(a->getPointerOperand(), b->getPointerOperand());
      auto element = type(a->getType(), b->getType());
      if (!state || !element) return;
      result = op("sela.va_arg", element, state);
    } else if (auto *a = llvm::dyn_cast<llvm::GetElementPtrInst>(&left)) {
      auto *b = llvm::cast<llvm::GetElementPtrInst>(&right);
      if (a->getNumIndices() != b->getNumIndices() || a->isInBounds() != b->isInBounds()) {
        fail("native address calculations differ between profiles"); return;
      }
      auto element = type(a->getSourceElementType(), b->getSourceElementType());
      llvm::SmallVector<mlir::Value> operands;
      for (unsigned i = 0; i < a->getNumOperands(); ++i) {
        auto value = operand(a->getOperand(i), b->getOperand(i));
        if (!value) return;
        operands.push_back(value);
      }
      if (!element) return;
      result = op("sela.gep", ir::PointerType::get(&context), operands,
          {attr("element", mlir::TypeAttr::get(element)),
           attr("inbounds", builder.getBoolAttr(a->isInBounds()))});
    } else if (auto *a = llvm::dyn_cast<llvm::LoadInst>(&left)) {
      auto *b = llvm::cast<llvm::LoadInst>(&right);
      if (a->isVolatile() != b->isVolatile() || a->isAtomic() || b->isAtomic()) {
        fail("atomic or profile-divergent volatile loads are not supported yet");
        return;
      }
      auto valueType = type(a->getType(), b->getType());
      auto pointer = operand(a->getPointerOperand(), b->getPointerOperand());
      auto alignment = expression(a->getAlign().value(), b->getAlign().value());
      if (!valueType || !pointer || !alignment)
        return;
      result = op("sela.load", valueType, pointer, {attr("alignment", alignment),
          attr("volatile", builder.getBoolAttr(a->isVolatile()))});
    } else if (auto *a = llvm::dyn_cast<llvm::StoreInst>(&left)) {
      auto *b = llvm::cast<llvm::StoreInst>(&right);
      if (a->isVolatile() != b->isVolatile() || a->isAtomic() || b->isAtomic()) {
        fail("atomic or profile-divergent volatile stores are not supported yet");
        return;
      }
      auto value = operand(a->getValueOperand(), b->getValueOperand());
      auto pointer = operand(a->getPointerOperand(), b->getPointerOperand());
      auto alignment = expression(a->getAlign().value(), b->getAlign().value());
      if (!value || !pointer || !alignment)
        return;
      result = op("sela.store", {}, {value, pointer}, {attr("alignment", alignment),
          attr("volatile", builder.getBoolAttr(a->isVolatile()))});
    } else if (auto *branch = llvm::dyn_cast<llvm::CallBrInst>(&left)) {
      if (&left != &right) { fail("assembly branch requires a single-target import"); return; }
      result = inlineAssembly(*branch);
      if (!result) return;
    } else if (auto *a = llvm::dyn_cast<llvm::CallInst>(&left)) {
      auto *b = llvm::cast<llvm::CallInst>(&right);
      if (auto *assembly = llvm::dyn_cast<llvm::InlineAsm>(a->getCalledOperand())) {
        if (a != b) { fail("inline assembly requires a single-target import"); return; }
        result = inlineAssembly(*a);
        if (!result) return;
      } else if (auto *intrinsic = ir::findIntrinsic(a->getIntrinsicID())) {
        if (a->getIntrinsicID() != b->getIntrinsicID() || a->hasOperandBundles() || b->hasOperandBundles() ||
            a->arg_size() != intrinsic->operands || b->arg_size() != intrinsic->operands)
          { fail("invalid registered intrinsic operands"); return; }
        llvm::SmallVector<mlir::Value> arguments;
        for (unsigned index = 0; index < a->arg_size(); ++index) {
          auto value = operand(a->getArgOperand(index), b->getArgOperand(index));
          if (!value) return;
          arguments.push_back(value);
        }
        llvm::SmallVector<mlir::Type> results;
        if (!a->getType()->isVoidTy()) {
          auto resultType = type(a->getType(), b->getType());
          if (!resultType) return;
          results.push_back(resultType);
        }
        result = op("sela.intrinsic", results, arguments, {attr("name", builder.getStringAttr(intrinsic->name)),
            attr("attributes", attributeList(a->getAttributes(), b->getAttributes(), a->arg_size())),
            attr("tail", builder.getI32IntegerAttr(a->getTailCallKind()))});
      } else if (a->getIntrinsicID() == llvm::Intrinsic::bswap || b->getIntrinsicID() == llvm::Intrinsic::bswap) {
        if (!a->getCalledFunction() || !b->getCalledFunction() ||
            !byteSwapPrimitive(*a->getCalledFunction()) || !byteSwapPrimitive(*b->getCalledFunction()) ||
            a->hasOperandBundles() || b->hasOperandBundles() ||
            !a->getAttributes().isEmpty() || !b->getAttributes().isEmpty() ||
            a->getCallingConv() || b->getCallingConv() || a->isTailCall() || b->isTailCall()) {
          fail("byte-swap primitive requires the exact qualified intrinsic ABI"); return;
        }
        auto resultType = type(a->getType(), b->getType());
        auto value = operand(a->getArgOperand(0), b->getArgOperand(0));
        if (!resultType || !value) return;
        result = op("sela.bswap", resultType, value);
      } else {
      if (llvm::isa<llvm::FPMathOperator>(a) &&
          (a->getFastMathFlags().any() || b->getFastMathFlags().any())) {
        fail("floating call fast-math flags are not supported yet"); return;
      }
      bool direct = a->getCalledFunction() && b->getCalledFunction();
      if (bool(a->getCalledFunction()) != bool(b->getCalledFunction()) ||
          (direct && !singleDomain && pairs.lookup(a->getCalledFunction()) != b->getCalledFunction()) ||
          a->arg_size() != b->arg_size() || a->hasOperandBundles() ||
          b->hasOperandBundles() || a->getCallingConv() || b->getCallingConv() ||
          a->getTailCallKind() != b->getTailCallKind()) {
        fail("ABI-changing or bundled calls need additional normalization");
        return;
      }
      llvm::SmallVector<mlir::Value> arguments;
      if (!direct) {
        auto callee = operand(a->getCalledOperand(), b->getCalledOperand());
        if (!callee) return;
        arguments.push_back(callee);
      }
      for (unsigned i = 0; i < a->arg_size(); ++i) {
        auto argument = callArgument(a, b, i);
        if (!argument)
          return;
        arguments.push_back(argument);
      }
      auto returns = type(a->getType(), b->getType());
      auto attributes = attributeList(a->getAttributes(), b->getAttributes(), a->arg_size());
      if (!returns || !error.empty())
        return;
      llvm::SmallVector<mlir::Type> resultTypes;
      if (!mlir::isa<mlir::NoneType>(returns))
        resultTypes.push_back(returns);
      if (direct) {
        if (!symbols.count(a->getCalledFunction())) { fail("conditional call has no shared native symbol contract"); return; }
        result = op("sela.call", resultTypes, arguments,
                    {attr("callee", builder.getStringAttr(symbols.lookup(a->getCalledFunction()))),
                     attr("attributes", attributes), attr("tail", builder.getI32IntegerAttr(a->getTailCallKind()))});
      } else {
        auto *leftType = a->getFunctionType(), *rightType = b->getFunctionType();
        if (leftType->getNumParams() != rightType->getNumParams() || leftType->isVarArg() != rightType->isVarArg()) {
          fail("indirect function signatures differ"); return;
        }
        llvm::SmallVector<mlir::Type> parameters;
        for (unsigned i = 0; i < leftType->getNumParams(); ++i) {
          auto parameter = type(leftType->getParamType(i), rightType->getParamType(i));
          if (!parameter) return;
          parameters.push_back(parameter);
        }
        result = op("sela.call_indirect", resultTypes, arguments,
            {attr("type", mlir::TypeAttr::get(builder.getFunctionType(parameters, resultTypes))),
             attr("variadic", builder.getBoolAttr(leftType->isVarArg())), attr("attributes", attributes),
             attr("tail", builder.getI32IntegerAttr(a->getTailCallKind()))});
      }
      if (singleDomain) {
        const auto &aggregate = singleDomain == 1 ? leftAggregates : rightAggregates;
        auto *logical = aggregate.calls.lookup(const_cast<llvm::CallInst *>(a));
        nativeABI(result, logical, logical);
      } else nativeABI(result, leftAggregates.calls.lookup(const_cast<llvm::CallInst *>(a)),
                               rightAggregates.calls.lookup(const_cast<llvm::CallInst *>(b)));
      }
    } else if (auto *a = llvm::dyn_cast<llvm::SwitchInst>(&left)) {
      auto *b = llvm::cast<llvm::SwitchInst>(&right);
      if (conditionalCFG) {
        auto found = conditionalCFG->leftSwitches.find(a);
        if (found == conditionalCFG->leftSwitches.end()) { fail("switch lacks a conditional CFG proof"); return; }
        const auto &proof = conditionalCFG->switches[found->second];
        if (proof.right != b) { fail("conditional switch correspondence changed"); return; }
        auto condition = operand(a->getCondition(), b->getCondition());
        if (!condition) return;
        auto successor = [&](unsigned index) {
          const auto &pair = conditionalCFG->blocks[index];
          return blocks.lookup(pair.left ? pair.left : pair.right);
        };
        llvm::SmallVector<Attribute> cases, domains;
        llvm::SmallVector<int32_t> counts{0};
        llvm::SmallVector<mlir::Block *> successors{successor(proof.defaultSuccessor)};
        for (const auto &entry : proof.cases) {
          const auto *native = entry.left ? entry.left : entry.right;
          // The graph proof pairs only exact same-width labels. One-domain
          // labels are fixed native constants, selected with their own edge.
          cases.push_back(builder.getIntegerAttr(builder.getI64Type(), native->getValue().sextOrTrunc(64)));
          domains.push_back(domain(entry.domain));
          counts.push_back(0);
          successors.push_back(successor(entry.successor));
        }
        mlir::OperationState state(builder.getUnknownLoc(), "sela.switch");
        state.addOperands(condition);
        state.addSuccessors(successors);
        state.addAttribute("cases", builder.getArrayAttr(cases));
        state.addAttribute("case_domains", builder.getArrayAttr(domains));
        state.addAttribute("argument_counts", builder.getDenseI32ArrayAttr(counts));
        result = builder.create(state);
      } else {
      if (a->getNumCases() != b->getNumCases()) {
        fail("switch case inventories differ between profiles"); return;
      }
      auto condition = operand(a->getCondition(), b->getCondition());
      if (!condition) return;
      llvm::SmallVector<mlir::Value> arguments{condition};
      llvm::SmallVector<int32_t> counts;
      llvm::SmallVector<Attribute> cases;
      for (unsigned i = 0; i < a->getNumSuccessors(); ++i) {
        size_t before = arguments.size();
        if (!edgeArguments(a->getSuccessor(i), b->getSuccessor(i),
                           a->getParent(), b->getParent(), arguments)) return;
        counts.push_back(arguments.size() - before);
      }
      auto rightCase = b->case_begin();
      for (const auto &entry : a->cases()) {
        const auto &av = entry.getCaseValue()->getValue();
        const auto &bv = (rightCase++)->getCaseValue()->getValue();
        Attribute value;
        if (av.sextOrTrunc(64) == bv.sextOrTrunc(64))
          value = builder.getIntegerAttr(builder.getI64Type(), av.sextOrTrunc(64));
        else if (av.zextOrTrunc(64) == bv.zextOrTrunc(64))
          value = builder.getIntegerAttr(builder.getI64Type(), av.zextOrTrunc(64));
        else value = expression(av.getZExtValue(), bv.getZExtValue());
        if (!value) return;
        cases.push_back(value);
      }
      mlir::OperationState state(builder.getUnknownLoc(), "sela.switch");
      state.addOperands(arguments);
      for (unsigned i = 0; i < a->getNumSuccessors(); ++i)
        state.addSuccessors(blocks.lookup(a->getSuccessor(i)));
      state.addAttribute("cases", builder.getArrayAttr(cases));
      state.addAttribute("argument_counts", builder.getDenseI32ArrayAttr(counts));
      result = builder.create(state);
      }
    } else if (auto *a = llvm::dyn_cast<llvm::BranchInst>(&left)) {
      auto *b = llvm::cast<llvm::BranchInst>(&right);
      if (a->isConditional() != b->isConditional()) {
        fail("branch kinds differ between profiles"); return;
      }
      llvm::SmallVector<mlir::Value> arguments;
      if (a->isConditional()) {
        auto condition = operand(a->getCondition(), b->getCondition());
        if (!condition) return;
        arguments.push_back(condition);
      }
      if (!edgeArguments(a->getSuccessor(0), b->getSuccessor(0),
                         a->getParent(), b->getParent(), arguments)) return;
      unsigned trueCount = arguments.size() - (a->isConditional() ? 1 : 0);
      if (a->isConditional() &&
          !edgeArguments(a->getSuccessor(1), b->getSuccessor(1),
                         a->getParent(), b->getParent(), arguments)) return;
      mlir::OperationState state(builder.getUnknownLoc(),
          a->isConditional() ? "sela.cond_br" : "sela.br");
      state.addOperands(arguments);
      for (unsigned i = 0; i < a->getNumSuccessors(); ++i)
        state.addSuccessors(blocks.lookup(a->getSuccessor(i)));
      if (a->isConditional())
        state.addAttribute("true_count", builder.getI32IntegerAttr(trueCount));
      result = builder.create(state);
    } else if (llvm::isa<llvm::UnreachableInst>(&left)) {
      result = op("sela.unreachable");
    } else if (auto *a = llvm::dyn_cast<llvm::ReturnInst>(&left)) {
      auto *b = llvm::cast<llvm::ReturnInst>(&right);
      if (bool(a->getReturnValue()) != bool(b->getReturnValue())) {
        fail("return ABI differs between profiles");
        return;
      }
      llvm::SmallVector<mlir::Value> returns;
      if (a->getReturnValue()) {
        auto value = operand(a->getReturnValue(), b->getReturnValue());
        if (!value)
          return;
        returns.push_back(value);
      }
      result = op("sela.return", {}, returns);
    } else if (auto *a = llvm::dyn_cast<llvm::BinaryOperator>(&left)) {
      auto *b = llvm::cast<llvm::BinaryOperator>(&right);
      if (llvm::isa<llvm::FPMathOperator>(a) &&
          (a->getFastMathFlags().any() || b->getFastMathFlags().any())) {
        fail("fast-math flags require an explicitly supported Sela representation"); return;
      }
      auto valueType = type(a->getType(), b->getType());
      auto x = operand(a->getOperand(0), b->getOperand(0));
      auto y = operand(a->getOperand(1), b->getOperand(1));
      if (!valueType || !x || !y)
        return;
      unsigned af = arithmeticFlags(*a), bf = arithmeticFlags(*b);
      if (!validArithmeticFlags(a->getOpcode(), af) || !validArithmeticFlags(b->getOpcode(), bf)) {
        fail("arithmetic flags are not valid for this operation");
        return;
      }
      result = op("sela.binary", valueType, {x, y},
                  {attr("opcode", choice(builder.getStringAttr(a->getOpcodeName()), builder.getStringAttr(b->getOpcodeName()))),
                   attr("flags", expression(af, bf))});
    } else if (auto *a = llvm::dyn_cast<llvm::SelectInst>(&left)) {
      auto *b = llvm::cast<llvm::SelectInst>(&right);
      if (llvm::isa<llvm::FPMathOperator>(a) &&
          (a->getFastMathFlags().any() || b->getFastMathFlags().any())) {
        fail("fast-math select flags are not supported yet"); return;
      }
      auto resultType = type(a->getType(), b->getType());
      auto condition = operand(a->getCondition(), b->getCondition());
      auto yes = operand(a->getTrueValue(), b->getTrueValue());
      auto no = operand(a->getFalseValue(), b->getFalseValue());
      if (!resultType || !condition || !yes || !no) return;
      result = op("sela.select", resultType, {condition, yes, no});
    } else if (auto *a = llvm::dyn_cast<llvm::UnaryOperator>(&left)) {
      auto *b = llvm::cast<llvm::UnaryOperator>(&right);
      if (a->getOpcode() != llvm::Instruction::FNeg ||
          a->getFastMathFlags().any() || b->getFastMathFlags().any()) {
        fail("unsupported floating unary operation or flags"); return;
      }
      auto resultType = type(a->getType(), b->getType());
      auto value = operand(a->getOperand(0), b->getOperand(0));
      if (!resultType || !value) return;
      result = op("sela.fneg", resultType, value);
    } else if (auto *a = llvm::dyn_cast<llvm::CastInst>(&left)) {
      auto *b = llvm::cast<llvm::CastInst>(&right);
      auto valueType = type(a->getType(), b->getType());
      auto value = operand(a->getOperand(0), b->getOperand(0));
      if (!valueType || !value)
        return;
      result = op("sela.cast", valueType, value,
                  {attr("opcode", choice(builder.getStringAttr(a->getOpcodeName()), builder.getStringAttr(b->getOpcodeName())))});
    } else if (auto *a = llvm::dyn_cast<llvm::ExtractValueInst>(&left)) {
      auto *b = llvm::cast<llvm::ExtractValueInst>(&right);
      auto value = operand(a->getAggregateOperand(), b->getAggregateOperand());
      auto resultType = type(a->getType(), b->getType());
      if (!value || !resultType || a->getIndices() != b->getIndices()) { fail("aggregate extraction indices differ"); return; }
      llvm::SmallVector<int32_t> indices(a->getIndices().begin(), a->getIndices().end());
      result = op("sela.extract_value", resultType, value, {attr("indices", builder.getDenseI32ArrayAttr(indices))});
    } else if (auto *a = llvm::dyn_cast<llvm::InsertValueInst>(&left)) {
      auto *b = llvm::cast<llvm::InsertValueInst>(&right);
      auto aggregate = operand(a->getAggregateOperand(), b->getAggregateOperand());
      auto value = operand(a->getInsertedValueOperand(), b->getInsertedValueOperand());
      auto resultType = type(a->getType(), b->getType());
      if (!aggregate || !value || !resultType || a->getIndices() != b->getIndices()) { fail("aggregate insertion indices differ"); return; }
      llvm::SmallVector<int32_t> indices(a->getIndices().begin(), a->getIndices().end());
      result = op("sela.insert_value", resultType, {aggregate, value}, {attr("indices", builder.getDenseI32ArrayAttr(indices))});
    } else if (llvm::isa<llvm::ExtractElementInst, llvm::InsertElementInst, llvm::ShuffleVectorInst>(&left)) {
      llvm::SmallVector<mlir::Value> operands;
      for (unsigned i = 0; i < left.getNumOperands(); ++i) {
        auto value = operand(left.getOperand(i), right.getOperand(i));
        if (!value) return;
        operands.push_back(value);
      }
      auto resultType = type(left.getType(), right.getType());
      if (!resultType) return;
      if (auto *shuffle = llvm::dyn_cast<llvm::ShuffleVectorInst>(&left)) {
        auto *other = llvm::cast<llvm::ShuffleVectorInst>(&right);
        if (shuffle->getShuffleMask() != other->getShuffleMask()) { fail("vector shuffle masks differ"); return; }
        result = op("sela.shuffle", resultType, operands, {attr("mask", builder.getDenseI32ArrayAttr(shuffle->getShuffleMask()))});
      } else result = op(llvm::isa<llvm::ExtractElementInst>(left) ? "sela.extract_element" : "sela.insert_element", resultType, operands);
    } else if (auto *a = llvm::dyn_cast<llvm::CmpInst>(&left)) {
      auto *b = llvm::cast<llvm::CmpInst>(&right);
      if (llvm::isa<llvm::FCmpInst>(a) &&
          (a->getFastMathFlags().any() || b->getFastMathFlags().any())) {
        fail("fast floating comparison flags are not supported yet"); return;
      }
      auto x = operand(a->getOperand(0), b->getOperand(0));
      auto y = operand(a->getOperand(1), b->getOperand(1));
      if (!x || !y)
        return;
      auto resultType = type(a->getType(), b->getType());
      if (!resultType) return;
      result = op("sela.compare", resultType, {x, y},
                  {attr("predicate", choice(builder.getI32IntegerAttr(a->getPredicate()), builder.getI32IntegerAttr(b->getPredicate())))});
    } else {
      fail("unsupported first-checkpoint instruction: " + StringRef(left.getOpcodeName()));
      return;
    }
    auto loopOptions = [&](const llvm::Instruction &instruction) -> mlir::ArrayAttr {
      auto *node = instruction.getMetadata(llvm::LLVMContext::MD_loop);
      if (!node) return {};
      if (!llvm::isa<llvm::BranchInst>(instruction) || !node->isDistinct() ||
          node->getNumOperands() < 1 || node->getOperand(0).get() != node) {
        fail("unsupported loop metadata shape"); return {};
      }
      llvm::SmallVector<Attribute> options;
      for (unsigned i = 1; i < node->getNumOperands(); ++i) {
        // Loop source locations are private debug information, not an LLVM
        // loop transformation/semantic option. StripDebugInfo removes the
        // same operands in the private native reconstruction comparison.
        if (llvm::isa_and_nonnull<llvm::DILocation>(node->getOperand(i))) continue;
        auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(i));
        auto *name = entry && entry->getNumOperands() >= 1
            ? llvm::dyn_cast_or_null<llvm::MDString>(entry->getOperand(0)) : nullptr;
        if (!name) { fail("invalid loop option name"); return {}; }
        Attribute option;
        if (entry->getNumOperands() == 1) option = builder.getStringAttr(name->getString());
        else if (entry->getNumOperands() == 2) {
          auto *number = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(entry->getOperand(1));
          if (number && number->getType()->isIntegerTy(32)) option = builder.getDictionaryAttr({
              attr("name", builder.getStringAttr(name->getString())), attr("value", builder.getI32IntegerAttr(number->getZExtValue()))});
        }
        if (!option) { fail("loop metadata requires an unsupported semantic option"); return {}; }
        if (auto error = detail::validateLoopOption(option)) { fail(llvm::toString(std::move(error))); return {}; }
        options.push_back(option);
      }
      return options.empty() ? mlir::ArrayAttr() : builder.getArrayAttr(options);
    };
    auto leftLoop = loopOptions(left), rightLoop = loopOptions(right);
    if (!error.empty()) return;
    if (leftLoop != rightLoop) { fail("loop options differ between profiles"); return; }
    if (leftLoop) {
      auto *a = left.getMetadata(llvm::LLVMContext::MD_loop);
      auto *b = right.getMetadata(llvm::LLVMContext::MD_loop);
      if ((loopPairs.count(a) && loopPairs.lookup(a) != b) ||
          (reverseLoopPairs.count(b) && reverseLoopPairs.lookup(b) != a)) {
        fail("native loop identity correspondence differs between profiles"); return;
      }
      if (!loopIDs.count(a)) loopIDs[a] = "l" + std::to_string(loopIDs.size());
      loopPairs[a] = b;
      reverseLoopPairs[b] = a;
      result->setAttr("loop", leftLoop);
      result->setAttr("loop_id", builder.getStringAttr(loopIDs.lookup(a)));
    }
    if (!left.getType()->isVoidTy()) {
      domainValues[&left] = result->getResult(0);
      if (!singleDomain) {
        domainValues[&right] = result->getResult(0);
        values[&left] = result->getResult(0);
        pairs[&left] = &right;
      }
    }
  }

  // A single-observation traversal: no peer module, definition matching,
  // block pairing, or cross-target proof is needed to import a native unit.
  // The instruction/type encoding primitives below are also used by the
  // optional correspondence prover; identical operands select literal forms.
  void importModule(llvm::Module &input) {
    llvm::SmallVector<Attribute> flags;
    for (const auto &[name, value] : moduleFlags(input))
      flags.push_back(builder.getDictionaryAttr({
          attr("name", builder.getStringAttr(name)),
          attr("behavior", builder.getI32IntegerAttr(value.first)),
          attr("value", builder.getI32IntegerAttr(value.second)),
          attr("targets", domain(1))}));
    (*module)->setAttr("sela.module_flags", builder.getArrayAttr(flags));
    unsigned globalIndex = 0, functionIndex = 0;
    // Assembly templates may name a local symbol without an LLVM SSA use.
    // Renaming only the SSA graph would silently break that native reference.
    const bool retainSymbolNames = hasAssembly(input);
    for (auto &global : input.globals()) {
      const auto linkage = global.getLinkage();
      if (global.getAddressSpace() || global.isThreadLocal() || global.isExternallyInitialized() ||
          global.hasSection() || global.hasComdat() ||
          (linkage != llvm::GlobalValue::PrivateLinkage && linkage != llvm::GlobalValue::InternalLinkage &&
           linkage != llvm::GlobalValue::ExternalLinkage && linkage != llvm::GlobalValue::CommonLinkage &&
           linkage != llvm::GlobalValue::WeakAnyLinkage &&
           !(linkage == llvm::GlobalValue::AppendingLinkage &&
             (global.getName() == "llvm.global_ctors" || global.getName() == "llvm.global_dtors")))) {
        fail("unsupported single-target global storage/linkage: " + global.getName()); return;
      }
      symbols[&global] = global.hasLocalLinkage() && !retainSymbolNames ? "g" + std::to_string(globalIndex) : global.getName().str();
      ++globalIndex;
      pairs[&global] = &global;
    }
    for (auto &function : input) {
      if (debugFunction(function) || byteSwapPrimitive(function) || ir::findIntrinsic(function.getIntrinsicID())) continue;
      if (function.getCallingConv() || function.hasPersonalityFn() || function.hasPrefixData() ||
          function.hasPrologueData() || function.hasComdat() || function.hasSection() ||
          (function.getLinkage() != llvm::GlobalValue::ExternalLinkage &&
           function.getLinkage() != llvm::GlobalValue::InternalLinkage &&
           function.getLinkage() != llvm::GlobalValue::AvailableExternallyLinkage &&
           function.getLinkage() != llvm::GlobalValue::WeakAnyLinkage)) {
        fail("unsupported single-target function ABI/linkage: " + function.getName()); return;
      }
      if (function.isIntrinsic() && !memoryIntrinsic(function) &&
          function.getName() != "llvm.lifetime.start.p0" && function.getName() != "llvm.lifetime.end.p0" &&
          function.getName() != "llvm.va_start" && function.getName() != "llvm.va_end" && function.getName() != "llvm.va_copy" &&
          function.getName() != "llvm.fabs.f32" && function.getName() != "llvm.fabs.f64" &&
          function.getName() != "llvm.fmuladd.f32" && function.getName() != "llvm.fmuladd.f64" &&
          function.getName() != "llvm.fma.f32" && function.getName() != "llvm.fma.f64") {
        fail("unsupported Sela intrinsic import: " + function.getName()); return;
      }
      auto identity = function.hasLocalLinkage() && !retainSymbolNames ? "f" + std::to_string(functionIndex) : function.getName().str();
      if (memoryIntrinsic(function) && function.arg_size() >= 3 &&
          function.getArg(2)->getType()->isIntegerTy(leftTarget.wordBits))
        identity = function.getName().drop_back(4).str() + ".word";
      if (!function.isDeclaration()) ++functionIndex;
      symbols[&function] = identity;
      pairs[&function] = &function;
    }
    auto visibility = [&](const llvm::GlobalValue &value) {
      return builder.getStringAttr(value.getVisibility() == llvm::GlobalValue::HiddenVisibility ? "hidden" :
          value.getVisibility() == llvm::GlobalValue::ProtectedVisibility ? "protected" : "default");
    };
    for (auto &global : input.globals()) {
      auto *data = global.hasInitializer() ? llvm::dyn_cast<llvm::ConstantDataSequential>(global.getInitializer()) : nullptr;
      auto alignment = builder.getI64IntegerAttr(global.getAlign() ? global.getAlign()->value() : 0);
      if (data && data->isString() && global.isConstant() && global.hasPrivateLinkage() && global.getAlign()) {
        op("sela.global", {}, {}, {attr("id", builder.getStringAttr(symbols.lookup(&global))),
            attr("bytes", builder.getStringAttr(data->getAsString())), attr("alignment", alignment),
            attr("unnamed", builder.getI32IntegerAttr(unsigned(global.getUnnamedAddr())))});
        continue;
      }
      auto element = type(global.getValueType(), global.getValueType());
      if (!element) return;
      Attribute value = global.hasInitializer() ? initializer(global.getInitializer(), global.getInitializer()) : builder.getUnitAttr();
      if (!value) return;
      StringRef linkage = global.hasPrivateLinkage() ? "private" : global.hasInternalLinkage() ? "internal" :
          global.hasCommonLinkage() ? "common" : global.hasWeakAnyLinkage() ? "weak" :
          global.hasAppendingLinkage() ? "appending" : "external";
      op("sela.global", {}, {}, {attr("id", builder.getStringAttr(symbols.lookup(&global))),
          attr("element", mlir::TypeAttr::get(element)), attr("initializer", value),
          attr("constant", builder.getBoolAttr(global.isConstant())), attr("declaration", builder.getBoolAttr(global.isDeclaration())),
          attr("linkage", builder.getStringAttr(linkage)), attr("dso_local", builder.getBoolAttr(global.isDSOLocal())),
          attr("visibility", visibility(global)), attr("alignment", alignment),
          attr("unnamed", builder.getI32IntegerAttr(unsigned(global.getUnnamedAddr())))});
    }
    for (auto &native : input) {
      if (debugFunction(native) || byteSwapPrimitive(native) || ir::findIntrinsic(native.getIntrinsicID())) continue;
      builder.setInsertionPointToEnd(module->getBody());
      llvm::SmallVector<mlir::Type> parameters, returns;
      for (auto &argument : native.args()) {
        auto parameter = leftVarargs.forwardedParameters.lookup(&native).contains(argument.getArgNo())
            ? mlir::Type(ir::VaListArgumentType::get(&context)) : type(argument.getType(), argument.getType());
        if (!parameter) return;
        parameters.push_back(parameter);
      }
      auto result = type(native.getReturnType(), native.getReturnType());
      if (!result) return;
      if (!mlir::isa<mlir::NoneType>(result)) returns.push_back(result);
      auto attributes = attributeList(native.getAttributes(), native.getAttributes(), native.arg_size());
      if (!error.empty()) return;
      auto *function = op("sela.func", {}, {}, {
          attr("id", builder.getStringAttr(symbols.lookup(&native))),
          attr("type", mlir::TypeAttr::get(builder.getFunctionType(parameters, returns))),
          attr("declaration", builder.getBoolAttr(native.isDeclaration())), attr("variadic", builder.getBoolAttr(native.isVarArg())),
          attr("internal", builder.getBoolAttr(native.hasInternalLinkage())), attr("weak", builder.getBoolAttr(native.hasWeakAnyLinkage())),
          attr("available_externally", builder.getBoolAttr(native.hasAvailableExternallyLinkage())),
          attr("dso_local", builder.getBoolAttr(native.isDSOLocal())), attr("visibility", visibility(native)),
          attr("attributes", attributes)}, true);
      llvm::SmallVector<mlir::NamedAttribute> codegen;
      for (const auto &[nativeName, publicName, baseline] : std::vector<std::tuple<StringRef, StringRef, StringRef>>{
             {"target-cpu", "cpu", leftTarget.cpu}, {"target-features", "features", leftTarget.features},
             {"tune-cpu", "tune", "generic"}, {"min-legal-vector-width", "min_vector_bits", "0"}}) {
        auto value = native.getFnAttribute(nativeName);
        if (!value.isStringAttribute() || value.getValueAsString() == baseline) continue;
        if (publicName == "min_vector_bits") {
          unsigned width;
          if (value.getValueAsString().getAsInteger(10, width) || width > 65536) { fail("invalid minimum vector width"); return; }
          codegen.push_back(attr(publicName, builder.getI32IntegerAttr(width)));
        } else codegen.push_back(attr(publicName, builder.getStringAttr(value.getValueAsString())));
      }
      if (!codegen.empty()) function->setAttr("codegen", builder.getDictionaryAttr(codegen));
      if (memoryIntrinsic(native))
        function->setAttr("intrinsic", builder.getStringAttr(native.getIntrinsicID() == llvm::Intrinsic::memcpy ? "memcpy" :
            native.getIntrinsicID() == llvm::Intrinsic::memmove ? "memmove" : "memset"));
      nativeABI(function, leftNativeABIs.lookup(&native), leftNativeABIs.lookup(&native));
      if (!error.empty()) return;
      if (native.isDeclaration()) continue;
      values.clear(); domainValues.clear(); conditionalValues.clear(); blocks.clear(); blockPairs.clear();
      for (auto &block : native) {
        auto *body = new mlir::Block();
        function->getRegion(0).push_back(body);
        blocks[&block] = body; blockPairs[&block] = &block;
      }
      auto *entry = blocks.lookup(&native.getEntryBlock());
      for (auto &argument : native.args()) {
        auto value = entry->addArgument(parameters[argument.getArgNo()], builder.getUnknownLoc());
        values[&argument] = value; domainValues[&argument] = value; pairs[&argument] = &argument;
      }
      for (auto &block : native) for (auto &phi : block.phis()) {
        if (block.isEntryBlock() || !permittedMetadata(phi)) { fail("unsupported native phi metadata"); return; }
        auto phiType = type(phi.getType(), phi.getType());
        if (!phiType) return;
        auto value = blocks.lookup(&block)->addArgument(phiType, builder.getUnknownLoc());
        values[&phi] = value; domainValues[&phi] = value; pairs[&phi] = &phi;
      }
      for (auto &block : native) {
        builder.setInsertionPointToEnd(blocks.lookup(&block));
        for (auto &nativeInstruction : block) {
          if (debugInstruction(nativeInstruction) || llvm::isa<llvm::PHINode>(nativeInstruction) ||
              leftVarargs.forwardingScaffolding.contains(&nativeInstruction)) continue;
          mergeInstruction(nativeInstruction, nativeInstruction);
          if (!error.empty()) return;
        }
      }
    }
  }

};

} // namespace

llvm::Error mergeProfiles(llvm::ArrayRef<CaptureObservation> observations,
                          StringRef bytecodeOutput, ArtifactSummary *summary) {
  if (observations.empty() || observations.size() > 256) return failure("invalid native observation inventory");
  struct Prepared {
    const targets::TargetInfo *target;
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> original, normalized;
    detail::NativeOverlaps overlaps;
    detail::NativeVarargs varargs;
    detail::NormalizedAggregateModule aggregates;
    bool normalizedSemantics = true;
  };
  std::vector<Prepared> inputs;
  std::set<std::string> seen;
  llvm::SmallVector<StringRef> targetIDs;
  for (const auto &observation : observations) {
    auto *target = targets::find(observation.target);
    if (!target || !seen.insert(observation.target).second)
      return failure("unknown or duplicate native observation target: " + observation.target);
    auto context = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic diagnostic;
    auto original = llvm::parseIRFile(observation.capture, diagnostic, *context);
    if (!original) return failure("cannot parse " + observation.target + " LLVM capture: " + diagnostic.getMessage());
    if (auto error = validateCapture(*original, *target)) return error;
    if (llvm::verifyModule(*original)) return failure("input LLVM capture verification failed: " + observation.target);
    // Keep the untouched capture alive through every final native inverse.
    // Normalization is a proved private transformation, never replacement of
    // missing observations with a previously synthesized target's output.
    auto normalized = llvm::CloneModule(*original);
    detail::NativeOverlaps overlaps;
    detail::NativeVarargs varargs;
    detail::NormalizedAggregateModule aggregates;
    auto recover = [&]() -> llvm::Error {
      auto storage = detail::discoverNativeOverlaps(*normalized);
      if (!storage) return storage.takeError();
      overlaps = std::move(*storage);
      auto cursors = detail::normalizeNativeVarargs(*normalized, target->id);
      if (!cursors) return cursors.takeError();
      varargs = std::move(*cursors);
      auto boundaries = detail::normalizeNativeAggregates(*normalized, target->id, nullptr, &varargs);
      if (!boundaries) return boundaries.takeError();
      aggregates = std::move(*boundaries);
      return llvm::Error::success();
    };
    bool normalizedSemantics = true;
    if (auto error = recover()) {
      // Recovering a language-neutral ABI/layout abstraction is optional.
      // The native signature and every coercion remain expressible in Sela.
      // Restart from the untouched observation; never keep a partial proof.
      llvm::consumeError(std::move(error));
      normalized = llvm::CloneModule(*original);
      overlaps.clear(); varargs = detail::NativeVarargs(); aggregates = detail::NormalizedAggregateModule();
      normalizedSemantics = false;
    }
    auto byteSwaps = detail::normalizeNativeByteSwaps(*normalized);
    if (!byteSwaps) return byteSwaps.takeError();
    if (llvm::verifyModule(*normalized)) return failure("private native normalization produced invalid LLVM IR");
    inputs.push_back({target, std::move(context), std::move(original), std::move(normalized),
                      std::move(overlaps), std::move(varargs), std::move(aggregates), normalizedSemantics});
    targetIDs.push_back(target->id);
  }
  std::vector<std::unique_ptr<LLVMImporter>> importers;
  std::vector<mlir::OwningOpRef<mlir::ModuleOp>> projections;
  llvm::SmallVector<mlir::ModuleOp> views;
  for (auto &input : inputs) {
    auto importer = std::make_unique<LLVMImporter>(input.varargs, input.overlaps, input.aggregates, *input.target);
    importer->importModule(*input.normalized);
    if (!importer->error.empty()) return failure(input.target->id + ": " + importer->error);
    importers.push_back(std::move(importer));
    std::string text;
    llvm::raw_string_ostream stream(text);
    importers.back()->module->print(stream);
    auto imported = mlir::parseSourceString<mlir::ModuleOp>(text, &importers.front()->context);
    if (!imported) return failure("cannot construct shared-context Sela projection");
    // Text parsing introduces locations in this private serialization buffer;
    // the emitter's public operations had unknown locations before transport.
    imported->walk([&](Operation *operation) {
      operation->setLoc(mlir::UnknownLoc::get(&importers.front()->context));
      for (auto &region : operation->getRegions()) for (auto &block : region)
        for (auto argument : block.getArguments()) argument.setLoc(operation->getLoc());
    });
    projections.push_back(std::move(imported));
    views.push_back(*projections.back());
  }
  mlir::OwningOpRef<mlir::ModuleOp> merged;
  std::vector<std::map<std::string, std::string>> sharedStorageIdentities;
  auto common = detail::mergeCommonModules(views, targetIDs);
  bool scoped = !common;
  auto retainScoped = [&]() -> llvm::Error {
    auto literal = detail::factorScopedModules(views, targetIDs);
    if (!literal) return literal.takeError();
    merged = std::move(literal->module);
    sharedStorageIdentities.clear();
    return llvm::Error::success();
  };
  if (common) {
    merged = std::move(common->module);
    sharedStorageIdentities = std::move(common->storageIdentities);
  } else {
    llvm::consumeError(common.takeError());
    if (auto error = retainScoped()) return error;
  }
  auto prove = [&]() -> llvm::Error {
  if (auto error = verifyModuleStructure(*merged)) return error;
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto &input = inputs[i];
    llvm::LLVMContext loweredContext;
    detail::NativeABIInverseHints inverseHints;
    auto lowered = detail::lowerModule(*merged, loweredContext, input.target->id, &inverseHints);
    if (!lowered) return lowered.takeError();
    if (input.normalizedSemantics) {
      auto varargs = detail::normalizeNativeVarargs(**lowered, input.target->id);
      if (!varargs) return varargs.takeError();
      auto inverse = detail::normalizeNativeAggregates(**lowered, input.target->id, &inverseHints, &*varargs);
      if (!inverse) return inverse.takeError();
    }
    if (llvm::verifyModule(**lowered)) return failure("native aggregate inverse normalization produced invalid LLVM IR");
    auto referenceCopy = llvm::CloneModule(*input.normalized);
    auto &reference = *referenceCopy;
    const auto &recordIDs = importers[i]->leftRecordIDs;
    for (auto *record : reference.getIdentifiedStructTypes()) record->setName("");
    for (const auto &entry : recordIDs) {
      std::string identity = entry.second;
      if (!sharedStorageIdentities.empty()) {
        auto found = sharedStorageIdentities[i].find(identity);
        if (found != sharedStorageIdentities[i].end()) identity = found->second;
      }
      entry.first->setName(identity);
    }
    canonicalize(reference, *input.target);
    canonicalize(**lowered, *input.target);
    auto expected = moduleText(reference), actual = moduleText(**lowered);
    if (expected != actual) {
      size_t mismatch = 0;
      while (mismatch < expected.size() && mismatch < actual.size() && expected[mismatch] == actual[mismatch]) ++mismatch;
      auto line = [&](const std::string &text) {
        size_t start = mismatch ? text.rfind('\n', std::min(mismatch - 1, text.size())) : std::string::npos;
        start = start == std::string::npos ? 0 : start + 1;
        size_t end = text.find('\n', start);
        // Attribute comments alone cannot distinguish an ABI-attribute fault
        // from an import-order fault. Include the adjacent signature as well.
        if (text.compare(start, 17, "; Function Attrs:") == 0 && end != std::string::npos)
          end = text.find('\n', end + 1);
        return text.substr(start, std::min<size_t>(end == std::string::npos ? text.size() - start : end - start, 300));
      };
      return failure(llvm::Twine(input.target->id) + " semantic round-trip comparison failed; expected `" +
          line(expected) + "`, reconstructed `" + line(actual) + "`");
    }
  }
  return llvm::Error::success();
  };
  if (auto error = prove()) {
    if (scoped) return error;
    // Factoring is optional, native equivalence is not. A failed factoring
    // proof retries only the already-imported Sela definitions, never LLVM or
    // native payloads. The final scoped graph must pass the identical proof.
    llvm::consumeError(std::move(error));
    if (auto error = retainScoped()) return error;
    if (auto error = prove()) return error;
  }
  std::error_code ec;
  llvm::raw_fd_ostream output(bytecodeOutput, ec, llvm::sys::fs::OF_None);
  if (ec) return llvm::errorCodeToError(ec);
  if (mlir::failed(mlir::writeBytecodeToFile(merged->getOperation(), output)))
    return failure("cannot serialize common MLIR bytecode");
  output.flush();
  if (output.has_error()) return failure("failed writing common MLIR bytecode");
  if (summary) return inspectArtifact(bytecodeOutput, *summary, targetIDs);
  return llvm::Error::success();
}

} // namespace sela
