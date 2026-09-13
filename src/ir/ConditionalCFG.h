#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"
#include <optional>

namespace llvm {
class BasicBlock;
class ConstantInt;
class Function;
class SwitchInst;
}

namespace sela::detail {

// Private binary correspondence labels only: 1=left, 2=right, 3=both.
// Public qualification uses explicit target identities, never these bit masks.
// A missing original is permitted only for a proved one-sided switch arm.
struct ConditionalCFGBlockPair {
  const llvm::BasicBlock *left = nullptr;
  const llvm::BasicBlock *right = nullptr;
  unsigned domain = 3;
};
struct ConditionalCFGCasePair {
  const llvm::ConstantInt *left = nullptr;
  const llvm::ConstantInt *right = nullptr;
  std::optional<unsigned> leftIndex;
  std::optional<unsigned> rightIndex;
  unsigned successor = 0; // Index in ConditionalCFG::blocks.
  unsigned domain = 3;
};
struct ConditionalCFGSwitchPair {
  const llvm::SwitchInst *left = nullptr;
  const llvm::SwitchInst *right = nullptr;
  unsigned defaultSuccessor = 0;
  llvm::SmallVector<ConditionalCFGCasePair, 4> cases;
};
struct ConditionalCFG {
  llvm::SmallVector<ConditionalCFGBlockPair, 16> blocks;
  llvm::SmallVector<ConditionalCFGSwitchPair, 4> switches;
  llvm::DenseMap<const llvm::BasicBlock *, unsigned> leftBlocks, rightBlocks;
  llvm::DenseMap<const llvm::SwitchInst *, unsigned> leftSwitches, rightSwitches;
  bool conditional = false;
};

// Read-only graph proof, independent of block/function/source names. Input LLVM
// must already verify. Entry and branch orientation anchor shared blocks; switch
// default and exact same-width integer labels anchor shared cases. The resulting
// block/case lists preserve BOTH original orders when filtered by domain.
//
// Differences may only add distinct switch arms: each is a nonempty closed
// chain of unconditional branches, entered by exactly its one switch edge and
// rejoining a shared block. No PHIs anywhere in a conditional function, escaping
// arm SSA, block addresses, dead/unpaired regions or other differing terminators
// are admitted. Unchanged paired graphs may retain PHIs for the caller to merge.
//
// This does not prove instruction/type/condition/metadata correspondence or
// lower one-sided instructions. Those remain producer and exact-inverse checks;
// success here is never permission to discard or replace any native body.
llvm::Expected<ConditionalCFG> pairConditionalCFG(const llvm::Function &left,
                                                 const llvm::Function &right);

} // namespace sela::detail
