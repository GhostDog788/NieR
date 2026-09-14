#include "sela/Producer/LLVM.h"
#include "sela/IR/Domains.h"
#include "sela/Targets.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <map>

namespace {
const char *cfg = R"llvm(
define hidden i32 @choose(i32 %input) {
entry:
  %positive = icmp sgt i32 %input, 0
  br i1 %positive, label %yes, label %no
yes:
  %left = add nsw i32 %input, 42
  br label %join
no:
  %right = sub nsw i32 7, %input
  br label %join
join:
  %value = phi i32 [ %right, %no ], [ %left, %yes ]
  ret i32 %value
}

define i32 @count() {
entry:
  br label %loop
loop:
  %index = phi i32 [ 0, %entry ], [ %next, %loop ]
  %next = add i32 %index, 1
  %again = icmp slt i32 %next, 5
  br i1 %again, label %loop, label %done, !llvm.loop !0
done:
  ret i32 %next
}

define double @floating(double %input) {
entry:
  %sum = fadd double %input, 2.500000e+00
  %positive = fcmp ogt double %sum, 0.000000e+00
  br i1 %positive, label %yes, label %no
yes:
  %quotient = fdiv double %sum, 2.000000e+00
  ret double %quotient
no:
  ret double 0.000000e+00
}

define i32 @array_storage() {
entry:
  %array = alloca [4 x i32], align 16
  %element = getelementptr inbounds [4 x i32], ptr %array, i32 0, i32 2
  store i32 42, ptr %element, align 4
  %value = load i32, ptr %element, align 4
  ret i32 %value
}

define weak protected i32 @weak_hook() {
entry:
  ret i32 17
}

define i32 @selection(i32 %input) {
entry:
  switch i32 %input, label %other [ i32 1, label %one
                                  i32 2, label %two ]
one:
  ret i32 10
two:
  ret i32 20
other:
  %positive = icmp sgt i32 %input, 0
  %selected = select i1 %positive, i32 30, i32 40
  ret i32 %selected
}

define available_externally double @negation(double %value) {
entry:
  %negative = fneg double %value
  ret double %negative
}

define i32 @reverse_bytes(i32 %input) {
  %a = lshr i32 %input, 24
  %b0 = and i32 %input, 16711680
  %b = lshr i32 %b0, 8
  %c0 = and i32 %input, 65280
  %c = shl i32 %c0, 8
  %d = shl i32 %input, 24
  %ab = or i32 %a, %b
  %cd = or i32 %c, %d
  %result = or i32 %ab, %cd
  ret i32 %result
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.mustprogress"}
!llvm.module.flags = !{!2}
!2 = !{i32 7, !"frame-pointer", i32 2}
)llvm";

bool write(llvm::StringRef path, llvm::StringRef contents) {
  std::error_code ec;
  llvm::raw_fd_ostream stream(path, ec, llvm::sys::fs::OF_None);
  if (ec) return false;
  stream << contents;
  stream.flush();
  return !stream.has_error();
}
} // namespace

