#include "NativeTargets.h"
#include "nier/IR/Compiler.h"

#if !defined(NIER_HAS_X86_64) || !defined(NIER_HAS_I686)
#error "The core library must declare its statically linked native backends"
#endif
#if !NIER_HAS_X86_64 && !NIER_HAS_I686
#error "At least one native backend must be linked"
#endif

namespace nier {
llvm::ArrayRef<llvm::StringRef> supportedNativeTargets() {
  static const llvm::StringRef targets[] = {
#if NIER_HAS_X86_64
      "x86_64",
#endif
#if NIER_HAS_I686
      "i686",
#endif
  };
  return targets;
}

const detail::NativeTargetBackend *detail::findNativeTarget(llvm::StringRef id) {
#if NIER_HAS_X86_64
  if (id == "x86_64") return &native64::backend();
#endif
#if NIER_HAS_I686
  if (id == "i686") return &native32::backend();
#endif
  return nullptr;
}

llvm::Expected<std::unique_ptr<llvm::Module>> detail::lowerModule(
    mlir::ModuleOp source, llvm::LLVMContext &context, bool x64,
    NativeABIInverseHints *inverseHints) {
  auto *target = findNativeTarget(x64 ? "x86_64" : "i686");
  if (!target)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "requested native target is unavailable in this NieR library");
  if (auto error = verifyModuleStructure(source)) return std::move(error);
  return target->lower(source, context, inverseHints);
}
} // namespace nier
