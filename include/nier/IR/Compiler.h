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
// Structural admission is shared across device builds. It validates the public
// schema, including all conditional domains, but makes no native ABI claim.
llvm::Error verifyModuleStructure(mlir::ModuleOp module);
llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>>
readModuleStructure(llvm::StringRef bytecodeInput, mlir::MLIRContext &context);
llvm::Error inspectArtifactStructure(llvm::StringRef bytecodeInput,
                                     ArtifactSummary &summary);

// Exactly the native implementations statically linked into this library.
// This is a capability query, not the list of domains understood by the schema.
llvm::ArrayRef<llvm::StringRef> supportedNativeTargets();

// Native validation is explicit and strict: every requested target must be
// available. Empty, duplicate, unknown and unavailable target requests fail.
llvm::Error verifyModule(mlir::ModuleOp module,
    llvm::ArrayRef<llvm::StringRef> targets);
llvm::Error writeModule(mlir::ModuleOp module, llvm::StringRef bytecodeOutput,
    llvm::ArrayRef<llvm::StringRef> targets);
llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>>
readModule(llvm::StringRef bytecodeInput, mlir::MLIRContext &context,
    llvm::ArrayRef<llvm::StringRef> targets);

// A known profile is not necessarily available in this linked device compiler.
llvm::Error lowerArtifact(llvm::StringRef bytecodeInput,
                          llvm::StringRef profile,
                          llvm::StringRef llvmIROutput);

llvm::Error inspectArtifact(llvm::StringRef bytecodeInput,
                            ArtifactSummary &summary,
    llvm::ArrayRef<llvm::StringRef> targets);

} // namespace nier
