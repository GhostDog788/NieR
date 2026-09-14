#include "CommonMerge.h"
#include "ConditionalCFG.h"
#include "sela/IR/Dialect.h"
#include "sela/IR/Domains.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/ADT/SmallString.h"
#include <map>
#include <set>

namespace sela::detail {
namespace {
using mlir::Attribute;
using mlir::Operation;
using llvm::StringRef;
llvm::Error fail(const llvm::Twine &text) {
  return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), "N-target merge: " + text);
}
template <typename T> using Observations = llvm::SmallVector<std::pair<size_t, T>>;
struct Graph {
  llvm::Function *function = nullptr;
  llvm::DenseMap<const llvm::BasicBlock *, mlir::Block *> source;
  llvm::DenseMap<mlir::Block *, const llvm::BasicBlock *> native;
};
struct BlockGroup { llvm::SmallVector<mlir::Block *> blocks; };

struct Merger {
  mlir::MLIRContext *context;
  mlir::OpBuilder builder;
  llvm::ArrayRef<StringRef> ids;
  mlir::OwningOpRef<mlir::ModuleOp> output;
  llvm::LLVMContext graphContext;
  llvm::Module graphs{"sela-private-cfg-proof", graphContext};
  std::map<std::vector<const void *>, mlir::Value> values;
  std::vector<llvm::DenseMap<mlir::Block *, mlir::Block *>> blocks;
  std::vector<std::map<std::string, std::string>> storageIdentities;
  unsigned nextStorageIdentity = 0;

