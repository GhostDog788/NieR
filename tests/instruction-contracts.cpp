#include "sela/IR/Compiler.h"
#include "sela/IR/Dialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

namespace {
const char *valid = R"mlir(
module attributes {sela.schema = 1 : i32, sela.targets = ["x86_64", "i686", "armv7", "aarch64"]} {
  "sela.func"() ({
  ^entry(%vector: vector<4xi32>, %x: i32):
    %shuffled = "sela.shuffle"(%vector, %vector) {mask = array<i32: 3, 2, -1, 0>} : (vector<4xi32>, vector<4xi32>) -> vector<4xi32>
    %count = "sela.intrinsic"(%x) {name = "population_count", attributes = [[], [], []], tail = 0 : i32} : (i32) -> i32
    %truth = "sela.constant"() {value = -1 : i64} : () -> i1
    %zeros = "sela.intrinsic"(%x, %truth) {name = "count_leading_zeros", attributes = [[], [], [], []], tail = 0 : i32} : (i32, i1) -> i32
    "sela.intrinsic"(%truth) {name = "assume", attributes = [[], [], []], tail = 0 : i32} : (i1) -> ()
    "sela.return"(%count) : (i32) -> ()
  }) {id = "test", type = (vector<4xi32>, i32) -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], [], [], []]} : () -> ()
}
)mlir";
std::string change(std::string source, llvm::StringRef from, llvm::StringRef to) {
  auto index = source.find(from.str());
  if (index == std::string::npos) llvm_unreachable("invalid fixture edit");
  source.replace(index, from.size(), to.str());
  return source;
}
}
int main() {
  unsigned failures = 0;
  auto check = [&](llvm::StringRef name, const std::string &text, bool expected) {
    mlir::MLIRContext context;
    context.getOrLoadDialect<sela::ir::SelaDialect>();
    auto module = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
    if (!module) { ++failures; llvm::errs() << "fixture parse failed: " << name << '\n'; return; }
    module->walk([&](mlir::Operation *operation) {
      operation->setLoc(mlir::UnknownLoc::get(&context));
      for (auto &region : operation->getRegions()) for (auto &block : region)
        for (auto argument : block.getArguments()) argument.setLoc(operation->getLoc());
    });
    // Deliberately no native backends here: foreign alternatives must reject
    // malformed overloads and immediates before an LLVM native API is called.
    auto error = sela::verifyModuleStructure(*module);
    if (bool(error) == expected) {
      ++failures;
      llvm::errs() << "wrong acceptance: " << name << '\n';
      if (error) llvm::logAllUnhandledErrors(std::move(error), llvm::errs());
    } else llvm::consumeError(std::move(error));
  };
  check("vector and intrinsic signatures", valid, true);
  check("bad shuffle lane", change(valid, "3, 2, -1, 0", "8, 2, -1, 0"), false);
  check("bad shuffle mask length", change(valid, "3, 2, -1, 0", "3, 2, 0"), false);
  check("unknown intrinsic", change(valid, "population_count", "arbitrary_llvm_escape"), false);
  check("scalar reduction overload", change(valid, "population_count", "vector_reduce_add"), false);
  check("wrong intrinsic arity", change(valid, "population_count", "unsigned_add_overflow"), false);
  check("missing intrinsic result", change(valid,
      "%count = \"sela.intrinsic\"(%x) {name = \"population_count\", attributes = [[], [], []], tail = 0 : i32} : (i32) -> i32",
      "%count = \"sela.constant\"() {value = 0 : i64} : () -> i32\n    \"sela.intrinsic\"(%x) {name = \"population_count\", attributes = [[], [], []], tail = 0 : i32} : (i32) -> ()"), false);
  check("bad constant type", change(valid, "value = -1 : i64", "value = 2 : i64"), false);
  check("foreign intrinsic domain", change(valid,
      "name = \"population_count\"", "name = \"x86_sse2_shift_left_i64x2\""), false);
  std::string oversized = valid;
  while (oversized.find("vector<4xi32>") != std::string::npos)
    oversized = change(oversized, "vector<4xi32>", "vector<2048xi32>");
  check("public vector extent bound", oversized, false);
  const std::string assembly = R"mlir(
module attributes {sela.schema = 1 : i32, sela.targets = ["x86_64", "i686"]} {
  "sela.func"() ({
  ^entry(%x: i32):
    %result = "sela.inline_asm"(%x) {template = "", constraints = "=r,0", backend = "X86",
      dialect = "att", side_effects = true, align_stack = false, can_throw = false,
      tail = 0 : i32, attributes = [[], [], []]} : (i32) -> i32
    "sela.return"(%result) : (i32) -> ()
  }) {id = "test", type = (i32) -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], [], []]} : () -> ()
}
)mlir";
  check("inline assembly structural contract", assembly, true);
  check("assembly backend domain", change(assembly, "backend = \"X86\"", "backend = \"ARM\""), false);
  check("assembly constraint arity", change(assembly, "constraints = \"=r,0\"", "constraints = \"=r\""), false);
  check("assembly dialect", change(assembly, "dialect = \"att\"", "dialect = \"unknown\""), false);
  check("assembly effect flag type", change(assembly, "side_effects = true", "side_effects = 1 : i32"), false);
  return failures ? 1 : 0;
}
