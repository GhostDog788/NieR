#include "sela/IR/Compiler.h"
#include "sela/IR/Dialect.h"
#include "../src/ir/ConditionalSpecialization.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

namespace {
const char *valid = R"mlir(
module attributes {sela.schema = 1 : i32, sela.targets = ["x86_64", "i686"]} {
  "sela.func"() ({
    %selector = "sela.constant"() {value = 8 : i64} : () -> i32
    "sela.switch"(%selector)[^small, ^wide] {cases = [8 : i64],
      case_domains = [["x86_64"]], argument_counts = array<i32: 0, 0>} : (i32) -> ()
  ^small:
    %four = "sela.constant"() {value = 4 : i64} : () -> i32
    "sela.br"(%four)[^done] : (i32) -> ()
  ^wide:
    %eight = "sela.constant"() {value = 8 : i64} : () -> i32
    "sela.br"(%eight)[^done] : (i32) -> ()
  ^done(%result: i32):
    "sela.return"(%result) : (i32) -> ()
  }) {id = "answer", type = () -> i32, declaration = false, variadic = false,
    internal = false, dso_local = true, attributes = [[], []],
    block_domains = [["x86_64", "i686"], ["x86_64", "i686"], ["x86_64"], ["x86_64", "i686"]]} : () -> ()
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
  context.getOrLoadDialect<sela::ir::SelaDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
  if (!module) return !accept;
  module->walk([&](mlir::Operation *op) {
    op->setLoc(mlir::UnknownLoc::get(&context));
    for (auto &region : op->getRegions())
      for (auto &block : region)
        for (auto argument : block.getArguments()) argument.setLoc(mlir::UnknownLoc::get(&context));
  });
  auto before = print(*module);
  auto verified = sela::verifyModuleStructure(*module);
  llvm::SmallVector<llvm::StringRef> native;
  for (auto target : sela::supportedNativeTargets())
    if (target == "x86_64" || target == "i686") native.push_back(target);
  if (!verified && !native.empty()) verified = sela::verifyModule(*module, native);
  bool admitted = !verified;
  if (verified) llvm::consumeError(std::move(verified));
  bool passed = admitted == accept;
  if (admitted && accept) {
    for (bool wide : {true, false}) {
      auto specialized = sela::detail::specializeConditionalCFG(*module, wide ? "x86_64" : "i686");
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
  const std::string domains = "[[\"x86_64\", \"i686\"], [\"x86_64\", \"i686\"], [\"x86_64\"], [\"x86_64\", \"i686\"]]";
  passed &= test("unknown block domain", replace(domains, "[[\"x86_64\", \"i686\"], [\"x86_64\", \"i686\"], [\"unknown\"], [\"x86_64\", \"i686\"]]"), false);
  passed &= test("conditional entry", replace(domains, "[[\"x86_64\"], [\"x86_64\", \"i686\"], [\"x86_64\"], [\"x86_64\", \"i686\"]]"), false);
  passed &= test("wrong block count", replace(domains, "[[\"x86_64\", \"i686\"]]"), false);
  passed &= test("unknown case domain", replace("case_domains = [[\"x86_64\"]]", "case_domains = [[\"unknown\"]]"), false);
  passed &= test("wrong case count", replace("case_domains = [[\"x86_64\"]]", "case_domains = []"), false);
  passed &= test("unguarded edge to absent block", replace("case_domains = [[\"x86_64\"]]", "case_domains = [[\"x86_64\", \"i686\"]]"), false);
  passed &= test("inactive default target", replace(domains, "[[\"x86_64\", \"i686\"], [\"x86_64\"], [\"x86_64\"], [\"x86_64\", \"i686\"]]"), false);
  passed &= test("argument segment overflow", replace("array<i32: 0, 0>", "array<i32: 1, 0>"), false);
  passed &= test("choice in impossible block domain",
      replace("value = 8 : i64} : () -> i32\n    \"sela.br\"(%eight)",
              "value = {target_cases = [{targets = [\"x86_64\"], value = 8 : i64}, {targets = [\"i686\"], value = {private = \"unreachable payload\"}}]}} : () -> i32\n    \"sela.br\"(%eight)"), false);
  return passed ? 0 : 1;
}
