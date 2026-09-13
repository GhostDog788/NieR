#include "Build.h"
#include "sela/Artifact/Artifact.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>

using namespace sela::driver;
namespace {
llvm::Error build(int argc, char **argv) {
  BuildRequest request;
  fs::path output, config = SELA_DEFAULT_CONFIG;
  Sdk sdk{std::getenv("SELA_SDK_ROOT") ? std::getenv("SELA_SDK_ROOT") : SELA_DEFAULT_SDK};
  bool keep = false;
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--help") {
      llvm::outs() << "Internal SDK service; use Sela.mk or Sela.cmake.\n"
                      "--system make|cmake --source DIR --output RELPATH --artifact FILE\n"
                      "[--target NAME] [--configure-arg ARG] [--cflag ARG] [--keep-private]\n";
      return llvm::Error::success();
    }
    if (argument == "--keep-private") { keep = true; continue; }
    if (++i == argc) return fail("missing value for " + argument);
    std::string value = argv[i];
    if (argument == "--system") request.system = value;
    else if (argument == "--source") request.sourceDirectory = value;
    else if (argument == "--output") request.output = value;
    else if (argument == "--artifact") output = value;
    else if (argument == "--target") request.targets.push_back(value);
    else if (argument == "--configure-arg") request.configureArgs.push_back(value);
    else if (argument == "--cflag") request.cflags.push_back(value);
    else if (argument == "--sdk") sdk.root = value;
    else if (argument == "--clang-config") config = value;
    else return fail("unknown internal build option: " + argument);
  }
  if (request.sourceDirectory.empty() || output.empty())
    return fail("SDK integration requires source, selected output and artifact paths");
  output = fs::absolute(output);
  if (fs::is_symlink(output) || fs::is_directory(output))
    return fail("artifact output cannot be a directory or symlink");
  if (fs::exists(output)) {
    auto previous = readPackage(output);
    if (!previous) return fail("refusing to overwrite a non-Sela build input/output: " +
                               llvm::toString(previous.takeError()));
    auto manifest = validatePackage(*previous);
    if (!manifest) return fail("refusing to replace an invalid existing Sela artifact: " +
                               llvm::toString(manifest.takeError()));
  }
  if (!fs::is_directory(output.parent_path())) return fail("artifact output directory does not exist");
  sdk.root = fs::weakly_canonical(fs::absolute(sdk.root));
  config = fs::weakly_canonical(fs::absolute(config));
  if (fs::weakly_canonical(output) == config)
    return fail("artifact output aliases the Clang configuration input");
  if (auto error = sdk.validate(true)) return error;
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  scratch->keep = keep;
  if (keep) llvm::errs() << "Private build evidence: " << scratch->path.string() << '\n';
  auto captured = captureBuild(request, sdk, scratch->path);
  if (!captured) return captured.takeError();
  std::vector<std::string> link{sdk.tool("clang").string(), "--config=" + config.string()};
  for (size_t i = 0; i < captured->units.size(); ++i) {
    const auto &unit = captured->units[i];
    const auto artifact = scratch->path / ("unit-" + std::to_string(i) + ".sela");
    std::vector<std::string> command{sdk.tool("clang").string(), "--config=" + config.string()};
    auto pluginArgument = [&](const std::string &value) {
      command.insert(command.end(), {"-Xclang", "-plugin-arg-sela", "-Xclang", value});
    };
    pluginArgument("mode=profiles");
    if (unit.pathsByTarget.empty()) return fail("selected native unit has no target captures");
    for (const auto &[target, paths] : unit.pathsByTarget) {
      if (paths.empty()) return fail("selected native target has no capture inputs");
      for (const auto &path : paths) pluginArgument("capture=" + target + "=" + path.string());
    }
    pluginArgument("optimization=" + unit.optimization);
    command.insert(command.end(), {"-x", "ir", "-c", unit.pathsByTarget.begin()->second.front().string(), "-o", artifact.string()});
    if (auto error = run(command)) return error;
    if (!fs::is_regular_file(artifact)) return fail("stock Clang did not emit the multi-profile Sela unit");
    link.push_back(artifact.string());
  }
  if (captured->kind == "shared") link.push_back("-shared");
  if (captured->kind == "static") {
    link.insert(link.end(), {"-Xlinker", "--sela-static"});
    for (const auto &unit : captured->units)
      link.insert(link.end(), {"-Xlinker", "--sela-member-name=" + unit.archiveMemberName});
  }
  for (const auto &library : captured->libraries) link.push_back("-l" + library);
  for (const auto &option : captured->linkOptions) { link.push_back("-Xlinker"); link.push_back(option); }
  for (const auto &[target, indices] : captured->ordersByTarget) {
    std::string order;
    for (size_t index : indices) {
      if (!order.empty()) order += ',';
      order += std::to_string(index);
    }
    link.insert(link.end(), {"-Xlinker", "--sela-unit-order=" + target + ":" + order});
  }
  if (!captured->versionScript.empty()) {
    const auto script = scratch->path / "publication.version.script";
    if (auto error = write(script, captured->versionScript)) return error;
    link.push_back("-Wl,--version-script," + script.string());
  }
  // Stock Clang may unlink its -o output after a failed linker. Never point
  // that cleanup at the user's previously valid publication artifact.
  const auto staged = scratch->path / "linked.sela";
  link.insert(link.end(), {"-o", staged.string()});
  if (auto error = run(link)) return error;
  auto finalFiles = readPackage(staged);
  if (!finalFiles) return finalFiles.takeError();
  auto finalManifest = validatePackage(*finalFiles);
  if (!finalManifest) return finalManifest.takeError();
  return replaceFile(staged, output);
}
}
int main(int argc, char **argv) {
  try {
    if (auto error = build(argc, argv)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "sela-build: "); return 1;
    }
    return 0;
  } catch (const std::exception &error) { llvm::errs() << "sela-build: " << error.what() << '\n'; return 1; }
}
