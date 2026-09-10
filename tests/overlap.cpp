#include "nier/IR/Compiler.h"
#include "nier/IR/Dialect.h"
#include "../src/ir/OverlapEvidence.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Parser/Parser.h"

namespace {
const char *valid = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.global"() {id = "rdata", element = !nier.overlap<"r0", [i8, i16, i32, i64, f32, f64], [3, 3, 3, 1, 3, 3]>,
    initializer = "zero", constant = false, declaration = false,
    linkage = "internal", dso_local = true, alignment = "pointer_bytes", unnamed = 0 : i32} : () -> ()
}
)mlir";
bool check(llvm::StringRef label, bool passed) {
  if (!passed) llvm::errs() << "overlap test failed: " << label << '\n';
  return passed;
}
bool validate(std::string text, bool accept) {
  mlir::MLIRContext context;
  context.getOrLoadDialect<nier::ir::NIERDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
  if (!module) return !accept;
  module->walk([&](mlir::Operation *op) { op->setLoc(mlir::UnknownLoc::get(&context)); });
  auto error = nier::verifyModule(*module);
  bool passed = !error;
  if (error) llvm::consumeError(std::move(error));
  return passed == accept;
}
std::string replace(llvm::StringRef from, llvm::StringRef to) {
  std::string result(valid);
  result.replace(result.find(from.str()), from.size(), to.str());
  return result;
}
bool evidence(llvm::StringRef path) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(path, diagnostic, context);
  if (!module) return check("parse native evidence", false);
  auto overlaps = nier::detail::discoverNativeOverlaps(*module);
  if (!overlaps) {
    llvm::logAllUnhandledErrors(overlaps.takeError(), llvm::errs()); return false;
  }
  if (!check("native union evidence discovered", !overlaps->empty())) return false;
  auto *storage = overlaps->begin()->first;
  storage->setBody(llvm::ArrayRef<llvm::Type *>{llvm::Type::getInt8Ty(context)});
  auto mutated = nier::detail::discoverNativeOverlaps(*module);
  if (mutated) return check("contradictory carrier rejected", false);
  llvm::consumeError(mutated.takeError());
  return true;
}
}
int main(int argc, char **argv) {
  bool passed = check("generic overlap accepted", validate(valid, true));
  passed &= check("mismatched masks rejected", validate(replace("[3, 3, 3, 1, 3, 3]", "[3]"), false));
  passed &= check("unknown domain rejected", validate(replace("[3, 3, 3, 1, 3, 3]", "[3, 3, 3, 4, 3, 3]"), false));
  passed &= check("missing domain rejected", validate(replace("[3, 3, 3, 1, 3, 3]", "[1, 1, 1, 1, 1, 1]"), false));
  passed &= check("private identity rejected", validate(replace("\"r0\"", "\"SourceUnion\""), false));
  passed &= check("unqualified alternative rejected", validate(replace("i64, f32", "!nier.array<8, i8>, f32"), false));
  passed &= check("inactive alternative validated", validate(replace("i64, f32", "i128, f32"), false));
  for (int i = 1; i < argc; ++i) passed &= evidence(argv[i]);
  return passed ? 0 : 1;
}
