#include "sela/Artifact/Artifact.h"
#include "sela/IR/Compiler.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <optional>

using namespace sela::driver;
namespace {
llvm::Error rejectInputAlias(const fs::path &input, const fs::path &output) {
  auto source = fs::weakly_canonical(fs::absolute(input));
  auto destination = fs::weakly_canonical(fs::absolute(output));
  if (source == destination) return fail("publication output must not overwrite an input");
  std::error_code error;
  if (fs::equivalent(source, destination, error))
    return fail("publication output must not overwrite an input");
  if (error && error != std::errc::no_such_file_or_directory)
    return fail("cannot compare publication input/output file identities: " + error.message());
  return llvm::Error::success();
}
llvm::Error expand(const std::vector<std::string> &input, std::vector<std::string> &output, unsigned depth = 0) {
  if (depth > 8) return fail("publication linker response-file nesting limit exceeded");
  for (auto &arg : input) {
    if (output.size() > 65536) return fail("too many linker arguments");
    if (arg.size() > 1 && arg.front() == '@') {
      auto bytes = read(arg.substr(1), 1024 * 1024);
      if (!bytes) return bytes.takeError();
      llvm::BumpPtrAllocator allocator;
      llvm::StringSaver saver(allocator);
      llvm::SmallVector<const char *, 32> tokens;
      llvm::cl::TokenizeGNUCommandLine(*bytes, saver, tokens);
      std::vector<std::string> nested;
      for (auto token : tokens) nested.emplace_back(token);
      if (auto error = expand(nested, output, depth + 1)) return error;
    } else output.push_back(arg);
  }
  return llvm::Error::success();
}
llvm::Error link(int argc, char **argv) {
  std::vector<std::string> args, original;
  for (int i = 1; i < argc; ++i) original.emplace_back(argv[i]);
  if (auto error = expand(original, args)) return error;
  fs::path output = "a.out";
  std::string kind = "executable";
  std::vector<fs::path> inputs;
  std::vector<std::string> libraries, linkOptions;
  std::vector<std::string> archiveNames;
  std::optional<std::vector<size_t>> narrowUnitOrder;
  std::string versionScript, interpreter, emulation = "elf_x86_64";
  bool bothHashStyles = false;
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef arg(args[i]);
    if (arg == "-o" || arg == "-m" || arg == "-z" || arg == "-dynamic-linker" || arg == "--dynamic-linker" || arg == "-L" || arg == "-soname" || arg == "--soname" || arg == "--version-script") {
      if (++i == args.size()) return fail("missing linker argument after " + arg.str());
      if (arg == "-o") output = args[i];
      else if (arg == "-m" && args[i] != "elf_x86_64" && args[i] != "elf_i386") return fail("unsupported linker emulation");
      else if (arg == "-m") emulation = args[i];
      else if (arg == "-dynamic-linker" || arg == "--dynamic-linker") interpreter = args[i];
      else if (arg == "-z" && args[i] != "relro" && args[i] != "now") return fail("unqualified native link setting: -z " + args[i]);
      else if (arg == "-soname" || arg == "--soname") linkOptions.push_back("-soname=" + args[i]);
      else if (arg == "--version-script") {
        if (!versionScript.empty()) return fail("multiple version scripts are not qualified");
        auto contents = read(args[i], 65536);
        if (!contents) return contents.takeError();
        versionScript = std::move(*contents);
      }
    } else if (arg.starts_with("--sela-unit-order-i686=")) {
      if (narrowUnitOrder) return fail("multiple i686 native-unit permutations are not qualified");
      auto order = arg.drop_front(llvm::StringRef("--sela-unit-order-i686=").size());
      if (order.empty() || order.size() > 4096) return fail("invalid i686 native-unit permutation");
      llvm::SmallVector<llvm::StringRef> indices;
      order.split(indices, ',');
      if (indices.size() > 512) return fail("oversized i686 native-unit permutation");
      narrowUnitOrder.emplace();
      for (auto index : indices) {
        unsigned number;
        if (index.empty() || index.getAsInteger(10, number)) return fail("invalid i686 native-unit permutation index");
        narrowUnitOrder->push_back(number);
      }
    } else if (arg == "--sela-static") kind = "static";
    else if (arg.starts_with("--sela-member-name=")) {
      auto name = arg.drop_front(19);
      if (!validArchiveMember(name)) return fail("invalid static archive member name");
      archiveNames.push_back(name.str());
    } else if (arg == "-shared") kind = "shared";
    else if (arg == "-r") kind = "object";
    else if (arg == "--export-dynamic" || arg == "-export-dynamic" || arg == "-E")
      linkOptions.push_back("--export-dynamic");
    else if (arg == "--hash-style=both" || arg == "--hash-style=gnu")
      bothHashStyles = arg == "--hash-style=both";
    else if (arg == "--undefined-version") linkOptions.push_back(arg.str());
    else if (arg == "--as-needed")
      return fail("direct Sela linking cannot yet resolve --as-needed; use the paired native SDK build integration");
    else if (arg == "-pie" || arg == "--eh-frame-hdr" || arg == "--build-id" ||
             arg == "--no-as-needed") {
      // Clang's host linker defaults are not the destination's runtime paths.
    } else if (arg.starts_with("--dynamic-linker=")) interpreter = arg.drop_front(17).str();
    else if (arg.starts_with("-L") || arg.starts_with("--sysroot=")) {
      // Managed native dependencies are resolved by the destination SDK.
    } else if (arg == "-lc") {
      // The declared managed runtime supplies libc.
    } else if (arg.starts_with("-l")) {
      auto name = arg.drop_front(2);
      if (!validLibrary(name)) return fail("invalid declared native dependency");
      libraries.push_back(name.str());
    }
    else if (arg.starts_with("--version-script=")) {
      if (!versionScript.empty()) return fail("multiple version scripts are not qualified");
      auto contents = read(arg.drop_front(17).str(), 65536);
      if (!contents) return contents.takeError();
      versionScript = std::move(*contents);
    }
    else if (arg.starts_with("-soname=")) linkOptions.push_back(arg.str());
    else if (!arg.empty() && !arg.starts_with("-")) inputs.emplace_back(arg.str());
    else return fail("unqualified publication link option: " + arg.str());
  }
  // Only Clang's ordinary profile loader is a portable default. An explicit
  // custom interpreter cannot silently become the destination SDK loader.
  if (!interpreter.empty() && interpreter != (emulation == "elf_i386"
        ? "/lib/ld-linux.so.2" : "/lib64/ld-linux-x86-64.so.2"))
    return fail("unqualified native dynamic interpreter " + interpreter);
  if (bothHashStyles) linkOptions.push_back("--hash-style=both");
  if (inputs.empty() && kind != "static") return fail("publication link has no Sela object inputs");
  // Check the entire inventory before creating or writing private work files.
  for (const auto &input : inputs)
    if (auto error = rejectInputAlias(input, output)) return error;
  std::vector<ArtifactModule> modules;
  CompilationPlan compilationPlan;
  size_t aggregateBytes = 0;
  std::vector<std::string> targets{"x86_64", "i686"};
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  for (auto &input : inputs) {
    auto files = readPackage(input);
    if (!files) return fail("expected a Sela object at " + input.string() + ": " + llvm::toString(files.takeError()));
    auto manifest = validatePackage(*files);
    if (!manifest) return manifest.takeError();
    auto &object = *manifest->getAsObject();
    if (object.getString("kind") != "object") return fail("publication link requires relocatable Sela inputs: " + input.string());
    std::vector<std::string> admitted;
    for (auto &entry : *object.getArray("targets")) admitted.push_back(entry.getAsString()->str());
    llvm::SmallVector<llvm::StringRef> domain;
    for (auto &target : admitted) domain.push_back(target);
    targets.erase(std::remove_if(targets.begin(), targets.end(), [&](auto &t) {
      return std::find(admitted.begin(), admitted.end(), t) == admitted.end();
    }), targets.end());
    auto inputPlan = readCompilationPlan(object);
    if (!inputPlan) return inputPlan.takeError();
    for (const auto &target : targets) {
      auto &units = inputPlan->at(target);
      if (kind == "static" && units.size() != 1)
        return fail("static output requires one native compilation unit per archive input member");
      for (auto unit : units) {
        for (auto &index : unit.modules) index += modules.size();
        compilationPlan[target].push_back(std::move(unit));
      }
    }
    for (auto &library : *object.getArray("libraries")) libraries.push_back(library.getAsString()->str());
    for (auto &entry : *object.getArray("modules")) {
      auto &record = *entry.getAsObject();
      auto &bytes = files->at(record.getString("path")->str());
      if (modules.size() >= 510 || bytes.size() > 64 * 1024 * 1024 - aggregateBytes)
        return fail("linked Sela artifact exceeds module or byte limits");
      aggregateBytes += bytes.size();
      auto staged = scratch->path / "unit.selabc";
      if (auto error = write(staged, bytes)) return error;
      sela::ArtifactSummary summary;
      if (auto error = sela::inspectArtifact(staged.string(), summary, domain)) return error;
      modules.push_back({bytes});
    }
  }
  if (targets.empty()) return fail("input target domains do not intersect");
  for (auto it = compilationPlan.begin(); it != compilationPlan.end();)
    if (std::find(targets.begin(), targets.end(), it->first) == targets.end()) it = compilationPlan.erase(it);
    else ++it;
  for (const auto &target : targets) compilationPlan.try_emplace(target);
  if (kind == "static") {
    for (auto &[target, units] : compilationPlan) {
      if (!archiveNames.empty() && archiveNames.size() != units.size())
        return fail("static archive member-name count does not match input members");
      for (size_t i = 0; i < units.size(); ++i)
        units[i].archiveMember = archiveNames.empty() ? "m" + std::to_string(i) + ".o" : archiveNames[i];
    }
  } else if (!archiveNames.empty()) return fail("archive member names require --sela-static");
  if (narrowUnitOrder) {
    auto plan = compilationPlan.find("i686");
    if (plan == compilationPlan.end() || narrowUnitOrder->size() != plan->second.size())
      return fail("i686 native-unit permutation does not match the admitted unit inventory");
    std::vector<bool> seen(plan->second.size());
    std::vector<ArtifactUnit> ordered;
    for (auto index : *narrowUnitOrder) {
      if (index >= seen.size() || seen[index]) return fail("i686 native-unit permutation must reference each unit exactly once");
      seen[index] = true;
      ordered.push_back(std::move(plan->second[index]));
    }
    plan->second = std::move(ordered);
  }
  auto artifact = createArtifact(kind, modules, libraries, linkOptions, targets, versionScript, compilationPlan);
  if (!artifact) return artifact.takeError();
  for (const auto &input : inputs)
    if (auto error = rejectInputAlias(input, output)) return error;
  return writePackage(output, *artifact);
}
}
int main(int argc, char **argv) {
  try {
    if (auto error = link(argc, argv)) { llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "sela-ld: "); return 1; }
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "sela-ld: " << error.what() << '\n';
    return 1;
  }
}
