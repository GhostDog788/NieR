#pragma once

// Each object library implements one registered device ABI. Word size is a
// property of that ABI, never a backend selector.
#if !defined(SELA_NATIVE_NAMESPACE) || !defined(SELA_NATIVE_TARGET_ID)
#error "Native sources require their registered target and namespace"
#endif

#include "NativeTargets.h"
#include "sela/Targets.h"
#include <string_view>
namespace sela::detail::SELA_NATIVE_NAMESPACE {
inline constexpr bool word64 = SELA_NATIVE_WORD_BITS == 64;
inline constexpr llvm::StringRef TargetID = SELA_NATIVE_TARGET_ID;
inline const targets::TargetInfo &targetInfo() { return *targets::find(TargetID); }
inline const llvm::StringRef TargetTriple = targetInfo().triple;
inline const llvm::StringRef TargetLayout = targetInfo().layout;
// These traits belong to the isolated ABI implementation, not the public IR.
inline constexpr bool x64 = std::string_view(SELA_NATIVE_ABI) == "sysv-amd64";
inline constexpr bool aapcs64 = std::string_view(SELA_NATIVE_ABI) == "aapcs64";

const NativeTargetBackend &backend();
llvm::Expected<std::unique_ptr<llvm::Module>> lowerModule(
    mlir::ModuleOp, llvm::LLVMContext &, NativeABIInverseHints *);
llvm::Expected<NativeABISignature> classifyNativeABI(
    llvm::FunctionType *, llvm::ArrayRef<llvm::StructType *>);
llvm::Expected<NativeABISignature> classifyNativeLayoutABI(
    llvm::FunctionType *, llvm::ArrayRef<NativeABIRecordLayout>);
llvm::Type *nativeVaListType(llvm::LLVMContext &);
llvm::Type *nativeVaListArgumentType(llvm::LLVMContext &);
llvm::Value *nativeVaForward(llvm::IRBuilderBase &, llvm::Value *, bool incoming = false);
llvm::Value *nativeVaArg(llvm::IRBuilderBase &, llvm::Value *, llvm::Type *);
bool nativeModuleFlag(llvm::StringRef, uint64_t);
} // namespace sela::detail::SELA_NATIVE_NAMESPACE
