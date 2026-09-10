#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "mlir/IR/BuiltinOps.h"

namespace nier {

struct ArtifactSummary {
  unsigned functions = 0;
  unsigned globals = 0;
  unsigned operations = 0;
  unsigned symbolicTypes = 0;
  unsigned symbolicConstants = 0;
};

// Public producer-independent module interfaces. Inputs must already obey the
// publication contract, including unknown source locations and admitted types.
// The default is the current qualified core domain, not capture provenance.
// Artifact consumers pass the artifact's declared semantic target constraints.
llvm::Error verifyModule(mlir::ModuleOp module,
    llvm::ArrayRef<llvm::StringRef> targets = {"x86_64", "i686"});
llvm::Error writeModule(mlir::ModuleOp module, llvm::StringRef bytecodeOutput,
    llvm::ArrayRef<llvm::StringRef> targets = {"x86_64", "i686"});
llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>>
readModule(llvm::StringRef bytecodeInput, mlir::MLIRContext &context,
    llvm::ArrayRef<llvm::StringRef> targets = {"x86_64", "i686"});

// Accepted profile IDs are exactly "x86_64" and "i686".
llvm::Error lowerArtifact(llvm::StringRef bytecodeInput,
                          llvm::StringRef profile,
                          llvm::StringRef llvmIROutput);

llvm::Error inspectArtifact(llvm::StringRef bytecodeInput,
                            ArtifactSummary &summary,
    llvm::ArrayRef<llvm::StringRef> targets = {"x86_64", "i686"});

} // namespace nier
