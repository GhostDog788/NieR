#include "sela/IR/Domains.h"
#include "sela/IR/Dialect.h"
#include "ConditionalSpecialization.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include <set>
#include <functional>

namespace sela::ir {
namespace {
llvm::Error fail(const llvm::Twine &text) {
  return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), text);
}
llvm::Error checkSet(mlir::Attribute input, const std::set<std::string> &universe) {
  auto values = mlir::dyn_cast_or_null<mlir::ArrayAttr>(input);
  if (!values || values.empty()) return fail("empty or malformed public target set");
  std::set<std::string> seen;
  for (auto value : values) {
    auto id = mlir::dyn_cast<mlir::StringAttr>(value);
    if (!id || !universe.count(id.getValue().str()) || !seen.insert(id.getValue().str()).second)
      return fail("unknown or duplicate identity in public target set");
  }
  return llvm::Error::success();
}
bool literalTail(mlir::Attribute value) {
  if (mlir::isa<mlir::IntegerAttr>(value)) return true;
  if (auto number = mlir::dyn_cast<mlir::FloatAttr>(value)) return number.getType().isF32() || number.getType().isF64();
  if (auto text = mlir::dyn_cast<mlir::StringAttr>(value)) return text.getValue() == "zero" || text.getValue() == "null";
  if (auto array = mlir::dyn_cast<mlir::ArrayAttr>(value))
    return array.size() <= 1024 * 1024 && llvm::all_of(array, literalTail);
  return false;
}
llvm::Error checkAttribute(mlir::Attribute input, const std::set<std::string> &universe,
                           unsigned depth = 0) {
  if (depth > 64) return fail("public target expression nesting exceeds its bound");
  if (auto dictionary = mlir::dyn_cast<mlir::DictionaryAttr>(input)) {
    if (auto raw = dictionary.get("target_cases")) {
      auto cases = mlir::dyn_cast<mlir::ArrayAttr>(raw);
      if (dictionary.size() != 1 || !cases || cases.empty() || cases.size() > universe.size())
        return fail("invalid public target choice");
      std::set<std::string> seen;
      for (auto value : cases) {
        auto entry = mlir::dyn_cast<mlir::DictionaryAttr>(value);
        if (!entry || entry.size() != 2 || !entry.get("value"))
          return fail("invalid public target choice case");
        if (auto error = checkSet(entry.get("targets"), universe)) return error;
        std::set<std::string> active;
        for (auto target : mlir::cast<mlir::ArrayAttr>(entry.get("targets")))
          if (!seen.insert(mlir::cast<mlir::StringAttr>(target).getValue().str()).second)
            return fail("overlapping public target choice cases");
          else active.insert(mlir::cast<mlir::StringAttr>(target).getValue().str());
        if (auto error = checkAttribute(entry.get("value"), active, depth + 1)) return error;
      }
      return llvm::Error::success();
    }
    if (dictionary.get("array") || dictionary.get("count")) {
      auto elements = dictionary.getAs<mlir::ArrayAttr>("array");
      if (dictionary.size() != 2 || !elements || elements.size() > 1024 * 1024 || !dictionary.get("count"))
        return fail("invalid public conditional array inventory");
      uint64_t maximum = 0, minimum = elements.size();
      for (const auto &id : universe) {
        auto count = evaluateInteger(dictionary.get("count"), *targets::find(id));
        if (!count) return count.takeError();
        maximum = std::max(maximum, *count); minimum = std::min(minimum, *count);
      }
      if (maximum != elements.size()) return fail("conditional array contains data outside every qualified extent");
      for (uint64_t i = minimum; i < maximum; ++i)
        if (!literalTail(elements[i])) return fail("conditional array tails require pure literal initializers");
    }
    for (auto entry : dictionary)
      if (auto error = checkAttribute(entry.getValue(), universe, depth + 1)) return error;
  } else if (auto array = mlir::dyn_cast<mlir::ArrayAttr>(input)) {
    for (auto value : array)
      if (auto error = checkAttribute(value, universe, depth + 1)) return error;
  }
  return llvm::Error::success();
}
llvm::Expected<mlir::Attribute> specializeAttribute(mlir::Attribute input,
                                                   const targets::TargetInfo &target,
                                                   bool numeric = false) {
  auto selected = selectAttribute(input, target.id);
  if (!selected) return selected.takeError();
  input = *selected;
  if (auto type = mlir::dyn_cast<mlir::TypeAttr>(input)) {
    auto result = specializeType(type.getValue(), target);
    if (!result) return result.takeError();
    return mlir::TypeAttr::get(*result);
  }
  if (auto array = mlir::dyn_cast<mlir::ArrayAttr>(input)) {
    llvm::SmallVector<mlir::Attribute> values;
    for (auto entry : array) {
      auto value = specializeAttribute(entry, target, numeric);
      if (!value) return value.takeError();
      values.push_back(*value);
    }
    return mlir::ArrayAttr::get(input.getContext(), values);
  }
  if (auto dictionary = mlir::dyn_cast<mlir::DictionaryAttr>(input)) {
    llvm::SmallVector<mlir::NamedAttribute> values;
    for (auto entry : dictionary) {
      auto value = specializeAttribute(entry.getValue(), target, numeric || entry.getName() == "integer");
      if (!value) return value.takeError();
      values.emplace_back(entry.getName(), *value);
    }
    auto result = mlir::DictionaryAttr::get(input.getContext(), values);
    if (auto elements = result.getAs<mlir::ArrayAttr>("array"); elements && result.get("count")) {
      auto count = evaluateInteger(result.get("count"), target);
      if (!count) return count.takeError();
      if (*count > elements.size()) return fail("initializer count exceeds represented data");
      mlir::NamedAttrList fields(result);
      fields.set("array", mlir::ArrayAttr::get(input.getContext(), elements.getValue().take_front(*count)));
      return fields.getDictionary(input.getContext());
    }
    return result;
  }
  if (auto text = mlir::dyn_cast<mlir::StringAttr>(input); numeric && text && text.getValue() == "pointer_bytes")
    return mlir::IntegerAttr::get(mlir::IntegerType::get(input.getContext(), 64), target.wordBits / 8);
  return input;
}
}

