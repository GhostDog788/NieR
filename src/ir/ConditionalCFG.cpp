#include "ConditionalCFG.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include <map>

namespace sela::detail {
namespace {
llvm::Error fail(llvm::StringRef message) {
  return llvm::createStringError(std::make_error_code(std::errc::invalid_argument),
                                 "conditional CFG: " + message);
}

// Merge two sequences of already established identities, without reordering
// either projection. Shared identities are mandatory anchors, not LCS guesses.
llvm::Expected<llvm::SmallVector<unsigned, 16>> mergeOrder(
    llvm::ArrayRef<unsigned> left, llvm::ArrayRef<unsigned> right,
    llvm::ArrayRef<unsigned> domains) {
  llvm::SmallVector<unsigned, 16> result;
  unsigned a = 0, b = 0;
  while (a < left.size() || b < right.size()) {
    if (a < left.size() && domains[left[a]] == 1) result.push_back(left[a++]);
    else if (b < right.size() && domains[right[b]] == 2) result.push_back(right[b++]);
    else if (a < left.size() && b < right.size() && left[a] == right[b]) {
      result.push_back(left[a++]); ++b;
    } else return fail("shared block or case order differs between native inputs");
  }
  return result;
}

struct PrivateArm {
  const llvm::SwitchInst *source;
  const llvm::BasicBlock *first;
  unsigned domain;
};
struct PendingCase {
  const llvm::ConstantInt *left = nullptr, *right = nullptr;
  const llvm::BasicBlock *leftTarget = nullptr, *rightTarget = nullptr;
  std::optional<unsigned> leftIndex, rightIndex;
  unsigned domain = 3;
};
struct PendingSwitch {
  const llvm::SwitchInst *left, *right;
  llvm::SmallVector<PendingCase, 4> cases;
};

class Pairing {
  const llvm::Function &left, &right;
  llvm::SmallVector<ConditionalCFGBlockPair, 16> pairs;
  llvm::DenseMap<const llvm::BasicBlock *, unsigned> leftPairs, rightPairs;
  llvm::SmallVector<PendingSwitch, 4> switches;
  llvm::SmallVector<PrivateArm, 4> arms;

  llvm::Error pair(const llvm::BasicBlock *a, const llvm::BasicBlock *b) {
    if (!a || !b || a->getParent() != &left || b->getParent() != &right)
      return fail("edge leaves its input function");
    auto existingA = leftPairs.find(a), existingB = rightPairs.find(b);
    if (existingA != leftPairs.end() || existingB != rightPairs.end()) {
      if (existingA == leftPairs.end() || existingB == rightPairs.end() ||
          existingA->second != existingB->second)
        return fail("shared edge correspondence is inconsistent");
      return llvm::Error::success();
    }
    unsigned index = pairs.size();
    leftPairs[a] = rightPairs[b] = index;
    pairs.push_back({a, b, 3});
    return llvm::Error::success();
  }

