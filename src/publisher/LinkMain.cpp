#include "sela/Artifact/Artifact.h"
#include "sela/IR/Compiler.h"
#include "sela/Targets.h"
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
  std::map<std::string, std::vector<size_t>> unitOrders;
  std::map<size_t, std::vector<std::string>> inputDomains;
  LinkPlan targetLinks, inputLinks;
  std::string versionScript, interpreter, emulation = "elf_x86_64";
  bool bothHashStyles = false;
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef arg(args[i]);
    if (arg == "-o" || arg == "-m" || arg == "-z" || arg == "-dynamic-linker" || arg == "--dynamic-linker" || arg == "-L" || arg == "-soname" || arg == "--soname" || arg == "--version-script") {
      if (++i == args.size()) return fail("missing linker argument after " + arg.str());
      if (arg == "-o") output = args[i];
      else if (arg == "-m") {
        if (llvm::none_of(sela::targets::all(), [&](const auto &target) {
              return target.lldEmulation == args[i];
            })) return fail("unsupported linker emulation");
        emulation = args[i];
      }
      else if (arg == "-dynamic-linker" || arg == "--dynamic-linker") interpreter = args[i];
      else if (arg == "-z" && args[i] != "relro" && args[i] != "now") return fail("unqualified native link setting: -z " + args[i]);
      else if (arg == "-soname" || arg == "--soname") linkOptions.push_back("-soname=" + args[i]);
      else if (arg == "--version-script") {
        if (!versionScript.empty()) return fail("multiple version scripts are not qualified");
        auto contents = read(args[i], 65536);
        if (!contents) return contents.takeError();
        versionScript = std::move(*contents);
      }
    } else if (arg.consume_front("--sela-target-links=")) {
      if (!targetLinks.empty()) return fail("duplicate native target link settings");
      auto contents = read(arg.str(), 1024 * 1024);
      if (!contents) return contents.takeError();
      auto parsed = llvm::json::parse(*contents);
      if (!parsed) return parsed.takeError();
      auto *records = parsed->getAsObject();
      if (!records || records->empty()) return fail("invalid native target link settings");
      for (const auto &entry : *records) {
        auto *record = entry.second.getAsObject();
        if (!sela::targets::find(entry.first) || !record || record->size() != 3 ||
            !record->getArray("libraries") || !record->getArray("link_options") || !record->getString("version_script"))
          return fail("invalid native target link record");
        auto &link = targetLinks[entry.first.str()];
        for (const auto &library : *record->getArray("libraries")) {
          if (!library.getAsString()) return fail("invalid target library declaration");
          link.libraries.push_back(library.getAsString()->str());
        }
        const auto *options = record->getArray("link_options");
        for (size_t index = 0; index < options->size(); ++index) {
          auto option = (*options)[index].getAsString();
          if (!option) return fail("invalid target link option");
          if (*option == "-soname") {
            if (++index == options->size() || !(*options)[index].getAsString()) return fail("missing target soname");
            link.options.push_back("-soname=" + (*options)[index].getAsString()->str());
          } else link.options.push_back(option->str());
        }
        link.versionScript = record->getString("version_script")->str();
      }
    } else if (arg.consume_front("--sela-input-domain=")) {
      auto [indexText, ids] = arg.split(':');
      unsigned index;
      if (indexText.empty() || indexText.getAsInteger(10, index) || inputDomains.count(index))
        return fail("invalid or duplicate target-scoped input index");
      auto domain = parseTargetSelection(ids);
      if (!domain) return domain.takeError();
      inputDomains[index] = std::move(*domain);
    } else if (arg.starts_with("--sela-unit-order=")) {
      auto [target, order] = arg.drop_front(llvm::StringRef("--sela-unit-order=").size()).split(':');
      if (!sela::targets::find(target) || unitOrders.count(target.str()))
        return fail("unsupported or duplicate target native-unit permutation");
      if (order.empty() || order.size() > 4096) return fail("invalid target native-unit permutation");
      llvm::SmallVector<llvm::StringRef> indices;
      order.split(indices, ',');
      if (indices.size() > 512) return fail("oversized target native-unit permutation");
      auto &permutation = unitOrders[target.str()];
      for (auto index : indices) {
        unsigned number;
        if (index.empty() || index.getAsInteger(10, number)) return fail("invalid target native-unit permutation index");
        permutation.push_back(number);
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
  const auto nativeTarget = llvm::find_if(sela::targets::all(), [&](const auto &target) {
    return target.lldEmulation == emulation;
  });
  const auto ordinaryLoader = std::string(nativeTarget->id == "x86_64" ? "/lib64/" : "/lib/") + nativeTarget->loader.str();
  if (!interpreter.empty() && interpreter != ordinaryLoader)
    return fail("unqualified native dynamic interpreter " + interpreter);
  if (bothHashStyles) linkOptions.push_back("--hash-style=both");
  if (inputs.empty() && kind != "static") return fail("publication link has no Sela object inputs");
  // Check the entire inventory before creating or writing private work files.
  for (const auto &input : inputs)
    if (auto error = rejectInputAlias(input, output)) return error;
  std::vector<ArtifactModule> modules;
  CompilationPlan compilationPlan;
  size_t aggregateBytes = 0;
  auto requested = publicationTargets();
  if (!requested) return requested.takeError();
  const auto &targets = *requested;
  if (!targetLinks.empty()) {
    if (targetLinks.size() != targets.size() || !libraries.empty() || !linkOptions.empty() || !versionScript.empty())
      return fail("native target link settings require the exact selected targets and no common override");
    for (const auto &target : targets)
      if (!targetLinks.count(target)) return fail("missing native target link settings");
  }
  for (const auto &[index, domain] : inputDomains) {
    if (index >= inputs.size()) return fail("target-scoped input index is outside link inventory");
    for (const auto &target : domain)
      if (!llvm::is_contained(targets, target)) return fail("input scope is outside requested publication targets");
  }
  if (!archiveNames.empty() && (kind != "static" || archiveNames.size() != inputs.size()))
    return fail("archive member-name count does not match input members");
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  for (size_t inputIndex = 0; inputIndex < inputs.size(); ++inputIndex) {
    const auto &input = inputs[inputIndex];
    const auto domainOverride = inputDomains.find(inputIndex);
    const auto &requiredTargets = domainOverride == inputDomains.end() ? targets : domainOverride->second;
    auto files = readPackage(input);
    if (!files) return fail("expected a Sela object at " + input.string() + ": " + llvm::toString(files.takeError()));
    auto manifest = validatePackage(*files);
    if (!manifest) return manifest.takeError();
    auto &object = *manifest->getAsObject();
    if (object.getString("kind") != "object") return fail("publication link requires relocatable Sela inputs: " + input.string());
    std::vector<std::string> admitted;
    for (auto &entry : *object.getArray("targets")) admitted.push_back(entry.getAsString()->str());
    for (const auto &target : requiredTargets)
      if (!llvm::is_contained(admitted, target))
        return fail("requested target " + target + " is missing from input " + input.string() +
                    "; publication never silently narrows its target selection");
    auto inputPlan = readCompilationPlan(object);
    if (!inputPlan) return inputPlan.takeError();
    std::vector<std::optional<size_t>> moduleIndices;
    auto dependencies = readLinkPlan(object, *files);
    for (const auto &target : requiredTargets) {
      auto &destination = inputLinks[target].libraries;
      const auto &source = dependencies.at(target).libraries;
      destination.insert(destination.end(), source.begin(), source.end());
    }
    for (auto &entry : *object.getArray("modules")) {
      auto &record = *entry.getAsObject();
      llvm::SmallVector<llvm::StringRef> domain;
      std::vector<std::string> selectedDomain;
      for (auto &value : *record.getArray("targets")) {
        domain.push_back(*value.getAsString());
        if (llvm::is_contained(requiredTargets, value.getAsString()->str()))
          selectedDomain.push_back(value.getAsString()->str());
      }
      auto &bytes = files->at(record.getString("path")->str());
      if (modules.size() >= 510 || bytes.size() > 64 * 1024 * 1024 - aggregateBytes)
        return fail("linked Sela artifact exceeds module or byte limits");
      aggregateBytes += bytes.size();
      auto staged = scratch->path / "unit.selabc";
      if (auto error = write(staged, bytes)) return error;
      sela::ArtifactSummary summary;
      if (auto error = sela::inspectArtifact(staged.string(), summary, domain)) return error;
      if (selectedDomain.empty()) moduleIndices.push_back(std::nullopt);
      else {
        moduleIndices.push_back(modules.size());
        modules.push_back({bytes, "O2", {}, std::move(selectedDomain)});
      }
    }
    for (const auto &target : requiredTargets) {
      auto &units = inputPlan->at(target);
      if (kind == "static" && units.size() != 1)
        return fail("static output requires one native compilation unit per archive input member");
      for (auto unit : units) {
        if (kind == "static") unit.archiveMember = archiveNames.empty()
            ? "m" + std::to_string(compilationPlan[target].size()) + ".o" : archiveNames[inputIndex];
        for (auto &index : unit.modules) {
          if (!moduleIndices[index]) return fail("active compilation unit references an inactive input fragment");
          index = *moduleIndices[index];
        }
        compilationPlan[target].push_back(std::move(unit));
      }
    }
  }
  for (auto it = compilationPlan.begin(); it != compilationPlan.end();)
    if (std::find(targets.begin(), targets.end(), it->first) == targets.end()) it = compilationPlan.erase(it);
    else ++it;
  for (const auto &target : targets) compilationPlan.try_emplace(target);
  for (const auto &[target, indices] : unitOrders) {
    auto plan = compilationPlan.find(target);
    if (plan == compilationPlan.end() || indices.size() != plan->second.size())
      return fail(target + " native-unit permutation does not match the admitted unit inventory");
    std::vector<bool> seen(plan->second.size());
    std::vector<ArtifactUnit> ordered;
    for (auto index : indices) {
      if (index >= seen.size() || seen[index]) return fail(target + " native-unit permutation must reference each unit exactly once");
      seen[index] = true;
      ordered.push_back(std::move(plan->second[index]));
    }
    plan->second = std::move(ordered);
  }
  LinkPlan finalLinks;
  for (const auto &target : targets) {
    auto &link = finalLinks[target];
    link = targetLinks.empty() ? ArtifactLink{libraries, linkOptions, versionScript} : targetLinks.at(target);
    const auto &dependencies = inputLinks[target].libraries;
    link.libraries.insert(link.libraries.end(), dependencies.begin(), dependencies.end());
  }
  auto artifact = createArtifact(kind, modules, {}, {}, targets, {}, compilationPlan, finalLinks);
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
