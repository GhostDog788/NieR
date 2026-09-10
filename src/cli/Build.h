#pragma once
#include "nier/Support.h"

namespace nier::driver {
struct CapturedUnit {
  std::vector<fs::path> x64Paths, i686Paths;
  std::string optimization;
  std::string archiveMemberName;
};
struct CapturedBuild {
  std::vector<CapturedUnit> units;
  // When native selection order differs, move whole paired native TUs, never
  // bodies or flags, into their observed i686 link order at publication link.
  std::vector<size_t> i686Order;
  std::vector<std::string> libraries, linkOptions;
  std::string kind = "executable";
  std::string versionScript;
};
struct BuildRequest {
  std::string system;
  fs::path sourceDirectory, output;
  std::vector<std::string> configureArgs, targets, cflags;
};
// Internal SDK service: real stock Clang in two normal native build trees,
// including native configure probes and project generators. No JSON recipes.
llvm::Expected<CapturedBuild> captureBuild(const BuildRequest &request,
    const Sdk &sdk, const fs::path &scratch);
// Internal regression helper, not a publication entry point: revalidate a
// retained pair of native build lanes without creating or changing evidence.
// laneRelativeOutput is relative to each build-PROFILE lane (source/... for
// Make, build/... for CMake). This does not rerun native builds or their tests.
llvm::Expected<CapturedBuild> selectRetainedBuild(const fs::path &scratch,
    const fs::path &laneRelativeOutput);
// Internal observer selected by stock Clang's --ld-path; delegates to stock LLD.
int nativeLinkMain(int argc, char **argv);
}
