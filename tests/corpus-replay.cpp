#include "Build.h"
#include "sela/Artifact/Artifact.h"
#include "llvm/Support/raw_ostream.h"
#include <set>

using namespace sela::driver;
namespace {
llvm::Error sameArtifact(const fs::path &original, const fs::path &replayed) {
  auto first = readPackage(original);
  if (!first) return first.takeError();
  auto second = readPackage(replayed);
  if (!second) return second.takeError();
  auto firstManifest = validatePackage(*first);
  if (!firstManifest) return firstManifest.takeError();
  auto secondManifest = validatePackage(*second);
  if (!secondManifest) return secondManifest.takeError();
  if (*first != *second)
    return fail("replayed artifact contents differ: " + original.string() + " versus " + replayed.string());
  auto firstBytes = read(original);
  if (!firstBytes) return firstBytes.takeError();
  auto secondBytes = read(replayed);
  if (!secondBytes) return secondBytes.takeError();
  if (*firstBytes != *secondBytes)
    return fail("replayed artifact archive bytes differ: " + original.string() + " versus " + replayed.string());
  return llvm::Error::success();
}

llvm::Error replay(const fs::path &configuration, const Sdk &sdk,
                   const fs::path &retained, const fs::path &laneOutput,
                   const fs::path &published) {
  // This is a regression test over retained evidence, never a source rebuild.
  // Shared selection verifies native objects, immutable journals, dependencies,
  // final native witnesses and the exact existing source/settings pairing rule.
  if (auto error = sdk.validate(true)) return error;
  auto captured = selectRetainedBuild(retained, laneOutput);
  if (!captured) return captured.takeError();
  if (auto error = sameArtifact(retained / "linked.sela", published)) return error;
  std::set<std::string> expected;
  for (size_t i = 0; i < captured->units.size(); ++i)
    expected.insert("unit-" + std::to_string(i) + ".sela");
  for (const auto &entry : fs::directory_iterator(retained)) {
    auto name = entry.path().filename().string();
    if (llvm::StringRef(name).starts_with("unit-") && llvm::StringRef(name).ends_with(".sela")) {
      if (!entry.is_regular_file() || !expected.erase(name))
        return fail("unexpected retained publication unit: " + entry.path().string());
    }
  }
  if (!expected.empty()) return fail("missing retained publication unit: " + *expected.begin());
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  scratch->keep = true;
  llvm::outs() << "Retained replay evidence: " << scratch->path.string() << '\n';
  std::string receipt = "Retained native-capture regression replay\n"
      "No source compilation, configure, native reference build or upstream test was rerun.\n"
      "Strict inverse verification is performed by the current stock-Clang Sela plugin.\n"
      "Prior publication and evidence remain unchanged; all replay outputs are new.\n"
      "Retained workspace: " + retained.string() + "\nSelected lane output: " + laneOutput.string() +
      "\nPrior publication: " + published.string() + "\n";
  const auto binaryDirectory = configuration.parent_path();
  for (const auto &path : {configuration, sdk.tool("clang"), binaryDirectory / "libsela-clang.so",
                           binaryDirectory / "sela-ld", sdk.root / "sdk-lock.sha256"}) {
    auto bytes = read(path, 512 * 1024 * 1024);
    if (!bytes) return bytes.takeError();
    receipt += "Tool/input SHA256 " + digest(*bytes) + " " + path.string() + "\n";
  }
  auto environment = sdk.toolEnvironment();
  environment["SELA_SDK_ROOT"] = sdk.root.string();
  environment["LD_PRELOAD"] = "";
  for (const auto *variable : {"SELA_BUILD_METADATA", "SELA_BUILD_LANE", "SELA_BUILD_PROFILE",
                               "SELA_CAPTURE_PATH", "SELA_CAPTURE_RECORD"})
    environment[variable] = "";
  std::vector<std::string> link{sdk.tool("clang").string(), "--config=" + configuration.string()};
  for (size_t i = 0; i < captured->units.size(); ++i) {
    const auto &unit = captured->units[i];
    if (unit.x64Paths.empty() || unit.i686Paths.empty()) return fail("retained paired unit is empty");
    const auto name = "unit-" + std::to_string(i) + ".sela";
    const auto artifact = scratch->path / name;
    std::vector<std::string> command{sdk.tool("clang").string(), "--config=" + configuration.string()};
    auto argument = [&](const std::string &value) {
      command.insert(command.end(), {"-Xclang", "-plugin-arg-sela", "-Xclang", value});
    };
    if (unit.x64Paths.size() == 1 && unit.i686Paths.size() == 1) {
      argument("mode=pair"); argument("peer=" + unit.i686Paths.front().string());
    } else {
      argument("mode=group");
      for (const auto &path : unit.x64Paths) argument("left=" + path.string());
      for (const auto &path : unit.i686Paths) argument("right=" + path.string());
    }
    argument("optimization=" + unit.optimization);
    command.insert(command.end(), {"-x", "ir", "-c", unit.x64Paths.front().string(), "-o", artifact.string()});
    if (auto error = run(command, {}, environment)) return error;
    if (auto error = sameArtifact(retained / name, artifact)) return error;
    auto bytes = read(artifact);
    if (!bytes) return bytes.takeError();
    receipt += "Byte-identical unit SHA256 " + digest(*bytes) + " " + name + "\n";
    link.push_back(artifact.string());
  }
  if (captured->kind == "shared") link.push_back("-shared");
  if (captured->kind == "static") {
    link.insert(link.end(), {"-Xlinker", "--sela-static"});
    for (const auto &unit : captured->units)
      link.insert(link.end(), {"-Xlinker", "--sela-member-name=" + unit.archiveMemberName});
  }
  for (const auto &library : captured->libraries) link.push_back("-l" + library);
  for (const auto &option : captured->linkOptions) link.insert(link.end(), {"-Xlinker", option});
  if (!captured->i686Order.empty()) {
    std::string order;
    for (size_t index : captured->i686Order) {
      if (!order.empty()) order += ',';
      order += std::to_string(index);
    }
    link.insert(link.end(), {"-Xlinker", "--sela-unit-order-i686=" + order});
  }
  if (!captured->versionScript.empty()) {
    const auto script = scratch->path / "publication.version.script";
    if (auto error = write(script, captured->versionScript)) return error;
    link.insert(link.end(), {"-Xlinker", "--version-script=" + script.string()});
  }
  const auto linked = scratch->path / "linked.sela";
  link.insert(link.end(), {"-o", linked.string()});
  if (auto error = run(link, {}, environment)) return error;
  if (auto error = sameArtifact(retained / "linked.sela", linked)) return error;
  if (auto error = sameArtifact(published, linked)) return error;
  // Recheck evidence after compilation as well, without producing new capture
  // data. This is not a claim of protection against concurrent hostile writes.
  auto checkedAgain = selectRetainedBuild(retained, laneOutput);
  if (!checkedAgain) return checkedAgain.takeError();
  auto bytes = read(linked);
  if (!bytes) return bytes.takeError();
  receipt += "Byte-identical publication SHA256 " + digest(*bytes) + "\nResult: PASS\n";
  if (auto error = write(scratch->path / "revalidation.txt", receipt)) return error;
  llvm::outs() << "Retained replay PASS: " << captured->units.size() << " units, " << published.string() << '\n';
  return llvm::Error::success();
}
}
int main(int argc, char **argv) {
  if (argc != 6) {
    llvm::errs() << "test-only usage: corpus_replay_tests CONFIG SDK RETAINED_ROOT LANE_RELATIVE_OUTPUT PUBLISHED_ARTIFACT\n";
    return 2;
  }
  try {
    if (auto error = replay(fs::canonical(argv[1]), Sdk{fs::canonical(argv[2])},
                            fs::canonical(argv[3]), argv[4], fs::canonical(argv[5]))) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "retained replay: "); return 1;
    }
  } catch (const std::exception &error) {
    llvm::errs() << "retained replay: " << error.what() << '\n'; return 1;
  }
  return 0;
}
