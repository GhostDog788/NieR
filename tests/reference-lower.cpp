// Development-only reference lowering. Never installed in a device package.
#include "nier/Artifact/Artifact.h"
#include "nier/IR/CompilationUnits.h"
#include "llvm/Support/raw_ostream.h"

using namespace nier::driver;
namespace {
llvm::Error lower(int argc, char **argv) {
  if (argc != 7 || std::string(argv[1]) != "lower")
    return fail("usage: nier_reference_lower lower INPUT --target PROFILE --output-dir DIR");
  fs::path input = fs::absolute(argv[2]), output;
  std::string target;
  for (int i = 3; i < argc; i += 2) {
    if (std::string(argv[i]) == "--target") target = argv[i + 1];
    else if (std::string(argv[i]) == "--output-dir") output = fs::absolute(argv[i + 1]);
    else return fail("unknown reference lowering option");
  }
  if (target.empty() || output.empty() || fs::exists(output))
    return fail("reference lowering requires a target and a new output directory");
  auto files = readPackage(input);
  if (!files) return files.takeError();
  auto manifest = validatePackage(*files);
  if (!manifest) return manifest.takeError();
  auto &object = *manifest->getAsObject();
  auto plans = readCompilationPlan(object);
  if (!plans) return plans.takeError();
  if (!plans->count(target)) return fail("reference target not declared by artifact");
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  std::vector<std::string> fragments;
  for (auto &record : *object.getArray("modules")) {
    auto path = scratch->path / (std::to_string(fragments.size()) + ".nierbc");
    if (auto error = write(path, files->at(record.getAsObject()->getString("path")->str()))) return error;
    fragments.push_back(path.string());
  }
  size_t index = 0;
  for (auto &unit : plans->at(target)) {
    llvm::SmallVector<llvm::StringRef> selected;
    for (auto ordinal : unit.modules) selected.push_back(fragments.at(ordinal));
    if (auto error = nier::lowerCompilationUnit(selected, target,
        (scratch->path / (std::to_string(index++) + ".ll")).string())) return error;
  }
  fs::create_directories(output);
  for (size_t i = 0; i < index; ++i) {
    auto name = std::to_string(i) + ".ll";
    if (auto error = replaceFile(scratch->path / name, output / name)) return error;
  }
  return llvm::Error::success();
}
}
int main(int argc, char **argv) {
  try {
    if (auto error = lower(argc, argv)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "reference lowering: "); return 1;
    }
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "reference lowering: " << error.what() << '\n'; return 1;
  }
}
