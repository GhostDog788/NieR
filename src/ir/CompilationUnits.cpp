#include "sela/IR/CompilationUnits.h"
#include "Internal.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <set>

namespace sela {
llvm::Error lowerCompilationUnit(llvm::ArrayRef<llvm::StringRef> fragments,
                                llvm::StringRef profile,
                                llvm::StringRef llvmIROutput) {
  auto fail = [](llvm::StringRef text) {
    return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), text);
  };
  if (fragments.empty() || fragments.size() > 512 ||
      (profile != "x86_64" && profile != "i686"))
    return fail("invalid Sela compilation-unit contract");
  mlir::MLIRContext context;
  llvm::LLVMContext nativeContext;
  std::unique_ptr<llvm::Module> result;
  std::set<std::string> definitions;
  for (auto path : fragments) {
    auto source = readModule(path, context, {profile});
    if (!source) return source.takeError();
    auto lowered = detail::lowerModule(**source, nativeContext, profile == "x86_64");
    if (!lowered) return lowered.takeError();
    // A grouped unit is semantic reconstruction, not a native-link symbol
    // selection mechanism. Never allow weak/common resolution or local-name
    // renaming to conceal duplicated program definitions across fragments.
    if (fragments.size() > 1) {
      for (const auto &global : (*lowered)->global_values()) {
        if (global.isDeclaration()) continue;
        if (global.hasLocalLinkage() || !global.hasExternalLinkage() ||
            !definitions.insert(global.getName().str()).second)
          return fail("grouped Sela units require unique external definition identities");
      }
    }
    if (!result) result = std::move(*lowered);
    else if (llvm::Linker::linkModules(*result, std::move(*lowered)))
      return fail("cannot reconstruct declared Sela translation unit");
  }
  if (llvm::verifyModule(*result)) return fail("reconstructed Sela translation unit is invalid");
  std::error_code error;
  llvm::raw_fd_ostream output(llvmIROutput, error, llvm::sys::fs::OF_Text);
  if (error) return llvm::errorCodeToError(error);
  result->print(output, nullptr);
  output.flush();
  if (output.has_error()) return fail("cannot write reconstructed native translation unit");
  return llvm::Error::success();
}
}
