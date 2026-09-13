#pragma once
#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/Error.h"
#include <map>
#include <string>
#include <vector>

namespace sela::detail {
// Private producer factoring. Inputs are semantic projections in one MLIR
// context; every original native observation is retained by the caller and
// independently compared with the final shared result.
struct CommonModule {
  mlir::OwningOpRef<mlir::ModuleOp> module;
  // Per observation, local opaque storage identity -> shared identity. Names
  // are private bookkeeping; the bijection is established by actual uses.
  std::vector<std::map<std::string, std::string>> storageIdentities;
};
llvm::Expected<CommonModule> mergeCommonModules(
    llvm::ArrayRef<mlir::ModuleOp> modules, llvm::ArrayRef<llvm::StringRef> targets);
}
