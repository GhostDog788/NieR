#include "Varargs.h"
#include "sela/Targets.h"

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
bool stateType(llvm::Type *type, llvm::StringRef abi) {
  if (abi == "sysv-i386") return type->isPointerTy() && type->getPointerAddressSpace() == 0;
  if (abi == "aapcs32-vfp" || abi == "aapcs64") {
    auto *record = llvm::dyn_cast<llvm::StructType>(type);
    const unsigned pointers = abi == "aapcs64" ? 3 : 1;
    const unsigned fields = abi == "aapcs64" ? 5 : 1;
    if (!record || record->isOpaque() || record->isPacked() || record->getNumElements() != fields) return false;
    for (unsigned index = 0; index < fields; ++index) {
      auto *field = record->getElementType(index);
      if (index < pointers ? !(field->isPointerTy() && field->getPointerAddressSpace() == 0)
                           : !field->isIntegerTy(32)) return false;
    }
    return true;
  }
  if (abi != "sysv-amd64") return false;
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
const llvm::AllocaInst *stateRoot(const llvm::Value *pointer, llvm::StringRef abi) {
  auto *state = llvm::dyn_cast<llvm::AllocaInst>(pointer);
  if (!state && abi == "sysv-amd64") {
    auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
    state = gep ? llvm::dyn_cast<llvm::AllocaInst>(gep->getPointerOperand()) : nullptr;
    if (!state || !decay(gep, state)) return nullptr;
  }
  return state && stateType(state->getAllocatedType(), abi) ? state : nullptr;
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
  auto *type = state->getAllocatedType();
  if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) type = array->getElementType();
  return gep && plain(gep) && gep->isInBounds() && gep->getSourceElementType() == type &&
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

// AAPCS32's double-width promoted scalars align the cursor to eight bytes.
// The ptrmask is an intrinsic with a fully proved argument and use closure.
bool armAligned(llvm::LoadInst *result, const NativeVarargs &information) {
  if (!result->getType()->isIntegerTy(64) && !result->getType()->isDoubleTy()) return false;
  auto *write = llvm::dyn_cast_or_null<llvm::StoreInst>(result->getPrevNode());
  auto *advance = write ? llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(write->getPrevNode()) : nullptr;
  auto *mask = advance ? llvm::dyn_cast_or_null<llvm::IntrinsicInst>(advance->getPrevNode()) : nullptr;
  auto *rounded = mask ? llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(mask->getPrevNode()) : nullptr;
  auto *cursor = rounded ? llvm::dyn_cast_or_null<llvm::LoadInst>(rounded->getPrevNode()) : nullptr;
  auto *state = cursor ? llvm::dyn_cast<llvm::AllocaInst>(cursor->getPointerOperand()) : nullptr;
  if (!state || !information.states.contains(state) || mask->getIntrinsicID() != llvm::Intrinsic::ptrmask ||
      mask->arg_size() != 2 || mask->getNumOperandBundles() || mask->getCallingConv() || mask->isTailCall()) return false;
  auto &context = result->getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  if (!load(cursor, state, llvm::PointerType::get(context, 0), 4) ||
      !bytes(rounded, cursor, llvm::ConstantInt::get(i32, 7), true) ||
      mask->getArgOperand(0) != rounded || !number(mask->getArgOperand(1), UINT32_C(0xfffffff8)) ||
      !bytes(advance, mask, llvm::ConstantInt::get(i32, 8), true) ||
      !store(write, advance, state, 4) || !load(result, mask, result->getType(), 8) ||
      !closed({cursor, rounded, mask, advance, write, result}, result)) return false;
  auto *argument = new llvm::VAArgInst(state, result->getType(), "", cursor);
  result->replaceAllUsesWith(argument);
  for (auto *instruction : llvm::SmallVector<llvm::Instruction *>{result, write, advance, mask, rounded, cursor})
    instruction->eraseFromParent();
  return true;
}

// Linux AAPCS64 uses a signed register offset and a separate overflow check.
// The whole four-block state machine must match, including both stack edges.
bool armWide(llvm::LoadInst *result, const NativeVarargs &information) {
  if (!scalar(result->getType())) return false;
  auto *join = result->getParent();
  auto *phi = llvm::dyn_cast<llvm::PHINode>(&join->front());
  if (!phi || phi->getNextNode() != result || phi->getNumIncomingValues() != 2 ||
      !phi->getType()->isPointerTy() || !phi->hasOneUse() || result->getPointerOperand() != phi ||
      !join->hasNPredecessors(2)) return false;
  auto *registers = phi->getIncomingBlock(0), *overflow = phi->getIncomingBlock(1);
  if (registers == overflow || registers == join || overflow == join ||
      registers->size() != 4 || overflow->size() != 5 || !registers->hasNPredecessors(1) ||
      !overflow->hasNPredecessors(2)) return false;
  auto *check = registers->getSinglePredecessor();
  if (!check || check == join || check == registers || check == overflow ||
      check->size() != 4 || !check->hasNPredecessors(1)) return false;
  auto *head = check->getSinglePredecessor();
  if (!head || head == join || head == registers || head == overflow || head == check) return false;
  auto *branch = llvm::dyn_cast<llvm::BranchInst>(head->getTerminator());
  auto *compare = branch ? llvm::dyn_cast_or_null<llvm::ICmpInst>(branch->getPrevNode()) : nullptr;
  auto *offset = compare ? llvm::dyn_cast_or_null<llvm::LoadInst>(compare->getPrevNode()) : nullptr;
  auto *position = offset ? llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(offset->getPrevNode()) : nullptr;
  auto *state = position ? llvm::dyn_cast<llvm::AllocaInst>(position->getPointerOperand()) : nullptr;
  if (!state || !information.states.contains(state) || !branch->isConditional() ||
      branch->getCondition() != compare || branch->getSuccessor(0) != overflow || branch->getSuccessor(1) != check ||
      compare->getPredicate() != llvm::CmpInst::ICMP_SGE || compare->getOperand(0) != offset ||
      !number(compare->getOperand(1), 0)) return false;
  bool floating = result->getType()->isDoubleTy();
  auto &context = result->getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *pointer = llvm::PointerType::get(context, 0);
  uint64_t stateAlign = state->getAlign().value();
  uint64_t offsetAlign = llvm::commonAlignment(llvm::Align(stateAlign), floating ? 28 : 24).value();
  if (!field(position, state, floating ? 4 : 3) || !load(offset, position, i32, offsetAlign)) return false;
  llvm::SmallVector<llvm::Instruction *> c, r, o;
  for (auto &instruction : *check) c.push_back(&instruction);
  for (auto &instruction : *registers) r.push_back(&instruction);
  for (auto &instruction : *overflow) o.push_back(&instruction);
  auto *increment = llvm::dyn_cast<llvm::BinaryOperator>(c[0]);
  auto *available = llvm::dyn_cast<llvm::ICmpInst>(c[2]);
  auto *checkBranch = llvm::dyn_cast<llvm::BranchInst>(c[3]);
  auto *saveAddress = llvm::dyn_cast<llvm::GetElementPtrInst>(r[0]);
  auto *save = llvm::dyn_cast<llvm::LoadInst>(r[1]);
  auto *address = llvm::dyn_cast<llvm::GetElementPtrInst>(r[2]);
  auto *registerBranch = llvm::dyn_cast<llvm::BranchInst>(r[3]);
  auto *overflowAddress = llvm::dyn_cast<llvm::GetElementPtrInst>(o[0]);
  auto *cursor = llvm::dyn_cast<llvm::LoadInst>(o[1]);
  auto *advance = llvm::dyn_cast<llvm::GetElementPtrInst>(o[2]);
  auto *overflowBranch = llvm::dyn_cast<llvm::BranchInst>(o[4]);
  if (!increment || increment->getOpcode() != llvm::Instruction::Add ||
      increment->hasNoSignedWrap() || increment->hasNoUnsignedWrap() || increment->getOperand(0) != offset ||
      !number(increment->getOperand(1), floating ? 16 : 8) ||
      !store(llvm::dyn_cast<llvm::StoreInst>(c[1]), increment, position, offsetAlign) ||
      !available || available->getPredicate() != llvm::CmpInst::ICMP_SLE || available->getOperand(0) != increment ||
      !number(available->getOperand(1), 0) || !checkBranch || !checkBranch->isConditional() ||
      checkBranch->getCondition() != available || checkBranch->getSuccessor(0) != registers || checkBranch->getSuccessor(1) != overflow ||
      !field(saveAddress, state, floating ? 2 : 1) ||
      !load(save, saveAddress, pointer, llvm::commonAlignment(llvm::Align(stateAlign), floating ? 16 : 8).value()) ||
      !bytes(address, save, offset, true) || !registerBranch || !registerBranch->isUnconditional() ||
      registerBranch->getSuccessor(0) != join || !field(overflowAddress, state, 0) ||
      !load(cursor, overflowAddress, pointer, stateAlign) ||
      !bytes(advance, cursor, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 8), true) ||
      !store(llvm::dyn_cast<llvm::StoreInst>(o[3]), advance, overflowAddress, stateAlign) ||
      !overflowBranch || !overflowBranch->isUnconditional() || overflowBranch->getSuccessor(0) != join ||
      phi->getIncomingValue(0) != address || phi->getIncomingValue(1) != cursor ||
      !load(result, phi, result->getType(), 8)) return false;
  llvm::SmallVector<llvm::Instruction *> consumed{position, offset, compare, branch};
  consumed.append(c); consumed.append(r); consumed.append(o); consumed.push_back(phi); consumed.push_back(result);
  if (!closed(consumed, result) || check->hasAddressTaken() || registers->hasAddressTaken() ||
      overflow->hasAddressTaken() || join->hasAddressTaken()) return false;
  auto *argument = new llvm::VAArgInst(state, result->getType(), "", position);
  result->replaceAllUsesWith(argument);
  result->eraseFromParent(); phi->eraseFromParent();
  branch->eraseFromParent(); compare->eraseFromParent();
  check->dropAllReferences(); registers->dropAllReferences(); overflow->dropAllReferences();
  check->eraseFromParent(); registers->eraseFromParent(); overflow->eraseFromParent();
  offset->eraseFromParent(); position->eraseFromParent();
  join->replaceAllUsesWith(head);
  head->splice(head->end(), join);
  join->eraseFromParent();
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

bool lifetimeOf(const llvm::Instruction *instruction, const llvm::AllocaInst *state, uint64_t size) {
  auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(instruction);
  return call && plain(call) && (call->getIntrinsicID() == llvm::Intrinsic::lifetime_start ||
      call->getIntrinsicID() == llvm::Intrinsic::lifetime_end) && call->arg_size() == 2 &&
      number(call->getArgOperand(0), size, 64) && call->getArgOperand(1) == state;
}

// AAPCS64 passes a by-value cursor through a closed caller-owned temporary.
// Prove that exact ABI copy, never the distinct source-level llvm.va_copy.
const llvm::MemCpyInst *ownedForwardCopy(const llvm::CallBase *call, unsigned index,
                                       NativeVarargs &information) {
  auto *temporary = llvm::dyn_cast<llvm::AllocaInst>(call->getArgOperand(index));
  if (!temporary || !stateType(temporary->getAllocatedType(), "aapcs64") ||
      temporary->getAlign().value() != 8 || !number(temporary->getArraySize(), 1) ||
      temporary->getAddressSpace() || temporary->isUsedWithInAlloca() || temporary->isSwiftError() ||
      !temporary->getParent()->isEntryBlock() || !plain(temporary)) return nullptr;
  const llvm::MemCpyInst *copy = nullptr;
  for (const auto *user : temporary->users()) {
    if (user == call) continue;
    if (auto *memory = llvm::dyn_cast<llvm::MemCpyInst>(user)) {
      if (copy || memory->getRawDest() != temporary || memory->isVolatile() || !plain(memory) ||
          !number(memory->getLength(), 32, 64) || memory->getDestAlign() != llvm::Align(8) ||
          memory->getSourceAlign() != llvm::Align(8) || memory->getNextNode() != call) return nullptr;
      auto *source = llvm::dyn_cast<llvm::AllocaInst>(memory->getRawSource());
      auto *incoming = llvm::dyn_cast<llvm::Argument>(memory->getRawSource());
      if (!(source && information.states.contains(source)) &&
          !(incoming && information.incomingArguments.contains(incoming))) return nullptr;
      copy = memory;
      continue;
    }
    auto *instruction = llvm::dyn_cast<llvm::Instruction>(user);
    if (!instruction || !lifetimeOf(instruction, temporary, 32)) return nullptr;
  }
  if (!copy) return nullptr;
  for (unsigned argument = 0; argument < call->arg_size(); ++argument)
    if (argument != index && call->getArgOperand(argument) == temporary) return nullptr;
  return copy;
}

// Eliminate only a closed compiler-generated spill of a proved incoming ABI
// value. One entry-block store dominates every load; no writes, escapes or
// observation of the storage are permitted. Debug anchors for this storage
// are consumed, but unrelated aggregate ABI debug evidence is preserved.
void normalizeIncomingSpills(llvm::Argument &argument, llvm::StringRef abi) {
  llvm::SmallVector<llvm::StoreInst *> writes;
  for (auto *user : argument.users())
    if (auto *write = llvm::dyn_cast<llvm::StoreInst>(user))
      if (write->getValueOperand() == &argument) writes.push_back(write);
  for (auto *write : writes) {
    auto *pointer = write->getPointerOperand();
    auto *initialField = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
    auto *slot = llvm::dyn_cast<llvm::AllocaInst>(initialField ? initialField->getPointerOperand() : pointer);
    const unsigned alignment = abi == "sysv-amd64" || abi == "aapcs64" ? 8 : 4;
    if (!slot || !slot->getParent()->isEntryBlock() || write->getParent() != slot->getParent() ||
        !slot->comesBefore(write) || !number(slot->getArraySize(), 1) || slot->getAddressSpace() ||
        slot->isUsedWithInAlloca() || slot->isSwiftError() || !plain(slot) ||
        slot->getAlign().value() != alignment || !store(write, &argument, pointer, alignment)) continue;
    if (abi == "aapcs32-vfp") {
      auto *array = llvm::dyn_cast<llvm::ArrayType>(argument.getType());
      if (!array || array->getNumElements() != 1 || !array->getElementType()->isIntegerTy(32) ||
          !stateType(slot->getAllocatedType(), abi) || !initialField || !field(initialField, slot, 0)) continue;
    } else if (initialField || slot->getAllocatedType() != argument.getType() ||
               !argument.getType()->isPointerTy()) continue;
    llvm::SmallVector<llvm::GetElementPtrInst *> fields;
    llvm::SmallVector<llvm::LoadInst *> reads;
    llvm::SmallVector<llvm::Instruction *> lifetimes;
    llvm::SmallVector<llvm::Value *> pointers{slot};
    bool proved = true;
    for (auto *user : slot->users()) {
      auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
      if (!gep) continue;
      if (abi != "aapcs32-vfp" || !field(gep, slot, 0)) { proved = false; break; }
      fields.push_back(gep); pointers.push_back(gep);
    }
    for (auto *address : pointers) {
      for (auto *user : address->users()) {
        if (user == write) continue;
        if (address == slot && llvm::is_contained(fields, user)) continue;
        auto *instruction = llvm::dyn_cast<llvm::Instruction>(user);
        if (instruction && lifetimeOf(instruction, slot, alignment)) {
          lifetimes.push_back(instruction); continue;
        }
        auto *read = llvm::dyn_cast<llvm::LoadInst>(user);
        if (!load(read, address, argument.getType(), alignment) ||
            (read->getParent() == write->getParent() && !write->comesBefore(read))) {
          proved = false; break;
        }
        reads.push_back(read);
      }
      if (!proved) break;
    }
    if (!proved) continue;
    llvm::SmallVector<llvm::DbgVariableIntrinsic *> debug;
    llvm::findDbgUsers(debug, slot);
    for (auto *field : fields) llvm::findDbgUsers(debug, field);
    llvm::DenseSet<llvm::DbgVariableIntrinsic *> uniqueDebug(debug.begin(), debug.end());
    for (auto *anchor : uniqueDebug) anchor->eraseFromParent();
    for (auto *read : reads) { read->replaceAllUsesWith(&argument); read->eraseFromParent(); }
    write->eraseFromParent();
    for (auto *lifetime : lifetimes) lifetime->eraseFromParent();
    for (auto *field : fields) field->eraseFromParent();
    slot->eraseFromParent();
  }
}

void recognizeForwarding(llvm::Module &module, llvm::StringRef abi, NativeVarargs &information) {
  llvm::DenseMap<std::pair<const llvm::CallBase *, unsigned>,
                 llvm::SmallVector<const llvm::Instruction *>> provenScaffolding;
  llvm::DenseMap<const llvm::CallBase *, llvm::DenseMap<unsigned, const llvm::Value *>> provenCopies;
  bool changed;
  do {
    changed = false;
  for (auto &function : module) {
    for (auto &instruction : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      auto *callee = call ? call->getCalledFunction() : nullptr;
      if (!callee || callee->isIntrinsic() || call->getCallingConv() || call->getNumOperandBundles() ||
          call->isMustTailCall() || !plain(call)) continue;
      for (unsigned index = 0; index < callee->arg_size() && index < call->arg_size(); ++index) {
        auto *value = call->getArgOperand(index);
        if (auto *incoming = llvm::dyn_cast<llvm::Argument>(value)) {
          if (abi != "aapcs64" && information.incomingArguments.contains(incoming)) {
            information.forwardedValues[call][index] = incoming;
            information.forwardedParameters[callee].insert(index);
          }
          continue;
        }
        const llvm::AllocaInst *state = nullptr;
        llvm::SmallVector<const llvm::Instruction *> scaffolding;
        if (abi == "sysv-amd64") {
          state = llvm::dyn_cast<llvm::AllocaInst>(value);
          if (!state || !information.states.contains(state)) continue;
        } else if (abi == "aapcs64") {
          auto *copy = ownedForwardCopy(call, index, information);
          if (!copy) continue;
          provenCopies[call][index] = copy->getRawSource();
          information.forwardedParameters[callee].insert(index);
          continue;
        } else {
          auto *loaded = llvm::dyn_cast<llvm::LoadInst>(value);
          if (!loaded || !loaded->hasOneUse() || loaded->getNextNode() != call) continue;
          auto *pointer = loaded->getPointerOperand();
          auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
          state = llvm::dyn_cast<llvm::AllocaInst>(gep ? gep->getPointerOperand() : pointer);
          if (!state || !information.states.contains(state)) continue;
          if (abi == "aapcs32-vfp") {
            auto *array = llvm::dyn_cast<llvm::ArrayType>(loaded->getType());
            if (!array || array->getNumElements() != 1 || !array->getElementType()->isIntegerTy(32)) continue;
            if (gep && (!field(gep, state, 0) || !gep->hasOneUse() || gep->getNextNode() != loaded)) continue;
          } else if (abi == "sysv-i386") {
            if (gep || !loaded->getType()->isPointerTy()) continue;
          } else continue;
          if (!load(loaded, pointer, loaded->getType(), 4)) continue;
          if (gep) scaffolding.push_back(gep);
          scaffolding.push_back(loaded);
        }
        information.forwardedArguments[call][index] = state;
        information.forwardedParameters[callee].insert(index);
        provenScaffolding[{call, index}] = std::move(scaffolding);
      }
    }
  }
  // A type is not a va_list merely because one call passed cursor-looking
  // bytes. Every use of the formal must be a direct call with matching proof.
  for (auto &entry : information.forwardedParameters) {
    auto *function = entry.first;
    llvm::SmallVector<unsigned> rejected;
    for (unsigned index : entry.second) {
      bool proved = !function->use_empty();
      for (const auto *user : function->users()) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(user);
        auto found = call ? information.forwardedArguments.find(call) : information.forwardedArguments.end();
        auto incoming = call ? information.forwardedValues.find(call) : information.forwardedValues.end();
        auto copy = call ? provenCopies.find(call) : provenCopies.end();
        if (!call || call->getCalledFunction() != function ||
            !((found != information.forwardedArguments.end() && found->second.contains(index)) ||
              (incoming != information.forwardedValues.end() && incoming->second.contains(index)) ||
              (copy != provenCopies.end() && copy->second.contains(index)))) {
          proved = false; break;
        }
      }
      if (!proved) rejected.push_back(index);
      else if (!function->isDeclaration()) {
        auto *argument = const_cast<llvm::Argument *>(function->getArg(index));
        if (information.incomingArguments.insert(argument).second) {
          normalizeIncomingSpills(*argument, abi);
          changed = true;
        }
      }
    }
    for (unsigned index : rejected) entry.second.erase(index);
  }
  } while (changed);
  for (auto &entry : information.forwardedArguments) {
    const auto *callee = entry.first->getCalledFunction();
    const auto &parameters = information.forwardedParameters.find(callee)->second;
    llvm::SmallVector<unsigned> rejected;
    for (auto argument : entry.second) {
      if (!parameters.contains(argument.first)) rejected.push_back(argument.first);
      else {
        const auto &scaffolding = provenScaffolding[{entry.first, argument.first}];
        for (const auto *instruction : scaffolding) {
          if (const auto *field = llvm::dyn_cast<llvm::GetElementPtrInst>(instruction)) {
            // The proved ARM32 first field has offset zero. Canonicalize the
            // native reference too, so inverse comparison sees the backend's
            // direct ABI-value load rather than a redundant field address.
            auto *mutableField = const_cast<llvm::GetElementPtrInst *>(field);
            mutableField->replaceAllUsesWith(mutableField->getPointerOperand());
            mutableField->eraseFromParent();
          } else information.forwardingScaffolding.insert(instruction);
        }
      }
    }
    for (unsigned index : rejected) entry.second.erase(index);
  }
  for (auto &entry : information.forwardedValues) {
    const auto &parameters = information.forwardedParameters.find(entry.first->getCalledFunction())->second;
    llvm::SmallVector<unsigned> rejected;
    for (auto argument : entry.second)
      if (!parameters.contains(argument.first)) rejected.push_back(argument.first);
    for (unsigned index : rejected) entry.second.erase(index);
  }
  for (auto &entry : provenCopies) {
    const auto &parameters = information.forwardedParameters.find(entry.first->getCalledFunction())->second;
    llvm::SmallVector<unsigned> rejected;
    for (auto argument : entry.second) {
      if (!parameters.contains(argument.first)) { rejected.push_back(argument.first); continue; }
      auto *call = const_cast<llvm::CallBase *>(entry.first);
      auto *temporary = llvm::cast<llvm::AllocaInst>(call->getArgOperand(argument.first));
      llvm::SmallVector<llvm::Instruction *> scaffold;
      for (auto *user : temporary->users())
        if (user != call) scaffold.push_back(llvm::cast<llvm::Instruction>(user));
      llvm::SmallVector<llvm::DbgVariableIntrinsic *> debug;
      llvm::findDbgUsers(debug, temporary);
      for (auto *anchor : debug) anchor->eraseFromParent();
      // The public va_forward operation owns these copy semantics. Fold the
      // same proved scaffold in both pristine and reconstructed native IR;
      // the native backend recreates the copy before the call. No source
      // va_copy, cursor read, or observable storage effect is discarded.
      call->setArgOperand(argument.first, const_cast<llvm::Value *>(argument.second));
      for (auto *instruction : scaffold) instruction->eraseFromParent();
      temporary->eraseFromParent();
      if (auto *state = llvm::dyn_cast<llvm::AllocaInst>(argument.second))
        information.forwardedArguments[call][argument.first] = state;
      else
        information.forwardedValues[call][argument.first] = llvm::cast<llvm::Argument>(argument.second);
    }
    for (unsigned index : rejected) entry.second.erase(index);
  }
}
}

