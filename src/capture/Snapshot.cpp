#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>

namespace {
struct Snapshot : llvm::PassInfoMixin<Snapshot> {
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &) {
    const char *path = std::getenv("AOT_CAPTURE_PATH");
    if (!path || !*path)
      llvm::report_fatal_error("aot capture plugin requires AOT_CAPTURE_PATH");
    std::error_code error;
    llvm::raw_fd_ostream stream(path, error, llvm::sys::fs::OF_None);
    if (error)
      llvm::report_fatal_error(llvm::Twine("cannot write private AOT capture: ") + error.message());
    llvm::WriteBitcodeToFile(module, stream);
    stream.flush();
    if (stream.has_error())
      llvm::report_fatal_error("failed writing private AOT capture");
    return llvm::PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};
}

extern "C" LLVM_ATTRIBUTE_WEAK llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "aot-capture", "0.1", [](llvm::PassBuilder &builder) {
    builder.registerPipelineStartEPCallback([](llvm::ModulePassManager &passes, llvm::OptimizationLevel) {
      passes.addPass(Snapshot());
    });
  }};
}