  llvm::Error matchSwitch(const llvm::SwitchInst &a, const llvm::SwitchInst &b) {
    if (a.getCondition()->getType()->getIntegerBitWidth() !=
            b.getCondition()->getType()->getIntegerBitWidth())
      return fail("switch label widths differ");
    if (a.getCondition()->getType()->getIntegerBitWidth() > 64 ||
        a.getNumCases() > 65536 || b.getNumCases() > 65536)
      return fail("switch inventory exceeds the qualified bound");
    if (auto error = pair(a.getDefaultDest(), b.getDefaultDest())) return error;
    PendingSwitch pending{&a, &b, {}};
    std::map<uint64_t, unsigned> labels;
    llvm::SmallVector<unsigned, 16> leftOrder, rightOrder, domains;
    unsigned ordinal = 0;
    for (const auto &entry : a.cases()) {
      unsigned index = pending.cases.size();
      if (!labels.emplace(entry.getCaseValue()->getZExtValue(), index).second)
        return fail("duplicate native switch label");
      pending.cases.push_back({entry.getCaseValue(), nullptr,
          entry.getCaseSuccessor(), nullptr, ordinal++, std::nullopt, 1});
      leftOrder.push_back(index);
    }
    ordinal = 0;
    for (const auto &entry : b.cases()) {
      auto found = labels.find(entry.getCaseValue()->getZExtValue());
      unsigned index;
      if (found == labels.end()) {
        index = pending.cases.size();
        pending.cases.push_back({nullptr, entry.getCaseValue(), nullptr,
            entry.getCaseSuccessor(), std::nullopt, ordinal, 2});
      } else {
        index = found->second;
        auto &candidate = pending.cases[index];
        if (candidate.right) return fail("duplicate native switch label");
        candidate.right = entry.getCaseValue();
        candidate.rightTarget = entry.getCaseSuccessor();
        candidate.rightIndex = ordinal;
        candidate.domain = 3;
        if (auto error = pair(candidate.leftTarget, candidate.rightTarget)) return error;
      }
      rightOrder.push_back(index); ++ordinal;
    }
    for (const auto &entry : pending.cases) domains.push_back(entry.domain);
    auto order = mergeOrder(leftOrder, rightOrder, domains);
    if (!order) return order.takeError();
    auto original = std::move(pending.cases);
    pending.cases.clear();
    for (unsigned index : *order) {
      const auto &entry = original[index];
      pending.cases.push_back(entry);
      if (entry.domain == 1) arms.push_back({&a, entry.leftTarget, 1});
      if (entry.domain == 2) arms.push_back({&b, entry.rightTarget, 2});
    }
    switches.push_back(std::move(pending));
    return llvm::Error::success();
  }

  llvm::Error matchShared() {
    if (auto error = pair(&left.getEntryBlock(), &right.getEntryBlock())) return error;
    for (unsigned index = 0; index < pairs.size(); ++index) {
      // pair() may grow/reallocate the vector; keep pointers, not a reference.
      auto *a = pairs[index].left->getTerminator();
      auto *b = pairs[index].right->getTerminator();
      if (!a || !b || a->getOpcode() != b->getOpcode())
        return fail("shared terminator kinds differ");
      if (auto *branch = llvm::dyn_cast<llvm::BranchInst>(a)) {
        auto *other = llvm::cast<llvm::BranchInst>(b);
        if (branch->isConditional() != other->isConditional())
          return fail("shared branch orientations differ");
        for (unsigned edge = 0; edge < branch->getNumSuccessors(); ++edge)
          if (auto error = pair(branch->getSuccessor(edge), other->getSuccessor(edge))) return error;
      } else if (auto *instruction = llvm::dyn_cast<llvm::SwitchInst>(a)) {
        if (auto error = matchSwitch(*instruction, *llvm::cast<llvm::SwitchInst>(b))) return error;
      } else if (!llvm::isa<llvm::ReturnInst, llvm::UnreachableInst>(a)) {
        return fail("native terminator is outside the qualified graph contract");
      }
    }
    return llvm::Error::success();
  }

