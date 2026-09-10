#include "aot/IR/Compiler.h"
#include "Dialect.h"

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

#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace aot {
namespace {

using mlir::Attribute;
using mlir::Operation;
using llvm::StringRef;

constexpr StringRef X64Layout =
    "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";
constexpr StringRef I686Layout =
    "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128";

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

// Schema validation is intentionally closed: unknown optional-looking fields
// cannot hide semantic requirements or private debug payloads from consumers.
llvm::Error validateSchema(mlir::ModuleOp module) {
  const std::map<std::string, std::set<std::string>> allowed = {
      {"builtin.module", {"aot.schema", "aot.profiles", "aot.module_flags"}},
      {"aot.func", {"id", "type", "declaration", "variadic", "internal", "dso_local", "attributes"}},
      {"aot.global", {"id", "bytes", "alignment", "unnamed"}},
      {"aot.constant", {"value"}}, {"aot.address", {"global"}},
      {"aot.alloca", {"element", "alignment"}}, {"aot.load", {"alignment"}},
      {"aot.store", {"alignment"}}, {"aot.call", {"callee", "attributes", "tail"}},
      {"aot.binary", {"opcode", "flags"}}, {"aot.cast", {"opcode"}},
      {"aot.compare", {"predicate"}}, {"aot.return", {}}};
  std::string error;
  module.walk([&](Operation *operation) {
    auto found = allowed.find(operation->getName().getStringRef().str());
    if (found == allowed.end()) { error = "unknown required common IR operation"; return; }
    if (!mlir::isa<mlir::UnknownLoc>(operation->getLoc())) {
      error = "private source/debug locations are forbidden in publication IR"; return;
    }
    if (operation->getName().getStringRef() == "builtin.module" && operation != module.getOperation()) {
      error = "nested modules are forbidden in common IR"; return;
    }
    for (auto attribute : operation->getAttrs()) {
      if (!found->second.count(attribute.getName().str())) {
        error = "unknown common IR attribute: " + attribute.getName().str(); return;
      }
      attribute.getValue().walk([&](Attribute nested) {
        if (auto integer = mlir::dyn_cast<mlir::IntegerAttr>(nested))
          if (integer.getValue().getBitWidth() > 64)
            error = "oversized integer attribute in common IR";
      });
    }
    for (auto &region : operation->getRegions())
      for (auto &block : region)
        for (auto argument : block.getArguments())
          if (!mlir::isa<mlir::UnknownLoc>(argument.getLoc()))
            error = "private parameter debug location in common IR";
  });
  if (!error.empty()) return failure(error);
  return llvm::Error::success();
}

bool debugFunction(const llvm::Function &function) {
  return function.getName().starts_with("llvm.dbg.");
}

bool debugInstruction(const llvm::Instruction &instruction) {
  return llvm::isa<llvm::DbgInfoIntrinsic>(instruction);
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
        entry.first != llvm::LLVMContext::MD_DIAssignID)
      return false;
  return true;
}

