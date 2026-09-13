#pragma once
#include "sela/Support.h"

namespace sela::driver {
struct CapturedUnit {
  std::map<std::string, std::vector<fs::path>> pathsByTarget;
  std::string optimization;
  std::string archiveMemberName;
};
struct CapturedBuild {
  std::vector<CapturedUnit> units;
  // Preserve each target's observed order of whole native TUs, never bodies
  // or flags. Equal pointer widths do not identify a native target.
  std::map<std::string, std::vector<size_t>> ordersByTarget;
  std::vector<std::string> libraries, linkOptions;
  std::string kind = "executable";
  std::string versionScript;
};
struct BuildRequest {
  std::string system;
  fs::path sourceDirectory, output;
  std::vector<std::string> configureArgs, targets, cflags;
};
// Internal SDK service: real stock Clang in one normal build tree per target,
// including native configure probes and project generators. No JSON recipes.
llvm::Expected<CapturedBuild> captureBuild(const BuildRequest &request,
    const Sdk &sdk, const fs::path &scratch);
// Internal regression helper, not a publication entry point: revalidate a
// retained set of native build lanes without creating or changing evidence.
// laneRelativeOutput is relative to each build-PROFILE lane (source/... for
// Make, build/... for CMake). This does not rerun native builds or their tests.
llvm::Expected<CapturedBuild> selectRetainedBuild(const fs::path &scratch,
    const fs::path &laneRelativeOutput);
// Internal observer selected by stock Clang's --ld-path; delegates to stock LLD.
int nativeLinkMain(int argc, char **argv);
}
