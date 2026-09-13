#include "nier/Artifact/Artifact.h"
#include "nier/IR/Compiler.h"
#include "nier/IR/CompilationUnits.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>

using namespace nier::driver;
namespace {
llvm::Error rejectInputAlias(const fs::path &input, const fs::path &output) {
  // Resolve parent-directory symlinks as well as a symlink at the leaf. Hard
  // links have different canonical names, so also compare filesystem identity.
  auto source = fs::weakly_canonical(fs::absolute(input));
  auto destination = fs::weakly_canonical(fs::absolute(output));
  if (source == destination) return fail("input and output must be different files");
  std::error_code error;
  if (fs::equivalent(source, destination, error))
    return fail("input and output must be different files");
  if (error && error != std::errc::no_such_file_or_directory)
    return fail("cannot compare input/output file identities: " + error.message());
  return llvm::Error::success();
}
struct Options {
  std::string command = "compile", target = NIER_DEVICE_TARGET;
  fs::path input, output;
  std::vector<fs::path> libraryDirectories;
  Sdk sdk;
  bool keepWork = false;
};
void usage() {
  llvm::outs() << "NieR compiler — pre-alpha; no backward-compatibility promise\n"
      "Native target: " NIER_DEVICE_TARGET " (this compiler does not cross-compile)\n"
      "  nierc INPUT.nier -o OUTPUT [--sdk DIR] [--library-dir DIR] [--keep-work]\n"
      "  nierc inspect INPUT.nier\n"
      "  nierc lower INPUT.nier --output-dir DIR [--target " NIER_DEVICE_TARGET "]\n"
      "  nierc --print-target\n"
      "  nierc --check-sdk\n"
      "C publication uses stock clang --config=nier.cfg, not this program.\n";
}
Sdk discoverSdk() {
  Sdk result;
  const char *sdk = std::getenv("NIER_SDK_ROOT");
  result.root = fs::absolute(sdk ? sdk : NIER_DEFAULT_SDK);
  if (!sdk) {
    std::error_code ec;
    auto executable = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
      auto bundled = executable.parent_path().parent_path() / "sdk";
      if (fs::is_directory(bundled / "host")) result.root = bundled;
    }
  }
  return result;
}
llvm::Expected<Options> parse(int argc, char **argv) {
  if (argc < 2) return fail("expected an artifact; use nierc --help");
  Options result;
  result.sdk = discoverSdk();
  int start = 1;
  if (std::string(argv[1]) == "inspect" || std::string(argv[1]) == "lower") {
    result.command = argv[1];
    start = 2;
  }
  for (int i = start; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--keep-work") result.keepWork = true;
    else if (arg == "--sdk" || arg == "--library-dir" || arg == "-o" || arg == "--output-dir" || arg == "--target") {
      if (++i == argc) return fail("missing argument after " + arg);
      if (arg == "--sdk") result.sdk.root = fs::absolute(argv[i]);
      else if (arg == "--library-dir") result.libraryDirectories.push_back(fs::absolute(argv[i]));
      else if (arg == "--target") result.target = argv[i];
      else result.output = fs::absolute(argv[i]);
    } else if (!arg.empty() && arg[0] != '-' && result.input.empty()) result.input = fs::absolute(arg);
    else return fail("unexpected argument: " + arg);
  }
  if (result.input.empty()) return fail("missing input artifact");
  if (result.command != "inspect" && result.output.empty()) return fail("missing output (-o or --output-dir)");
  if (!result.output.empty())
    if (auto error = rejectInputAlias(result.input, result.output)) return error;
  return result;
}
llvm::Error execute(const Options &options) {
  if (options.target != NIER_DEVICE_TARGET)
    return fail("native target unavailable: " + options.target + "; this compiler supports only " NIER_DEVICE_TARGET);
  auto files = readPackage(options.input);
  if (!files) return files.takeError();
  auto manifest = validatePackage(*files);
  if (!manifest) return manifest.takeError();
  auto &object = *manifest->getAsObject();
  llvm::SmallVector<llvm::StringRef> targetDomain;
  for (auto &target : *object.getArray("targets")) targetDomain.push_back(*target.getAsString());
  bool inspect = options.command == "inspect", lower = options.command == "lower";
  if (!inspect) {
    bool admitted = false;
    for (auto &target : *object.getArray("targets")) admitted |= target.getAsString() == options.target;
    if (!admitted) return fail("artifact does not support requested target: " + options.target);
    if (!lower && object.getString("kind") == "object") return fail("relocatable NieR unit requires publication linking through stock Clang");
    if (!lower) if (auto error = options.sdk.validate()) return error;
  }
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  scratch->keep = options.keepWork;
  if (options.keepWork) llvm::errs() << "Private compiler workspace: " << scratch->path.string() << '\n';
  if (inspect) {
    llvm::outs() << "Contract: " << Contract << "\nKind: " << *object.getString("kind")
                 << "\nCompiler native target: " NIER_DEVICE_TARGET "\nDeclared targets:";
    for (auto target : targetDomain) llvm::outs() << ' ' << target;
    llvm::outs() << '\n';
  }
  std::vector<fs::path> nativeObjects;
  std::vector<fs::path> bytecodes;
  size_t index = 0;
  for (auto &entry : *object.getArray("modules")) {
    auto &record = *entry.getAsObject();
    std::string member = record.getString("path")->str();
    std::string stem = std::to_string(index++);
    fs::path bytecode = scratch->path / (stem + ".nierbc");
    if (auto error = write(bytecode, files->at(member))) return error;
    bytecodes.push_back(bytecode);
    // Admit every public module before any selected-target native lowering.
    // Native lowering remains limited to this device backend.
    nier::ArtifactSummary summary;
    if (auto error = nier::inspectArtifactStructure(bytecode.string(), summary)) return error;
    if (inspect) {
      llvm::outs() << member << ": " << summary.functions << " functions, " << summary.operations << " operations\n";
    }
  }
  auto plans = readCompilationPlan(object);
  if (!plans) return plans.takeError();
  if (inspect) {
    for (const auto &[target, units] : *plans) {
      if (target != NIER_DEVICE_TARGET) {
        llvm::outs() << target << ": " << units.size()
                     << " native compilation units; not validated (native backend unavailable)\n";
        continue;
      }
      for (const auto &unit : units) {
        std::vector<std::string> paths;
        for (auto fragment : unit.modules) paths.push_back(bytecodes[fragment].string());
        llvm::SmallVector<llvm::StringRef> references(paths.begin(), paths.end());
        if (auto error = nier::lowerCompilationUnit(references, target,
            (scratch->path / "inspection.ll").string())) return error;
      }
      llvm::outs() << target << ": " << units.size() << " native compilation units; native validation passed\n";
    }
    return llvm::Error::success();
  }
  index = 0;
  for (const auto &unit : plans->at(options.target)) {
    const auto stem = std::to_string(index++);
    const auto nativeIR = scratch->path / (stem + ".ll");
    std::vector<std::string> paths;
    for (auto fragment : unit.modules) paths.push_back(bytecodes[fragment].string());
    llvm::SmallVector<llvm::StringRef> references(paths.begin(), paths.end());
    if (auto error = nier::lowerCompilationUnit(references, options.target, nativeIR.string())) return error;
    if (lower) continue;
    const auto &level = unit.optimization;
    fs::path optimized = scratch->path / (stem + ".opt.bc");
    if (auto error = run({options.sdk.tool("opt").string(), "-passes=default<" + level + ">", "-verify-each", nativeIR.string(), "-o", optimized.string()}, {}, options.sdk.toolEnvironment())) return error;
    fs::path nativeObject = scratch->path / (stem + ".o");
    if (object.getString("kind") == "static") {
      auto directory = scratch->path / ("member-" + stem);
      fs::create_directory(directory);
      nativeObject = directory / unit.archiveMember;
    }
    std::string backendLevel = level == "O0" ? "0" : level == "O1" ? "1" : level == "O3" ? "3" : "2";
    if (auto error = run({options.sdk.tool("llc").string(), "-O=" + backendLevel, "-filetype=obj", "-relocation-model=pic", optimized.string(), "-o", nativeObject.string()}, {}, options.sdk.toolEnvironment())) return error;
    nativeObjects.push_back(nativeObject);
  }
  if (lower) {
    if (fs::exists(options.output)) return fail("diagnostic output directory already exists");
    fs::create_directories(options.output);
    for (size_t i = 0; i < index; ++i) {
      auto name = std::to_string(i) + ".ll";
      if (auto error = replaceFile(scratch->path / name, options.output / name)) return error;
    }
    return llvm::Error::success();
  }
  std::vector<std::string> libraries, linkOptions;
  if (object.getString("kind") == "static") {
    auto archive = scratch->path / "native-archive.a";
    if (nativeObjects.empty())
      if (auto error = run({options.sdk.tool("llvm-ar").string(), "qcD", archive.string()}, {}, options.sdk.toolEnvironment())) return error;
    // Quick append (not replace) preserves physical duplicate member names.
    // A separate directory per ordinal prevents staging from overwriting one.
    for (auto &member : nativeObjects)
      if (auto error = run({options.sdk.tool("llvm-ar").string(), "qcD", archive.string(), member.string()}, {}, options.sdk.toolEnvironment())) return error;
    if (auto error = run({options.sdk.tool("llvm-ar").string(), "sD", archive.string()}, {}, options.sdk.toolEnvironment())) return error;
    if (auto error = rejectInputAlias(options.input, options.output)) return error;
    if (auto error = replaceFile(archive, options.output)) return error;
    llvm::outs() << "Native archive: " << options.output.string() << '\n';
    return llvm::Error::success();
  }
  for (auto &library : *object.getArray("libraries")) libraries.push_back(library.getAsString()->str());
  for (auto &option : *object.getArray("link_options")) linkOptions.push_back(option.getAsString()->str());
  if (object.get("version_script")) {
    auto script = scratch->path / "version.script";
    if (auto error = write(script, files->at("link/version.script"))) return error;
    linkOptions.push_back("--version-script=" + script.string());
  }
  auto native = scratch->path / "native-output";
  auto command = options.sdk.linkCommand(options.target, nativeObjects, native, libraries,
                                        object.getString("kind") == "shared", linkOptions, options.libraryDirectories);
  if (!command) return command.takeError();
  if (auto error = run(*command, {}, options.sdk.toolEnvironment())) return error;
  if (auto error = rejectInputAlias(options.input, options.output)) return error;
  if (auto error = replaceFile(native, options.output, true)) return error;
  llvm::outs() << "Native output: " << options.output.string() << '\n';
  return llvm::Error::success();
}
}
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") { usage(); return 0; }
    if (argc == 2 && std::string(argv[1]) == "--print-target") {
      llvm::outs() << NIER_DEVICE_TARGET << '\n'; return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--check-sdk") {
      if (auto error = discoverSdk().validate()) {
        llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "nierc: "); return 1;
      }
      llvm::outs() << "Matching native SDK: " NIER_DEVICE_TARGET "\n"; return 0;
    }
    auto options = parse(argc, argv);
    if (!options) { llvm::logAllUnhandledErrors(options.takeError(), llvm::errs(), "nierc: "); return 1; }
    if (auto error = execute(*options)) { llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "nierc: "); return 1; }
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "nierc: " << error.what() << '\n';
    return 1;
  }
}
