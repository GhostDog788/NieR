#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace aot {

struct ArtifactSummary {
  unsigned functions = 0;
  unsigned globals = 0;
  unsigned operations = 0;
  unsigned symbolicTypes = 0;
  unsigned symbolicConstants = 0;
};

// The argument order is part of the interface: x86_64 first, i686 second.
// Both captures are required even when only x86_64 execution is requested.
llvm::Error mergeProfiles(llvm::StringRef x86_64Capture,
                          llvm::StringRef i686Capture,
                          llvm::StringRef bytecodeOutput,
                          ArtifactSummary *summary = nullptr);

// Accepted profile IDs are exactly "x86_64" and "i686".
llvm::Error lowerArtifact(llvm::StringRef bytecodeInput,
                          llvm::StringRef profile,
                          llvm::StringRef llvmIROutput);

llvm::Error inspectArtifact(llvm::StringRef bytecodeInput,
                            ArtifactSummary &summary);

} // namespace aot
