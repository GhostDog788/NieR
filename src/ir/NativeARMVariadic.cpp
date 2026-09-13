#include "NativeTargetConfig.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"

namespace sela::detail::SELA_NATIVE_NAMESPACE {
bool nativeModuleFlag(llvm::StringRef name, uint64_t value) {
  return !aapcs64 && name == "min_enum_size" && value == 4;
}

llvm::Type *nativeVaListType(llvm::LLVMContext &context) {
  auto *pointer = llvm::PointerType::get(context, 0);
  llvm::SmallVector<llvm::Type *> fields{pointer};
  if constexpr (aapcs64) {
    auto *i32 = llvm::Type::getInt32Ty(context);
    fields = {pointer, pointer, pointer, i32, i32};
  }
  if (auto *record = llvm::StructType::getTypeByName(context, "v0"))
    if (!record->isOpaque() && record->elements() == llvm::ArrayRef<llvm::Type *>(fields)) return record;
  return llvm::StructType::create(context, fields, "v0");
}

llvm::Type *nativeVaListArgumentType(llvm::LLVMContext &context) {
  if constexpr (aapcs64) return llvm::PointerType::get(context, 0);
  return llvm::ArrayType::get(llvm::Type::getInt32Ty(context), 1);
}

llvm::Value *nativeVaForward(llvm::IRBuilderBase &builder, llvm::Value *state, bool incoming) {
  if constexpr (aapcs64) {
    // AAPCS64 transfers va_list by value through an owned indirect copy.
    // This is native ABI lowering, distinct from a source-level va_copy.
    // Allocate once in the entry block, including when forwarding in a loop.
    auto *function = builder.GetInsertBlock()->getParent();
    llvm::IRBuilder<llvm::NoFolder> allocationBuilder(&function->getEntryBlock(), function->getEntryBlock().begin());
    auto *copy = allocationBuilder.CreateAlloca(nativeVaListType(builder.getContext()));
    copy->setAlignment(llvm::Align(8));
    builder.CreateMemCpy(copy, llvm::Align(8), state, llvm::Align(8), builder.getInt64(32));
    return copy;
  }
  if (incoming) return state;
  return builder.CreateAlignedLoad(nativeVaListArgumentType(builder.getContext()), state, llvm::Align(4));
}

llvm::Value *nativeVaArg(llvm::IRBuilderBase &builder, llvm::Value *state, llvm::Type *type) {
  auto &context = builder.getContext();
  auto *pointer = llvm::PointerType::get(context, 0);
  auto &layout = builder.GetInsertBlock()->getModule()->getDataLayout();
  if constexpr (!aapcs64) {
    auto *cursor = builder.CreateAlignedLoad(pointer, state, llvm::Align(4));
    llvm::Value *aligned = cursor;
    unsigned alignment = std::clamp<unsigned>(layout.getABITypeAlign(type).value(), 4, 8);
    if (alignment > 4) {
      auto *next = builder.CreateInBoundsGEP(builder.getInt8Ty(), cursor, builder.getInt32(alignment - 1));
      aligned = builder.CreateIntrinsic(llvm::Intrinsic::ptrmask, {pointer, builder.getInt32Ty()},
          {next, builder.getInt32(-int32_t(alignment))});
    }
    auto size = llvm::alignTo(uint64_t(layout.getTypeAllocSize(type)), uint64_t(4));
    auto *next = builder.CreateInBoundsGEP(builder.getInt8Ty(), aligned, builder.getInt32(size));
    builder.CreateAlignedStore(next, state, llvm::Align(4));
    return builder.CreateAlignedLoad(type, aligned, llvm::Align(alignment));
  } else {
    // Linux AAPCS64 requires this explicit transition. LLVM 18's va_arg
    // instruction lowering for AArch64 implements Darwin, not Linux AAPCS64.
    // Match pinned Clang's register/overflow graph and memory effects.
    bool floating = type->isDoubleTy();
    auto *record = nativeVaListType(context);
    unsigned offsetField = floating ? 4 : 3;
    auto offsetAlignment = llvm::Align(floating ? 4 : 8);
    auto *offsetAddress = builder.CreateStructGEP(record, state, offsetField);
    auto *offset = builder.CreateAlignedLoad(builder.getInt32Ty(), offsetAddress, offsetAlignment);
    auto *origin = builder.GetInsertBlock();
    auto *function = origin->getParent();
    auto *following = origin->getNextNode();
    auto block = [&](const char *name) { return llvm::BasicBlock::Create(context, name, function, following); };
    auto *maybeRegisters = block("va.maybe_registers");
    auto *inRegisters = block("va.registers");
    auto *onStack = block("va.stack");
    auto *continuation = block("va.continue");
    builder.CreateCondBr(builder.CreateICmpSGE(offset, builder.getInt32(0)), onStack, maybeRegisters);

    builder.SetInsertPoint(maybeRegisters);
    auto *nextOffset = builder.CreateAdd(offset, builder.getInt32(floating ? 16 : 8));
    builder.CreateAlignedStore(nextOffset, offsetAddress, offsetAlignment);
    builder.CreateCondBr(builder.CreateICmpSLE(nextOffset, builder.getInt32(0)), inRegisters, onStack);

    builder.SetInsertPoint(inRegisters);
    auto *topAddress = builder.CreateStructGEP(record, state, floating ? 2 : 1);
    auto *top = builder.CreateAlignedLoad(pointer, topAddress, llvm::Align(8));
    auto *registerAddress = builder.CreateInBoundsGEP(builder.getInt8Ty(), top, offset);
    builder.CreateBr(continuation);

    builder.SetInsertPoint(onStack);
    auto *stackAddress = builder.CreateStructGEP(record, state, 0);
    auto *stack = builder.CreateAlignedLoad(pointer, stackAddress, llvm::Align(8));
    auto *nextStack = builder.CreateInBoundsGEP(builder.getInt8Ty(), stack, builder.getInt64(8));
    builder.CreateAlignedStore(nextStack, stackAddress, llvm::Align(8));
    builder.CreateBr(continuation);

    builder.SetInsertPoint(continuation);
    auto *address = builder.CreatePHI(pointer, 2);
    address->addIncoming(registerAddress, inRegisters);
    address->addIncoming(stack, onStack);
    return builder.CreateAlignedLoad(type, address, llvm::Align(8));
  }
}
} // namespace sela::detail::SELA_NATIVE_NAMESPACE
