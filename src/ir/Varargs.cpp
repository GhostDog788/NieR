#include "Varargs.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Operator.h"

namespace sela::detail {
namespace {
bool number(const llvm::Value *value, uint64_t expected, unsigned width = 32) {
  auto *integer = llvm::dyn_cast_or_null<llvm::ConstantInt>(value);
  return integer && integer->getBitWidth() == width && integer->equalsInt(expected);
}
bool plain(const llvm::Instruction *instruction) {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>> metadata;
  instruction->getAllMetadataOtherThanDebugLoc(metadata);
  for (auto [kind, node] : metadata)
    if (kind != llvm::LLVMContext::MD_tbaa && kind != llvm::LLVMContext::MD_tbaa_struct &&
        kind != llvm::LLVMContext::MD_DIAssignID) return false;
  return true;
}
bool scalar(llvm::Type *type) {
  return type->isIntegerTy(32) || type->isIntegerTy(64) || type->isDoubleTy() ||
         (type->isPointerTy() && type->getPointerAddressSpace() == 0);
}
bool stateType(llvm::Type *type, bool wide) {
  if (!wide) return type->isPointerTy() && type->getPointerAddressSpace() == 0;
  auto *array = llvm::dyn_cast<llvm::ArrayType>(type);
  auto *record = array ? llvm::dyn_cast<llvm::StructType>(array->getElementType()) : nullptr;
  return array && array->getNumElements() == 1 && record && !record->isOpaque() &&
         !record->isPacked() && record->getNumElements() == 4 &&
         record->getElementType(0)->isIntegerTy(32) && record->getElementType(1)->isIntegerTy(32) &&
         record->getElementType(2)->isPointerTy() && record->getElementType(2)->getPointerAddressSpace() == 0 &&
         record->getElementType(3)->isPointerTy() && record->getElementType(3)->getPointerAddressSpace() == 0;
}
bool decay(const llvm::GetElementPtrInst *gep, const llvm::AllocaInst *state) {
  return gep && plain(gep) && gep->isInBounds() && gep->getPointerOperand() == state &&
      gep->getSourceElementType() == state->getAllocatedType() && gep->getNumIndices() == 2 &&
      number(gep->getOperand(1), 0, 64) && number(gep->getOperand(2), 0, 64);
}
const llvm::AllocaInst *stateRoot(const llvm::Value *pointer, bool wide) {
  auto *state = llvm::dyn_cast<llvm::AllocaInst>(pointer);
  if (!state && wide) {
    auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
    state = gep ? llvm::dyn_cast<llvm::AllocaInst>(gep->getPointerOperand()) : nullptr;
    if (!state || !decay(gep, state)) return nullptr;
  }
  return state && stateType(state->getAllocatedType(), wide) ? state : nullptr;
}
bool load(const llvm::LoadInst *value, const llvm::Value *pointer, llvm::Type *type, uint64_t alignment) {
  return value && plain(value) && !value->isVolatile() && !value->isAtomic() &&
      value->getPointerOperand() == pointer && value->getType() == type && value->getAlign().value() == alignment;
}
bool store(const llvm::StoreInst *instruction, const llvm::Value *value,
           const llvm::Value *pointer, uint64_t alignment) {
  return instruction && plain(instruction) && !instruction->isVolatile() && !instruction->isAtomic() &&
      instruction->getValueOperand() == value && instruction->getPointerOperand() == pointer &&
      instruction->getAlign().value() == alignment;
}
bool bytes(const llvm::GetElementPtrInst *gep, const llvm::Value *pointer,
           const llvm::Value *offset, bool inbounds) {
  return gep && plain(gep) && gep->getNumIndices() == 1 && gep->getSourceElementType()->isIntegerTy(8) &&
      gep->getPointerOperand() == pointer && gep->getOperand(1) == offset && gep->isInBounds() == inbounds;
}
bool field(const llvm::GetElementPtrInst *gep, const llvm::AllocaInst *state, unsigned index) {
  auto *array = llvm::cast<llvm::ArrayType>(state->getAllocatedType());
  return gep && plain(gep) && gep->isInBounds() && gep->getSourceElementType() == array->getElementType() &&
      gep->getPointerOperand() == state && gep->getNumIndices() == 2 &&
      number(gep->getOperand(1), 0) && number(gep->getOperand(2), index);
}
bool closed(llvm::ArrayRef<llvm::Instruction *> instructions, const llvm::Instruction *result) {
  llvm::DenseSet<const llvm::Instruction *> allowed(instructions.begin(), instructions.end());
  for (auto *instruction : instructions) {
    if (!plain(instruction)) return false;
    if (instruction == result) continue;
    for (const auto *user : instruction->users()) {
      auto *use = llvm::dyn_cast<llvm::Instruction>(user);
      if (!use || !allowed.contains(use)) return false;
    }
  }
  return true;
}

// i686 promoted scalar va_arg: load cursor; advance by rounded ABI size;
// store cursor; load value. Nothing may observe the consumed intermediate.
bool narrow(llvm::LoadInst *result, const NativeVarargs &information) {
  if (!scalar(result->getType())) return false;
  auto *write = llvm::dyn_cast_or_null<llvm::StoreInst>(result->getPrevNode());
  auto *advance = write ? llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(write->getPrevNode()) : nullptr;
  auto *cursor = advance ? llvm::dyn_cast_or_null<llvm::LoadInst>(advance->getPrevNode()) : nullptr;
  auto *state = cursor ? llvm::dyn_cast<llvm::AllocaInst>(cursor->getPointerOperand()) : nullptr;
  if (!state || !information.states.contains(state)) return false;
  auto &context = result->getContext();
  uint64_t size = result->getType()->isDoubleTy() || result->getType()->isIntegerTy(64) ? 8 : 4;
  if (!load(cursor, state, llvm::PointerType::get(context, 0), 4) ||
      !bytes(advance, cursor, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), size), true) ||
      !store(write, advance, state, 4) || !load(result, cursor, result->getType(), 4) ||
      !closed({cursor, advance, write, result}, result)) return false;
  auto *argument = new llvm::VAArgInst(state, result->getType(), "", cursor);
  result->replaceAllUsesWith(argument);
  for (auto *instruction : llvm::SmallVector<llvm::Instruction *>{result, write, advance, cursor})
    instruction->eraseFromParent();
  return true;
}

