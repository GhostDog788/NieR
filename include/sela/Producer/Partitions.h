#pragma once
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <string>
#include <vector>
#include <map>

namespace sela {
struct MergedPartitions {
  std::vector<std::string> fragments;
  std::map<std::string, std::vector<std::vector<size_t>>> unitsByTarget;
};
struct ProfilePartitionInput {
  std::string target;
  std::vector<std::string> captures;
};
// Private producer evidence only. All input units already have validated native
// build provenance. This bounded API preserves strong definition identities and
// each original native TU, rejecting unsupported/ambiguous repartitioning.
llvm::Expected<MergedPartitions> mergeProfilePartitions(
    llvm::ArrayRef<ProfilePartitionInput> observations,
    llvm::StringRef privateDirectory);
}
