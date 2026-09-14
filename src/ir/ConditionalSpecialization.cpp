#include "ConditionalSpecialization.h"
#include "sela/IR/Domains.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/DenseMap.h"

namespace sela::detail {
namespace {
llvm::Error fail(llvm::StringRef text) {
  return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), text);
}
}
llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>> specializeConditionalCFG(
    mlir::ModuleOp source, llvm::StringRef selected) {
  auto targets = ir::declaredTargets(source);
  if (!targets) return targets.takeError();
  if (!llvm::is_contained(*targets, selected)) return fail("conditional target is outside module qualification");
  if (auto error = ir::verifyTargetDomains(source)) return std::move(error);
  mlir::OwningOpRef<mlir::ModuleOp> result(source.clone());
  auto all = ir::targetSet(source.getContext(), *targets);
  for (auto &operation : llvm::make_early_inc_range(result->getBody()->getOperations()))
    if (auto domain = operation.getAttr("targets"); domain && !ir::containsTarget(domain, selected))
      operation.erase();
  for (auto &function : result->getBody()->getOperations()) {
    if (function.getName().getStringRef() != "sela.func") continue;
    if (function.getNumRegions() != 1) return fail("conditional CFG requires one function region");
    auto &region = function.getRegion(0);
    auto functionDomain = function.getAttrOfType<mlir::ArrayAttr>("targets");
    if (!functionDomain) functionDomain = all;
    auto rawDomains = function.getAttr("block_domains");
    auto domains = mlir::dyn_cast_or_null<mlir::ArrayAttr>(rawDomains);
    if (rawDomains && (!domains || domains.size() != region.getBlocks().size() || region.empty()))
      return fail("conditional CFG block inventory mismatch");
    llvm::DenseMap<mlir::Block *, mlir::ArrayAttr> blockDomains;
    size_t index = 0;
    for (auto &block : region) {
      auto domain = domains ? mlir::cast<mlir::ArrayAttr>(domains[index++]) : functionDomain;
      if (block.isEntryBlock() && !llvm::all_of(functionDomain, [&](mlir::Attribute id) {
            return ir::containsTarget(domain, mlir::cast<mlir::StringAttr>(id).getValue());
          }))
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
          auto domain = edge && caseDomains ? mlir::cast<mlir::ArrayAttr>(caseDomains[edge - 1]) : all;
          auto *successor = operation.getSuccessor(edge);
          if (!blockDomains.count(successor)) return fail("conditional switch leaves its function");
          for (auto id : *targets)
            if (ir::containsTarget(blockDomains[&block], id) && ir::containsTarget(domain, id) &&
                !ir::containsTarget(blockDomains[successor], id))
              return fail("active conditional switch edge reaches an absent block");
          if (ir::containsTarget(domain, selected)) {
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
      if (!ir::containsTarget(blockDomains[&block], selected)) continue;
      for (auto &operation : block) {
        for (auto *successor : operation.getSuccessors())
          if (!blockDomains.count(successor) || !ir::containsTarget(blockDomains[successor], selected))
            return fail("active branch reaches an absent conditional block");
        for (auto operand : operation.getOperands()) {
          auto *owner = operand.getParentBlock();
          if (!blockDomains.count(owner) || !ir::containsTarget(blockDomains[owner], selected))
            return fail("active operation uses an absent conditional value");
        }
      }
    }
    llvm::SmallVector<mlir::Block *> removed;
    for (auto &block : region)
      if (!ir::containsTarget(blockDomains[&block], selected)) removed.push_back(&block);
    for (auto *block : removed) block->dropAllReferences();
    for (auto *block : removed) block->erase();
    function.removeAttr("block_domains");
  }
  if (mlir::failed(mlir::verify(*result))) return fail("specialized conditional CFG failed structural verification");
  return result;
}
}
