#pragma once

#include "sela/Targets.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/Error.h"

namespace sela::ir {

// Target identities qualify a finite semantic domain, not a source language or
// a request to load a foreign native compiler. No architecture owns a bit index.
llvm::Expected<llvm::SmallVector<llvm::StringRef>> declaredTargets(mlir::ModuleOp);
mlir::ArrayAttr targetSet(mlir::MLIRContext *, llvm::ArrayRef<llvm::StringRef>);
bool containsTarget(mlir::Attribute set, llvm::StringRef target);

// Cases are disjoint target sets with explicitly represented semantic values.
// Equal values are coalesced. A single value shared by every supplied target is
// emitted directly; callers still retain the module's qualification boundary.
mlir::Attribute targetChoice(mlir::MLIRContext *,
    llvm::ArrayRef<std::pair<llvm::StringRef, mlir::Attribute>>);
llvm::Expected<mlir::Attribute> selectAttribute(mlir::Attribute, llvm::StringRef);
llvm::Expected<uint64_t> evaluateInteger(mlir::Attribute,
                                       const targets::TargetInfo &);
llvm::Expected<mlir::Type> specializeType(mlir::Type,
                                        const targets::TargetInfo &);
llvm::Error verifyTargetDomains(mlir::ModuleOp);

// Resolves public properties/choices after inactive CFG regions are discarded.
// It does not perform ABI lowering and needs no target-specific machine code.
llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>> specializeDomains(
    mlir::ModuleOp, const targets::TargetInfo &);
}