int main() {
  llvm::SmallString<256> directory;
  if (auto error = llvm::sys::fs::createUniqueDirectory("sela-producer-tests", directory)) {
    llvm::errs() << error.message() << '\n'; return 1;
  }
  std::string base = directory.str().str();
  std::string left = base + "/x64.ll", right = base + "/i686.ll";
  std::string artifact = base + "/module.mlirbc";
  bool passed = write(left,
      std::string("target triple = \"x86_64-unknown-linux-gnu\"\n") +
      "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n" + cfg +
      "define i64 @wordcast(i64 %input) { %low = trunc i64 %input to i32\n %wide = zext i32 %low to i64\n ret i64 %wide }\n"
      "define i64 @signed_domain(i64 %input) { %value = sub i64 %input, 1\n ret i64 %value }\n") &&
      write(right,
      std::string("target triple = \"i686-unknown-linux-gnu\"\n") +
      "target datalayout = \"e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128\"\n" + cfg +
      "define i32 @wordcast(i32 %input) { ret i32 %input }\n"
      "define i32 @signed_domain(i32 %input) { %value = sub nsw i32 %input, 1\n ret i32 %value }\n");
  if (passed) {
    sela::ArtifactSummary summary;
    if (auto error = sela::mergeProfiles({{"x86_64", left}, {"i686", right}}, artifact, &summary)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "producer: ");
      passed = false;
    } else if (summary.functions != 10) {
      llvm::errs() << "incorrect merged CFG inventory\n"; passed = false;
    }
  }
  std::map<std::string, std::string> captures;
  std::map<std::string, std::string> captureTexts;
  for (const auto &target : sela::targets::all()) {
    auto path = base + "/" + target.id.str() + "-generic.ll";
    captures[target.id.str()] = path;
    std::string text = "target triple = \"" + target.triple.str() + "\"\ntarget datalayout = \"" + target.layout.str() + "\"\n" + cfg;
    text += target.wordBits == 64
        ? "define i64 @wordcast(i64 %input) { %low = trunc i64 %input to i32\n %wide = zext i32 %low to i64\n ret i64 %wide }\n"
        : "define i32 @wordcast(i32 %input) { ret i32 %input }\n";
    text += "define i32 @character(i8 %input) { %value = " +
        std::string(target.plainCharSigned ? "sext" : "zext") + " i8 %input to i32\n ret i32 %value }\n";
    // Opaque source spellings have no cross-target identity. Nested uses must
    // establish and consistently retain their structural correspondence.
    auto outer = "%private_outer_" + target.id.str(), inner = "%private_inner_" + target.id.str();
    text += inner + " = type { i32 }\n" + outer + " = type { i8, " + inner + " }\n";
    text += "define i32 @nested_storage() { %storage = alloca " + outer + ", align 8\n"
        " %field = getelementptr inbounds " + outer + ", ptr %storage, i32 0, i32 1, i32 0\n"
        " store i32 17, ptr %field, align 4\n %result = load i32, ptr %field, align 4\n ret i32 %result }\n";
    auto memoryName = "llvm.memcpy.p0.p0.i" + std::to_string(target.wordBits);
    auto lengthType = "i" + std::to_string(target.wordBits);
    std::string declaration = "declare void @" + memoryName + "(ptr, ptr, " + lengthType + ", i1 immarg)\n";
    // An ABI transfer can cause the same still-used intrinsic declaration to
    // appear earlier without changing the definition graph or native code.
    if (!target.plainCharSigned) text += declaration;
    text += "define internal i32 @private_copy(ptr %out, ptr %in) { call void @" + memoryName +
        "(ptr %out, ptr %in, " + lengthType + " 8, i1 false)\n ret i32 23 }\n"
        "define i32 @use_private_copy(ptr %out, ptr %in) { %value = call i32 @private_copy(ptr %out, ptr %in)\n"
        " ret i32 %value }\n";
    if (target.plainCharSigned) text += declaration;
    captureTexts[target.id.str()] = text;
    passed &= write(path, text);
  }
  // Exercise arbitrary observation order, more than two targets, and
  // same-width ABI families. No ordering encodes a public word-size domain.
  const llvm::SmallVector<llvm::SmallVector<llvm::StringRef>> orders = {
      {"x86_64", "i686", "armv7", "aarch64"}, {"aarch64", "armv7", "i686", "x86_64"},
      {"i686", "armv7"}, {"x86_64", "aarch64"}, {"armv7"}};
  for (const auto &order : orders) {
    llvm::SmallVector<sela::CaptureObservation> observations;
    for (auto target : order) observations.push_back({target.str(), captures.at(target.str())});
    sela::ArtifactSummary summary;
    if (auto error = sela::mergeProfiles(observations, artifact, &summary)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "generic producer: "); passed = false;
    } else {
      mlir::MLIRContext context;
      auto module = sela::readModuleStructure(artifact, context);
      if (!module) { llvm::consumeError(module.takeError()); passed = false; }
      else {
        auto declared = sela::ir::declaredTargets(**module);
        if (!declared) { llvm::consumeError(declared.takeError()); passed = false; }
        else if (llvm::ArrayRef<llvm::StringRef>(*declared) != llvm::ArrayRef<llvm::StringRef>(order) || summary.functions != 14) {
          llvm::errs() << "incorrect generic declaration/definition inventory: " << summary.functions << '\n';
          passed = false;
        }
      }
    }
  }
  for (auto observations : {
        std::vector<sela::CaptureObservation>{{"x86_64", captures.at("x86_64")}, {"x86_64", captures.at("x86_64")}},
        std::vector<sela::CaptureObservation>{{"riscv64", captures.at("x86_64")}},
        std::vector<sela::CaptureObservation>{{"armv7", captures.at("i686")}}}) {
    auto error = sela::mergeProfiles(observations, artifact);
    if (!error) passed = false;
    else llvm::consumeError(std::move(error));
  }
  const auto *arm = sela::targets::find("armv7");
  auto softFloat = base + "/soft-float.ll";
  passed &= write(softFloat, "target triple = \"armv7-unknown-linux-gnueabi\"\ntarget datalayout = \"" +
      arm->layout.str() + "\"\ndefine i32 @main() { ret i32 0 }\n");
  auto wrongABI = sela::mergeProfiles({{"armv7", softFloat}}, artifact);
  if (!wrongABI) passed = false;
  else llvm::consumeError(std::move(wrongABI));
  // Unmatched target-only definitions remain present instead of making
  // cross-target factoring an admission requirement. The inverse checks each.
  auto extraDefinition = base + "/extra-definition.ll";
  passed &= write(extraDefinition, captureTexts.at("aarch64") +
      "define internal i32 @unmatched_definition() { ret i32 0 }\n");
  auto differentDefinitions = sela::mergeProfiles({{"x86_64", captures.at("x86_64")},
      {"i686", captures.at("i686")}, {"armv7", captures.at("armv7")}, {"aarch64", extraDefinition}}, artifact);
  if (differentDefinitions) {
    llvm::logAllUnhandledErrors(std::move(differentDefinitions), llvm::errs(), "target-only definition: ");
    passed = false;
  }
  llvm::sys::fs::remove(extraDefinition);
  llvm::sys::fs::remove(softFloat);
  for (const auto &[target, path] : captures) llvm::sys::fs::remove(path);
  for (const auto &path : {left, right, artifact}) llvm::sys::fs::remove(path);
  llvm::sys::fs::remove(directory);
  return passed ? 0 : 1;
}
