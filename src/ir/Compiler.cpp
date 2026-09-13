#include "nier/IR/Compiler.h"
#include "Internal.h"
#include "NativeTargets.h"
#include "NativeABIBridge.h"
#include "OverlapLayout.h"
#include "ConditionalSpecialization.h"
#include "nier/IR/Dialect.h"

#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/Bytecode/BytecodeReader.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
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
#include <functional>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nier {
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

// These are public finite-domain expressions, not requests to run a foreign
// native compiler. Both sides are checked even in a one-target device library.
llvm::Expected<uint64_t> publicInteger(Attribute value, bool word64) {
  if (auto integer = mlir::dyn_cast_or_null<mlir::IntegerAttr>(value)) {
    if (integer.getValue().getBitWidth() <= 64) return integer.getValue().getZExtValue();
  } else if (auto text = mlir::dyn_cast_or_null<mlir::StringAttr>(value)) {
    if (text.getValue() == "pointer_bytes") return word64 ? 8 : 4;
  } else if (auto record = mlir::dyn_cast_or_null<mlir::DictionaryAttr>(value)) {
    auto wide = record.getAs<mlir::IntegerAttr>("word64");
    auto narrow = record.getAs<mlir::IntegerAttr>("word32");
    if (record.size() == 2 && wide && narrow && wide.getValue().getBitWidth() <= 64 &&
        narrow.getValue().getBitWidth() <= 64)
      return (word64 ? wide : narrow).getValue().getZExtValue();
  }
  return failure("invalid public word-domain integer expression");
}

bool pureLiteral(Attribute value) {
  if (mlir::isa<mlir::IntegerAttr>(value)) return true;
  if (auto number = mlir::dyn_cast<mlir::FloatAttr>(value))
    return number.getType().isF32() || number.getType().isF64();
  if (auto text = mlir::dyn_cast<mlir::StringAttr>(value))
    return text.getValue() == "zero" || text.getValue() == "null";
  if (auto elements = mlir::dyn_cast<mlir::ArrayAttr>(value))
    return elements.size() <= 1024 * 1024 && llvm::all_of(elements, pureLiteral);
  return false;
}

llvm::Error validateInitializer(Attribute value, unsigned depth = 0) {
  if (!value || depth > 64) return failure("invalid or excessively nested public initializer");
  if (mlir::isa<mlir::IntegerAttr>(value)) return llvm::Error::success();
  if (auto number = mlir::dyn_cast<mlir::FloatAttr>(value)) {
    if (number.getType().isF32() || number.getType().isF64()) return llvm::Error::success();
  }
  if (auto text = mlir::dyn_cast<mlir::StringAttr>(value)) {
    if (text.getValue() == "zero" || text.getValue() == "undef" || text.getValue() == "poison" ||
        text.getValue() == "null" || text.getValue() == "pointer_bytes") return llvm::Error::success();
  }
  if (auto elements = mlir::dyn_cast<mlir::ArrayAttr>(value)) {
    if (elements.size() > 1024 * 1024) return failure("oversized public initializer inventory");
    for (auto element : elements) if (auto error = validateInitializer(element, depth + 1)) return error;
    return llvm::Error::success();
  }
  if (auto record = mlir::dyn_cast<mlir::DictionaryAttr>(value)) {
    if (record.get("word64") || record.get("word32")) {
      auto integer = publicInteger(record, true);
      return integer ? llvm::Error::success() : integer.takeError();
    }
    if (record.size() == 1 && record.getAs<mlir::StringAttr>("symbol")) return llvm::Error::success();
    if (record.size() == 2 && record.getAs<mlir::ArrayAttr>("array") && record.get("count"))
      return validateInitializer(record.get("array"), depth + 1);
    auto opcode = record.getAs<mlir::StringAttr>("op");
    auto indices = record.getAs<mlir::ArrayAttr>("indices");
    if (record.size() == 5 && opcode && opcode.getValue() == "gep" && indices &&
        record.getAs<mlir::TypeAttr>("element") && record.getAs<mlir::BoolAttr>("inbounds")) {
      if (auto error = validateInitializer(record.get("base"), depth + 1)) return error;
      for (auto index : indices) {
        auto entry = mlir::dyn_cast<mlir::DictionaryAttr>(index);
        if (!entry || entry.size() != 2 || !entry.getAs<mlir::TypeAttr>("type"))
          return failure("invalid public address index record");
        auto integer = publicInteger(entry.get("value"), true);
        if (!integer) return integer.takeError();
      }
      return llvm::Error::success();
    }
  }
  return failure("unknown public initializer expression");
}