// SysV AMD64 promoted scalar va_arg. Match the entire register/overflow
// diamond, exact state fields, bounds, strides, alignment and use closure.
bool wide(llvm::LoadInst *result, const NativeVarargs &information) {
  if (!scalar(result->getType())) return false;
  auto *join = result->getParent();
  auto *phi = llvm::dyn_cast<llvm::PHINode>(&join->front());
  if (!phi || phi->getNextNode() != result || phi->getNumIncomingValues() != 2 ||
      !phi->getType()->isPointerTy() || !phi->hasOneUse() || result->getPointerOperand() != phi) return false;
  auto *registers = phi->getIncomingBlock(0), *overflow = phi->getIncomingBlock(1);
  // Clang's incoming order is register then overflow, independently of names.
  if (registers->size() != 6 || overflow->size() != 5 ||
      !registers->hasNPredecessors(1) || !overflow->hasNPredecessors(1) || !join->hasNPredecessors(2)) return false;
  auto *head = registers->getSinglePredecessor();
  if (!head || overflow->getSinglePredecessor() != head || head == join || head == registers || head == overflow) return false;
  auto *branch = llvm::dyn_cast<llvm::BranchInst>(head->getTerminator());
  auto *compare = branch ? llvm::dyn_cast_or_null<llvm::ICmpInst>(branch->getPrevNode()) : nullptr;
  auto *offset = compare ? llvm::dyn_cast_or_null<llvm::LoadInst>(compare->getPrevNode()) : nullptr;
  auto *position = offset ? llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(offset->getPrevNode()) : nullptr;
  auto *state = position ? llvm::dyn_cast<llvm::AllocaInst>(position->getPointerOperand()) : nullptr;
  if (!state || !information.states.contains(state) || !branch->isConditional() ||
      branch->getCondition() != compare || branch->getSuccessor(0) != registers || branch->getSuccessor(1) != overflow ||
      compare->getPredicate() != llvm::CmpInst::ICMP_ULE) return false;
  bool floating = result->getType()->isDoubleTy();
  auto &context = result->getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *pointer = llvm::PointerType::get(context, 0);
  uint64_t stateAlign = state->getAlign().value();
  uint64_t offsetAlign = llvm::commonAlignment(llvm::Align(stateAlign), floating ? 4 : 0).value();
  if (!field(position, state, floating ? 1 : 0) || !load(offset, position, i32, offsetAlign) ||
      compare->getOperand(0) != offset || !number(compare->getOperand(1), floating ? 160 : 40)) return false;
  llvm::SmallVector<llvm::Instruction *> r, o;
  for (auto &instruction : *registers) r.push_back(&instruction);
  for (auto &instruction : *overflow) o.push_back(&instruction);
  auto *saveAddress = llvm::dyn_cast<llvm::GetElementPtrInst>(r[0]);
  auto *save = llvm::dyn_cast<llvm::LoadInst>(r[1]);
  auto *address = llvm::dyn_cast<llvm::GetElementPtrInst>(r[2]);
  auto *increment = llvm::dyn_cast<llvm::BinaryOperator>(r[3]);
  auto *registerBranch = llvm::dyn_cast<llvm::BranchInst>(r[5]);
  auto *overflowAddress = llvm::dyn_cast<llvm::GetElementPtrInst>(o[0]);
  auto *cursor = llvm::dyn_cast<llvm::LoadInst>(o[1]);
  auto *advance = llvm::dyn_cast<llvm::GetElementPtrInst>(o[2]);
  auto *overflowBranch = llvm::dyn_cast<llvm::BranchInst>(o[4]);
  if (!field(saveAddress, state, 3) || !load(save, saveAddress, pointer, llvm::commonAlignment(llvm::Align(stateAlign), 16).value()) ||
      !bytes(address, save, offset, false) || !increment || increment->getOpcode() != llvm::Instruction::Add ||
      increment->hasNoSignedWrap() || increment->hasNoUnsignedWrap() || increment->getOperand(0) != offset ||
      !number(increment->getOperand(1), floating ? 16 : 8) ||
      !store(llvm::dyn_cast<llvm::StoreInst>(r[4]), increment, position, offsetAlign) ||
      !registerBranch || !registerBranch->isUnconditional() || registerBranch->getSuccessor(0) != join ||
      !field(overflowAddress, state, 2) || !load(cursor, overflowAddress, pointer, llvm::commonAlignment(llvm::Align(stateAlign), 8).value()) ||
      !bytes(advance, cursor, llvm::ConstantInt::get(i32, 8), false) ||
      !store(llvm::dyn_cast<llvm::StoreInst>(o[3]), advance, overflowAddress, llvm::commonAlignment(llvm::Align(stateAlign), 8).value()) ||
      !overflowBranch || !overflowBranch->isUnconditional() || overflowBranch->getSuccessor(0) != join ||
      phi->getIncomingValue(0) != address || phi->getIncomingValue(1) != cursor ||
      !load(result, phi, result->getType(), result->getType()->isIntegerTy(32) ? 4 : 8)) return false;
  llvm::SmallVector<llvm::Instruction *> consumed{position, offset, compare, branch};
  consumed.append(r); consumed.append(o); consumed.push_back(phi); consumed.push_back(result);
  if (!closed(consumed, result) || registers->hasAddressTaken() || overflow->hasAddressTaken() || join->hasAddressTaken()) return false;
  auto *argument = new llvm::VAArgInst(state, result->getType(), "", position);
  result->replaceAllUsesWith(argument);
  result->eraseFromParent(); phi->eraseFromParent();
  branch->eraseFromParent(); compare->eraseFromParent();
  // Drop the dead diamond's references before erasing values used across its
  // blocks. Only the proven result can escape this closed instruction set.
  registers->dropAllReferences(); overflow->dropAllReferences();
  registers->eraseFromParent(); overflow->eraseFromParent();
  offset->eraseFromParent(); position->eraseFromParent();
  join->replaceAllUsesWith(head);
  head->splice(head->end(), join);
  join->eraseFromParent();
  return true;
}
}

