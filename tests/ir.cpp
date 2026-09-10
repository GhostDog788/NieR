#include "aot/IR/Compiler.h"
#include "../src/ir/Dialect.h"
#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <cstdlib>

namespace {
const char *valid = R"mlir(
module attributes {aot.schema = 1 : i32, aot.profiles = ["x86_64", "i686"], aot.module_flags = []} {
  "aot.func"() ({
    %0 = "aot.constant"() {value = 1 : i64} : () -> i32
    %1 = "aot.constant"() {value = 2 : i64} : () -> i32
    %2 = "aot.binary"(%0, %1) {opcode = "add", flags = 0 : i32} : (i32, i32) -> i32
    "aot.return"(%2) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

std::string replace(std::string source, const std::string &from,
                    const std::string &to) {
  auto position = source.find(from);
  if (position == std::string::npos) {
    llvm::errs() << "bad test replacement: " << from << '\n';
    std::exit(2);
  }
  source.replace(position, from.size(), to);
  return source;
}

bool check(llvm::StringRef directory, llvm::StringRef label,
           const std::string &text, bool expectSuccess, bool bytecode = true,
           bool retainLocations = false) {
  llvm::SmallString<256> path(directory);
  llvm::sys::path::append(path, label);
  std::error_code ec;
  {
    llvm::raw_fd_ostream output(path, ec, llvm::sys::fs::OF_None);
    if (ec) { llvm::errs() << ec.message() << '\n'; return false; }
    if (bytecode) {
      mlir::MLIRContext context;
      context.getOrLoadDialect<aot::ir::AOTDialect>();
      auto module = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
      if (module && !retainLocations)
        module->walk([&](mlir::Operation *operation) {
          operation->setLoc(mlir::UnknownLoc::get(&context));
        });
      if (!module || mlir::failed(mlir::writeBytecodeToFile(module->getOperation(), output))) {
        llvm::errs() << "cannot construct test fixture " << label << '\n';
        return false;
      }
    } else {
      output << text;
    }
  }
  aot::ArtifactSummary summary;
  auto error = aot::inspectArtifact(path, summary);
  bool success = !error;
  if (error) {
    std::string message = llvm::toString(std::move(error));
    if (expectSuccess) llvm::errs() << label << ": " << message << '\n';
  }
  llvm::sys::fs::remove(path);
  if (success != expectSuccess) {
    llvm::errs() << label << ": incorrect acceptance result\n";
    return false;
  }
  return true;
}
} // namespace

int main() {
  llvm::SmallString<256> directory;
  if (auto ec = llvm::sys::fs::createUniqueDirectory("aot-ir-tests", directory)) {
    llvm::errs() << ec.message() << '\n';
    return 1;
  }
  bool passed = true;
  passed &= check(directory, "valid.mlirbc", valid, true);
  passed &= check(directory, "text-is-not-bytecode.mlirbc", valid, false, false);
  passed &= check(directory, "truncated.mlirbc", "ML", false, false);
  passed &= check(directory, "invalid-exact-add.mlirbc",
                  replace(valid, "flags = 0", "flags = 4"), false);
  passed &= check(directory, "invalid-wrap-div.mlirbc",
                  replace(replace(valid, "flags = 0", "flags = 2"),
                          "opcode = \"add\"", "opcode = \"udiv\""), false);
  passed &= check(directory, "valid-wrap-add.mlirbc",
                  replace(valid, "flags = 0", "flags = 3"), true);
  passed &= check(directory, "valid-exact-div.mlirbc",
                  replace(replace(valid, "flags = 0", "flags = 4"),
                          "opcode = \"add\"", "opcode = \"udiv\""), true);
  passed &= check(directory, "unknown-schema.mlirbc",
                  replace(valid, "aot.schema = 1", "aot.schema = 2"), false);
  passed &= check(directory, "unknown-module-field.mlirbc",
                  replace(valid, "aot.schema = 1", "aot.private_source = \"secret.c\", aot.schema = 1"), false);
  passed &= check(directory, "unknown-op-field.mlirbc",
                  replace(valid, "flags = 0", "future_semantics = true, flags = 0"), false);
  passed &= check(directory, "oversized-integer.mlirbc",
                  replace(valid, "value = 1 : i64", "value = 18446744073709551616 : i128"), false);
  passed &= check(directory, "unknown-function-attribute.mlirbc",
                  replace(valid, "attributes = [[], []]", "attributes = [[{name = \"invented\"}], []]"), false);
  passed &= check(directory, "private-locations.mlirbc", valid, false, true, true);
  llvm::sys::fs::remove(directory);
  return passed ? 0 : 1;
}