void configureModule(llvm::Module &module, bool x64) {
  module.setModuleIdentifier("aot");
  module.setSourceFileName("aot");
  module.setTargetTriple(x64 ? "x86_64-unknown-linux-gnu"
                            : "i686-unknown-linux-gnu");
  module.setDataLayout(x64 ? X64Layout : I686Layout);
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

llvm::Error validateCapture(const llvm::Module &module, bool x64) {
  llvm::Triple triple(module.getTargetTriple());
  if (!triple.isOSLinux() ||
      triple.getArch() != (x64 ? llvm::Triple::x86_64 : llvm::Triple::x86))
    return failure("capture has the wrong Linux CPU profile");
  if (module.getDataLayoutStr() != (x64 ? X64Layout : I686Layout))
    return failure("capture data layout is not the pinned LLVM 18 profile");
  if (!module.getModuleInlineAsm().empty() || !module.alias_empty() ||
      !module.ifunc_empty())
    return failure("module assembly, aliases and ifuncs are not supported yet");
  const std::map<std::string, std::string> targetAttributes = {
      {"target-cpu", x64 ? "x86-64" : "i686"},
      {"target-features", x64 ? "+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" : "+cmov,+cx8,+x87"},
      {"tune-cpu", "generic"}, {"min-legal-vector-width", "0"}};
  for (const auto &function : module)
    for (const auto &[name, expected] : targetAttributes) {
      auto value = function.getFnAttribute(name);
      if (value.isValid() && (!value.isStringAttribute() || value.getValueAsString() != expected))
        return failure("capture changes a pinned CPU/profile attribute: " + llvm::Twine(name));
    }
  const std::map<std::string, uint64_t> expected = {
      {"NumRegisterParameters", 0}, {"wchar_size", 4}, {"PIC Level", 2},
      {"PIE Level", 2}, {"uwtable", 2}};
  llvm::SmallVector<llvm::Module::ModuleFlagEntry> flags;
  module.getModuleFlagsMetadata(flags);
  for (const auto &flag : flags) {
    StringRef key = flag.Key->getString();
    if (key == "Dwarf Version" || key == "Debug Info Version" ||
        key == "debug-info-assignment-tracking")
      continue;
    auto found = expected.find(key.str());
    auto *number = llvm::mdconst::dyn_extract<llvm::ConstantInt>(flag.Val);
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
void canonicalize(llvm::Module &module, bool x64) {
  llvm::StripDebugInfo(module);
  if (auto *ident = module.getNamedMetadata("llvm.ident"))
    module.eraseNamedMetadata(ident);
  std::vector<llvm::Function *> erase;
  unsigned functionIndex = 0;
  for (auto &function : module) {
    if (debugFunction(function)) {
      erase.push_back(&function);
      continue;
    }
    if (function.hasLocalLinkage())
      function.setName("f" + std::to_string(functionIndex));
    ++functionIndex;
    for (StringRef name : {"target-cpu", "target-features", "tune-cpu",
                           "min-legal-vector-width"})
      function.removeFnAttr(name);
    for (auto &argument : function.args())
      argument.setName("");
    for (auto &block : function) {
      block.setName("");
      for (auto &instruction : block) {
        instruction.setName("");
        instruction.setMetadata(llvm::LLVMContext::MD_tbaa, nullptr);
        instruction.setMetadata(llvm::LLVMContext::MD_tbaa_struct, nullptr);
      }
    }
  }
  for (auto *function : erase)
    function->eraseFromParent();
  unsigned globalIndex = 0;
  for (auto &global : module.globals())
    global.setName("g" + std::to_string(globalIndex++));
  configureModule(module, x64);
  canonicalizeFlags(module);
}

std::string moduleText(const llvm::Module &module) {
  std::string result;
  llvm::raw_string_ostream stream(result);
  module.print(stream, nullptr);
  return result;
}

class Merger {
public:
  mlir::MLIRContext context;
  mlir::OpBuilder builder;
  mlir::OwningOpRef<mlir::ModuleOp> module;
  std::string error;
  llvm::DenseMap<const llvm::Value *, mlir::Value> values;
  llvm::DenseMap<const llvm::Value *, const llvm::Value *> pairs;
  llvm::DenseMap<const llvm::GlobalValue *, std::string> symbols;

  Merger() : builder(&context) {
    context.getOrLoadDialect<ir::AOTDialect>();
    module = mlir::ModuleOp::create(builder.getUnknownLoc());
    (*module)->setAttr("aot.schema", builder.getI32IntegerAttr(1));
    (*module)->setAttr("aot.profiles", builder.getStrArrayAttr({"x86_64", "i686"}));
    builder.setInsertionPointToEnd(module->getBody());
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

  mlir::Type type(llvm::Type *left, llvm::Type *right) {
    if (left->isVoidTy() && right->isVoidTy())
      return builder.getNoneType();
    if (left->isPointerTy() && right->isPointerTy() &&
        left->getPointerAddressSpace() == 0 && right->getPointerAddressSpace() == 0)
      return ir::PointerType::get(&context);
    if (left->isIntegerTy() && right->isIntegerTy()) {
      unsigned lw = left->getIntegerBitWidth(), rw = right->getIntegerBitWidth();
      if (lw == rw && (lw == 1 || lw == 8 || lw == 16 || lw == 32 || lw == 64))
        return builder.getIntegerType(lw);
      if (lw == 64 && rw == 32)
        return ir::WordType::get(&context);
    }
    fail("unsupported scalar type correspondence; aggregates and FP are not in the first checkpoint");
    return {};
  }

  Attribute expression(uint64_t left, uint64_t right) {
    if (left == right)
      return builder.getI64IntegerAttr(left);
    if (left == 8 && right == 4)
      return builder.getStringAttr("pointer_bytes");
    fail("integer/layout values do not fit the verified symbolic expression set");
    return {};
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
      } else {
        fail("type/range ABI attributes are not supported in the first checkpoint");
        return {};
      }
      result.push_back(builder.getDictionaryAttr(fields));
    }
    return builder.getArrayAttr(result);
  }

  mlir::ArrayAttr attributeList(llvm::AttributeList left,
                               llvm::AttributeList right, unsigned count) {
    llvm::SmallVector<Attribute> result;
    auto add = [&](llvm::AttributeSet a, llvm::AttributeSet b) {
      auto x = attributes(a), y = attributes(b);
      if (!x || !y)
        return;
      if (x != y)
        fail("profile-dependent ABI attributes are not supported yet");
      result.push_back(x);
    };
    add(left.getFnAttrs(), right.getFnAttrs());
    add(left.getRetAttrs(), right.getRetAttrs());
    for (unsigned index = 0; index < count; ++index)
      add(left.getParamAttrs(index), right.getParamAttrs(index));
    return builder.getArrayAttr(result);
  }

  mlir::Value operand(const llvm::Value *left, const llvm::Value *right) {
    if (!error.empty())
      return {};
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
      if (av.sextOrTrunc(64) == bv.sextOrTrunc(64))
        value = builder.getIntegerAttr(builder.getI64Type(), av.sextOrTrunc(64));
      else if (av.zextOrTrunc(64) == bv.zextOrTrunc(64))
        value = builder.getIntegerAttr(builder.getI64Type(), av.zextOrTrunc(64));
      else
        value = expression(av.getZExtValue(), bv.getZExtValue());
      if (!value)
        return {};
      return op("aot.constant", mergedType, {}, {attr("value", value)})->getResult(0);
    }
    if (llvm::isa<llvm::ConstantPointerNull>(left) &&
        llvm::isa<llvm::ConstantPointerNull>(right))
      return op("aot.constant", mergedType, {},
                {attr("value", builder.getStringAttr("null"))})->getResult(0);
    const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(left);
    if (global && pairs.lookup(left) == right) {
      return op("aot.address", mergedType, {},
                {attr("global", builder.getStringAttr(symbols.lookup(global)))})
          ->getResult(0);
    }
    fail("unsupported constant, forward SSA reference or function address");
    return {};
  }

  void mergeInstruction(const llvm::Instruction &left,
                         const llvm::Instruction &right) {
    if (!error.empty())
      return;
    if (left.getOpcode() != right.getOpcode() || !permittedMetadata(left) ||
        !permittedMetadata(right)) {
      fail("unsupported instruction correspondence or semantic metadata");
      return;
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
      auto element = type(a->getAllocatedType(), b->getAllocatedType());
      auto alignment = expression(a->getAlign().value(), b->getAlign().value());
      if (!element || !alignment)
        return;
      result = op("aot.alloca", ir::PointerType::get(&context), {},
                  {attr("element", mlir::TypeAttr::get(element)),
                   attr("alignment", alignment)});
    } else if (auto *a = llvm::dyn_cast<llvm::LoadInst>(&left)) {
      auto *b = llvm::cast<llvm::LoadInst>(&right);
      if (a->isVolatile() || b->isVolatile() || a->isAtomic() || b->isAtomic()) {
        fail("volatile and atomic loads are not supported yet");
        return;
      }
      auto valueType = type(a->getType(), b->getType());
      auto pointer = operand(a->getPointerOperand(), b->getPointerOperand());
      auto alignment = expression(a->getAlign().value(), b->getAlign().value());
      if (!valueType || !pointer || !alignment)
        return;
      result = op("aot.load", valueType, pointer, {attr("alignment", alignment)});
    } else if (auto *a = llvm::dyn_cast<llvm::StoreInst>(&left)) {
      auto *b = llvm::cast<llvm::StoreInst>(&right);
      if (a->isVolatile() || b->isVolatile() || a->isAtomic() || b->isAtomic()) {
        fail("volatile and atomic stores are not supported yet");
        return;
      }
      auto value = operand(a->getValueOperand(), b->getValueOperand());
      auto pointer = operand(a->getPointerOperand(), b->getPointerOperand());
      auto alignment = expression(a->getAlign().value(), b->getAlign().value());
      if (!value || !pointer || !alignment)
        return;
      result = op("aot.store", {}, {value, pointer}, {attr("alignment", alignment)});
    } else if (auto *a = llvm::dyn_cast<llvm::CallInst>(&left)) {
      auto *b = llvm::cast<llvm::CallInst>(&right);
      if (!a->getCalledFunction() || !b->getCalledFunction() ||
          pairs.lookup(a->getCalledFunction()) != b->getCalledFunction() ||
          a->arg_size() != b->arg_size() || a->hasOperandBundles() ||
          b->hasOperandBundles() || a->getCallingConv() || b->getCallingConv() ||
          a->getTailCallKind() != b->getTailCallKind()) {
        fail("indirect/ABI-changing/bundled calls are not supported yet");
        return;
      }
      llvm::SmallVector<mlir::Value> arguments;
      for (unsigned i = 0; i < a->arg_size(); ++i) {
        auto argument = operand(a->getArgOperand(i), b->getArgOperand(i));
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
      result = op("aot.call", resultTypes, arguments,
                  {attr("callee", builder.getStringAttr(symbols.lookup(a->getCalledFunction()))),
                   attr("attributes", attributes),
                   attr("tail", builder.getI32IntegerAttr(a->getTailCallKind()))});
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
      result = op("aot.return", {}, returns);
    } else if (auto *a = llvm::dyn_cast<llvm::BinaryOperator>(&left)) {
      auto *b = llvm::cast<llvm::BinaryOperator>(&right);
      if (!a->getType()->isIntegerTy() || !b->getType()->isIntegerTy()) {
        fail("floating point is not supported in the first checkpoint");
        return;
      }
      auto valueType = type(a->getType(), b->getType());
      auto x = operand(a->getOperand(0), b->getOperand(0));
      auto y = operand(a->getOperand(1), b->getOperand(1));
      if (!valueType || !x || !y)
        return;
      unsigned af = arithmeticFlags(*a), bf = arithmeticFlags(*b);
      if (af != bf) {
        fail("arithmetic flags differ between profiles");
        return;
      }
      result = op("aot.binary", valueType, {x, y},
                  {attr("opcode", builder.getStringAttr(a->getOpcodeName())),
                   attr("flags", builder.getI32IntegerAttr(af))});
    } else if (auto *a = llvm::dyn_cast<llvm::CastInst>(&left)) {
      auto *b = llvm::cast<llvm::CastInst>(&right);
      auto valueType = type(a->getType(), b->getType());
      auto value = operand(a->getOperand(0), b->getOperand(0));
      if (!valueType || !value)
        return;
      result = op("aot.cast", valueType, value,
                  {attr("opcode", builder.getStringAttr(a->getOpcodeName()))});
    } else if (auto *a = llvm::dyn_cast<llvm::ICmpInst>(&left)) {
      auto *b = llvm::cast<llvm::ICmpInst>(&right);
      if (a->getPredicate() != b->getPredicate()) {
        fail("comparison predicates differ between profiles");
        return;
      }
      auto x = operand(a->getOperand(0), b->getOperand(0));
      auto y = operand(a->getOperand(1), b->getOperand(1));
      if (!x || !y)
        return;
      result = op("aot.compare", builder.getI1Type(), {x, y},
                  {attr("predicate", builder.getI32IntegerAttr(a->getPredicate()))});
    } else {
      fail("unsupported first-checkpoint instruction: " + StringRef(left.getOpcodeName()));
      return;
    }
    if (!left.getType()->isVoidTy()) {
      values[&left] = result->getResult(0);
      pairs[&left] = &right;
    }
  }

  void merge(llvm::Module &left, llvm::Module &right) {
    auto lf = moduleFlags(left), rf = moduleFlags(right);
    auto numReg = rf.find("NumRegisterParameters");
    bool hasNumReg = numReg != rf.end();
    std::pair<unsigned, uint64_t> numRegValue;
    if (hasNumReg) { numRegValue = numReg->second; rf.erase(numReg); }
    if (lf != rf) { fail("profile module flags differ beyond the supported native ABI setting"); return; }
    llvm::SmallVector<Attribute> flags;
    auto flag = [&](StringRef name, std::pair<unsigned, uint64_t> value, StringRef profile) {
      flags.push_back(builder.getDictionaryAttr({
          attr("name", builder.getStringAttr(name)),
          attr("behavior", builder.getI32IntegerAttr(value.first)),
          attr("value", builder.getI32IntegerAttr(value.second)),
          attr("profile", builder.getStringAttr(profile))}));
    };
    for (const auto &[name, value] : lf) flag(name, value, "both");
    if (hasNumReg) flag("NumRegisterParameters", numRegValue, "i686");
    (*module)->setAttr("aot.module_flags", builder.getArrayAttr(flags));
    unsigned globalIndex = 0;
    for (auto &a : left.globals()) {
      auto *b = right.getNamedGlobal(a.getName());
      auto *ad = a.hasInitializer() ? llvm::dyn_cast<llvm::ConstantDataSequential>(a.getInitializer()) : nullptr;
      auto *bd = b && b->hasInitializer() ? llvm::dyn_cast<llvm::ConstantDataSequential>(b->getInitializer()) : nullptr;
      if (!b || !ad || !bd || !ad->isString() || !bd->isString() ||
          ad->getAsString() != bd->getAsString() ||
          !a.isConstant() || !b->isConstant() ||
          a.getLinkage() != llvm::GlobalValue::PrivateLinkage ||
          b->getLinkage() != llvm::GlobalValue::PrivateLinkage ||
          a.getAddressSpace() || b->getAddressSpace() ||
          a.isThreadLocal() || b->isThreadLocal() ||
          a.hasSection() || b->hasSection() || a.hasComdat() || b->hasComdat() ||
          a.getUnnamedAddr() != b->getUnnamedAddr()) {
        fail("only matching private constant byte-string globals are supported yet");
        return;
      }
      auto alignment = expression(a.getAlign().valueOrOne().value(),
                                  b->getAlign().valueOrOne().value());
      if (!alignment)
        return;
      std::string id = "g" + std::to_string(globalIndex++);
      symbols[&a] = id;
      pairs[&a] = b;
      op("aot.global", {}, {},
         {attr("id", builder.getStringAttr(id)),
          attr("bytes", builder.getStringAttr(ad->getAsString())),
          attr("alignment", alignment),
          attr("unnamed", builder.getI32IntegerAttr(unsigned(a.getUnnamedAddr())))});
    }
    if (globalIndex != right.global_size()) {
      fail("global inventory differs between profiles");
      return;
    }
    unsigned index = 0;
    std::vector<std::pair<llvm::Function *, llvm::Function *>> functions;
    for (auto &a : left) {
      if (debugFunction(a))
        continue;
      auto *b = right.getFunction(a.getName());
      if (!b || a.isDeclaration() != b->isDeclaration() ||
          a.arg_size() != b->arg_size() || a.isVarArg() != b->isVarArg() ||
          a.getLinkage() != b->getLinkage() ||
          a.isDSOLocal() != b->isDSOLocal() ||
          a.getCallingConv() || b->getCallingConv() ||
          a.hasPersonalityFn() || b->hasPersonalityFn() ||
          a.hasPrefixData() || b->hasPrefixData() ||
          a.hasPrologueData() || b->hasPrologueData() ||
          a.hasComdat() || b->hasComdat() || a.hasSection() || b->hasSection() ||
          a.getVisibility() != llvm::GlobalValue::DefaultVisibility ||
          b->getVisibility() != llvm::GlobalValue::DefaultVisibility ||
          (a.getLinkage() != llvm::GlobalValue::ExternalLinkage &&
           a.getLinkage() != llvm::GlobalValue::InternalLinkage)) {
        fail("unsupported function inventory, linkage or ABI difference");
        return;
      }
      if (a.isDeclaration() && a.isIntrinsic() &&
          a.getName() != "llvm.lifetime.start.p0" &&
          a.getName() != "llvm.lifetime.end.p0") {
        fail("unsupported first-checkpoint import: " + a.getName());
        return;
      }
      std::string id = (!a.hasLocalLinkage())
                           ? a.getName().str() : "f" + std::to_string(index);
      ++index;
      symbols[&a] = id;
      pairs[&a] = b;
      functions.emplace_back(&a, b);
    }
    unsigned rightCount = 0;
    for (auto &f : right)
      rightCount += !debugFunction(f);
    if (functions.size() != rightCount) {
      fail("function inventory differs between profiles");
      return;
    }
    for (auto [a, b] : functions) {
      builder.setInsertionPointToEnd(module->getBody());
      llvm::SmallVector<mlir::Type> parameters, returns;
      for (unsigned i = 0; i < a->arg_size(); ++i) {
        auto t = type(a->getArg(i)->getType(), b->getArg(i)->getType());
        if (!t)
          return;
        parameters.push_back(t);
      }
      auto rt = type(a->getReturnType(), b->getReturnType());
      if (!rt)
        return;
      if (!mlir::isa<mlir::NoneType>(rt))
        returns.push_back(rt);
      auto attrs = attributeList(a->getAttributes(), b->getAttributes(), a->arg_size());
      if (!error.empty())
        return;
      auto *function = op("aot.func", {}, {},
          {attr("id", builder.getStringAttr(symbols.lookup(a))),
           attr("type", mlir::TypeAttr::get(builder.getFunctionType(parameters, returns))),
           attr("declaration", builder.getBoolAttr(a->isDeclaration())),
           attr("variadic", builder.getBoolAttr(a->isVarArg())),
           attr("internal", builder.getBoolAttr(a->hasInternalLinkage())),
           attr("dso_local", builder.getBoolAttr(a->isDSOLocal())),
           attr("attributes", attrs)}, true);
      if (a->isDeclaration())
        continue;
      if (a->size() != 1 || b->size() != 1) {
        fail("multiple-block CFG merging is not implemented in the first checkpoint");
        return;
      }
      auto *block = new mlir::Block();
      function->getRegion(0).push_back(block);
      for (unsigned i = 0; i < parameters.size(); ++i) {
        auto argument = block->addArgument(parameters[i], builder.getUnknownLoc());
        values[a->getArg(i)] = argument;
        pairs[a->getArg(i)] = b->getArg(i);
      }
      builder.setInsertionPointToEnd(block);
      auto ai = captureInstructions(*a), bi = captureInstructions(*b);
      if (ai.size() != bi.size()) {
        fail("profile instruction sequences need a normalization not implemented yet");
        return;
      }
      for (unsigned i = 0; i < ai.size(); ++i) {
        mergeInstruction(*ai[i], *bi[i]);
        if (!error.empty())
          return;
      }
    }
  }
};

class Lowerer {
public:
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  llvm::IRBuilder<llvm::NoFolder> builder;
  bool x64;
  std::string error;
  std::map<std::string, llvm::GlobalValue *> symbols;
  llvm::DenseMap<mlir::Value, llvm::Value *> values;

  explicit Lowerer(bool x64)
      : module(std::make_unique<llvm::Module>("aot", context)), builder(context), x64(x64) {
    configureModule(*module, x64);
  }

  void fail(const llvm::Twine &message) {
    if (error.empty())
      error = message.str();
  }

  llvm::Type *type(mlir::Type input) {
    if (mlir::isa<ir::PointerType>(input))
      return llvm::PointerType::get(context, 0);
    if (mlir::isa<ir::WordType>(input))
      return llvm::IntegerType::get(context, x64 ? 64 : 32);
    if (auto integer = mlir::dyn_cast<mlir::IntegerType>(input)) {
      unsigned width = integer.getWidth();
      if (width == 1 || width == 8 || width == 16 || width == 32 || width == 64)
        return llvm::IntegerType::get(context, width);
    }
    fail("artifact contains an unsupported type");
    return nullptr;
  }

  uint64_t expression(Attribute value) {
    if (auto integer = mlir::dyn_cast_or_null<mlir::IntegerAttr>(value))
      return integer.getValue().getZExtValue();
    if (auto text = mlir::dyn_cast_or_null<mlir::StringAttr>(value))
      if (text.getValue() == "pointer_bytes")
        return x64 ? 8 : 4;
    fail("invalid symbolic integer/layout expression");
    return 0;
  }

  llvm::MaybeAlign alignment(Operation &operation) {
    uint64_t n = expression(operation.getAttr("alignment"));
    if (n == 0 || n > (1ULL << 29) || !llvm::isPowerOf2_64(n)) {
      fail("invalid memory alignment");
      return llvm::MaybeAlign();
    }
    return llvm::Align(n);
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
      if (auto integer = dictionary.getAs<mlir::IntegerAttr>("integer")) {
        if (!llvm::Attribute::isIntAttrKind(kind)) {
          fail("ABI integer attribute has the wrong kind");
          return {};
        }
        uint64_t number = integer.getValue().getZExtValue();
        if (!((kind == llvm::Attribute::UWTable && number >= 1 && number <= 2) ||
              (kind == llvm::Attribute::Memory && number <= 63) ||
              ((kind == llvm::Attribute::Alignment || kind == llvm::Attribute::StackAlignment) &&
               number && number <= (1ULL << 29) && llvm::isPowerOf2_64(number)) ||
              kind == llvm::Attribute::Dereferenceable || kind == llvm::Attribute::DereferenceableOrNull)) {
          fail("unsupported integer code-generation attribute or value"); return {};
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

  llvm::Function *function(Operation &operation) {
    auto id = operation.getAttrOfType<mlir::StringAttr>("id");
    auto ft = operation.getAttrOfType<mlir::TypeAttr>("type");
    auto variadic = operation.getAttrOfType<mlir::BoolAttr>("variadic");
    auto internal = operation.getAttrOfType<mlir::BoolAttr>("internal");
    auto local = operation.getAttrOfType<mlir::BoolAttr>("dso_local");
    auto declaration = operation.getAttrOfType<mlir::BoolAttr>("declaration");
    auto signature = ft ? mlir::dyn_cast<mlir::FunctionType>(ft.getValue()) : mlir::FunctionType();
    if (!id || id.getValue().empty() || !signature || !variadic || !internal ||
        !local || !declaration || signature.getNumResults() > 1 ||
        symbols.count(id.getValue().str())) {
      fail("invalid or duplicate function declaration");
      return nullptr;
    }
    llvm::SmallVector<llvm::Type *> inputs;
    for (auto t : signature.getInputs()) {
      auto *native = type(t);
      if (!native)
        return nullptr;
      inputs.push_back(native);
    }
    llvm::Type *returns = signature.getNumResults() ? type(signature.getResult(0))
                                                   : llvm::Type::getVoidTy(context);
    if (!returns)
      return nullptr;
    auto *nativeType = llvm::FunctionType::get(returns, inputs, variadic.getValue());
    auto *result = llvm::Function::Create(nativeType,
        internal.getValue() ? llvm::GlobalValue::InternalLinkage : llvm::GlobalValue::ExternalLinkage,
        id.getValue(), module.get());
    result->setDSOLocal(local.getValue());
    result->setAttributes(attributeList(operation, inputs.size()));
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

  void instruction(Operation &operation) {
    StringRef name = operation.getName().getStringRef();
    llvm::Value *result = nullptr;
    if (name == "aot.constant") {
      if (!shape(operation, 0, 1)) return;
      auto *t = type(operation.getResult(0).getType());
      if (!t) return;
      auto value = operation.getAttr("value");
      if (t->isPointerTy()) {
        auto text = mlir::dyn_cast_or_null<mlir::StringAttr>(value);
        if (!text || text.getValue() != "null") {
          fail("pointer constant must be null"); return;
        }
        result = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(t));
      } else {
        uint64_t n = expression(value);
        if (!error.empty()) return;
        result = llvm::ConstantInt::get(t, n);
      }
    } else if (name == "aot.address") {
      if (!shape(operation, 0, 1)) return;
      auto id = operation.getAttrOfType<mlir::StringAttr>("global");
      auto found = id ? symbols.find(id.getValue().str()) : symbols.end();
      if (found == symbols.end() || !llvm::isa<llvm::GlobalVariable>(found->second)) {
        fail("address refers to an unknown byte global"); return;
      }
      result = found->second;
    } else if (name == "aot.alloca") {
      if (!shape(operation, 0, 1)) return;
      auto element = operation.getAttrOfType<mlir::TypeAttr>("element");
      auto *t = element ? type(element.getValue()) : nullptr;
      auto align = alignment(operation);
      if (!t || !align) { fail("invalid scalar allocation"); return; }
      auto *allocation = builder.CreateAlloca(t, 0, nullptr);
      allocation->setAlignment(*align);
      result = allocation;
    } else if (name == "aot.load") {
      if (!shape(operation, 1, 1)) return;
      auto *pointer = operand(operation.getOperand(0));
      auto *t = type(operation.getResult(0).getType());
      auto align = alignment(operation);
      if (!pointer || !t || !align || !pointer->getType()->isPointerTy()) {
        fail("invalid scalar load"); return;
      }
      result = builder.CreateAlignedLoad(t, pointer, *align);
    } else if (name == "aot.store") {
      if (!shape(operation, 2, 0)) return;
      auto *value = operand(operation.getOperand(0));
      auto *pointer = operand(operation.getOperand(1));
      auto align = alignment(operation);
      if (!value || !pointer || !align || !pointer->getType()->isPointerTy()) {
        fail("invalid scalar store"); return;
      }
      builder.CreateAlignedStore(value, pointer, *align);
    } else if (name == "aot.call") {
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
      result = call;
    } else if (name == "aot.return") {
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
    } else if (name == "aot.binary") {
      if (!shape(operation, 2, 1)) return;
      auto opcode = operation.getAttrOfType<mlir::StringAttr>("opcode");
      auto flags = operation.getAttrOfType<mlir::IntegerAttr>("flags");
      unsigned code = 0;
      if (opcode)
        for (unsigned i = llvm::Instruction::BinaryOpsBegin; i < llvm::Instruction::BinaryOpsEnd; ++i)
          if (opcode.getValue() == llvm::Instruction::getOpcodeName(i)) code = i;
      auto *left = operand(operation.getOperand(0));
      auto *right = operand(operation.getOperand(1));
      if (!code || !flags || flags.getInt() < 0 || flags.getInt() > 7 ||
          !validArithmeticFlags(code, unsigned(flags.getInt())) ||
          !left || !right || !left->getType()->isIntegerTy() || left->getType() != right->getType() ||
          code == llvm::Instruction::FAdd || code == llvm::Instruction::FSub ||
          code == llvm::Instruction::FMul || code == llvm::Instruction::FDiv ||
          code == llvm::Instruction::FRem) {
        fail("invalid scalar integer binary operation"); return;
      }
      auto *binary = llvm::cast<llvm::BinaryOperator>(
          builder.CreateBinOp(llvm::Instruction::BinaryOps(code), left, right));
      if (flags.getInt() & 1) binary->setHasNoUnsignedWrap();
      if (flags.getInt() & 2) binary->setHasNoSignedWrap();
      if (flags.getInt() & 4) binary->setIsExact();
      result = binary;
    } else if (name == "aot.cast") {
      if (!shape(operation, 1, 1)) return;
      auto opcode = operation.getAttrOfType<mlir::StringAttr>("opcode");
      unsigned code = 0;
      if (opcode)
        for (unsigned i = llvm::Instruction::CastOpsBegin; i < llvm::Instruction::CastOpsEnd; ++i)
          if (opcode.getValue() == llvm::Instruction::getOpcodeName(i)) code = i;
      auto *input = operand(operation.getOperand(0));
      auto *output = type(operation.getResult(0).getType());
      if (!code || !input || !output ||
          !llvm::CastInst::castIsValid(llvm::Instruction::CastOps(code), input, output)) {
        fail("invalid scalar cast"); return;
      }
      result = builder.CreateCast(llvm::Instruction::CastOps(code), input, output);
    } else if (name == "aot.compare") {
      if (!shape(operation, 2, 1)) return;
      auto predicate = operation.getAttrOfType<mlir::IntegerAttr>("predicate");
      auto *left = operand(operation.getOperand(0));
      auto *right = operand(operation.getOperand(1));
      if (!predicate || predicate.getInt() < llvm::CmpInst::FIRST_ICMP_PREDICATE ||
          predicate.getInt() > llvm::CmpInst::LAST_ICMP_PREDICATE || !left || !right ||
          left->getType() != right->getType() ||
          (!left->getType()->isIntegerTy() && !left->getType()->isPointerTy())) {
        fail("invalid scalar comparison"); return;
      }
      result = builder.CreateICmp(llvm::CmpInst::Predicate(predicate.getInt()), left, right);
    } else {
      fail("unknown required common operation: " + name); return;
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
    if (auto e = validateSchema(source)) { fail(llvm::toString(std::move(e))); return; }
    auto schema = source->getAttrOfType<mlir::IntegerAttr>("aot.schema");
    auto profiles = source->getAttrOfType<mlir::ArrayAttr>("aot.profiles");
    if (!schema || schema.getInt() != 1 || !profiles || profiles.size() != 2 ||
        profiles[0] != mlir::StringAttr::get(source.getContext(), "x86_64") ||
        profiles[1] != mlir::StringAttr::get(source.getContext(), "i686")) {
      fail("unsupported common IR schema or profile domain"); return;
    }
    auto flags = source->getAttrOfType<mlir::ArrayAttr>("aot.module_flags");
    if (!flags) { fail("missing module compilation flags"); return; }
    for (Attribute entry : flags) {
      auto record = mlir::dyn_cast<mlir::DictionaryAttr>(entry);
      auto name = record ? record.getAs<mlir::StringAttr>("name") : mlir::StringAttr();
      auto behavior = record ? record.getAs<mlir::IntegerAttr>("behavior") : mlir::IntegerAttr();
      auto value = record ? record.getAs<mlir::IntegerAttr>("value") : mlir::IntegerAttr();
      auto profile = record ? record.getAs<mlir::StringAttr>("profile") : mlir::StringAttr();
      if (!name || !behavior || !value || !profile || behavior.getInt() < 1 || behavior.getInt() > 8 ||
          (profile.getValue() != "both" && profile.getValue() != "i686")) {
        fail("invalid module compilation flag"); return;
      }
      for (auto field : record)
        if (field.getName() != "name" && field.getName() != "behavior" &&
            field.getName() != "value" && field.getName() != "profile") {
          fail("unknown module flag record field"); return;
        }
      bool ordinary = (name.getValue() == "wchar_size" && value.getInt() == 4) ||
                      ((name.getValue() == "PIC Level" || name.getValue() == "PIE Level" ||
                        name.getValue() == "uwtable") && value.getInt() == 2);
      bool native32 = name.getValue() == "NumRegisterParameters" && value.getInt() == 0;
      if ((!ordinary && !native32) || (ordinary && profile.getValue() != "both") ||
          (native32 && profile.getValue() != "i686") ||
          module->getModuleFlag(name.getValue())) {
        fail("unsupported or duplicate native module compilation flag"); return;
      }
      if (profile.getValue() == "both" || !x64)
        module->addModuleFlag(llvm::Module::ModFlagBehavior(behavior.getInt()),
                              name.getValue(), uint32_t(value.getInt()));
    }
    for (auto &operation : source.getBody()->getOperations()) {
      StringRef name = operation.getName().getStringRef();
      if (name == "aot.func") {
        function(operation);
      } else if (name == "aot.global") {
        if (!shape(operation, 0, 0)) return;
        auto id = operation.getAttrOfType<mlir::StringAttr>("id");
        auto bytes = operation.getAttrOfType<mlir::StringAttr>("bytes");
        auto unnamed = operation.getAttrOfType<mlir::IntegerAttr>("unnamed");
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
      if (operation.getName().getStringRef() != "aot.func") continue;
      if (operation.getNumRegions() != 1) { fail("invalid function region count"); return; }
      auto declaration = operation.getAttrOfType<mlir::BoolAttr>("declaration");
      auto &region = operation.getRegion(0);
      if (declaration.getValue()) {
        if (!region.empty()) fail("declaration unexpectedly contains code");
        if (!error.empty()) return;
        continue;
      }
      if (!region.hasOneBlock()) { fail("only one-block functions are supported yet"); return; }
      auto id = operation.getAttrOfType<mlir::StringAttr>("id");
      auto *function = llvm::cast<llvm::Function>(symbols[id.getValue().str()]);
      auto &block = region.front();
      if (block.getNumArguments() != function->arg_size() || block.empty()) {
        fail("function body parameter count or terminator mismatch"); return;
      }
      values.clear();
      for (unsigned i = 0; i < block.getNumArguments(); ++i) {
        if (type(block.getArgument(i).getType()) != function->getArg(i)->getType()) {
          fail("function body parameter type mismatch"); return;
        }
        values[block.getArgument(i)] = function->getArg(i);
      }
      builder.SetInsertPoint(llvm::BasicBlock::Create(context, "", function));
      for (auto &child : block) {
        if (builder.GetInsertBlock()->getTerminator()) {
          fail("operation follows a function terminator"); return;
        }
        instruction(child);
        if (!error.empty()) return;
      }
      if (!builder.GetInsertBlock()->getTerminator()) {
        fail("function is missing its return"); return;
      }
    }
    std::string diagnostics;
    llvm::raw_string_ostream stream(diagnostics);
    if (llvm::verifyModule(*module, &stream))
      fail("specialized LLVM verification failed: " + stream.str());
  }
};

void summarize(mlir::ModuleOp module, ArtifactSummary &summary) {
  summary = {};
  module.walk([&](Operation *operation) {
    StringRef name = operation->getName().getStringRef();
    if (name == "aot.func") ++summary.functions;
    else if (name == "aot.global") ++summary.globals;
    else if (name.starts_with("aot.")) ++summary.operations;
    for (auto type : operation->getResultTypes())
      summary.symbolicTypes += mlir::isa<ir::WordType>(type);
    for (auto attribute : operation->getAttrs()) {
      if (auto text = mlir::dyn_cast<mlir::StringAttr>(attribute.getValue()))
        summary.symbolicConstants += text.getValue() == "pointer_bytes";
      if (auto typed = mlir::dyn_cast<mlir::TypeAttr>(attribute.getValue()))
        summary.symbolicTypes += mlir::isa<ir::WordType>(typed.getValue());
    }
  });
}

llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>> readArtifact(
    StringRef path, mlir::MLIRContext &context) {
  context.getOrLoadDialect<ir::AOTDialect>();
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) return llvm::errorCodeToError(buffer.getError());
  if ((*buffer)->getBufferSize() > 64 * 1024 * 1024 ||
      !mlir::isBytecode((*buffer)->getMemBufferRef()))
    return failure("expected bounded MLIR bytecode, not textual IR");
  llvm::SourceMgr manager;
  manager.AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(manager, &context);
  if (!module || mlir::failed(mlir::verify(*module)))
    return failure("cannot parse or verify the common MLIR artifact");
  return std::move(module);
}

} // namespace

llvm::Error mergeProfiles(StringRef x86_64Capture, StringRef i686Capture,
                          StringRef bytecodeOutput, ArtifactSummary *summary) {
  llvm::LLVMContext leftContext, rightContext;
  llvm::SMDiagnostic diagnostic;
  auto left = llvm::parseIRFile(x86_64Capture, diagnostic, leftContext);
  if (!left) return failure("cannot parse x86_64 LLVM capture: " + diagnostic.getMessage());
  auto right = llvm::parseIRFile(i686Capture, diagnostic, rightContext);
  if (!right) return failure("cannot parse i686 LLVM capture: " + diagnostic.getMessage());
  if (auto e = validateCapture(*left, true)) return e;
  if (auto e = validateCapture(*right, false)) return e;
  if (llvm::verifyModule(*left) || llvm::verifyModule(*right))
    return failure("input LLVM capture verification failed");
  Merger merger;
  merger.merge(*left, *right);
  if (!merger.error.empty()) return failure(merger.error);
  if (mlir::failed(mlir::verify(*merger.module)))
    return failure("generated common MLIR verification failed");
  for (bool x64 : {true, false}) {
    Lowerer lowerer(x64);
    lowerer.lower(*merger.module);
    if (!lowerer.error.empty()) return failure(lowerer.error);
    auto &reference = x64 ? *left : *right;
    canonicalize(reference, x64);
    canonicalize(*lowerer.module, x64);
    if (moduleText(reference) != moduleText(*lowerer.module))
      return failure(x64 ? "x86_64 semantic round-trip comparison failed"
                         : "i686 semantic round-trip comparison failed");
  }
  std::error_code ec;
  llvm::raw_fd_ostream output(bytecodeOutput, ec, llvm::sys::fs::OF_None);
  if (ec) return llvm::errorCodeToError(ec);
  if (mlir::failed(mlir::writeBytecodeToFile(merger.module->getOperation(), output)))
    return failure("cannot serialize common MLIR bytecode");
  output.flush();
  if (output.has_error()) return failure("failed writing common MLIR bytecode");
  if (summary) summarize(*merger.module, *summary);
  return llvm::Error::success();
}

llvm::Error lowerArtifact(StringRef bytecodeInput, StringRef profile,
                          StringRef llvmIROutput) {
  if (profile != "x86_64" && profile != "i686")
    return failure("unsupported target profile; expected x86_64 or i686");
  mlir::MLIRContext context;
  auto source = readArtifact(bytecodeInput, context);
  if (!source) return source.takeError();
  Lowerer lowerer(profile == "x86_64");
  lowerer.lower(**source);
  if (!lowerer.error.empty()) return failure(lowerer.error);
  std::error_code ec;
  llvm::raw_fd_ostream output(llvmIROutput, ec, llvm::sys::fs::OF_Text);
  if (ec) return llvm::errorCodeToError(ec);
  lowerer.module->print(output, nullptr);
  output.flush();
  if (output.has_error()) return failure("failed writing specialized LLVM IR");
  return llvm::Error::success();
}

llvm::Error inspectArtifact(StringRef bytecodeInput, ArtifactSummary &summary) {
  mlir::MLIRContext context;
  auto source = readArtifact(bytecodeInput, context);
  if (!source) return source.takeError();
  for (bool x64 : {true, false}) {
    Lowerer lowerer(x64);
    lowerer.lower(**source);
    if (!lowerer.error.empty()) return failure(lowerer.error);
  }
  summarize(**source, summary);
  return llvm::Error::success();
}

} // namespace aot
