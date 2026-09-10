#pragma once
#include "Support.h"

namespace aot::driver {
struct CapturedUnit {
  fs::path x64Path;
  fs::path i686Path;
  std::string optimization;
};
struct CapturedBuild {
  std::vector<CapturedUnit> units;
  std::vector<std::string> libraries;
};

// First existing-build checkpoint: one native executable linked from direct C
// objects, plus SDK libc/libm. Archives/shared outputs are explicitly rejected.
llvm::Expected<CapturedBuild> captureBuild(
    const llvm::json::Object &build, const Sdk &sdk,
    const fs::path &recipeDirectory, const fs::path &scratch,
    const std::vector<std::string> &cflags);

// Main dispatches argv[0] basenames aot-clang/aot-ar/aot-ranlib/aot-ld here.
// Wrappers are only meaningful inside the private captureBuild environment.
int wrapperMain(int argc, char **argv);
}