llvm::Expected<NativeVarargs> normalizeNativeVarargs(llvm::Module &module, llvm::StringRef targetID) {
  auto *target = targets::find(targetID);
  if (!target) return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), "unknown variadic ABI target");
  bool word64 = target->abi == "sysv-amd64";
  NativeVarargs information;
  for (auto &function : module)
    for (auto &instruction : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
      if (!call || (call->getIntrinsicID() != llvm::Intrinsic::vastart &&
                    call->getIntrinsicID() != llvm::Intrinsic::vacopy &&
                    call->getIntrinsicID() != llvm::Intrinsic::vaend)) continue;
      for (auto &argument : call->args())
        if (auto *state = stateRoot(argument.get(), target->abi)) information.states.insert(state);
    }
  if (information.states.empty()) return information;
  // Preserve function signatures and unrelated debug anchors: aggregate ABI
  // recovery still consumes them after variadic normalization. Debug metadata
  // is not an executable effect and plain() already excludes debug locations.
  // Pinned optimized captures enable assignment tracking, which inserts
  // dbg.assign between a cursor store and its value load/branch. Remove only
  // assignments describing known cursor storage, not unrelated aggregate or
  // source-variable evidence needed by later producer passes.
  llvm::SmallVector<llvm::DbgAssignIntrinsic *> cursorAssignments;
  for (auto &function : module)
    for (auto &instruction : llvm::instructions(function)) {
      auto *assignment = llvm::dyn_cast<llvm::DbgAssignIntrinsic>(&instruction);
      if (!assignment || assignment->isKillAddress()) continue;
      const llvm::Value *address = assignment->getAddress();
      for (unsigned depth = 0; depth != 4; ++depth) {
        auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(address);
        if (!gep) break;
        address = gep->getPointerOperand();
      }
      auto *state = llvm::dyn_cast<llvm::AllocaInst>(address);
      if (state && information.states.contains(state)) cursorAssignments.push_back(assignment);
    }
  for (auto *assignment : cursorAssignments) assignment->eraseFromParent();
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
        bool normalized = result &&
            (word64 ? wide(result, information) :
             target->abi == "aapcs64" ? armWide(result, information) :
             target->abi == "aapcs32-vfp" && (result->getType()->isIntegerTy(64) || result->getType()->isDoubleTy())
                 ? armAligned(result, information) : narrow(result, information));
        if (normalized) {
          ++information.scalarExtractions;
          changed = true;
          break;
        }
      }
      if (changed) break;
    }
  } while (changed);
  recognizeForwarding(module, target->abi, information);
  llvm::SmallVector<llvm::Function *> unusedTemplates;
  for (auto &function : module)
    if (function.use_empty() && (function.getIntrinsicID() == llvm::Intrinsic::ptrmask ||
        function.getIntrinsicID() == llvm::Intrinsic::memcpy ||
        function.getIntrinsicID() == llvm::Intrinsic::lifetime_start ||
        function.getIntrinsicID() == llvm::Intrinsic::lifetime_end))
      unusedTemplates.push_back(&function);
  for (auto *function : unusedTemplates) function->eraseFromParent();
  // An implicit AAPCS64 temporary can make Clang declare lifetime.end before
  // va_end; other ABIs first encounter these in the opposite order. Removing
  // that proved temporary must not leave a declaration-order-only inverse
  // mismatch. Keep every signature/attribute and every call unchanged.
  llvm::SmallVector<llvm::Function *> cursorDeclarations;
  for (auto &function : module) {
    auto id = function.getIntrinsicID();
    if (function.isDeclaration() && (id == llvm::Intrinsic::vastart || id == llvm::Intrinsic::vacopy ||
        id == llvm::Intrinsic::vaend || id == llvm::Intrinsic::lifetime_start || id == llvm::Intrinsic::lifetime_end))
      cursorDeclarations.push_back(&function);
  }
  llvm::sort(cursorDeclarations, [](auto *left, auto *right) { return left->getName() < right->getName(); });
  for (auto *function : cursorDeclarations)
    module.getFunctionList().splice(module.end(), module.getFunctionList(), function->getIterator());
  return information;
}
}
