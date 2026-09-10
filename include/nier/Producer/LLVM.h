#pragma once

#include "nier/IR/Compiler.h"

namespace nier {
// Reference LLVM producer only. These private capture inputs are not part of
// the Nier code/consumer contract and are never required of another producer.
llvm::Error mergeProfiles(llvm::StringRef x86_64Capture,
                         llvm::StringRef i686Capture,
                         llvm::StringRef bytecodeOutput,
                         ArtifactSummary *summary = nullptr);
} // namespace nier
