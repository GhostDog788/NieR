#pragma once
#include "sela/Support.h"

namespace sela::driver {
using PackageFiles = std::map<std::string, std::string>;
struct ArtifactModule {
  std::string bytecode;
  std::string optimization = "O2";
  // Static outputs preserve physical member order, including duplicate names.
  std::string archiveMember;
  // Empty in the construction API means all artifact targets. On disk every
  // fragment has an explicit nonempty availability domain.
  std::vector<std::string> targets;
};
struct ArtifactUnit {
  std::vector<size_t> modules;
  std::string optimization = "O2";
  std::string archiveMember;
};
using CompilationPlan = std::map<std::string, std::vector<ArtifactUnit>>;
struct ArtifactLink {
  std::vector<std::string> libraries, options;
  std::string versionScript;
};
using LinkPlan = std::map<std::string, ArtifactLink>;
// Read only after validatePackage. Returns one complete link record per target.
LinkPlan readLinkPlan(const llvm::json::Object &manifest, const PackageFiles &files);
std::vector<std::string> defaultArtifactTargets();
// Call after validatePackage. Every active fragment occurs once per target;
// ordered groups restore native translation units before their optimization.
llvm::Expected<CompilationPlan> readCompilationPlan(const llvm::json::Object &manifest);
llvm::Expected<PackageFiles> createArtifact(
    llvm::StringRef kind, const std::vector<ArtifactModule> &modules,
    const std::vector<std::string> &libraries = {},
    const std::vector<std::string> &linkOptions = {},
    const std::vector<std::string> &targets = defaultArtifactTargets(),
    llvm::StringRef versionScript = {},
    const CompilationPlan &compilationPlan = {}, const LinkPlan &linkPlan = {});
bool validLibrary(llvm::StringRef name);
bool validArchiveMember(llvm::StringRef name);
llvm::Expected<std::string> normalizeVersionScript(llvm::StringRef text);
llvm::Error writePackage(const fs::path &output, const PackageFiles &files);
llvm::Expected<PackageFiles> readPackage(const fs::path &input);
llvm::Expected<llvm::json::Value> validatePackage(const PackageFiles &files);
bool validName(llvm::StringRef name);
}