llvm::Expected<NativeVarargs> normalizeNativeVarargs(llvm::Module &module, bool word64) {
  NativeVarargs information;
  for (auto &function : module)
    for (auto &instruction : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
      if (!call || (call->getIntrinsicID() != llvm::Intrinsic::vastart &&
                    call->getIntrinsicID() != llvm::Intrinsic::vacopy &&
                    call->getIntrinsicID() != llvm::Intrinsic::vaend)) continue;
      for (auto &argument : call->args())
        if (auto *state = stateRoot(argument.get(), word64)) information.states.insert(state);
    }
  if (information.states.empty()) return information;
  // Private debug hints have served their purpose. This is the same mandatory
  // publication stripping already performed by the native inverse comparison.
  llvm::StripDebugInfo(module);
  if (word64) {
    llvm::SmallVector<llvm::GetElementPtrInst *> decays;
    for (auto &function : module)
      for (auto &instruction : llvm::instructions(function)) {
        auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction);
        auto *state = gep ? llvm::dyn_cast<llvm::AllocaInst>(gep->getPointerOperand()) : nullptr;
        if (state && information.states.contains(state) && decay(gep, state)) decays.push_back(gep);
      }
    for (auto *gep : decays) {
      gep->replaceAllUsesWith(gep->getPointerOperand());
      gep->eraseFromParent();
    }
  }
  bool changed;
  do {
    changed = false;
    for (auto &function : module) {
      for (auto &instruction : llvm::instructions(function)) {
        auto *result = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (result && (word64 ? wide(result, information) : narrow(result, information))) {
          ++information.scalarExtractions;
          changed = true;
          break;
        }
      }
      if (changed) break;
    }
  } while (changed);
  return information;
}
}
