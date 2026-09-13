#include "ConditionalSpecialization.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/DenseMap.h"

namespace sela::detail {
namespace {
llvm::Error fail(llvm::StringRef text) {
  return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), text);
}
llvm::Expected<unsigned> mask(mlir::Attribute attribute) {
  auto value = mlir::dyn_cast_or_null<mlir::IntegerAttr>(attribute);
  if (!value || value.getValue().getBitWidth() > 64 || value.getInt() < 1 || value.getInt() > 3)
    return fail("invalid conditional CFG native-word domain");
  return unsigned(value.getInt());
}
}
llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>> specializeConditionalCFG(
    mlir::ModuleOp source, bool word64) {
  mlir::OwningOpRef<mlir::ModuleOp> result(source.clone());
  const unsigned selected = word64 ? 1 : 2;
  for (auto &function : result->getBody()->getOperations()) {
    if (function.getName().getStringRef() != "sela.func") continue;
    if (function.getNumRegions() != 1) return fail("conditional CFG requires one function region");
    auto &region = function.getRegion(0);
    auto rawDomains = function.getAttr("block_domains");
    auto domains = mlir::dyn_cast_or_null<mlir::ArrayAttr>(rawDomains);
    if (rawDomains && (!domains || domains.size() != region.getBlocks().size() || region.empty()))
      return fail("conditional CFG block inventory mismatch");
    llvm::DenseMap<mlir::Block *, unsigned> blockDomains;
    size_t index = 0;
    for (auto &block : region) {
      unsigned domain = 3;
      if (domains) {
        auto value = mask(domains[index++]);
        if (!value) return value.takeError();
        domain = *value;
      }
      if (block.isEntryBlock() && domain != 3)
        return fail("conditional CFG must retain a shared entry block");
      blockDomains[&block] = domain;
    }
    for (auto &block : region) {
      for (auto iterator = block.begin(); iterator != block.end();) {
        auto &operation = *iterator++;
        if (operation.getName().getStringRef() != "sela.switch") continue;
        auto rawCases = operation.getAttr("case_domains");
        auto caseDomains = mlir::dyn_cast_or_null<mlir::ArrayAttr>(rawCases);
        auto cases = operation.getAttrOfType<mlir::ArrayAttr>("cases");
        auto counts = operation.getAttrOfType<mlir::DenseI32ArrayAttr>("argument_counts");
        if ((rawCases && !caseDomains) || !cases || !counts ||
            (caseDomains && caseDomains.size() != cases.size()) ||
            counts.size() != cases.size() + 1 || operation.getNumSuccessors() != counts.size() ||
            operation.getNumOperands() < 1 || operation.getNumResults() || operation.getNumRegions())
          return fail("invalid conditional switch inventory");
        llvm::SmallVector<mlir::Attribute> selectedCases;
        llvm::SmallVector<int32_t> selectedCounts;
        llvm::SmallVector<mlir::Block *> successors;
        llvm::SmallVector<mlir::Value> operands{operation.getOperand(0)};
        size_t offset = 1;
        for (unsigned edge = 0; edge < counts.size(); ++edge) {
          auto count = counts[edge];
          if (count < 0 || uint64_t(count) > operation.getNumOperands() - offset)
            return fail("invalid conditional switch argument segment");
          unsigned domain = 3;
          if (edge && caseDomains) {
            auto value = mask(caseDomains[edge - 1]);
            if (!value) return value.takeError();
            domain = *value;
          }
          auto *successor = operation.getSuccessor(edge);
          if (!blockDomains.count(successor)) return fail("conditional switch leaves its function");
          if ((blockDomains[&block] & domain & ~blockDomains[successor]) != 0)
            return fail("active conditional switch edge reaches an absent block");
          if (domain & selected) {
            successors.push_back(successor);
            selectedCounts.push_back(count);
            if (edge) selectedCases.push_back(cases[edge - 1]);
            auto arguments = operation.getOperands().slice(offset, size_t(count));
            operands.append(arguments.begin(), arguments.end());
          }
          offset += size_t(count);
        }
        if (offset != operation.getNumOperands()) return fail("conditional switch has undeclared arguments");
        mlir::OpBuilder builder(&operation);
        mlir::OperationState state(operation.getLoc(), operation.getName().getStringRef());
        state.addAttributes(operation.getAttrs());
        state.attributes.erase("case_domains");
        state.attributes.set("cases", builder.getArrayAttr(selectedCases));
        state.attributes.set("argument_counts", builder.getDenseI32ArrayAttr(selectedCounts));
        state.addOperands(operands);
        state.addSuccessors(successors);
        builder.create(state);
        operation.erase();
      }
    }
    // Check all surviving edges and SSA uses before dropping blocks. Otherwise
    // erasing an inactive definition could leave malformed dangling IR behind.
    for (auto &block : region) {
      if (!(blockDomains[&block] & selected)) continue;
      for (auto &operation : block) {
        for (auto *successor : operation.getSuccessors())
          if (!blockDomains.count(successor) || !(blockDomains[successor] & selected))
            return fail("active branch reaches an absent conditional block");
        for (auto operand : operation.getOperands()) {
          auto *owner = operand.getParentBlock();
          if (!blockDomains.count(owner) || !(blockDomains[owner] & selected))
            return fail("active operation uses an absent conditional value");
        }
      }
    }
    llvm::SmallVector<mlir::Block *> removed;
    for (auto &block : region)
      if (!(blockDomains[&block] & selected)) removed.push_back(&block);
    for (auto *block : removed) block->dropAllReferences();
    for (auto *block : removed) block->erase();
    function.removeAttr("block_domains");
  }
  if (mlir::failed(mlir::verify(*result))) return fail("specialized conditional CFG failed structural verification");
  return result;
}
}
