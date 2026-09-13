#pragma once
#include "sela/Support.h"

namespace sela::driver {
using PackageFiles = std::map<std::string, std::string>;
struct ArtifactModule {
  std::string bytecode;
  std::string optimization = "O2";
  // Static outputs preserve physical member order, including duplicate names.
  std::string archiveMember;
};
struct ArtifactUnit {
  std::vector<size_t> modules;
  std::string optimization = "O2";
  std::string archiveMember;
};
using CompilationPlan = std::map<std::string, std::vector<ArtifactUnit>>;
std::vector<std::string> defaultArtifactTargets();
// Call after validatePackage. Every fragment occurs exactly once per target;
// ordered groups restore native translation units before their optimization.
llvm::Expected<CompilationPlan> readCompilationPlan(const llvm::json::Object &manifest);
llvm::Expected<PackageFiles> createArtifact(
    llvm::StringRef kind, const std::vector<ArtifactModule> &modules,
    const std::vector<std::string> &libraries = {},
    const std::vector<std::string> &linkOptions = {},
    const std::vector<std::string> &targets = defaultArtifactTargets(),
    llvm::StringRef versionScript = {},
    const CompilationPlan &compilationPlan = {});
bool validLibrary(llvm::StringRef name);
bool validArchiveMember(llvm::StringRef name);
llvm::Expected<std::string> normalizeVersionScript(llvm::StringRef text);
llvm::Error writePackage(const fs::path &output, const PackageFiles &files);
llvm::Expected<PackageFiles> readPackage(const fs::path &input);
llvm::Expected<llvm::json::Value> validatePackage(const PackageFiles &files);
bool validName(llvm::StringRef name);
}
