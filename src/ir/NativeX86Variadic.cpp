#include "NativeTargetConfig.h"

namespace sela::detail::SELA_NATIVE_NAMESPACE {
bool nativeModuleFlag(llvm::StringRef name, uint64_t value) {
  return !x64 && name == "NumRegisterParameters" && value == 0;
}

llvm::Type *nativeVaListType(llvm::LLVMContext &context) {
  auto *pointer = llvm::PointerType::get(context, 0);
  if constexpr (!x64) return pointer;
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *record = llvm::StructType::create(context, {i32, i32, pointer, pointer}, "v0");
  return llvm::ArrayType::get(record, 1);
}

llvm::Value *nativeVaForward(llvm::IRBuilderBase &builder, llvm::Value *state, bool incoming) {
  if (incoming) return state;
  if constexpr (x64) return state;
  return builder.CreateAlignedLoad(llvm::PointerType::get(builder.getContext(), 0), state, llvm::Align(4));
}

llvm::Type *nativeVaListArgumentType(llvm::LLVMContext &context) {
  return llvm::PointerType::get(context, 0);
}

llvm::Value *nativeVaArg(llvm::IRBuilderBase &builder, llvm::Value *state, llvm::Type *type) {
  return builder.CreateVAArg(state, type);
}
} // namespace sela::detail::SELA_NATIVE_NAMESPACE