llvm::Error validateInitializerShape(mlir::Type type, Attribute value, unsigned depth = 0) {
  if (depth > 64) return failure("public initializer shape nesting exceeds the qualified bound");
  auto elements = mlir::dyn_cast<mlir::ArrayAttr>(value);
  if (auto record = mlir::dyn_cast<mlir::DictionaryAttr>(value)) {
    if (auto explicitElements = record.getAs<mlir::ArrayAttr>("array")) {
      auto array = mlir::dyn_cast<ir::ArrayType>(type);
      auto wide = publicInteger(record.get("count"), true);
      auto narrow = publicInteger(record.get("count"), false);
      if (!wide || !narrow) {
        if (!wide) llvm::consumeError(wide.takeError());
        if (!narrow) llvm::consumeError(narrow.takeError());
        return failure("invalid public array initializer count");
      }
      if (!array || array.getNumElements(true) != *wide || array.getNumElements(false) != *narrow)
        return failure("array initializer and storage extents disagree in a public word domain");
      for (auto element : explicitElements)
        if (auto error = validateInitializerShape(array.getElementType(), element, depth + 1)) return error;
      return llvm::Error::success();
    }
  }
  if (!elements) return llvm::Error::success();
  if (auto array = mlir::dyn_cast<ir::ArrayType>(type)) {
    if (elements.size() != array.getNumElements(true) || elements.size() != array.getNumElements(false))
      return failure("literal array initializer requires matching extents in both public word domains");
    for (auto element : elements)
      if (auto error = validateInitializerShape(array.getElementType(), element, depth + 1)) return error;
    return llvm::Error::success();
  }
  if (auto record = mlir::dyn_cast<ir::RecordType>(type)) {
    if (elements.size() != record.getFields().size()) return failure("public record initializer field count mismatch");
    for (unsigned i = 0; i < elements.size(); ++i)
      if (auto error = validateInitializerShape(record.getFields()[i], elements[i], depth + 1)) return error;
    return llvm::Error::success();
  }
  // Overlap carriers are chosen by the native layout implementation. Their
  // public storage is one carrier field, but its scalar type is not guessed.
  if (mlir::isa<ir::OverlapType>(type) && elements.size() == 1) return llvm::Error::success();
  return failure("aggregate initializer does not match a public storage type");
}

llvm::Error validateAttributeList(Attribute value) {
  auto groups = mlir::dyn_cast<mlir::ArrayAttr>(value);
  if (!groups) return failure("invalid public ABI attribute list");
  for (auto group : groups) {
    auto attributes = mlir::dyn_cast<mlir::ArrayAttr>(group);
    if (!attributes) return failure("invalid public ABI attribute group");
    for (auto attribute : attributes) {
      auto record = mlir::dyn_cast<mlir::DictionaryAttr>(attribute);
      auto name = record ? record.getAs<mlir::StringAttr>("name") : mlir::StringAttr();
      if (!name || (record.get("integer") && record.get("string")))
        return failure("invalid or ambiguous public ABI attribute");
      for (auto field : record)
        if (field.getName() != "name" && field.getName() != "integer" && field.getName() != "string")
          return failure("unknown public ABI attribute record field");
      if (auto raw = record.get("string")) {
        if (!mlir::isa<mlir::StringAttr>(raw) || configurationAttribute(name.getValue()) ||
            (name.getValue() != "no-trapping-math" && name.getValue() != "stack-protector-buffer-size" &&
             name.getValue() != "frame-pointer"))
          return failure("unsupported public string code-generation attribute");
        continue;
      }
      auto kind = llvm::Attribute::getAttrKindFromName(name.getValue());
      if (kind == llvm::Attribute::None || llvm::Attribute::isTypeAttrKind(kind))
        return failure("unsupported public ABI attribute kind");
      if (auto raw = record.get("integer")) {
        if (!llvm::Attribute::isIntAttrKind(kind)) return failure("public ABI integer attribute has the wrong kind");
        for (bool word64 : {true, false}) {
          auto value = publicInteger(raw, word64);
          if (!value) return value.takeError();
          uint64_t number = *value;
          if (!((kind == llvm::Attribute::UWTable && number >= 1 && number <= 2) ||
                (kind == llvm::Attribute::Memory && number <= 63) || kind == llvm::Attribute::AllocSize ||
                ((kind == llvm::Attribute::Alignment || kind == llvm::Attribute::StackAlignment) &&
                 number && number <= (1ULL << 29) && llvm::isPowerOf2_64(number)) ||
                kind == llvm::Attribute::Dereferenceable || kind == llvm::Attribute::DereferenceableOrNull))
            return failure("unsupported public integer ABI attribute value");
        }
      } else if (!llvm::Attribute::isEnumAttrKind(kind))
        return failure("public ABI enum attribute has the wrong kind");
    }
  }
  return llvm::Error::success();
}

