// A real ELF32 link/run gate for the MLIR libraries consumed by nierc.
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
int main() {
  static_assert(sizeof(void *) == 4, "This probe must be a genuine 32-bit process");
  mlir::MLIRContext context;
  auto module = mlir::parseSourceString<mlir::ModuleOp>("module {}", &context);
  if (!module) return 1;
  llvm::outs() << "ELF32 MLIR parse: OK\n";
}
