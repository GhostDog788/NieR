#include "ByteSwap.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Transforms/Utils/Local.h"

namespace nier::detail {
namespace {
bool metadataAllowed(const llvm::Instruction &instruction, bool storage) {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> metadata;
  instruction.getAllMetadataOtherThanDebugLoc(metadata);
  for (auto [kind, node] : metadata)
    if (!storage || (kind != llvm::LLVMContext::MD_tbaa &&
                     kind != llvm::LLVMContext::MD_DIAssignID)) return false;
  return true;
}

struct Candidate {
  llvm::Instruction *root;
  llvm::DominatorTree &dominance;
  llvm::Argument *provider = nullptr;
  llvm::AllocaInst *storage = nullptr;
  llvm::StoreInst *initialization = nullptr;
  llvm::SmallPtrSet<llvm::Instruction *, 32> nodes;

  bool argument(llvm::Argument *value) {
    if (!value || value->getParent() != root->getFunction() || value->getType() != root->getType())
      return false;
    if (provider && provider != value) return false;
    provider = value;
    return true;
  }

  bool load(llvm::LoadInst *value) {
    auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(value->getPointerOperand());
    if (!value->isSimple() || !metadataAllowed(*value, true) || !allocation ||
        !allocation->isStaticAlloca() || allocation->getAddressSpace() != 0 ||
        allocation->getAllocatedType() != root->getType() ||
        !llvm::isa<llvm::ConstantInt>(allocation->getArraySize()) ||
        !llvm::cast<llvm::ConstantInt>(allocation->getArraySize())->isOne() ||
        !metadataAllowed(*allocation, true) || value->getAlign() > allocation->getAlign())
      return false;
    if (storage && storage != allocation) return false;
    if (!storage) {
      llvm::StoreInst *store = nullptr;
      for (auto *user : allocation->users()) {
        if (auto *access = llvm::dyn_cast<llvm::LoadInst>(user)) {
          if (!access->isSimple() || access->getType() != root->getType() ||
              access->getPointerOperand() != allocation || !metadataAllowed(*access, true) ||
              access->getAlign() > allocation->getAlign()) return false;
        } else if (auto *write = llvm::dyn_cast<llvm::StoreInst>(user)) {
          if (store || !write->isSimple() || write->getPointerOperand() != allocation ||
              write->getValueOperand()->getType() != root->getType() ||
              write->getAlign() > allocation->getAlign() || !metadataAllowed(*write, true)) return false;
          store = write;
        } else return false; // Escapes, casts and lifetime changes are not this idiom.
      }
      if (!store || !argument(llvm::dyn_cast<llvm::Argument>(store->getValueOperand()))) return false;
      storage = allocation;
      initialization = store;
    }
    return dominance.dominates(initialization, value);
  }

  bool collect(llvm::Value *value, unsigned depth = 0) {
    if (depth > 32 || value->getType() != root->getType()) return false;
    if (llvm::isa<llvm::ConstantInt>(value)) return true;
    if (auto *input = llvm::dyn_cast<llvm::Argument>(value)) return argument(input);
    auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
    if (!instruction || instruction->getFunction() != root->getFunction()) return false;
    if (nodes.contains(instruction)) return true;
    if (nodes.size() >= 128) return false;
    nodes.insert(instruction);
    if (auto *access = llvm::dyn_cast<llvm::LoadInst>(instruction)) return load(access);
    auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(instruction);
    if (!binary || !metadataAllowed(*binary, false) || binary->hasPoisonGeneratingFlags()) return false;
    switch (binary->getOpcode()) {
    case llvm::Instruction::Or:
      break;
    case llvm::Instruction::And:
      if (!llvm::isa<llvm::ConstantInt>(binary->getOperand(1))) return false;
      break;
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr: {
      auto *shift = llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(1));
      if (!shift || shift->getValue().uge(root->getType()->getIntegerBitWidth())) return false;
      break;
    }
    default: return false;
    }
    return collect(binary->getOperand(0), depth + 1) && collect(binary->getOperand(1), depth + 1);
  }

