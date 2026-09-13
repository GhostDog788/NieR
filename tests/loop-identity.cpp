#include "nier/Producer/LLVM.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/IR/Builders.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace {
const char *body = R"llvm(
define i32 @walk_loops() {
entry:
  br label %header
header:
  %index = phi i32 [ 0, %entry ], [ %a, %odd ], [ %b, %even ]
  %bit = and i32 %index, 1
  %isodd = icmp eq i32 %bit, 1
  br i1 %isodd, label %odd, label %even
odd:
  %a = add i32 %index, 1
  %again_a = icmp ult i32 %a, 5
  br i1 %again_a, label %header, label %done_a, !llvm.loop !0
even:
  %b = add i32 %index, 1
  %again_b = icmp ult i32 %b, 5
  br i1 %again_b, label %header, label %done_b, !llvm.loop SECOND
done_a:
  ret i32 %a
done_b:
  ret i32 %b
}
!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.mustprogress"}
!2 = distinct !{!2, !1}
)llvm";

std::string native(bool wide, bool shared) {
  std::string result = std::string("target triple = \"") +
      (wide ? "x86_64-unknown-linux-gnu" : "i686-unknown-linux-gnu") + "\"\n" +
      "target datalayout = \"" + (wide ?
      "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128" :
      "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128") + "\"\n" + body;
  result.replace(result.find("SECOND"), 6, shared ? "!0" : "!2");
  return result;
}
bool write(const std::string &path, const std::string &text) {
  std::error_code error;
  llvm::raw_fd_ostream output(path, error, llvm::sys::fs::OF_None);
  if (error) return false;
  output << text;
  output.flush();
  return !output.has_error();
}
bool rejected(llvm::Error error, llvm::StringRef expected, llvm::StringRef label) {
  if (!error) {
    llvm::errs() << label << ": unexpectedly accepted\n";
    return false;
  }
  auto message = llvm::toString(std::move(error));
  if (!llvm::StringRef(message).contains(expected)) {
    llvm::errs() << label << ": unexpected rejection: " << message << '\n';
    return false;
  }
  return true;
}
std::vector<mlir::Operation *> loopBranches(mlir::ModuleOp module) {
  std::vector<mlir::Operation *> result;
  module.walk([&](mlir::Operation *operation) {
    if (operation->hasAttr("loop")) result.push_back(operation);
  });
  return result;
}
bool checkNativeIdentity(const std::string &path, bool shared) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(path, diagnostic, context);
  if (!module) return false;
  auto *function = module->getFunction("walk_loops");
  if (!function) return false;
  std::vector<llvm::MDNode *> loops;
  for (auto &instruction : llvm::instructions(*function))
    if (auto *loop = instruction.getMetadata(llvm::LLVMContext::MD_loop)) loops.push_back(loop);
  if (loops.size() != 2 || ((loops[0] == loops[1]) != shared)) return false;
  for (auto *loop : loops) {
    if (!loop->isDistinct() || loop->getNumOperands() != 2 || loop->getOperand(0).get() != loop)
      return false;
    auto *option = llvm::dyn_cast<llvm::MDNode>(loop->getOperand(1));
    auto *name = option && option->getNumOperands() == 1
        ? llvm::dyn_cast<llvm::MDString>(option->getOperand(0)) : nullptr;
    if (!name || name->getString() != "llvm.loop.mustprogress") return false;
  }
  return true;
}
bool directNegative(const std::string &artifact, llvm::StringRef label,
                    llvm::StringRef expected,
                    const std::function<void(std::vector<mlir::Operation *> &, mlir::Builder &)> &change) {
  mlir::MLIRContext context;
  auto module = nier::readModule(artifact, context, nier::supportedNativeTargets());
  if (!module) { llvm::logAllUnhandledErrors(module.takeError(), llvm::errs()); return false; }
  auto branches = loopBranches(**module);
  if (branches.size() != 2) return false;
  mlir::Builder builder(&context);
  change(branches, builder);
  return rejected(nier::verifyModule(**module, nier::supportedNativeTargets()), expected, label);
}
}

