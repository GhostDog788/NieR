#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <cstdlib>

namespace {
struct Snapshot : llvm::PassInfoMixin<Snapshot> {
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &) {
    const char *path = std::getenv("NIER_CAPTURE_PATH");
    if (!path || !*path)
      llvm::report_fatal_error("nier capture plugin requires NIER_CAPTURE_PATH");
    std::error_code error;
    llvm::raw_fd_ostream stream(path, error, llvm::sys::fs::OF_None);
    if (error)
      llvm::report_fatal_error(llvm::Twine("cannot write private Nier capture: ") + error.message());
    llvm::WriteBitcodeToFile(module, stream);
    stream.flush();
    if (stream.has_error())
      llvm::report_fatal_error("failed writing private Nier capture");
    const char *record = std::getenv("NIER_CAPTURE_RECORD");
    const char *metadata = std::getenv("NIER_BUILD_METADATA");
    if (record && *record && metadata && *metadata) {
      // Native build objects may be moved or archived before the publication
      // graph is selected. Carry an immutable private provenance reference in
      // a non-executable section. Crucially, this is added AFTER the pristine
      // LLVM snapshot and can never enter a published Nier payload.
      auto input = llvm::MemoryBuffer::getFile(record);
      auto captured = llvm::MemoryBuffer::getFile(path);
      if (!input || !captured)
        llvm::report_fatal_error("cannot finalize the private capture journal");
      auto parsed = llvm::json::parse((*input)->getBuffer());
      auto *object = parsed ? parsed->getAsObject() : nullptr;
      if (!object) llvm::report_fatal_error("invalid private capture journal");
      auto bytes = (*captured)->getBuffer();
      auto hash = llvm::SHA256::hash(llvm::ArrayRef(
          reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
      (*object)["capture_sha256"] = llvm::toHex(hash, true);
      llvm::raw_fd_ostream journal(record, error, llvm::sys::fs::OF_Text);
      if (error) llvm::report_fatal_error("cannot update private capture journal");
      journal << llvm::formatv("{0:2}", *parsed) << '\n';
      journal.flush();
      if (journal.has_error()) llvm::report_fatal_error("private capture journal write failed");
      auto *value = llvm::ConstantDataArray::getString(module.getContext(), record, true);
      auto *marker = new llvm::GlobalVariable(module, value->getType(), true,
          llvm::GlobalValue::PrivateLinkage, value, "__nier_private_capture");
      marker->setSection(".nier.capture");
      marker->setAlignment(llvm::Align(1));
      llvm::appendToCompilerUsed(module, {marker});
      return llvm::PreservedAnalyses::none();
    }
    return llvm::PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};
}

extern "C" LLVM_ATTRIBUTE_WEAK llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "nier-capture", "pre-alpha", [](llvm::PassBuilder &builder) {
    builder.registerPipelineStartEPCallback([](llvm::ModulePassManager &passes, llvm::OptimizationLevel) {
      passes.addPass(Snapshot());
    });
  }};
}
