#include "sela/Artifact/Artifact.h"
#include "llvm/Support/raw_ostream.h"

using namespace sela::driver;

llvm::Error makeStatic(const fs::path &input, const fs::path &output) {
  auto files = readPackage(input);
  if (!files) return files.takeError();
  auto manifest = validatePackage(*files);
  if (!manifest) return manifest.takeError();
  auto plan = readCompilationPlan(*manifest->getAsObject());
  if (!plan) return plan.takeError();
  std::vector<ArtifactModule> modules;
  for (auto &record : *manifest->getAsObject()->getArray("modules")) {
    auto &module = *record.getAsObject();
    modules.push_back({files->at(module.getString("path")->str())});
  }
  for (auto &[target, units] : *plan)
    for (size_t i = 0; i < units.size(); ++i) units[i].archiveMember = "member" + std::to_string(i) + ".o";
  std::vector<std::string> targets;
  for (const auto &target : *manifest->getAsObject()->getArray("targets"))
    targets.push_back(target.getAsString()->str());
  auto archive = createArtifact("static", modules, {}, {}, targets, {}, *plan);
  if (!archive) return archive.takeError();
  return writePackage(output, *archive);
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  if (auto error = makeStatic(argv[1], argv[2])) {
    llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "static fixture: ");
    return 1;
  }
  return 0;
}
