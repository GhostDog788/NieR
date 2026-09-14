#include "sela/IR/Compiler.h"
#include "sela/IR/Dialect.h"
#include "sela/IR/Domains.h"
#include "sela/Targets.h"
#include "mlir/IR/Builders.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

namespace {
constexpr const char *source = R"mlir(
module attributes {sela.schema = 1 : i32, sela.targets = ["x86_64", "i686", "armv7", "aarch64"]} {
  "sela.func"() ({
    %x = "sela.call"() {callee = "implementation", attributes = [[], []], tail = 0 : i32} : () -> i32
    "sela.return"(%x) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
  "sela.func"() ({
    %x = "sela.constant"() {value = 2 : i64} : () -> i32
    "sela.return"(%x) : (i32) -> ()
  }) {id = "implementation", type = () -> i32, declaration = false, variadic = false,
      internal = true, dso_local = true, attributes = [[], []], targets = ["x86_64", "i686"]} : () -> ()
  "sela.func"() ({
    %x = "sela.constant"() {value = 7 : i64} : () -> i32
    "sela.return"(%x) : (i32) -> ()
  }) {id = "implementation", type = () -> i32, declaration = false, variadic = false,
      internal = true, dso_local = true, attributes = [[], []], targets = ["armv7", "aarch64"]} : () -> ()
}
)mlir";
}
int main() {
  mlir::MLIRContext context;
  context.getOrLoadDialect<sela::ir::SelaDialect>();
  mlir::Builder builder(&context);
  unsigned failures = 0;
  auto verify = [&](mlir::ModuleOp module, bool expected, llvm::StringRef name) {
    auto error = sela::verifyModuleStructure(module);
    const bool good = !error;
    if (good != expected) {
      ++failures;
      llvm::errs() << "FAIL " << name << '\n';
      if (error) llvm::logAllUnhandledErrors(std::move(error), llvm::errs());
    } else {
      llvm::consumeError(std::move(error));
      llvm::outs() << "PASS " << name << '\n';
    }
  };
  auto module = mlir::parseSourceString<mlir::ModuleOp>(source, &context);
  if (!module) return 1;
  module->walk([&](mlir::Operation *operation) {
    operation->setLoc(builder.getUnknownLoc());
    for (auto &region : operation->getRegions()) for (auto &block : region)
      for (auto argument : block.getArguments()) argument.setLoc(builder.getUnknownLoc());
  });
  verify(*module, true, "disjoint definitions implement one shared symbol");
  for (const auto &target : sela::targets::all()) {
    auto selected = sela::ir::specializeDomains(*module, target);
    if (!selected) { llvm::logAllUnhandledErrors(selected.takeError(), llvm::errs()); return 1; }
    if (std::distance((*selected)->getBody()->begin(), (*selected)->getBody()->end()) != 2) ++failures;
    auto &implementation = (*selected)->getBody()->back();
    auto value = implementation.getRegion(0).front().front().getAttrOfType<mlir::IntegerAttr>("value");
    if (value.getInt() != (target.llvmBackend == "X86" ? 2 : 7)) ++failures;
    verify(**selected, true, target.id);
  }
  auto &arm = module->getBody()->back();
  auto originalDomain = arm.getAttr("targets");
  arm.setAttr("targets", sela::ir::targetSet(&context, {"i686", "armv7", "aarch64"}));
  verify(*module, false, "overlapping definitions rejected");
  arm.setAttr("targets", sela::ir::targetSet(&context, {"armv7"}));
  verify(*module, false, "missing target implementation rejected");
  arm.setAttr("targets", builder.getArrayAttr({}));
  verify(*module, false, "empty definition domain rejected");
  arm.setAttr("targets", originalDomain);
  auto &constant = arm.getRegion(0).front().front();
  constant.setAttr("private_source", builder.getStringAttr("secret.c"));
  verify(*module, false, "foreign target schema is checked before selection");
  constant.removeAttr("private_source");
  verify(*module, true, "restored definition domains");
  return failures ? 1 : 0;
}