llvm::Expected<llvm::SmallVector<llvm::StringRef>> declaredTargets(mlir::ModuleOp module) {
  auto values = module->getAttrOfType<mlir::ArrayAttr>("sela.targets");
  if (!values || values.empty() || values.size() > 256)
    return fail("Sela module needs a bounded explicit target qualification set");
  llvm::SmallVector<llvm::StringRef> result;
  std::set<std::string> seen;
  for (auto value : values) {
    auto id = mlir::dyn_cast<mlir::StringAttr>(value);
    if (!id || !targets::find(id.getValue()) || !seen.insert(id.getValue().str()).second)
      return fail("unknown or duplicate qualified Sela target");
    result.push_back(id.getValue());
  }
  return result;
}

mlir::ArrayAttr targetSet(mlir::MLIRContext *context, llvm::ArrayRef<llvm::StringRef> ids) {
  llvm::SmallVector<mlir::Attribute> values;
  for (auto id : ids) values.push_back(mlir::StringAttr::get(context, id));
  return mlir::ArrayAttr::get(context, values);
}
bool containsTarget(mlir::Attribute input, llvm::StringRef target) {
  auto values = mlir::dyn_cast_or_null<mlir::ArrayAttr>(input);
  return values && llvm::any_of(values, [&](mlir::Attribute value) {
    auto id = mlir::dyn_cast<mlir::StringAttr>(value);
    return id && id.getValue() == target;
  });
}
mlir::Attribute targetChoice(mlir::MLIRContext *context,
    llvm::ArrayRef<std::pair<llvm::StringRef, mlir::Attribute>> observations) {
  if (observations.empty()) return {};
  llvm::SmallVector<mlir::Attribute> unique;
  llvm::SmallVector<llvm::SmallVector<llvm::StringRef>> groups;
  for (const auto &[target, value] : observations) {
    auto found = llvm::find(unique, value);
    if (found == unique.end()) { unique.push_back(value); groups.push_back({target}); }
    else groups[std::distance(unique.begin(), found)].push_back(target);
  }
  if (unique.size() == 1) return unique.front();
  mlir::Builder builder(context);
  llvm::SmallVector<mlir::Attribute> cases;
  for (size_t i = 0; i < unique.size(); ++i)
    cases.push_back(builder.getDictionaryAttr({builder.getNamedAttr("targets", targetSet(context, groups[i])),
                                              builder.getNamedAttr("value", unique[i])}));
  return builder.getDictionaryAttr({builder.getNamedAttr("target_cases", builder.getArrayAttr(cases))});
}
llvm::Expected<mlir::Attribute> selectAttribute(mlir::Attribute input, llvm::StringRef target) {
  if (!input) return fail("missing target-dependent semantic attribute");
  for (unsigned depth = 0; depth < 64; ++depth) {
    auto dictionary = mlir::dyn_cast<mlir::DictionaryAttr>(input);
    if (!dictionary || !dictionary.get("target_cases")) return input;
    auto cases = dictionary.getAs<mlir::ArrayAttr>("target_cases");
    if (dictionary.size() != 1 || !cases) return fail("invalid target choice");
    mlir::Attribute selected;
    for (auto value : cases) {
      auto entry = mlir::dyn_cast<mlir::DictionaryAttr>(value);
      if (!entry || entry.size() != 2 || !entry.get("value")) return fail("invalid target choice case");
      if (!containsTarget(entry.get("targets"), target)) continue;
      if (selected) return fail("overlapping target choice");
      selected = entry.get("value");
    }
    if (!selected) return fail("semantic choice is not defined for qualified target: " + target);
    input = selected;
  }
  return fail("excessively nested target choice");
}
llvm::Expected<uint64_t> evaluateInteger(mlir::Attribute input, const targets::TargetInfo &target) {
  auto value = selectAttribute(input, target.id);
  if (!value) return value.takeError();
  if (auto integer = mlir::dyn_cast<mlir::IntegerAttr>(*value)) {
    if (integer.getValue().getBitWidth() <= 64) return integer.getValue().getZExtValue();
  } else if (auto text = mlir::dyn_cast<mlir::StringAttr>(*value)) {
    if (text.getValue() == "pointer_bytes") return target.wordBits / 8;
  }
  return fail("invalid public target integer expression");
}
llvm::Expected<mlir::Type> specializeType(mlir::Type type, const targets::TargetInfo &target) {
  auto *context = type.getContext();
  if (mlir::isa<WordType>(type)) return mlir::IntegerType::get(context, target.wordBits);
  if (auto choice = mlir::dyn_cast<ChoiceType>(type)) {
    auto selected = selectAttribute(choice.getCases(), target.id);
    if (!selected) return selected.takeError();
    auto concrete = mlir::dyn_cast<mlir::TypeAttr>(*selected);
    if (!concrete) return fail("type choice requires type-valued cases");
    return specializeType(concrete.getValue(), target);
  }
  if (auto array = mlir::dyn_cast<ArrayType>(type)) {
    auto count = evaluateInteger(array.getCount(), target);
    if (!count) return count.takeError();
    auto element = specializeType(array.getElementType(), target);
    if (!element) return element.takeError();
    return ArrayType::get(context, *element, *count);
  }
  if (auto record = mlir::dyn_cast<RecordType>(type)) {
    llvm::SmallVector<mlir::Type> fields;
    for (auto field : record.getFields()) {
      auto concrete = specializeType(field, target);
      if (!concrete) return concrete.takeError();
      fields.push_back(*concrete);
    }
    return RecordType::get(context, record.getIdentity(), record.isPacked(), fields);
  }
  if (auto overlap = mlir::dyn_cast<OverlapType>(type)) {
    if (overlap.getAlternatives().size() != overlap.getDomains().size())
      return fail("overlap alternative target inventory mismatch");
    llvm::SmallVector<mlir::Type> alternatives;
    llvm::SmallVector<mlir::Attribute> domains;
    for (size_t i = 0; i < overlap.getAlternatives().size(); ++i) {
      if (!containsTarget(overlap.getDomains()[i], target.id)) continue;
      auto concrete = specializeType(overlap.getAlternatives()[i], target);
      if (!concrete) return concrete.takeError();
      alternatives.push_back(*concrete);
      domains.push_back(targetSet(context, {target.id}));
    }
    if (alternatives.empty()) return fail("no overlap alternatives for qualified target");
    return OverlapType::get(context, overlap.getIdentity(), alternatives, mlir::ArrayAttr::get(context, domains));
  }
  if (auto function = mlir::dyn_cast<mlir::FunctionType>(type)) {
    llvm::SmallVector<mlir::Type> inputs, results;
    for (auto input : function.getInputs()) {
      auto concrete = specializeType(input, target);
      if (!concrete) return concrete.takeError();
      inputs.push_back(*concrete);
    }
    for (auto result : function.getResults()) {
      auto concrete = specializeType(result, target);
      if (!concrete) return concrete.takeError();
      results.push_back(*concrete);
    }
    return mlir::FunctionType::get(context, inputs, results);
  }
  return type;
}
llvm::Error verifyTargetDomains(mlir::ModuleOp module) {
  auto domain = declaredTargets(module);
  if (!domain) return domain.takeError();
  std::set<std::string> universe;
  for (auto id : *domain) universe.insert(id.str());
  std::string message;
  // Module flag presence is conditional too. Validate even records absent on
  // this device before selecting; otherwise a malformed inactive record could
  // disappear before the closed public schema sees it.
  if (auto raw = module->getAttr("sela.module_flags")) {
    auto flags = mlir::dyn_cast<mlir::ArrayAttr>(raw);
    if (!flags) return fail("invalid public module flag inventory");
    for (auto value : flags) {
      auto record = mlir::dyn_cast<mlir::DictionaryAttr>(value);
      if (!record || record.size() != 4 || !record.get("name") || !record.get("behavior") || !record.get("value"))
        return fail("invalid public module flag fields");
      if (auto error = checkSet(record.get("targets"), universe)) return error;
      std::set<std::string> present;
      for (auto id : mlir::cast<mlir::ArrayAttr>(record.get("targets")))
        present.insert(mlir::cast<mlir::StringAttr>(id).getValue().str());
      if (auto error = checkAttribute(record, present)) return error;
    }
  }
  std::function<void(mlir::Type, unsigned)> checkType = [&](mlir::Type type, unsigned depth) {
    if (!message.empty()) return;
    if (depth > 64) { message = "public type nesting exceeds the qualified bound"; return; }
    auto identity = [&](llvm::StringRef value) {
      if (value.size() < 2 || value.size() > 64 || !value.starts_with("r") ||
          !llvm::all_of(value.drop_front(), [](char c) { return c >= '0' && c <= '9'; }))
        message = "public storage identities must be opaque r-prefixed integers";
    };
    if (auto choice = mlir::dyn_cast<ChoiceType>(type)) {
      if (auto error = checkAttribute(choice.getCases(), universe)) { message = llvm::toString(std::move(error)); return; }
      std::function<void(mlir::Attribute, unsigned)> cases = [&](mlir::Attribute raw, unsigned level) {
        if (level > 64) { message = "public type choice nesting exceeds its bound"; return; }
        if (auto typed = mlir::dyn_cast<mlir::TypeAttr>(raw)) { checkType(typed.getValue(), depth + 1); return; }
        auto dictionary = mlir::dyn_cast<mlir::DictionaryAttr>(raw);
        auto alternatives = dictionary ? dictionary.getAs<mlir::ArrayAttr>("target_cases") : mlir::ArrayAttr();
        if (!alternatives) { message = "public type alternatives must contain types, not opaque payloads"; return; }
        for (auto alternative : alternatives) cases(mlir::cast<mlir::DictionaryAttr>(alternative).get("value"), level + 1);
      };
      cases(choice.getCases(), 0);
    } else if (auto array = mlir::dyn_cast<ArrayType>(type)) {
      if (auto error = checkAttribute(array.getCount(), universe)) { message = llvm::toString(std::move(error)); return; }
      checkType(array.getElementType(), depth + 1);
    } else if (auto record = mlir::dyn_cast<RecordType>(type)) {
      if (!record.getIdentity().empty()) identity(record.getIdentity());
      for (auto field : record.getFields()) checkType(field, depth + 1);
    } else if (auto overlap = mlir::dyn_cast<OverlapType>(type)) {
      identity(overlap.getIdentity());
      if (overlap.getAlternatives().empty() || overlap.getAlternatives().size() > 64 ||
          overlap.getAlternatives().size() != overlap.getDomains().size()) {
        message = "invalid public overlap alternative inventory"; return;
      }
      for (auto set : overlap.getDomains())
        if (auto error = checkSet(set, universe)) { message = llvm::toString(std::move(error)); return; }
      for (auto field : overlap.getAlternatives()) checkType(field, depth + 1);
    } else if (auto function = mlir::dyn_cast<mlir::FunctionType>(type)) {
      if (function.getNumResults() > 1) { message = "public function type has multiple results"; return; }
      for (auto input : function.getInputs()) checkType(input, depth + 1);
      for (auto result : function.getResults()) checkType(result, depth + 1);
    } else if (auto integer = mlir::dyn_cast<mlir::IntegerType>(type)) {
      unsigned width = integer.getWidth();
      if (width != 1 && width != 8 && width != 16 && width != 32 && width != 64)
        message = "unsupported public integer type in a declared alternative";
    } else if (!mlir::isa<PointerType, WordType, VaListType, VaListArgumentType>(type) && !type.isF32() && !type.isF64()) {
      message = "unsupported public type in a declared alternative";
    }
  };
  module.walk([&](mlir::Operation *operation) {
    if (!message.empty()) return;
    std::set<std::string> active = universe;
    if (auto *parent = operation->getParentOp(); parent && parent->getName().getStringRef() == "sela.func") {
      if (auto domains = parent->getAttrOfType<mlir::ArrayAttr>("block_domains")) {
        auto index = std::distance(parent->getRegion(0).begin(), operation->getBlock()->getIterator());
        if (size_t(index) >= domains.size()) { message = "local operation lacks a declared block domain"; return; }
        if (auto error = checkSet(domains[index], universe)) { message = llvm::toString(std::move(error)); return; }
        active.clear();
        for (auto id : mlir::cast<mlir::ArrayAttr>(domains[index])) active.insert(mlir::cast<mlir::StringAttr>(id).getValue().str());
      }
    }
    for (auto attribute : operation->getAttrs())
      if (auto error = checkAttribute(attribute.getValue(), active)) {
        message = llvm::toString(std::move(error)); return;
      }
    for (auto name : {"block_domains", "case_domains"}) if (auto raw = operation->getAttr(name)) {
      auto values = mlir::dyn_cast<mlir::ArrayAttr>(raw);
      if (!values) { message = "invalid conditional target domain inventory"; return; }
      for (auto value : values) if (auto error = checkSet(value, active)) {
        message = llvm::toString(std::move(error)); return;
      }
    }
    for (auto type : operation->getResultTypes()) checkType(type, 0);
    for (auto &region : operation->getRegions()) for (auto &block : region)
      for (auto argument : block.getArguments()) checkType(argument.getType(), 0);
    for (auto attribute : operation->getAttrs()) attribute.getValue().walk([&](mlir::Attribute value) {
      if (auto typed = mlir::dyn_cast<mlir::TypeAttr>(value)) checkType(typed.getValue(), 0);
    });
  });
  return message.empty() ? llvm::Error::success() : fail(message);
}
llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>> specializeDomains(
    mlir::ModuleOp module, const targets::TargetInfo &target) {
  auto domain = declaredTargets(module);
  if (!domain) return domain.takeError();
  if (!llvm::is_contained(*domain, target.id)) return fail("target lies outside artifact qualification: " + target.id);
  auto selected = ::sela::detail::specializeConditionalCFG(module, target.id);
  if (!selected) return selected.takeError();
  std::string error;
  (*selected)->walk([&](mlir::Operation *operation) {
    if (!error.empty()) return;
    llvm::SmallVector<mlir::NamedAttribute> attributes;
    for (auto entry : operation->getAttrs()) {
      auto name = entry.getName().getValue();
      mlir::Attribute raw = entry.getValue();
      if (name == "sela.module_flags") {
        auto flags = mlir::dyn_cast<mlir::ArrayAttr>(raw);
        if (!flags) { error = "invalid public module flag inventory"; return; }
        llvm::SmallVector<mlir::Attribute> kept;
        for (auto value : flags) {
          auto record = mlir::dyn_cast<mlir::DictionaryAttr>(value);
          if (!record) { error = "invalid public module flag"; return; }
          if (containsTarget(record.get("targets"), target.id)) kept.push_back(value);
        }
        raw = mlir::ArrayAttr::get(module.getContext(), kept);
      }
      auto concrete = specializeAttribute(raw, target,
          name == "alignment" || name == "value" || name == "initializer" || name == "flags");
      if (!concrete) { error = llvm::toString(concrete.takeError()); return; }
      attributes.emplace_back(entry.getName(), *concrete);
    }
    operation->setAttrs(mlir::DictionaryAttr::get(module.getContext(), attributes));
    auto rewriteValue = [&](mlir::Value value) {
      auto concrete = specializeType(value.getType(), target);
      if (!concrete) error = llvm::toString(concrete.takeError());
      else value.setType(*concrete);
    };
    for (auto value : operation->getResults()) rewriteValue(value);
    for (auto &region : operation->getRegions()) for (auto &block : region)
      for (auto argument : block.getArguments()) rewriteValue(argument);
  });
  if (!error.empty()) return fail(error);
  (*selected)->getOperation()->setAttr("sela.targets", targetSet(module.getContext(), {target.id}));
  if (mlir::failed(mlir::verify(**selected))) return fail("target-specialized public IR is structurally invalid");
  return std::move(*selected);
}
}