  llvm::Error qualifyArm(const PrivateArm &arm) {
    auto &mapping = arm.domain == 1 ? leftPairs : rightPairs;
    const auto &function = arm.domain == 1 ? left : right;
    llvm::SmallVector<const llvm::BasicBlock *, 8> chain;
    llvm::DenseSet<const llvm::BasicBlock *> local;
    auto *block = arm.first;
    const llvm::BasicBlock *predecessor = arm.source->getParent();
    while (!mapping.count(block)) {
      if (!block || block->getParent() != &function || !local.insert(block).second)
        return fail("one-sided arm cycles or leaves its function");
      if (chain.size() >= 4096) return fail("one-sided arm exceeds the qualified bound");
      auto predecessors = llvm::predecessors(block);
      if (std::distance(predecessors.begin(), predecessors.end()) != 1 ||
          *predecessors.begin() != predecessor)
        return fail("one-sided arm has an additional or unexpected incoming edge");
      auto *branch = llvm::dyn_cast_or_null<llvm::BranchInst>(block->getTerminator());
      if (!branch || branch->isConditional())
        return fail("one-sided arm must be an unconditional chain rejoining shared code");
      chain.push_back(block);
      predecessor = block;
      block = branch->getSuccessor(0);
    }
    if (chain.empty() || pairs[mapping.lookup(block)].domain != 3)
      return fail("one-sided case does not own a distinct arm rejoining shared code");
    for (auto *member : chain) {
      for (const auto &instruction : *member) {
        for (auto *user : instruction.users()) {
          auto *use = llvm::dyn_cast<llvm::Instruction>(user);
          if (!use || !local.contains(use->getParent()))
            return fail("one-sided arm SSA escapes its closed chain");
        }
      }
      unsigned index = pairs.size();
      mapping[member] = index;
      pairs.push_back({arm.domain == 1 ? member : nullptr,
                       arm.domain == 2 ? member : nullptr, arm.domain});
    }
    return llvm::Error::success();
  }

public:
  Pairing(const llvm::Function &a, const llvm::Function &b) : left(a), right(b) {}
  llvm::Expected<ConditionalCFG> run() {
    if (left.empty() || right.empty() || left.size() > 65536 || right.size() > 65536)
      return fail("requires bounded nonempty function definitions");
    for (auto *function : {&left, &right})
      for (const auto &block : *function)
        if (block.hasAddressTaken()) return fail("block addresses are not qualified");
    if (auto error = matchShared()) return error;
    if (!arms.empty())
      for (auto *function : {&left, &right})
        for (const auto &block : *function)
          if (!block.phis().empty()) return fail("conditional functions cannot contain PHIs yet");
    for (const auto &arm : arms)
      if (auto error = qualifyArm(arm)) return error;
    if (leftPairs.size() != left.size() || rightPairs.size() != right.size())
      return fail("unreachable or otherwise unpaired native region");
    llvm::SmallVector<unsigned, 16> leftOrder, rightOrder, domains;
    for (const auto &block : left) leftOrder.push_back(leftPairs.lookup(&block));
    for (const auto &block : right) rightOrder.push_back(rightPairs.lookup(&block));
    for (const auto &entry : pairs) domains.push_back(entry.domain);
    auto order = mergeOrder(leftOrder, rightOrder, domains);
    if (!order) return order.takeError();
    ConditionalCFG result;
    result.conditional = !arms.empty();
    for (unsigned oldIndex : *order) {
      const auto &entry = pairs[oldIndex];
      unsigned index = result.blocks.size();
      result.blocks.push_back(entry);
      if (entry.left) result.leftBlocks[entry.left] = index;
      if (entry.right) result.rightBlocks[entry.right] = index;
    }
    // Switch records follow the shared native block order as well.
    llvm::DenseMap<const llvm::BasicBlock *, unsigned> pendingSwitches;
    for (unsigned index = 0; index < switches.size(); ++index)
      pendingSwitches[switches[index].left->getParent()] = index;
    for (const auto &block : result.blocks) {
      auto found = pendingSwitches.find(block.left);
      if (found == pendingSwitches.end()) continue;
      const auto &pending = switches[found->second];
      ConditionalCFGSwitchPair entry;
      entry.left = pending.left; entry.right = pending.right;
      entry.defaultSuccessor = result.leftBlocks.lookup(pending.left->getDefaultDest());
      for (const auto &candidate : pending.cases) {
        unsigned successor = candidate.leftTarget
            ? result.leftBlocks.lookup(candidate.leftTarget)
            : result.rightBlocks.lookup(candidate.rightTarget);
        entry.cases.push_back({candidate.left, candidate.right, candidate.leftIndex,
                              candidate.rightIndex, successor, candidate.domain});
      }
      result.leftSwitches[entry.left] = result.rightSwitches[entry.right] = result.switches.size();
      result.switches.push_back(std::move(entry));
    }
    return result;
  }
};
}

llvm::Expected<ConditionalCFG> pairConditionalCFG(const llvm::Function &left,
                                                 const llvm::Function &right) {
  return Pairing(left, right).run();
}
} // namespace sela::detail