  Merger(mlir::MLIRContext *context, llvm::ArrayRef<StringRef> ids)
      : context(context), builder(context), ids(ids),
        output(mlir::ModuleOp::create(builder.getUnknownLoc())), blocks(ids.size()), storageIdentities(ids.size()) {
    (*output)->setAttr("sela.schema", builder.getI32IntegerAttr(1));
    (*output)->setAttr("sela.targets", ir::targetSet(context, ids));
  }
  llvm::Expected<std::string> storageIdentity(const Observations<mlir::Type> &observations) {
    auto identity = [](mlir::Type type) {
      if (auto record = mlir::dyn_cast<ir::RecordType>(type)) return record.getIdentity();
      if (auto overlap = mlir::dyn_cast<ir::OverlapType>(type)) return overlap.getIdentity();
      return StringRef();
    };
    bool anonymous = identity(observations.front().second).empty();
    std::string common;
    for (auto [index, type] : observations) {
      auto local = identity(type);
      if (local.empty() != anonymous) return fail("named and anonymous storage lack correspondence");
      auto found = storageIdentities[index].find(local.str());
      if (found != storageIdentities[index].end()) {
        if (!common.empty() && common != found->second) return fail("storage identity correspondence changed between uses");
        common = found->second;
      }
    }
    if (anonymous) return std::string();
    if (common.empty()) common = "r" + std::to_string(nextStorageIdentity++);
    for (auto [index, type] : observations) {
      auto local = identity(type).str();
      for (const auto &[other, mapped] : storageIdentities[index])
        if (mapped == common && other != local) return fail("distinct storage identities cannot collapse into one shared identity");
      storageIdentities[index][local] = common;
    }
    return common;
  }
  Attribute choice(const Observations<Attribute> &observations) {
    llvm::SmallVector<std::pair<StringRef, Attribute>> cases;
    for (auto [index, value] : observations) cases.emplace_back(ids[index], value);
    return ir::targetChoice(context, cases);
  }
  llvm::Expected<mlir::Type> type(const Observations<mlir::Type> &observations) {
    if (observations.empty()) return fail("missing type observations");
    auto first = observations.front().second;
    if (!mlir::isa<ir::ArrayType, ir::RecordType, ir::OverlapType, mlir::FunctionType>(first) &&
        llvm::all_of(observations, [&](auto value) { return value.second == first; })) return first;
    if (llvm::all_of(observations, [](auto value) { return mlir::isa<mlir::IntegerType>(value.second); })) {
      if (llvm::all_of(observations, [&](auto value) {
            return mlir::cast<mlir::IntegerType>(value.second).getWidth() == targets::find(ids[value.first])->wordBits;
          })) return ir::WordType::get(context);
      Observations<Attribute> cases;
      for (auto [index, value] : observations) cases.emplace_back(index, mlir::TypeAttr::get(value));
      return ir::ChoiceType::get(context, choice(cases));
    }
    if (llvm::all_of(observations, [](auto value) { return mlir::isa<ir::ArrayType>(value.second); })) {
      Observations<mlir::Type> elements;
      Observations<Attribute> counts;
      for (auto [index, value] : observations) {
        auto array = mlir::cast<ir::ArrayType>(value);
        elements.emplace_back(index, array.getElementType()); counts.emplace_back(index, array.getCount());
      }
      auto element = type(elements);
      if (!element) return element.takeError();
      return ir::ArrayType::get(context, *element, choice(counts));
    }
    if (auto record = mlir::dyn_cast<ir::RecordType>(first)) {
      bool sameShape = true;
      for (auto [index, value] : observations) {
        auto peer = mlir::dyn_cast<ir::RecordType>(value);
        if (!peer) return fail("logical record kind needs additional correspondence proof");
        sameShape &= peer.isPacked() == record.isPacked() && peer.getFields().size() == record.getFields().size();
      }
      auto identity = storageIdentity(observations);
      if (!identity) return identity.takeError();
      if (!sameShape) {
        // A local storage layout may genuinely have conditional fields (for
        // example the platform's jmp_buf tail padding). Keep explicit logical
        // record alternatives, not native type/module payloads or whole bodies.
        Observations<Attribute> cases;
        size_t maximum = 0;
        for (auto [index, value] : observations) maximum = std::max(maximum, mlir::cast<ir::RecordType>(value).getFields().size());
        llvm::SmallVector<mlir::Type> fields;
        for (size_t field = 0; field < maximum; ++field) {
          Observations<mlir::Type> input;
          for (auto [index, value] : observations) {
            auto peer = mlir::cast<ir::RecordType>(value);
            if (field < peer.getFields().size()) input.emplace_back(index, peer.getFields()[field]);
          }
          auto merged = type(input);
          if (!merged) return merged.takeError();
          fields.push_back(*merged);
        }
        for (auto [index, value] : observations) {
          auto peer = mlir::cast<ir::RecordType>(value);
          cases.emplace_back(index, mlir::TypeAttr::get(ir::RecordType::get(context, *identity, peer.isPacked(),
              llvm::ArrayRef<mlir::Type>(fields).take_front(peer.getFields().size()))));
        }
        return ir::ChoiceType::get(context, choice(cases));
      }
      llvm::SmallVector<mlir::Type> fields;
      for (size_t i = 0; i < record.getFields().size(); ++i) {
        Observations<mlir::Type> input;
        for (auto [index, value] : observations) input.emplace_back(index, mlir::cast<ir::RecordType>(value).getFields()[i]);
        auto field = type(input);
        if (!field) return field.takeError();
        fields.push_back(*field);
      }
      return ir::RecordType::get(context, *identity, record.isPacked(), fields);
    }
    if (auto function = mlir::dyn_cast<mlir::FunctionType>(first)) {
      for (auto [index, value] : observations) {
        auto peer = mlir::dyn_cast<mlir::FunctionType>(value);
        if (!peer || peer.getNumInputs() != function.getNumInputs() || peer.getNumResults() != function.getNumResults())
          return fail("logical callable arity differs");
      }
      llvm::SmallVector<mlir::Type> inputs, results;
      for (unsigned i = 0; i < function.getNumInputs() + function.getNumResults(); ++i) {
        Observations<mlir::Type> input;
        for (auto [index, value] : observations) {
          auto peer = mlir::cast<mlir::FunctionType>(value);
          input.emplace_back(index, i < function.getNumInputs() ? peer.getInput(i) : peer.getResult(i - function.getNumInputs()));
        }
        auto result = type(input);
        if (!result) return result.takeError();
        (i < function.getNumInputs() ? inputs : results).push_back(*result);
      }
      return builder.getFunctionType(inputs, results);
    }
    if (auto overlap = mlir::dyn_cast<ir::OverlapType>(first)) {
      llvm::SmallVector<mlir::Type> alternatives;
      llvm::SmallVector<Attribute> domains;
      for (auto [index, value] : observations) {
        auto peer = mlir::dyn_cast<ir::OverlapType>(value);
        if (!peer) return fail("overlap kinds disagree");
      }
      auto identity = storageIdentity(observations);
      if (!identity) return identity.takeError();
      // Alternative order is a semantic declaration order. Do not infer an
      // alternative correspondence merely from equal native carrier layouts.
      auto count = overlap.getAlternatives().size();
      for (auto [index, value] : observations)
        if (mlir::cast<ir::OverlapType>(value).getAlternatives().size() != count) {
          // Each observed alternative inventory already has producer-side
          // declaration evidence. Preserve the exact local storage alternatives
          // without inventing correspondences between unrelated union members.
          Observations<Attribute> cases;
          for (auto [target, observed] : observations) {
            auto peer = mlir::cast<ir::OverlapType>(observed);
            cases.emplace_back(target, mlir::TypeAttr::get(ir::OverlapType::get(context, *identity, peer.getAlternatives(), peer.getDomains())));
          }
          return ir::ChoiceType::get(context, choice(cases));
        }
      for (size_t i = 0; i < count; ++i) {
        Observations<mlir::Type> input;
        llvm::SmallVector<StringRef> domain;
        for (auto [index, value] : observations) {
          input.emplace_back(index, mlir::cast<ir::OverlapType>(value).getAlternatives()[i]); domain.push_back(ids[index]);
        }
        auto result = type(input);
        if (!result) return result.takeError();
        alternatives.push_back(*result); domains.push_back(ir::targetSet(context, domain));
      }
      return ir::OverlapType::get(context, *identity, alternatives, builder.getArrayAttr(domains));
    }
    return fail("unproved logical type correspondence");
  }
  llvm::Expected<Attribute> attribute(const Observations<Attribute> &observations) {
    auto first = observations.front().second;
    if (!mlir::isa<mlir::TypeAttr, mlir::ArrayAttr, mlir::DictionaryAttr>(first) &&
        llvm::all_of(observations, [&](auto value) { return value.second == first; })) return first;
    if (llvm::all_of(observations, [](auto value) { return mlir::isa<mlir::TypeAttr>(value.second); })) {
      Observations<mlir::Type> input;
      for (auto [index, value] : observations) input.emplace_back(index, mlir::cast<mlir::TypeAttr>(value).getValue());
      auto result = type(input);
      if (!result) return result.takeError();
      return mlir::TypeAttr::get(*result);
    }
    if (auto array = mlir::dyn_cast<mlir::ArrayAttr>(first)) {
      if (llvm::all_of(observations, [&](auto value) {
            auto peer = mlir::dyn_cast<mlir::ArrayAttr>(value.second); return peer && peer.size() == array.size(); })) {
        llvm::SmallVector<Attribute> result;
        for (size_t i = 0; i < array.size(); ++i) {
          Observations<Attribute> input;
          for (auto [index, value] : observations) input.emplace_back(index, mlir::cast<mlir::ArrayAttr>(value)[i]);
          auto value = attribute(input);
          if (!value) return value.takeError();
          result.push_back(*value);
        }
        return builder.getArrayAttr(result);
      }
    }
    if (auto dictionary = mlir::dyn_cast<mlir::DictionaryAttr>(first)) {
      bool matching = llvm::all_of(observations, [&](auto value) {
        auto peer = mlir::dyn_cast<mlir::DictionaryAttr>(value.second);
        return peer && peer.size() == dictionary.size() && llvm::all_of(dictionary, [&](auto entry) { return bool(peer.get(entry.getName())); });
      });
      if (matching) {
        llvm::SmallVector<mlir::NamedAttribute> result;
        for (auto field : dictionary) {
          Observations<Attribute> input;
          for (auto [index, value] : observations)
            input.emplace_back(index, mlir::cast<mlir::DictionaryAttr>(value).get(field.getName()));
          auto value = attribute(input);
          if (!value) return value.takeError();
          result.emplace_back(field.getName(), *value);
        }
        return builder.getDictionaryAttr(result);
      }
    }
    // Scalar/data/attribute alternatives retain precisely the observations.
    // No unobserved target receives a guessed value or an implicit default.
    return choice(observations);
  }
  llvm::Expected<llvm::SmallVector<mlir::NamedAttribute>> attributes(
      const Observations<Operation *> &operations, llvm::ArrayRef<StringRef> omitted = {}) {
    llvm::SmallVector<mlir::NamedAttribute> result;
    auto *first = operations.front().second;
    for (auto [index, operation] : operations)
      for (auto field : operation->getAttrs())
        if (!llvm::is_contained(omitted, field.getName().getValue()) && !first->getAttr(field.getName()))
          return fail("operation attribute inventories differ");
    for (auto field : first->getAttrs()) {
      if (llvm::is_contained(omitted, field.getName().getValue())) continue;
      Observations<Attribute> input;
      for (auto [index, operation] : operations) {
        auto value = operation->getAttr(field.getName());
        if (!value) return fail("operation attribute inventories differ");
        input.emplace_back(index, value);
      }
      auto value = attribute(input);
      if (!value) return value.takeError();
      result.emplace_back(field.getName(), *value);
    }
    return result;
  }
  std::vector<const void *> key(const Observations<mlir::Value> &observations) {
    std::vector<const void *> result(ids.size());
    for (auto [index, value] : observations) result[index] = value.getAsOpaquePointer();
    return result;
  }
  llvm::Expected<mlir::Value> operand(const Observations<mlir::Value> &observations) {
    auto tuple = key(observations);
    if (auto found = values.find(tuple); found != values.end()) return found->second;
    mlir::Value result;
    for (const auto &[candidate, value] : values) {
      bool match = true;
      for (size_t i = 0; i < tuple.size(); ++i) if (tuple[i] && candidate[i] != tuple[i]) match = false;
      if (!match) continue;
      if (result && result != value) return fail("ambiguous conditional SSA correspondence");
      result = value;
    }
    if (!result) return fail("missing or escaping conditional SSA correspondence");
    return result;
  }
  llvm::Expected<Graph> graph(Operation *source) {
    Graph result;
    result.function = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(graphContext), false),
                                             llvm::GlobalValue::InternalLinkage, "proof", graphs);
    auto &region = source->getRegion(0);
    for (auto &block : region) {
      auto *native = llvm::BasicBlock::Create(graphContext, "", result.function);
      result.source[native] = &block; result.native[&block] = native;
    }
    for (auto &block : region) {
      auto *native = const_cast<llvm::BasicBlock *>(result.native.lookup(&block));
      llvm::IRBuilder<> build(native);
      auto *terminator = block.getTerminator();
      auto name = terminator->getName().getStringRef();
      auto successor = [&](unsigned i) { return const_cast<llvm::BasicBlock *>(result.native.lookup(terminator->getSuccessor(i))); };
      if (name == "sela.br") build.CreateBr(successor(0));
      else if (name == "sela.cond_br") build.CreateCondBr(llvm::UndefValue::get(build.getInt1Ty()), successor(0), successor(1));
      else if (name == "sela.switch") {
        auto selector = mlir::dyn_cast<mlir::IntegerType>(terminator->getOperand(0).getType());
        auto cases = terminator->getAttrOfType<mlir::ArrayAttr>("cases");
        if (!selector || !cases) return fail("conditional graph proof needs specialized switch labels");
        auto *integer = build.getIntNTy(selector.getWidth());
        auto *instruction = build.CreateSwitch(llvm::UndefValue::get(integer), successor(0), cases.size());
        for (size_t i = 0; i < cases.size(); ++i) {
          auto label = mlir::dyn_cast<mlir::IntegerAttr>(cases[i]);
          if (!label) return fail("conditional graph proof needs exact integer labels");
          instruction->addCase(llvm::ConstantInt::get(integer, label.getValue().getZExtValue()), successor(i + 1));
        }
      } else if (name == "sela.return") build.CreateRetVoid();
      else if (name == "sela.unreachable") build.CreateUnreachable();
      else return fail("unsupported public terminator in correspondence proof");
    }
    // Preserve the presence of PHI/block arguments for the existing closed-arm
    // proof. The values themselves are proved separately in the shared graph.
    for (auto &block : region) if (!block.isEntryBlock() && block.getNumArguments()) {
      auto *native = const_cast<llvm::BasicBlock *>(result.native.lookup(&block));
      auto *phi = llvm::PHINode::Create(llvm::Type::getInt32Ty(graphContext), 0, "", &native->front());
      for (auto *predecessor : llvm::predecessors(native))
        phi->addIncoming(llvm::UndefValue::get(phi->getType()), predecessor);
    }
    if (llvm::verifyFunction(*result.function)) return fail("invalid private structural correspondence graph");
    return result;
  }
  llvm::Expected<std::vector<BlockGroup>> correspondence(const Observations<Operation *> &functions) {
    std::vector<Graph> inputs;
    for (auto [index, function] : functions) {
      auto input = graph(function);
      if (!input) return input.takeError();
      inputs.push_back(std::move(*input));
    }
    std::vector<BlockGroup> groups;
    std::vector<llvm::DenseMap<mlir::Block *, size_t>> indices(functions.size());
    auto baseIndex = functions.front().first;
    for (auto &block : functions.front().second->getRegion(0)) {
      indices[0][&block] = groups.size();
      groups.push_back({llvm::SmallVector<mlir::Block *>(ids.size(), nullptr)});
      groups.back().blocks[baseIndex] = &block;
    }
    std::map<std::string, size_t> arms;
    for (size_t i = 1; i < inputs.size(); ++i) {
      auto proof = pairConditionalCFG(*inputs[0].function, *inputs[i].function);
      if (!proof) return fail(functions.front().second->getAttrOfType<mlir::StringAttr>("id").getValue() +
          " (" + ids[baseIndex] + "/" + ids[functions[i].first] + "): " + llvm::toString(proof.takeError()));
      for (const auto &pair : proof->blocks) if (pair.left && pair.right) {
        auto group = indices[0].lookup(inputs[0].source.lookup(pair.left));
        auto *source = inputs[i].source.lookup(pair.right);
        groups[group].blocks[functions[i].first] = source; indices[i][source] = group;
      }
      for (const auto &selection : proof->switches) for (const auto &entry : selection.cases) {
        if (entry.domain != 2) continue;
        auto head = indices[0].lookup(inputs[0].source.lookup(selection.left->getParent()));
        llvm::SmallString<32> label;
        entry.right->getValue().toString(label, 16, false);
        auto prefix = std::to_string(head) + ":" + std::to_string(entry.right->getBitWidth()) + ":" + label.str().str() + ":";
        const llvm::BasicBlock *block = proof->blocks[entry.successor].right;
        size_t position = 0;
        while (block && !indices[i].count(inputs[i].source.lookup(block))) {
          auto [found, inserted] = arms.emplace(prefix + std::to_string(position++), groups.size());
          if (inserted) groups.push_back({llvm::SmallVector<mlir::Block *>(ids.size(), nullptr)});
          auto *source = inputs[i].source.lookup(block);
          groups[found->second].blocks[functions[i].first] = source;
          indices[i][source] = found->second;
          auto *branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
          if (!branch || !branch->isUnconditional()) return fail("conditional arm lacks its proved closed chain");
          block = branch->getSuccessor(0);
        }
      }
      for (auto &block : functions[i].second->getRegion(0))
        if (!indices[i].count(&block)) return fail("unmatched conditional block");
    }
    // A common physical order must preserve every native observation. Never
    // choose one target's TU/block order and silently reorder another target.
    std::vector<std::set<size_t>> edges(groups.size());
    std::vector<size_t> indegree(groups.size());
    for (size_t i = 0; i < functions.size(); ++i) {
      std::optional<size_t> previous;
      for (auto &block : functions[i].second->getRegion(0)) {
        auto current = indices[i].lookup(&block);
        if (previous && edges[*previous].insert(current).second) ++indegree[current];
        previous = current;
      }
    }
    std::set<size_t> available;
    for (size_t i = 0; i < groups.size(); ++i) if (!indegree[i]) available.insert(i);
    std::vector<BlockGroup> ordered;
    while (!available.empty()) {
      auto index = *available.begin(); available.erase(available.begin());
      ordered.push_back(groups[index]);
      for (auto next : edges[index]) if (!--indegree[next]) available.insert(next);
    }
    if (ordered.size() != groups.size()) return fail("target block orders contradict one another");
    return ordered;
  }
  llvm::Error switchOperation(const Observations<Operation *> &operations) {
    struct Case {
      mlir::Block *successor = nullptr;
      Observations<Attribute> labels;
      Observations<Operation *> operations;
      llvm::SmallVector<size_t> offsets, counts;
    };
    std::vector<Case> cases;
    std::map<std::pair<const void *, const void *>, size_t> identities;
    std::vector<std::vector<size_t>> orders;
    mlir::Block *defaultSuccessor = nullptr;
    Observations<mlir::Value> selectors;
    Observations<Operation *> defaults;
    size_t defaultCount = 0;
    for (auto [index, operation] : operations) {
      auto labels = operation->getAttrOfType<mlir::ArrayAttr>("cases");
      auto counts = operation->getAttrOfType<mlir::DenseI32ArrayAttr>("argument_counts");
      if (!labels || !counts || counts.size() != labels.size() + 1 ||
          operation->getNumSuccessors() != counts.size() || operation->getNumOperands() == 0)
        return fail("invalid switch observation");
      auto *mapped = blocks[index].lookup(operation->getSuccessor(0));
      if (!mapped || (defaultSuccessor && defaultSuccessor != mapped) ||
          (!defaults.empty() && defaultCount != size_t(counts[0])) || counts[0] < 0)
        return fail("switch default correspondence differs");
      defaultSuccessor = mapped; defaultCount = counts[0]; defaults.emplace_back(index, operation);
      selectors.emplace_back(index, operation->getOperand(0));
      size_t offset = 1 + defaultCount;
      std::vector<size_t> order;
      for (size_t i = 0; i < labels.size(); ++i) {
        auto *successor = blocks[index].lookup(operation->getSuccessor(i + 1));
        if (!successor || counts[i + 1] < 0 || offset + size_t(counts[i + 1]) > operation->getNumOperands())
          return fail("invalid switch operand segment");
        auto key = std::make_pair(static_cast<const void *>(successor), labels[i].getAsOpaquePointer());
        auto [found, inserted] = identities.emplace(key, cases.size());
        if (inserted) cases.push_back({successor, {}, {}, {}, {}});
        auto &entry = cases[found->second];
        if (!entry.counts.empty() && entry.counts.front() != size_t(counts[i + 1]))
          return fail("switch case argument counts differ");
        entry.labels.emplace_back(index, labels[i]); entry.operations.emplace_back(index, operation);
        entry.offsets.push_back(offset); entry.counts.push_back(counts[i + 1]);
        order.push_back(found->second); offset += counts[i + 1];
      }
      if (offset != operation->getNumOperands()) return fail("switch has undeclared operand segments");
      orders.push_back(std::move(order));
    }
    std::vector<std::set<size_t>> edges(cases.size());
    std::vector<size_t> indegree(cases.size());
    for (const auto &order : orders) for (size_t i = 1; i < order.size(); ++i)
      if (edges[order[i - 1]].insert(order[i]).second) ++indegree[order[i]];
    std::set<size_t> ready;
    for (size_t i = 0; i < cases.size(); ++i) if (!indegree[i]) ready.insert(i);
    llvm::SmallVector<size_t> ordered;
    while (!ready.empty()) {
      auto index = *ready.begin(); ready.erase(ready.begin()); ordered.push_back(index);
      for (auto next : edges[index]) if (!--indegree[next]) ready.insert(next);
    }
    if (ordered.size() != cases.size()) return fail("switch case orders contradict one another");
    auto selector = operand(selectors);
    if (!selector) return selector.takeError();
    mlir::OperationState state(builder.getUnknownLoc(), "sela.switch");
    state.addOperands(*selector); state.addSuccessors(defaultSuccessor);
    for (size_t i = 0; i < defaultCount; ++i) {
      Observations<mlir::Value> originals;
      for (auto [index, operation] : defaults) originals.emplace_back(index, operation->getOperand(1 + i));
      auto value = operand(originals);
      if (!value) return value.takeError();
      state.addOperands(*value);
    }
    llvm::SmallVector<int32_t> counts{int32_t(defaultCount)};
    llvm::SmallVector<Attribute> labels, domains;
    for (auto index : ordered) {
      auto &entry = cases[index];
      auto label = attribute(entry.labels);
      if (!label) return label.takeError();
      labels.push_back(*label); counts.push_back(entry.counts.front()); state.addSuccessors(entry.successor);
      llvm::SmallVector<StringRef> present;
      for (auto [target, operation] : entry.operations) present.push_back(ids[target]);
      domains.push_back(ir::targetSet(context, present));
      for (size_t i = 0; i < entry.counts.front(); ++i) {
        Observations<mlir::Value> originals;
        for (size_t j = 0; j < entry.operations.size(); ++j) {
          auto [target, operation] = entry.operations[j];
          originals.emplace_back(target, operation->getOperand(entry.offsets[j] + i));
        }
        auto value = operand(originals);
        if (!value) return value.takeError();
        state.addOperands(*value);
      }
    }
    state.addAttribute("cases", builder.getArrayAttr(labels));
    state.addAttribute("case_domains", builder.getArrayAttr(domains));
    state.addAttribute("argument_counts", builder.getDenseI32ArrayAttr(counts));
    builder.create(state);
    return llvm::Error::success();
  }
  llvm::Error operation(const Observations<Operation *> &operations) {
    auto *first = operations.front().second;
    if (first->getName().getStringRef() == "sela.switch" && llvm::all_of(operations, [](auto entry) {
          return entry.second->getName().getStringRef() == "sela.switch";
        })) return switchOperation(operations);
    for (auto [index, peer] : operations)
      if (peer->getName() != first->getName() || peer->getNumOperands() != first->getNumOperands() ||
          peer->getNumResults() != first->getNumResults() || peer->getNumRegions() ||
          peer->getNumSuccessors() != first->getNumSuccessors())
        return fail("instruction shape needs additional common-graph normalization");
    auto attrs = attributes(operations);
    if (!attrs) return attrs.takeError();
    mlir::OperationState state(builder.getUnknownLoc(), first->getName().getStringRef());
    state.addAttributes(*attrs);
    for (unsigned i = 0; i < first->getNumResults(); ++i) {
      Observations<mlir::Type> input;
      for (auto [index, peer] : operations) input.emplace_back(index, peer->getResult(i).getType());
      auto result = type(input);
      if (!result) return result.takeError();
      state.addTypes(*result);
    }
    for (unsigned i = 0; i < first->getNumOperands(); ++i) {
      Observations<mlir::Value> input;
      for (auto [index, peer] : operations) input.emplace_back(index, peer->getOperand(i));
      auto value = operand(input);
      if (!value) return value.takeError();
      state.addOperands(*value);
    }
    for (unsigned i = 0; i < first->getNumSuccessors(); ++i) {
      mlir::Block *successor = nullptr;
      for (auto [index, peer] : operations) {
        auto *mapped = blocks[index].lookup(peer->getSuccessor(i));
        if (!mapped || (successor && successor != mapped)) return fail("branch successors lack common correspondence");
        successor = mapped;
      }
      state.addSuccessors(successor);
    }
    auto *result = builder.create(state);
    for (unsigned i = 0; i < first->getNumResults(); ++i) {
      Observations<mlir::Value> input;
      for (auto [index, peer] : operations) input.emplace_back(index, peer->getResult(i));
      values[key(input)] = result->getResult(i);
    }
    return llvm::Error::success();
  }
  llvm::Error function(const Observations<Operation *> &functions) {
    auto attrs = attributes(functions, {"block_domains"});
    if (!attrs) return attrs.takeError();
    mlir::OperationState state(builder.getUnknownLoc(), "sela.func");
    state.addAttributes(*attrs); state.addRegion();
    auto *result = builder.create(state);
    auto &region = result->getRegion(0);
    bool empty = functions.front().second->getRegion(0).empty();
    for (auto [index, function] : functions)
      if (function->getRegion(0).empty() != empty) return fail("conditional function definition presence is not proved");
    if (empty) return llvm::Error::success();
    auto groups = correspondence(functions);
    if (!groups) return groups.takeError();
    values.clear();
    for (auto &mapping : blocks) mapping.clear();
    llvm::SmallVector<Attribute> domains;
    for (const auto &group : *groups) {
      auto *block = new mlir::Block; region.push_back(block);
      llvm::SmallVector<StringRef> present;
      std::optional<unsigned> count;
      for (size_t i = 0; i < ids.size(); ++i) if (auto *source = group.blocks[i]) {
        present.push_back(ids[i]); blocks[i][source] = block;
        if (count && *count != source->getNumArguments()) return fail("block argument arities differ");
        count = source->getNumArguments();
      }
      domains.push_back(ir::targetSet(context, present));
      for (unsigned argument = 0; argument < count.value_or(0); ++argument) {
        Observations<mlir::Type> input;
        Observations<mlir::Value> originals;
        for (size_t i = 0; i < ids.size(); ++i) if (auto *source = group.blocks[i]) {
          auto value = source->getArgument(argument);
          input.emplace_back(i, value.getType()); originals.emplace_back(i, value);
        }
        auto merged = type(input);
        if (!merged) return merged.takeError();
        values[key(originals)] = block->addArgument(*merged, builder.getUnknownLoc());
      }
    }
    result->setAttr("block_domains", builder.getArrayAttr(domains));
    size_t blockIndex = 0;
    for (const auto &group : *groups) {
      auto *destination = &*std::next(region.begin(), blockIndex++);
      builder.setInsertionPointToEnd(destination);
      std::vector<llvm::SmallVector<Operation *>> instructions(ids.size());
      for (size_t i = 0; i < ids.size(); ++i) if (auto *source = group.blocks[i]) {
        for (auto &instruction : *source) instructions[i].push_back(&instruction);
      }
      std::vector<size_t> positions(ids.size());
      while (true) {
        Observations<Operation *> input;
        bool complete = true, same = true;
        for (size_t i = 0; i < ids.size(); ++i) if (group.blocks[i]) {
          if (positions[i] == instructions[i].size()) { same = false; continue; }
          complete = false;
          auto *current = instructions[i][positions[i]];
          if (!input.empty() && current->getName() != input.front().second->getName()) same = false;
          input.emplace_back(i, current);
        }
        if (complete) break;
        if (same) {
          if (auto error = operation(input)) return error;
          for (auto [index, operation] : input) ++positions[index];
          continue;
        }
        // An integer extension/truncation can be absent where its source and
        // result types coincide. Reuse an already-proved source SSA tuple;
        // never pair it by instruction name, source spelling or a guess.
        Observations<Operation *> casts;
        for (auto [index, operation] : input) {
          auto opcode = operation->getAttrOfType<mlir::StringAttr>("opcode");
          if (operation->getName().getStringRef() != "sela.cast" || !opcode ||
              (opcode.getValue() != "sext" && opcode.getValue() != "zext" && opcode.getValue() != "trunc") ||
              operation->getNumOperands() != 1 || operation->getNumResults() != 1 ||
              !mlir::isa<mlir::IntegerType>(operation->getOperand(0).getType()) ||
              !mlir::isa<mlir::IntegerType>(operation->getResult(0).getType())) continue;
          casts.emplace_back(index, operation);
        }
        if (casts.empty()) {
          std::string detail;
          for (auto [index, operation] : input) {
            if (!detail.empty()) detail += ", ";
            detail += ids[index].str() + "=" + operation->getName().getStringRef().str() + "@" + std::to_string(positions[index]);
            if (auto callee = operation->getAttrOfType<mlir::StringAttr>("callee")) detail += ":" + callee.getValue().str();
          }
          return fail("instruction sequences need additional local semantic normalization (" + detail + ")");
        }
        const std::vector<const void *> *sourceTuple = nullptr;
        mlir::Value sharedSource;
        for (const auto &[tuple, value] : values) {
          bool match = true;
          for (auto [index, operation] : casts)
            if (tuple[index] != operation->getOperand(0).getAsOpaquePointer()) match = false;
          for (size_t i = 0; i < ids.size(); ++i) if (group.blocks[i] && !tuple[i]) match = false;
          if (!match) continue;
          if (sourceTuple && *sourceTuple != tuple) return fail("ambiguous optional-cast source correspondence");
          sourceTuple = &tuple; sharedSource = value;
        }
        if (!sourceTuple) return fail("optional cast lacks shared source evidence");
        Observations<mlir::Type> resultTypes;
        Observations<mlir::Value> resultValues;
        Observations<Attribute> opcodes;
        for (size_t i = 0; i < ids.size(); ++i) if (group.blocks[i]) {
          auto original = mlir::Value::getFromOpaquePointer(const_cast<void *>((*sourceTuple)[i]));
          auto found = llvm::find_if(casts, [&](auto entry) { return entry.first == i; });
          if (found != casts.end()) {
            original = found->second->getResult(0);
            auto opcode = found->second->getAttrOfType<mlir::StringAttr>("opcode").getValue();
            opcodes.emplace_back(i, builder.getStringAttr("native_" + opcode.str()));
            ++positions[i];
          } else opcodes.emplace_back(i, builder.getStringAttr("native_zext"));
          resultTypes.emplace_back(i, original.getType()); resultValues.emplace_back(i, original);
        }
        auto resultType = type(resultTypes);
        if (!resultType) return resultType.takeError();
        mlir::OperationState cast(builder.getUnknownLoc(), "sela.cast");
        cast.addOperands(sharedSource); cast.addTypes(*resultType); cast.addAttribute("opcode", choice(opcodes));
        auto *result = builder.create(cast); values[key(resultValues)] = result->getResult(0);
      }
    }
    builder.setInsertionPointAfter(result);
    return llvm::Error::success();
  }
  llvm::Error merge(llvm::ArrayRef<mlir::ModuleOp> modules) {
    builder.setInsertionPointToEnd(output->getBody());
    // Module flags may be present only in some ABIs. Coalesce identical records
    // while keeping their exact declared target sets and all flag semantics.
    llvm::SmallVector<Attribute> flagValues;
    llvm::SmallVector<llvm::SmallVector<StringRef>> flagTargets;
    for (size_t i = 0; i < modules.size(); ++i) {
      auto flags = modules[i]->getAttrOfType<mlir::ArrayAttr>("sela.module_flags");
      if (!flags) continue;
      for (auto raw : flags) {
        auto record = mlir::dyn_cast<mlir::DictionaryAttr>(raw);
        if (!record) return fail("invalid module flag observation");
        mlir::NamedAttrList fields(record); fields.erase("targets");
        auto key = fields.getDictionary(context);
        auto found = llvm::find(flagValues, key);
        if (found == flagValues.end()) { flagValues.push_back(key); flagTargets.push_back({ids[i]}); }
        else flagTargets[std::distance(flagValues.begin(), found)].push_back(ids[i]);
      }
    }
    llvm::SmallVector<Attribute> flags;
    for (size_t i = 0; i < flagValues.size(); ++i) {
      mlir::NamedAttrList fields(mlir::cast<mlir::DictionaryAttr>(flagValues[i]));
      fields.set("targets", ir::targetSet(context, flagTargets[i])); flags.push_back(fields.getDictionary(context));
    }
    (*output)->setAttr("sela.module_flags", builder.getArrayAttr(flags));
    std::vector<std::map<std::string, Operation *>> inventories(modules.size());
    for (size_t i = 0; i < modules.size(); ++i) for (auto &operation : modules[i]->getRegion(0).front().getOperations()) {
      auto id = operation.getAttrOfType<mlir::StringAttr>("id");
      if (!id || !inventories[i].emplace(id.getValue().str(), &operation).second)
        return fail("missing or duplicate semantic definition identity");
    }
    for (auto &operation : modules.front()->getRegion(0).front().getOperations()) {
      auto id = operation.getAttrOfType<mlir::StringAttr>("id").getValue().str();
      Observations<Operation *> input;
      for (size_t i = 0; i < modules.size(); ++i) {
        auto found = inventories[i].find(id);
        if (found == inventories[i].end() || found->second->getName() != operation.getName())
          return fail("semantic definition inventories do not correspond");
        input.emplace_back(i, found->second); inventories[i].erase(found);
      }
      if (operation.getName().getStringRef() == "sela.func") {
        if (auto error = function(input)) return fail(id + ": " + llvm::toString(std::move(error)));
      } else {
        auto attrs = attributes(input);
        if (!attrs) return attrs.takeError();
        mlir::OperationState state(builder.getUnknownLoc(), operation.getName().getStringRef());
        state.addAttributes(*attrs); builder.create(state);
      }
    }
    for (size_t i = 0; i < inventories.size(); ++i) if (!inventories[i].empty())
      return fail("unmatched semantic definition for " + ids[i] + ": " + inventories[i].begin()->first);
    return llvm::Error::success();
  }
};
}
llvm::Expected<CommonModule> mergeCommonModules(
    llvm::ArrayRef<mlir::ModuleOp> modules, llvm::ArrayRef<StringRef> ids) {
  if (modules.empty() || modules.size() != ids.size()) return fail("invalid observation inventory");
  Merger merger(modules.front()->getContext(), ids);
  if (auto error = merger.merge(modules)) return std::move(error);
  if (mlir::failed(mlir::verify(*merger.output))) return fail("factored shared graph failed structural verification");
  return CommonModule{std::move(merger.output), std::move(merger.storageIdentities)};
}
llvm::Expected<CommonModule> factorScopedModules(
    llvm::ArrayRef<mlir::ModuleOp> modules, llvm::ArrayRef<StringRef> ids) {
  if (modules.empty() || modules.size() != ids.size()) return fail("invalid scoped observation inventory");
  auto *context = modules.front()->getContext();
  mlir::Builder builder(context);
  mlir::OwningOpRef<mlir::ModuleOp> output(mlir::ModuleOp::create(builder.getUnknownLoc()));
  (*output)->setAttr("sela.schema", builder.getI32IntegerAttr(1));
  (*output)->setAttr("sela.targets", ir::targetSet(context, ids));
  std::vector<llvm::SmallVector<Operation *>> definitions(modules.size());
  llvm::SmallVector<Attribute> flags;
  size_t count = 0;
  for (size_t i = 0; i < modules.size(); ++i) {
    if (modules[i]->getContext() != context) return fail("scoped factoring requires one MLIR context");
    for (auto &operation : modules[i]->getRegion(0).front().getOperations()) definitions[i].push_back(&operation);
    count = std::max(count, definitions[i].size());
    if (auto values = modules[i]->getAttrOfType<mlir::ArrayAttr>("sela.module_flags"))
      flags.append(values.begin(), values.end());
  }
  (*output)->setAttr("sela.module_flags", builder.getArrayAttr(flags));
  // Positional grouping is conservative and preserves the native symbol order
  // even when one target has extra or reordered definitions. The richer common
  // merger remains responsible for proving nonliteral sharing opportunities.
  for (size_t index = 0; index < count; ++index) {
    struct Group { Operation *operation; llvm::SmallVector<StringRef> targets; };
    std::vector<Group> groups;
    std::map<std::string, size_t> keys;
    for (size_t i = 0; i < modules.size(); ++i) {
      if (index >= definitions[i].size()) continue;
      auto *operation = definitions[i][index];
      std::string text;
      llvm::raw_string_ostream stream(text);
      operation->print(stream);
      auto [found, inserted] = keys.emplace(text, groups.size());
      if (inserted) groups.push_back({operation, {}});
      groups[found->second].targets.push_back(ids[i]);
    }
    for (const auto &group : groups) {
      auto *copy = group.operation->clone();
      if (group.targets.size() != ids.size()) copy->setAttr("targets", ir::targetSet(context, group.targets));
      output->getBody()->push_back(copy);
    }
  }
  return CommonModule{std::move(output), {}};
}
}