llvm::Error validatePublicTypes(mlir::ModuleOp module) {
  std::map<std::string, mlir::Type> identities;
  llvm::DenseSet<mlir::Type> checked;
  std::function<llvm::Error(mlir::Type, unsigned)> visit = [&](mlir::Type type, unsigned depth) -> llvm::Error {
    if (depth > 64) return failure("public type nesting exceeds the qualified bound");
    if (!checked.insert(type).second) return llvm::Error::success();
    if (mlir::isa<ir::PointerType, ir::WordType, ir::VaListType>(type) || type.isF32() || type.isF64())
      return llvm::Error::success();
    if (auto integer = mlir::dyn_cast<mlir::IntegerType>(type)) {
      unsigned width = integer.getWidth();
      if (width == 1 || width == 8 || width == 16 || width == 32 || width == 64)
        return llvm::Error::success();
    }
    if (auto function = mlir::dyn_cast<mlir::FunctionType>(type)) {
      if (function.getNumResults() > 1) return failure("public function type has multiple results");
      for (auto input : function.getInputs()) if (auto error = visit(input, depth + 1)) return error;
      for (auto result : function.getResults()) if (auto error = visit(result, depth + 1)) return error;
      return llvm::Error::success();
    }
    if (auto array = mlir::dyn_cast<ir::ArrayType>(type)) {
      if (array.getNumElements(true) > (1ULL << 30) || array.getNumElements(false) > (1ULL << 30))
        return failure("oversized public word-domain array extent");
      return visit(array.getElementType(), depth + 1);
    }
    llvm::StringRef identity;
    llvm::ArrayRef<mlir::Type> fields;
    if (auto record = mlir::dyn_cast<ir::RecordType>(type)) {
      identity = record.getIdentity(); fields = record.getFields();
    } else if (auto overlap = mlir::dyn_cast<ir::OverlapType>(type)) {
      identity = overlap.getIdentity(); fields = overlap.getAlternatives();
      auto domains = overlap.getDomains();
      if (identity.empty() || fields.empty() || fields.size() > 64 || fields.size() != domains.size())
        return failure("invalid public overlap alternative inventory");
      unsigned inventory = 0;
      for (unsigned i = 0; i < fields.size(); ++i) {
        if (domains[i] < 1 || domains[i] > 3) return failure("invalid public overlap domain");
        inventory |= domains[i];
        auto field = fields[i];
        bool integer = field.isInteger(8) || field.isInteger(16) || field.isInteger(32) || field.isInteger(64);
        if (!integer && !field.isF32() && !field.isF64() && !mlir::isa<ir::WordType, ir::PointerType>(field))
          return failure("unsupported public overlapping-storage alternative");
      }
      if (inventory != 3) return failure("overlap has no storage in one public word domain");
    } else return failure("artifact contains an unsupported public type");
    if (!identity.empty()) {
      if (identity.size() < 2 || identity.size() > 64 || !identity.starts_with("r") ||
          !llvm::all_of(identity.drop_front(), [](char c) { return c >= '0' && c <= '9'; }))
        return failure("public storage identities must be opaque r-prefixed integers");
      auto inserted = identities.emplace(identity.str(), type);
      if (!inserted.second && inserted.first->second != type)
        return failure("conflicting definitions of a public storage identity");
    }
    for (auto field : fields) if (auto error = visit(field, depth + 1)) return error;
    return llvm::Error::success();
  };
  std::string message;
  auto check = [&](mlir::Type type) {
    if (auto error = visit(type, 0)) message = llvm::toString(std::move(error));
  };
  module.walk([&](Operation *operation) {
    for (auto type : operation->getResultTypes()) check(type);
    for (auto &region : operation->getRegions()) for (auto &block : region)
      for (auto argument : block.getArguments()) check(argument.getType());
    for (auto attribute : operation->getAttrs()) attribute.getValue().walk([&](Attribute nested) {
      if (auto typed = mlir::dyn_cast<mlir::TypeAttr>(nested)) check(typed.getValue());
    });
  });
  return message.empty() ? llvm::Error::success() : failure(message);
}