int main(int argc, char **argv) {
  if (argc == 4) {
    if (auto error = nier::mergeProfiles(argv[1], argv[2], argv[3])) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "real loop capture: ");
      return 1;
    }
    llvm::outs() << "Real native capture merge and both strict inverses passed.\n";
    return 0;
  }
  if (argc != 1) return 2;
  llvm::SmallString<256> temporary;
  if (auto error = llvm::sys::fs::createUniqueDirectory("nier-loop-identity", temporary)) {
    llvm::errs() << error.message() << '\n'; return 1;
  }
  const std::string directory = temporary.str().str();
  const auto left = directory + "/left.ll", right = directory + "/right.ll";
  const auto artifact = directory + "/module.nierbc";
  bool passed = true;
  for (bool shared : {false, true}) {
    if (!write(left, native(true, shared)) || !write(right, native(false, shared))) { passed = false; break; }
    if (auto error = nier::mergeProfiles(left, right, artifact)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "loop producer: ");
      passed = false; break;
    }
    mlir::MLIRContext context;
    auto module = nier::readModule(artifact, context, nier::supportedNativeTargets());
    if (!module) { llvm::logAllUnhandledErrors(module.takeError(), llvm::errs()); passed = false; break; }
    auto branches = loopBranches(**module);
    if (branches.size() != 2 ||
        ((branches[0]->getAttr("loop_id") == branches[1]->getAttr("loop_id")) != shared)) {
      llvm::errs() << "Shared NieR loop identities were duplicated/coalesced\n";
      passed = false; break;
    }
    for (auto target : {"x86_64", "i686"}) {
      const auto output = directory + "/" + target + ".ll";
      if (auto error = nier::lowerArtifact(artifact, target, output)) {
        llvm::logAllUnhandledErrors(std::move(error), llvm::errs()); passed = false;
      } else if (!checkNativeIdentity(output, shared)) {
        llvm::errs() << "Native loop identity/options were not reconstructed\n"; passed = false;
      }
    }
  }
  // The final positive artifact has one shared identity on two latch branches.
  passed &= directNegative(artifact, "conflicting options", "conflicting options for one native loop identity",
      [](auto &branches, auto &builder) {
        branches[1]->setAttr("loop", builder.getArrayAttr({builder.getStringAttr("llvm.loop.unroll.disable")}));
      });
  passed &= directNegative(artifact, "missing identity", "opaque loop identity",
      [](auto &branches, auto &) { branches[0]->removeAttr("loop_id"); });
  passed &= directNegative(artifact, "nonopaque identity", "opaque loop identity",
      [](auto &branches, auto &builder) { branches[0]->setAttr("loop_id", builder.getStringAttr("private/source.c")); });
  passed &= directNegative(artifact, "wrong identity type", "opaque loop identity",
      [](auto &branches, auto &builder) { branches[0]->setAttr("loop_id", builder.getI32IntegerAttr(0)); });
  passed &= directNegative(artifact, "orphan identity", "identity requires loop options",
      [](auto &branches, auto &) { branches[0]->removeAttr("loop"); });
  passed &= directNegative(artifact, "unknown loop option", "unknown loop semantic option",
      [](auto &branches, auto &builder) {
        branches[0]->setAttr("loop", builder.getArrayAttr({builder.getStringAttr("llvm.loop.unknown")}));
      });
  for (bool sharedLeft : {false, true}) {
    passed &= write(left, native(true, sharedLeft)) && write(right, native(false, !sharedLeft));
    const auto rejectedArtifact = directory + (sharedLeft ? "/split.nierbc" : "/coalesced.nierbc");
    passed &= rejected(nier::mergeProfiles(left, right, rejectedArtifact),
        "native loop identity correspondence differs between profiles", "cross-profile loop identity");
    passed &= !std::filesystem::exists(rejectedArtifact);
  }
  if (!passed) {
    llvm::errs() << "Loop identity evidence retained at " << directory << '\n'; return 1;
  }
  // Remove only the individually known files created by this test.
  for (const auto &path : {left, right, artifact, directory + "/x86_64.ll", directory + "/i686.ll"})
    llvm::sys::fs::remove(path);
  llvm::sys::fs::remove(temporary);
  llvm::outs() << "Shared/distinct loop IDs, exact inverses and malformed identities passed.\n";
  return 0;
}
