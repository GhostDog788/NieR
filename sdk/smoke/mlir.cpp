#include <archive.h>
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "llvm/Support/raw_ostream.h"

int main() {
  mlir::MLIRContext context;
  auto module = mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));
  module.print(llvm::outs());
  llvm::outs() << "\n" << archive_version_string() << "\n";
  module.erase();
  return 0;
}