// Schema validation is intentionally closed: unknown optional-looking fields
// cannot hide semantic requirements or private debug payloads from consumers.
llvm::Error validateSchema(mlir::ModuleOp module) {
  auto schema = module->getAttrOfType<mlir::IntegerAttr>("nier.schema");
  if (!schema || schema.getValue().getBitWidth() > 64 || schema.getInt() != 1)
    return failure("unsupported common IR schema");
  if (auto raw = module->getAttr("nier.module_flags")) {
    auto flags = mlir::dyn_cast<mlir::ArrayAttr>(raw);
    if (!flags) return failure("invalid public module flag list");
    std::set<std::string> names;
    for (auto entry : flags) {
      auto record = mlir::dyn_cast<mlir::DictionaryAttr>(entry);
      auto name = record ? record.getAs<mlir::StringAttr>("name") : mlir::StringAttr();
      auto behavior = record ? record.getAs<mlir::IntegerAttr>("behavior") : mlir::IntegerAttr();
      auto value = record ? record.getAs<mlir::IntegerAttr>("value") : mlir::IntegerAttr();
      auto profile = record ? record.getAs<mlir::StringAttr>("profile") : mlir::StringAttr();
      if (!record || record.size() != 4 || !name || !behavior || !value || !profile ||
          behavior.getValue().getBitWidth() > 64 || value.getValue().getBitWidth() > 64 ||
          behavior.getInt() < 1 || behavior.getInt() > 8 || !names.insert(name.getValue().str()).second)
        return failure("invalid or duplicate public module flag record");
      bool ordinary = (name.getValue() == "wchar_size" && value.getInt() == 4) ||
          (name.getValue() == "frame-pointer" && value.getInt() >= 0 && value.getInt() <= 2) ||
          ((name.getValue() == "PIC Level" || name.getValue() == "PIE Level" ||
            name.getValue() == "uwtable") && value.getInt() == 2);
      bool native32 = name.getValue() == "NumRegisterParameters" && value.getInt() == 0;
      if ((!ordinary && !native32) || (ordinary && profile.getValue() != "both") ||
          (native32 && profile.getValue() != "i686"))
        return failure("unsupported public module compilation flag");
    }
  }
  const std::map<std::string, std::set<std::string>> allowed = {
      {"builtin.module", {"nier.schema", "nier.module_flags"}},
      {"nier.func", {"id", "type", "declaration", "variadic", "internal", "weak", "available_externally", "dso_local", "visibility", "intrinsic", "attributes", "block_domains", "native_abi"}},
      {"nier.global", {"id", "bytes", "alignment", "unnamed", "element", "initializer", "constant", "declaration", "linkage", "dso_local", "visibility"}},
      {"nier.constant", {"value"}}, {"nier.address", {"global"}},
      {"nier.alloca", {"element", "alignment"}}, {"nier.load", {"alignment", "volatile"}},
      {"nier.store", {"alignment", "volatile"}}, {"nier.call", {"callee", "attributes", "tail", "native_abi"}},
      {"nier.call_indirect", {"type", "variadic", "attributes", "tail", "native_abi"}},
      {"nier.binary", {"opcode", "flags"}}, {"nier.cast", {"opcode"}},
      {"nier.compare", {"predicate"}}, {"nier.return", {}},
      {"nier.gep", {"element", "inbounds"}},
      {"nier.select", {}}, {"nier.fneg", {}}, {"nier.bswap", {}},
      {"nier.va_arg", {}}, {"nier.va_forward", {}},
      {"nier.br", {"loop", "loop_id"}}, {"nier.cond_br", {"true_count", "loop", "loop_id"}},
      {"nier.switch", {"cases", "argument_counts", "case_domains"}},
      {"nier.unreachable", {}}};
  const std::map<std::string, std::set<std::string>> required = {
      {"nier.func", {"id", "type", "declaration", "variadic", "internal", "dso_local", "attributes"}},
      {"nier.constant", {"value"}}, {"nier.address", {"global"}},
      {"nier.alloca", {"element", "alignment"}}, {"nier.load", {"alignment"}},
      {"nier.store", {"alignment"}}, {"nier.call", {"callee", "attributes", "tail"}},
      {"nier.call_indirect", {"type", "variadic", "attributes", "tail"}},
      {"nier.binary", {"opcode", "flags"}}, {"nier.cast", {"opcode"}},
      {"nier.compare", {"predicate"}}, {"nier.gep", {"element", "inbounds"}},
      {"nier.cond_br", {"true_count"}}, {"nier.switch", {"cases", "argument_counts"}}};
  const std::set<std::string> booleans = {"declaration", "variadic", "internal", "weak", "available_externally",
      "dso_local", "constant", "inbounds", "volatile"};
  const std::set<std::string> strings = {"id", "global", "callee", "visibility", "linkage", "intrinsic", "opcode", "bytes"};
  std::string error;
  module.walk([&](Operation *operation) {
    auto found = allowed.find(operation->getName().getStringRef().str());
    if (found == allowed.end()) { error = "unknown required common IR operation"; return; }
    if (auto fields = required.find(found->first); fields != required.end())
      for (auto field : fields->second) if (!operation->getAttr(field)) {
        error = "missing required public operation attribute: " + field; return;
      }
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
      std::string field = attribute.getName().str();
      if ((booleans.count(field) && !mlir::isa<mlir::BoolAttr>(attribute.getValue())) ||
          (strings.count(field) && !mlir::isa<mlir::StringAttr>(attribute.getValue())) ||
          ((field == "type" || field == "element" || field == "native_abi") &&
           !mlir::isa<mlir::TypeAttr>(attribute.getValue()))) {
        error = "invalid public operation attribute type: " + field; return;
      }
      if (field == "attributes")
        if (auto invalid = validateAttributeList(attribute.getValue())) {
          error = llvm::toString(std::move(invalid)); return;
        }
      if (field == "alignment") for (bool word64 : {true, false}) {
        auto value = publicInteger(attribute.getValue(), word64);
        if (!value) error = llvm::toString(value.takeError());
        else if (!*value && found->first == "nier.global" && !operation->getAttr("bytes")) {
          // Typed globals may leave alignment unspecified. Memory operations
          // and byte-string definitions require an explicit positive value.
        } else if (!*value || *value > (1ULL << 29) || !llvm::isPowerOf2_64(*value))
          error = "invalid memory alignment in a public word domain";
      }
      if (field == "value" || field == "initializer") {
        bool declaration = field == "initializer" && mlir::isa<mlir::UnitAttr>(attribute.getValue()) &&
            operation->getAttrOfType<mlir::BoolAttr>("declaration") &&
            operation->getAttrOfType<mlir::BoolAttr>("declaration").getValue();
        if (!declaration) if (auto invalid = validateInitializer(attribute.getValue())) {
          error = llvm::toString(std::move(invalid)); return;
        }
        mlir::Type expected;
        if (field == "value" && operation->getNumResults() == 1) expected = operation->getResult(0).getType();
        if (field == "initializer")
          if (auto element = operation->getAttrOfType<mlir::TypeAttr>("element")) expected = element.getValue();
        if (!declaration && expected)
          if (auto invalid = validateInitializerShape(expected, attribute.getValue())) {
            error = llvm::toString(std::move(invalid)); return;
          }
      }
      attribute.getValue().walk([&](Attribute nested) {
        if (auto integer = mlir::dyn_cast<mlir::IntegerAttr>(nested))
          if (integer.getValue().getBitWidth() > 64)
            error = "oversized integer attribute in common IR";
        if (auto record = mlir::dyn_cast<mlir::DictionaryAttr>(nested)) {
          if (record.get("word64") || record.get("word32")) {
            auto value = publicInteger(record, true);
            if (!value) error = llvm::toString(value.takeError());
          }
          if (record.get("array") || record.get("count")) {
            auto elements = record.getAs<mlir::ArrayAttr>("array");
            auto wide = publicInteger(record.get("count"), true);
            auto narrow = publicInteger(record.get("count"), false);
            if (!wide || !narrow) {
              if (!wide) error = llvm::toString(wide.takeError());
              if (!narrow) error = llvm::toString(narrow.takeError());
            } else if (record.size() != 2 || !elements || elements.size() > 1024 * 1024 ||
                       std::max(*wide, *narrow) != elements.size()) {
              error = "invalid public native-index-domain array initializer";
            } else for (uint64_t i = std::min(*wide, *narrow); i < elements.size(); ++i)
              if (!pureLiteral(elements[i])) error = "one-domain array tails require pure literal initializers";
          }
        }
      });
    }
    if (operation->getName().getStringRef() == "nier.binary") {
      auto opcode = operation->getAttrOfType<mlir::StringAttr>("opcode");
      unsigned code = 0;
      if (opcode) for (unsigned i = llvm::Instruction::BinaryOpsBegin; i < llvm::Instruction::BinaryOpsEnd; ++i)
        if (opcode.getValue() == llvm::Instruction::getOpcodeName(i)) code = i;
      for (bool word64 : {true, false}) {
        auto flags = publicInteger(operation->getAttr("flags"), word64);
        if (!flags) error = llvm::toString(flags.takeError());
        else if (!code || *flags > 7 || !validArithmeticFlags(code, unsigned(*flags)))
          error = "invalid arithmetic flags in a public word domain";
      }
    }
    if (operation->getAttr("loop") || operation->getAttr("loop_id")) {
      if (operation->getAttr("loop_id") && !operation->getAttr("loop")) {
        error = "native loop identity requires loop options"; return;
      }
      auto options = operation->getAttrOfType<mlir::ArrayAttr>("loop");
      auto identity = operation->getAttrOfType<mlir::StringAttr>("loop_id");
      auto id = identity ? identity.getValue() : StringRef();
      if (!options || options.empty() || id.size() < 2 || id.size() > 64 || !id.starts_with("l") ||
          !llvm::all_of(id.drop_front(), [](char c) { return c >= '0' && c <= '9'; }))
        error = "loop options require an opaque loop identity";
      if (options) for (auto option : options) {
        auto name = mlir::dyn_cast<mlir::StringAttr>(option);
        if (!name || (name.getValue() != "llvm.loop.mustprogress" && name.getValue() != "llvm.loop.unroll.disable" &&
                      name.getValue() != "llvm.loop.unroll.enable")) error = "unknown loop semantic option";
      }
    }
    for (auto &region : operation->getRegions())
      for (auto &block : region)
        for (auto argument : block.getArguments())
          if (!mlir::isa<mlir::UnknownLoc>(argument.getLoc()))
            error = "private parameter debug location in common IR";
  });
  if (!error.empty()) return failure(error);
  if (auto error = validatePublicTypes(module)) return error;
  // Specializing the public graph is target-independent structure work. Check
  // both masks without constructing LLVM types, ABI signatures or native code.
  for (bool word64 : {true, false}) {
    auto graph = detail::specializeConditionalCFG(module, word64);
    if (!graph) return graph.takeError();
  }
  return llvm::Error::success();
}


