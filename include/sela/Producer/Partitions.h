#pragma once
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <string>
#include <vector>

namespace sela {
struct MergedPartitions {
  std::vector<std::string> fragments;
  std::vector<std::vector<size_t>> x64Units, i686Units;
};
// Private producer evidence only. All input units already have validated native
// build provenance. This bounded API preserves strong definition identities and
// each original native TU, rejecting unsupported/ambiguous repartitioning.
llvm::Expected<MergedPartitions> mergeProfilePartitions(
    llvm::ArrayRef<std::string> x64Captures,
    llvm::ArrayRef<std::string> i686Captures,
    llvm::StringRef privateDirectory);
}