  bool closed() {
    if (!provider) return false;
    for (auto *node : nodes) {
      if (node == root) continue;
      for (auto *user : node->users()) {
        auto *instruction = llvm::dyn_cast<llvm::Instruction>(user);
        if (!instruction || !nodes.contains(instruction)) return false;
      }
    }
    if (storage)
      for (auto *user : storage->users())
        if (auto *access = llvm::dyn_cast<llvm::LoadInst>(user))
          if (!nodes.contains(access) || !dominance.dominates(initialization, access)) return false;
    return true;
  }

  bool recognized() {
    if (!collect(root) || !closed()) return false;
    // LLVM's helper inserts trial instructions and declarations on success,
    // including masked/partial swaps which this stricter contract rejects.
    // Probe a temporary module, never mutate/undo the real capture on failure.
    llvm::Module scratch("byte-swap-proof", root->getContext());
    auto *type = llvm::FunctionType::get(root->getType(), {root->getType()}, false);
    auto *function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "proof", scratch);
    auto *block = llvm::BasicBlock::Create(root->getContext(), "entry", function);
    llvm::DenseMap<llvm::Value *, llvm::Value *> mapped;
    mapped[provider] = function->getArg(0);
    auto copy = [&](auto &&self, llvm::Value *value) -> llvm::Value * {
      if (llvm::isa<llvm::ConstantInt>(value)) return value;
      if (auto found = mapped.find(value); found != mapped.end()) return found->second;
      if (llvm::isa<llvm::LoadInst>(value)) return mapped[value] = function->getArg(0);
      auto *binary = llvm::cast<llvm::BinaryOperator>(value);
      auto *left = self(self, binary->getOperand(0));
      auto *right = self(self, binary->getOperand(1));
      return mapped[value] = llvm::BinaryOperator::Create(binary->getOpcode(), left, right, "", block);
    };
    auto *trial = llvm::cast<llvm::Instruction>(copy(copy, root));
    llvm::ReturnInst::Create(root->getContext(), trial, block);
    llvm::SmallVector<llvm::Instruction *, 4> inserted;
    if (!llvm::recognizeBSwapOrBitReverseIdiom(trial, true, false, inserted) || inserted.size() != 1)
      return false;
    auto *swap = llvm::dyn_cast<llvm::IntrinsicInst>(inserted[0]);
    return swap && swap->getIntrinsicID() == llvm::Intrinsic::bswap &&
        swap->getType() == root->getType() && swap->getArgOperand(0) == function->getArg(0);
  }

  void commit() {
    auto *intrinsic = llvm::Intrinsic::getDeclaration(root->getModule(), llvm::Intrinsic::bswap, {root->getType()});
    auto *swap = llvm::CallInst::Create(intrinsic, {provider}, "", root);
    swap->setDebugLoc(root->getDebugLoc());
    swap->takeName(root);
    root->replaceAllUsesWith(swap);
    // Only the proved closed, nonvolatile expression is dead. The single
    // store and its alloca stay, as do all unrelated instructions/effects.
    llvm::RecursivelyDeleteTriviallyDeadInstructions(root);
  }
};
} // namespace

llvm::Expected<unsigned> normalizeNativeByteSwaps(llvm::Module &module) {
  unsigned normalized = 0;
  for (auto &function : module) {
    if (function.isDeclaration()) continue;
    llvm::DominatorTree dominance(function);
    llvm::SmallVector<llvm::WeakTrackingVH, 32> roots;
    for (auto &instruction : llvm::instructions(function))
      if (instruction.getOpcode() == llvm::Instruction::Or &&
          (instruction.getType()->isIntegerTy(16) || instruction.getType()->isIntegerTy(32) ||
           instruction.getType()->isIntegerTy(64))) roots.push_back(&instruction);
    // Outer complete expressions first; weak handles become null when their
    // interior OR nodes are removed by an accepted larger idiom.
    for (auto &handle : llvm::reverse(roots)) {
      auto *root = llvm::dyn_cast_or_null<llvm::Instruction>(handle);
      if (!root) continue;
      Candidate candidate{root, dominance};
      if (!candidate.recognized()) continue;
      candidate.commit();
      ++normalized;
    }
  }
  return normalized;
}
} // namespace nier::detail