void summarize(mlir::ModuleOp module, ArtifactSummary &summary) {
  summary = {};
  module.walk([&](Operation *operation) {
    StringRef name = operation->getName().getStringRef();
    if (name == "nier.func") ++summary.functions;
    else if (name == "nier.global") ++summary.globals;
    else if (name.starts_with("nier.")) ++summary.operations;
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
  context.getOrLoadDialect<ir::NIERDialect>();
  if (path.contains('\0')) return failure("invalid NieR input pathname");
  struct Descriptor { int value; ~Descriptor() { if (value >= 0) ::close(value); } };
  Descriptor descriptor{::open(path.str().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
  if (descriptor.value < 0) return failure("cannot open NieR bytecode input");
  struct stat status;
  if (::fstat(descriptor.value, &status) || !S_ISREG(status.st_mode) ||
      status.st_size < 0 || uint64_t(status.st_size) > 64 * 1024 * 1024)
    return failure("NieR bytecode input must be a bounded regular file");
  auto buffer = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(status.st_size, path);
  if (!buffer) return failure("cannot allocate bounded NieR input buffer");
  size_t offset = 0;
  while (offset < buffer->getBufferSize()) {
    auto count = ::read(descriptor.value, buffer->getBufferStart() + offset, buffer->getBufferSize() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return failure("NieR bytecode input was truncated or unreadable");
    offset += size_t(count);
  }
  char extra;
  ssize_t tail;
  do { tail = ::read(descriptor.value, &extra, 1); } while (tail < 0 && errno == EINTR);
  if (tail != 0 || ::fstat(descriptor.value, &status) || uint64_t(status.st_size) != offset)
    return failure("NieR bytecode input changed size while reading");
  if (!mlir::isBytecode(buffer->getMemBufferRef()))
    return failure("expected bounded MLIR bytecode, not textual IR");
  llvm::SourceMgr manager;
  manager.AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(manager, &context);
  if (!module || mlir::failed(mlir::verify(*module)))
    return failure("cannot parse or verify the common MLIR artifact");
  return std::move(module);
}

} // namespace

llvm::Error verifyModuleStructure(mlir::ModuleOp module) {
  if (mlir::failed(mlir::verify(module)))
    return failure("NieR structural verification failed");
  return validateSchema(module);
}

llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>>
readModuleStructure(StringRef bytecodeInput, mlir::MLIRContext &context) {
  auto source = readArtifact(bytecodeInput, context);
  if (!source) return source.takeError();
  if (auto error = verifyModuleStructure(**source)) return std::move(error);
  return std::move(*source);
}

llvm::Error inspectArtifactStructure(StringRef bytecodeInput, ArtifactSummary &summary) {
  mlir::MLIRContext context;
  auto source = readModuleStructure(bytecodeInput, context);
  if (!source) return source.takeError();
  summarize(**source, summary);
  return llvm::Error::success();
}

llvm::Error verifyModule(mlir::ModuleOp module, llvm::ArrayRef<StringRef> targets) {
  if (auto error = verifyModuleStructure(module)) return error;
  if (targets.empty()) return failure("NieR validation requires a semantic target domain");
  std::set<std::string> seen;
  for (auto target : targets) {
    if ((target != "x86_64" && target != "i686") || !seen.insert(target.str()).second)
      return failure("unsupported or duplicate NieR semantic target");
    if (!detail::findNativeTarget(target))
      return failure("requested native target is unavailable in this NieR library: " + target);
  }
  for (auto target : targets) {
    llvm::LLVMContext context;
    auto lowered = detail::lowerModule(module, context, target == "x86_64");
    if (!lowered) return lowered.takeError();
  }
  return llvm::Error::success();
}

llvm::Error writeModule(mlir::ModuleOp module, StringRef bytecodeOutput,
                        llvm::ArrayRef<StringRef> targets) {
  if (auto error = verifyModule(module, targets)) return error;
  std::error_code ec;
  llvm::raw_fd_ostream output(bytecodeOutput, ec, llvm::sys::fs::OF_None);
  if (ec) return llvm::errorCodeToError(ec);
  if (mlir::failed(mlir::writeBytecodeToFile(module, output)))
    return failure("cannot serialize NieR module");
  output.flush();
  if (output.has_error()) return failure("failed writing NieR bytecode");
  return llvm::Error::success();
}

llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>>
readModule(StringRef bytecodeInput, mlir::MLIRContext &context,
           llvm::ArrayRef<StringRef> targets) {
  auto module = readArtifact(bytecodeInput, context);
  if (!module) return module.takeError();
  if (auto error = verifyModule(**module, targets)) return error;
  return std::move(*module);
}


llvm::Error lowerArtifact(StringRef bytecodeInput, StringRef profile,
                          StringRef llvmIROutput) {
  if (profile != "x86_64" && profile != "i686")
    return failure("unsupported target profile; expected x86_64 or i686");
  mlir::MLIRContext context;
  if (!detail::findNativeTarget(profile))
    return failure("requested native target is unavailable in this NieR library: " + profile);
  auto source = readModuleStructure(bytecodeInput, context);
  if (!source) return source.takeError();
  llvm::LLVMContext llvmContext;
  auto lowered = detail::lowerModule(**source, llvmContext, profile == "x86_64");
  if (!lowered) return lowered.takeError();
  std::error_code ec;
  llvm::raw_fd_ostream output(llvmIROutput, ec, llvm::sys::fs::OF_Text);
  if (ec) return llvm::errorCodeToError(ec);
  (*lowered)->print(output, nullptr);
  output.flush();
  if (output.has_error()) return failure("failed writing specialized LLVM IR");
  return llvm::Error::success();
}

llvm::Error inspectArtifact(StringRef bytecodeInput, ArtifactSummary &summary,
                            llvm::ArrayRef<StringRef> targets) {
  mlir::MLIRContext context;
  auto source = readArtifact(bytecodeInput, context);
  if (!source) return source.takeError();
  if (auto error = verifyModule(**source, targets)) return error;
  summarize(**source, summary);
  return llvm::Error::success();
}

} // namespace nier
