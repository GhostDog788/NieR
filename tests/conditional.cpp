#include "nier/IR/Compiler.h"
#include "nier/IR/Dialect.h"
#include "../src/ir/ConditionalSpecialization.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

namespace {
const char *valid = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({
    %selector = "nier.constant"() {value = 8 : i64} : () -> i32
    "nier.switch"(%selector)[^small, ^wide] {cases = [8 : i64],
      case_domains = [1 : i32], argument_counts = array<i32: 0, 0>} : (i32) -> ()
  ^small:
    %four = "nier.constant"() {value = 4 : i64} : () -> i32
    "nier.br"(%four)[^done] : (i32) -> ()
  ^wide:
    %eight = "nier.constant"() {value = 8 : i64} : () -> i32
    "nier.br"(%eight)[^done] : (i32) -> ()
  ^done(%result: i32):
    "nier.return"(%result) : (i32) -> ()
  }) {id = "answer", type = () -> i32, declaration = false, variadic = false,
    internal = false, dso_local = true, attributes = [[], []],
    block_domains = [3 : i32, 3 : i32, 1 : i32, 3 : i32]} : () -> ()
}
)mlir";
std::string replace(llvm::StringRef from, llvm::StringRef to) {
  std::string result(valid);
  result.replace(result.find(from.str()), from.size(), to.str());
  return result;
}
std::string print(mlir::ModuleOp module) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  module.print(stream);
  return text;
}
bool test(llvm::StringRef name, const std::string &text, bool accept) {
  mlir::MLIRContext context;
  context.getOrLoadDialect<nier::ir::NIERDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
  if (!module) return !accept;
  module->walk([&](mlir::Operation *op) {
    op->setLoc(mlir::UnknownLoc::get(&context));
    for (auto &region : op->getRegions())
      for (auto &block : region)
        for (auto argument : block.getArguments()) argument.setLoc(mlir::UnknownLoc::get(&context));
  });
  auto before = print(*module);
  auto verified = nier::verifyModule(*module, nier::supportedNativeTargets());
  bool admitted = !verified;
  if (verified) llvm::consumeError(std::move(verified));
  bool passed = admitted == accept;
  if (admitted && accept) {
    for (bool wide : {true, false}) {
      auto specialized = nier::detail::specializeConditionalCFG(*module, wide);
      if (!specialized) {
        llvm::logAllUnhandledErrors(specialized.takeError(), llvm::errs()); passed = false; continue;
      }
      auto &function = (**specialized).getBody()->front();
      auto &region = function.getRegion(0);
      passed &= region.getBlocks().size() == (wide ? 4u : 3u) && !function.getAttr("block_domains");
      auto *dispatch = region.front().getTerminator();
      passed &= dispatch->getNumSuccessors() == (wide ? 2u : 1u) && !dispatch->getAttr("case_domains");
    }
  }
  passed &= print(*module) == before;
  if (!passed) llvm::errs() << "conditional CFG test failed: " << name << '\n';
  return passed;
}
}
int main() {
  bool passed = test("shared conditional switch", valid, true);
  passed &= test("unknown block domain", replace("1 : i32, 3 : i32]}", "4 : i32, 3 : i32]}"), false);
  passed &= test("conditional entry", replace("[3 : i32, 3 : i32, 1 : i32, 3 : i32]", "[1 : i32, 3 : i32, 1 : i32, 3 : i32]"), false);
  passed &= test("wrong block count", replace("[3 : i32, 3 : i32, 1 : i32, 3 : i32]", "[3 : i32]"), false);
  passed &= test("unknown case domain", replace("case_domains = [1 : i32]", "case_domains = [4 : i32]"), false);
  passed &= test("wrong case count", replace("case_domains = [1 : i32]", "case_domains = []"), false);
  passed &= test("unguarded edge to absent block", replace("case_domains = [1 : i32]", "case_domains = [3 : i32]"), false);
  passed &= test("inactive default target", replace("[3 : i32, 3 : i32, 1 : i32, 3 : i32]", "[3 : i32, 1 : i32, 1 : i32, 3 : i32]"), false);
  passed &= test("argument segment overflow", replace("array<i32: 0, 0>", "array<i32: 1, 0>"), false);
  return passed ? 0 : 1;
}
