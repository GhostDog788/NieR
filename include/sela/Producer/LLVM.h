#pragma once

#include "sela/IR/Compiler.h"

namespace sela {
struct CaptureObservation {
  std::string target;
  std::string capture;
};
// Reference LLVM producer only. These private capture inputs are not part of
// the Sela Code/consumer contract and are never required of another producer.
llvm::Error mergeProfiles(llvm::ArrayRef<CaptureObservation> observations,
                         llvm::StringRef bytecodeOutput,
                         ArtifactSummary *summary = nullptr);
} // namespace sela
