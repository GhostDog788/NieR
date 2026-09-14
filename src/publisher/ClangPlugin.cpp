#include "sela/Artifact/Artifact.h"
#include "sela/IR/Compiler.h"
#include "sela/Producer/LLVM.h"
#include "sela/Producer/Partitions.h"
#include "sela/Targets.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Basic/CodeGenOptions.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/Utils.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Job.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <set>
#include <stdexcept>
#include <unistd.h>

using namespace sela::driver;

namespace {
void diagnose(clang::CompilerInstance &compiler, llvm::Error error) {
  unsigned id = compiler.getDiagnostics().getCustomDiagID(
      clang::DiagnosticsEngine::Error, "Sela: %0");
  compiler.getDiagnostics().Report(id) << llvm::toString(std::move(error));
}

fs::path absolutePath(const fs::path &path) {
  return fs::weakly_canonical(fs::absolute(path));
}

bool sameFile(const fs::path &left, const fs::path &right) {
  if (absolutePath(left) == absolutePath(right)) return true;
  std::error_code error;
  return fs::equivalent(left, right, error) && !error;
}

std::string optimization(const clang::CodeGenOptions &options) {
  if (options.OptimizeSize == 2) return "Oz";
  if (options.OptimizeSize == 1) return "Os";
  return "O" + std::to_string(options.OptimizationLevel);
}

std::string replaceAll(std::string text, llvm::StringRef from,
                       llvm::StringRef to) {
  if (from.empty()) return text;
  size_t offset = 0;
  while ((offset = text.find(from.str(), offset)) != std::string::npos) {
    text.replace(offset, from.size(), to.str());
    offset += to.size();
  }
  return text;
}

void clearFrontendPlugins(clang::CompilerInvocation &invocation) {
  auto &frontend = invocation.getFrontendOpts();
  frontend.Plugins.clear();
  frontend.AddPluginActions.clear();
  frontend.PluginArgs.clear();
  frontend.ActionName.clear();
  frontend.ProgramAction = clang::frontend::EmitObj;
  frontend.DisableFree = false;
}

// The only language-aware component here is the stock-Clang invocation adapter.
// It does not inspect the AST or implement a C-to-Sela lowering.
llvm::Expected<clang::CompilerInvocation> nativeInvocation(
    const clang::CompilerInvocation &original, const Sdk &sdk,
    llvm::StringRef profile, clang::DiagnosticsEngine &diagnostics) {
  // cc1 has already collapsed explicit options into host defaults: for example
  // x86's default and explicit -fsigned-char have the same LangOptions value.
  // Use Clang's recorded driver command and its own driver again, never infer
  // an ARM language configuration by changing a host invocation's triple.
  const auto &recorded = original.getCodeGenOpts().RecordCommandLine;
  if (recorded.empty())
    return fail("source publication requires stock Clang's -frecord-command-line; use the matching sela.cfg and do not disable command recording");
  llvm::BumpPtrAllocator allocator;
  llvm::StringSaver saver(allocator);
  llvm::SmallVector<const char *> tokens;
  llvm::cl::TokenizeGNUCommandLine(recorded, saver, tokens);
  if (tokens.empty()) return fail("recorded stock-Clang driver command is empty");
  std::vector<std::string> arguments{sdk.tool("clang").string()};
  for (size_t i = 1; i < tokens.size(); ++i) {
    if (llvm::StringRef(tokens[i]) == "-cc1")
      return fail("source publication requires a recorded stock-Clang driver command, not direct cc1 input");
    arguments.emplace_back(tokens[i]);
  }
  const auto flags = sdk.compileFlags(profile);
  // Target ISA defaults precede the recorded user options; target/sysroot
  // identity remains fixed by the selected publication lane.
  arguments.insert(arguments.begin() + 1, flags.begin() + 3, flags.end());
  arguments.insert(arguments.end(), flags.begin(), flags.begin() + 3);
  // Reconstruct frontend actions only: genuine link flags in the original
  // command are intentionally unused by this private driver run. This is a
  // driver-only option, not a suppression of the user's frontend diagnostics.
  arguments.push_back("-Qunused-arguments");
  arguments.push_back("-fsyntax-only");
  llvm::SmallVector<const char *> driverArguments;
  for (const auto &argument : arguments) driverArguments.push_back(argument.c_str());
  clang::driver::Driver driver(sdk.tool("clang").string(),
                              sela::targets::find(profile)->triple, diagnostics);
  std::unique_ptr<clang::driver::Compilation> compilation(driver.BuildCompilation(driverArguments));
  if (!compilation || diagnostics.hasErrorOccurred())
    return fail("stock Clang could not construct the " + profile.str() + " native invocation");
  const auto source = absolutePath(original.getFrontendOpts().Inputs.front().getFile().str());
  std::optional<clang::CompilerInvocation> selected;
  for (const auto &job : compilation->getJobs()) {
    const auto &jobArgs = job.getArguments();
    if (jobArgs.empty() || llvm::StringRef(jobArgs.front()) != "-cc1") continue;
    bool matching = false;
    for (const auto &input : job.getInputInfos())
      if (input.isFilename() && absolutePath(input.getFilename()) == source) matching = true;
    if (!matching) continue;
    if (selected) return fail("recorded driver command has ambiguous repeated source actions");
    selected.emplace();
    if (!clang::CompilerInvocation::CreateFromArgs(*selected,
          llvm::ArrayRef<const char *>(jobArgs).drop_front(), diagnostics))
      return fail("stock Clang rejected its regenerated native frontend arguments");
  }
  if (!selected) return fail("recorded driver command does not identify this C source action");
  return std::move(*selected);
}

llvm::Error captureSource(const clang::CompilerInvocation &original,
                          const Sdk &sdk, llvm::StringRef profile,
                          const fs::path &capture, const fs::path &object,
                          bool preserveDependencies,
                          clang::DiagnosticsEngine &diagnostics,
                          const fs::path &dependencyOutput = {}) {
  const auto &originalTarget = original.getTargetOpts();
  if (!originalTarget.ABI.empty() ||
      !originalTarget.FPMath.empty())
    return fail("explicit ABI/FPU calling-convention changes require a matching registered profile");
  if (!original.getCodeGenOpts().PassPlugins.empty())
    return fail("additional LLVM pass plugins are not qualified by the source producer");
  auto regenerated = nativeInvocation(original, sdk, profile, diagnostics);
  if (!regenerated) return regenerated.takeError();
  auto invocation = std::move(*regenerated);
  clearFrontendPlugins(invocation);
  invocation.getFrontendOpts().OutputFile = object.string();
  if (!preserveDependencies)
    invocation.getDependencyOutputOpts() = clang::DependencyOutputOptions();
  else if (!dependencyOutput.empty())
    invocation.getDependencyOutputOpts().OutputFile = dependencyOutput.string();
  auto &headers = invocation.getHeaderSearchOpts();
  const std::string oldSysroot = headers.Sysroot;
  const std::string oldResource = headers.ResourceDir;
  headers.Sysroot = sdk.sysroot(profile).string();
  headers.ResourceDir = (sdk.root / "host/usr/lib/llvm-18/lib/clang/18").string();
  // Keep explicit project include paths; regenerate only the configured SDK
  // search entries for the selected native profile. Host system paths are not
  // silently accepted as an application's target SDK.
  auto &entries = headers.UserEntries;
  std::set<std::string> standardSdkEntries = {
      oldResource + "/include", oldSysroot + "/usr/local/include",
      oldSysroot + "/include", oldSysroot + "/usr/include"};
  for (const auto &target : sela::targets::all())
    standardSdkEntries.insert(oldSysroot + "/usr/include/" + target.multiarch.str());
  for (const auto &entry : entries) {
    llvm::StringRef path(entry.Path);
    if (path.starts_with("/usr/include") || path.starts_with("/usr/local/include") ||
        path.starts_with("/usr/lib/gcc") || path.starts_with("/include"))
      return fail("host system include paths are not a target SDK; use the supplied sela.cfg");
    if (!oldSysroot.empty() && oldSysroot != "/" &&
        path.starts_with(oldSysroot + "/") && !standardSdkEntries.count(path.str()))
      return fail("custom profile-specific SDK include paths require the native-build integration");
  }
  entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto &entry) {
    return standardSdkEntries.count(entry.Path) != 0;
  }), entries.end());
  headers.UseStandardSystemIncludes = false;
  headers.AddPath(headers.ResourceDir + "/include", clang::frontend::System,
                  false, true);
  const std::string nativeTriple = sela::targets::find(profile)->multiarch.str();
  headers.AddPath((sdk.sysroot(profile) / "usr/include" / nativeTriple).string(),
                  clang::frontend::ExternCSystem, false, true);
  headers.AddPath((sdk.sysroot(profile) / "usr/include").string(),
                  clang::frontend::ExternCSystem, false, true);

  auto &codegen = invocation.getCodeGenOpts();
  codegen.RecordCommandLine.clear();
  codegen.DwarfDebugFlags.clear();
  codegen.PassPlugins = {SELA_CAPTURE_PLUGIN};
  codegen.setDebugInfo(llvm::codegenoptions::FullDebugInfo);
  std::vector<std::string> command{sdk.tool("clang").string(), "-cc1"};
  auto flags = invocation.getCC1CommandLine();
  command.insert(command.end(), flags.begin(), flags.end());
  if (auto error = run(command, {}, {{"SELA_CAPTURE_PATH", capture.string()},
                                    {"SELA_BUILD_METADATA", ""}}))
    return error;
  if (!fs::is_regular_file(capture))
    return fail("stock Clang did not run the qualified LLVM capture hook");
  return llvm::Error::success();
}

class SelaAction final : public clang::PluginASTAction {
  std::string mode = "source", requestedOptimization, sdkArgument;
  std::map<std::string, std::vector<std::string>> capturesByTarget;
  bool keepWork = false;
  clang::DependencyOutputOptions requestedDependencies;

  llvm::Error emit(clang::CompilerInstance &compiler) {
    const auto &invocation = compiler.getInvocation();
    const auto &frontend = invocation.getFrontendOpts();
    if (frontend.OutputFile.empty() || frontend.OutputFile == "-")
      return fail("publication requires a file output (-o)");
    if (sameFile(frontend.OutputFile, getCurrentFile().str()))
      return fail("the Sela output must not overwrite a compiler input");
    for (const auto &[target, inputs] : capturesByTarget)
      for (const auto &input : inputs)
        if (sameFile(frontend.OutputFile, input)) return fail("grouped output aliases a native capture");
    auto scratch = Scratch::create();
    if (!scratch) return scratch.takeError();
    scratch->keep = keepWork;
    if (keepWork) llvm::errs() << "Private Sela producer workspace: " << scratch->path.string() << '\n';
    std::vector<sela::CaptureObservation> observations;
    std::vector<std::string> admittedTargets;
    std::string dependencyText;
    std::string opt = optimization(invocation.getCodeGenOpts());
    if (mode == "profiles") {
      if (getCurrentFileKind().getLanguage() != clang::Language::LLVM_IR || capturesByTarget.empty())
        return fail("profile capture mode requires -x ir and capture=<target>=<LLVM capture>");
      if (!requestedOptimization.empty()) opt = requestedOptimization;
      std::vector<sela::ProfilePartitionInput> partitions;
      bool grouped = false, mainInputFound = false;
      for (const auto &target : sela::targets::all()) {
        auto inputs = capturesByTarget.find(target.id.str());
        if (inputs == capturesByTarget.end()) continue;
        admittedTargets.push_back(target.id.str());
        partitions.push_back({target.id.str(), inputs->second});
        grouped |= inputs->second.size() != 1;
        for (const auto &path : inputs->second)
          mainInputFound |= sameFile(getCurrentFile().str(), path);
        observations.push_back({target.id.str(), inputs->second.front()});
      }
      if (!mainInputFound) return fail("frontend input is not among the labelled native captures");
      if (grouped) {
        auto partition = sela::mergeProfilePartitions(partitions, scratch->path.string());
        if (!partition) return partition.takeError();
        std::vector<ArtifactModule> modules;
        for (size_t index = 0; index < partition->fragments.size(); ++index) {
          const auto &path = partition->fragments[index];
          auto bytes = read(path);
          if (!bytes) return bytes.takeError();
          modules.push_back({std::move(*bytes), opt, {}, partition->fragmentTargets[index]});
        }
        CompilationPlan plan;
        for (const auto &[target, units] : partition->unitsByTarget)
          for (const auto &unit : units) plan[target].push_back({unit, opt});
        auto artifact = createArtifact("object", modules, {}, {}, admittedTargets, {}, plan);
        if (!artifact) return artifact.takeError();
        return writePackage(absolutePath(frontend.OutputFile), *artifact);
      }
    } else {
      if (getCurrentFileKind().getLanguage() != clang::Language::C ||
          getCurrentFileKind().isPreprocessed())
        return fail("the current source producer accepts ordinary C input, not preprocessed/other-language input");
      const char *sdkEnvironment = std::getenv("SELA_SDK_ROOT");
      Sdk sdk{absolutePath(!sdkArgument.empty() ? sdkArgument :
                       sdkEnvironment ? sdkEnvironment : SELA_DEFAULT_SDK)};
      auto selected = publicationTargets();
      if (!selected) return selected.takeError();
      if (auto error = sdk.validate(true, *selected)) return error;
      clang::CompilerInvocation sourceInvocation(invocation);
      sourceInvocation.getDependencyOutputOpts() = requestedDependencies;
      const bool emitDependencies = !requestedDependencies.OutputFile.empty();
      for (const auto &target : sela::targets::all()) {
        if (!llvm::is_contained(*selected, target.id.str())) continue;
        const auto capture = scratch->path / (target.id.str() + ".bc");
        const auto deps = emitDependencies ? scratch->path / (target.id.str() + ".d") : fs::path();
        if (auto error = captureSource(sourceInvocation, sdk, target.id, capture,
              scratch->path / (target.id.str() + ".o"), emitDependencies,
              compiler.getDiagnostics(), deps)) return error;
        observations.push_back({target.id.str(), capture.string()});
        admittedTargets.push_back(target.id.str());
        if (emitDependencies) {
          auto text = read(deps);
          if (!text) return text.takeError();
          // Repeated Make rules combine prerequisites and preserve Clang's
          // escaping, including every target's own SDK/header dependencies.
          dependencyText += *text;
        }
      }
    }
    const fs::path common = scratch->path / "unit.selabc";
    if (auto error = sela::mergeProfiles(observations, common.string()))
      return error;
    auto bytes = read(common);
    if (!bytes) return bytes.takeError();
    auto artifact = createArtifact("object", {{std::move(*bytes), opt}}, {}, {}, admittedTargets);
    if (!artifact) return artifact.takeError();
    if (!dependencyText.empty()) {
      if (requestedDependencies.OutputFile == "-") llvm::outs() << dependencyText;
      else if (auto error = write(requestedDependencies.OutputFile, dependencyText)) return error;
    }
    return writePackage(absolutePath(frontend.OutputFile), *artifact);
  }

public:
  // Explicit -plugin sela still selects this as the main frontend action.
  // Returning ReplaceAction would also auto-replace native compilation when
  // the DSO is merely loaded with -fplugin for the capture observer.
  ActionType getActionType() override { return CmdlineAfterMainAction; }
  bool hasIRSupport() const override { return true; }
  bool hasPCHSupport() const override { return false; }
  bool hasASTFileSupport() const override { return false; }
  bool usesPreprocessorOnly() const override { return true; }
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance &, llvm::StringRef) override {
    return std::make_unique<clang::ASTConsumer>();
  }
  bool ParseArgs(const clang::CompilerInstance &compiler,
                 const std::vector<std::string> &arguments) override {
    for (const auto &argument : arguments) {
      llvm::StringRef arg(argument);
      if (arg.consume_front("mode=")) mode = arg.str();
      else if (arg.consume_front("capture=")) {
        const auto [target, path] = arg.split('=');
        if (!sela::targets::find(target) || path.empty()) {
          unsigned id = compiler.getDiagnostics().getCustomDiagID(
              clang::DiagnosticsEngine::Error, "invalid labelled Sela capture: %0");
          compiler.getDiagnostics().Report(id) << arg;
          return false;
        }
        capturesByTarget[target.str()].push_back(path.str());
      }
      else if (arg.consume_front("optimization=")) requestedOptimization = arg.str();
      else if (arg.consume_front("sdk=")) sdkArgument = arg.str();
      else if (arg == "keep-work") keepWork = true;
      else {
        unsigned id = compiler.getDiagnostics().getCustomDiagID(
            clang::DiagnosticsEngine::Error, "unknown Sela plugin argument: %0");
        compiler.getDiagnostics().Report(id) << argument;
        return false;
      }
    }
    if ((mode != "source" && mode != "profiles") ||
        (mode == "source" && !capturesByTarget.empty())) {
      unsigned id = compiler.getDiagnostics().getCustomDiagID(
          clang::DiagnosticsEngine::Error, "unsupported Sela producer mode: %0");
      compiler.getDiagnostics().Report(id) << mode;
      return false;
    }
    return true;
  }
protected:
  bool BeginInvocation(clang::CompilerInstance &compiler) override {
    requestedDependencies = compiler.getInvocation().getDependencyOutputOpts();
    // This adapter does not parse the C input itself. Let the delegated stock
    // compiler own dependency emission; otherwise the adapter's empty
    // preprocessor overwrites the real dependency file at destruction.
    compiler.getInvocation().getDependencyOutputOpts() = clang::DependencyOutputOptions();
    return true;
  }
  void ExecuteAction() override {
    try {
      if (auto error = emit(getCompilerInstance()))
        diagnose(getCompilerInstance(), std::move(error));
    } catch (const std::exception &error) {
      diagnose(getCompilerInstance(), fail(error.what()));
    }
  }
};

class AllDependencies final : public clang::DependencyCollector {
public:
  bool needSystemDependencies() override { return true; }
  bool sawDependency(llvm::StringRef filename, bool, bool, bool,
                     bool missing) override {
    return !missing && !filename.empty() && !filename.starts_with("<");
  }
};

// Before-main records are provisional. The after-main consumer binds the
// completed object bytes, and the build recorder still requires overall build
// success and unchanged inputs before selecting anything for publication.
class CaptureConsumer final : public clang::ASTConsumer {
  clang::CompilerInstance &compiler;
  std::shared_ptr<AllDependencies> dependencies;
  fs::path lane, metadata, capture, output;
  std::string profile, recordedDriverCommand;

  std::string normalize(std::string text) const {
    return replaceAll(std::move(text), lane.string(), "$PRIVATE");
  }

  llvm::json::Array semanticFlags() const {
    const auto &invocation = compiler.getInvocation();
    const auto &headers = invocation.getHeaderSearchOpts();
    const auto *target = sela::targets::find(profile);
    if (!target || recordedDriverCommand.empty())
      throw std::runtime_error("native capture requires a labelled profile and recorded stock-Clang driver arguments");
    const std::set<std::string> optionWithValue = {
        "-o", "-target", "--target", "--sysroot", "-isysroot", "-resource-dir",
        "-MF", "-MT", "-MQ", "-MJ", "-L", "-l", "-Xlinker"};
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    llvm::SmallVector<const char *> flags;
    llvm::cl::TokenizeGNUCommandLine(recordedDriverCommand, saver, flags);
    std::set<std::string> policy;
    for (auto flag : sela::targets::clangArgs(*target)) policy.insert(flag.str());
    for (auto flag : sela::targets::publicationArgs(*target)) policy.insert(flag.str());
    llvm::json::Array result;
    const std::string source = compiler.getFrontendOpts().Inputs.front().getFile().str();
    for (size_t index = 1; index < flags.size(); ++index) {
      llvm::StringRef flag(flags[index]);
      if (optionWithValue.count(flag.str())) { ++index; continue; }
      if (flag == source || (!flag.starts_with("-") && absolutePath(flag.str()) == absolutePath(source)) ||
          flag == "-c" || flag == "-frecord-command-line" || flag == "-fno-temp-file" ||
          flag.starts_with("--target=") || flag.starts_with("--sysroot=") ||
          flag.starts_with("-resource-dir=") || flag.starts_with("--ld-path=") ||
          flag.starts_with("-Wl,") || flag.starts_with("-L") || flag.starts_with("-l") ||
          flag.starts_with("-fplugin=") || flag.starts_with("-fpass-plugin=") ||
          policy.count(flag.str())) continue;
      // Compare source/settings intent from the driver, not target-generated
      // cc1 defaults such as ARM's unsigned char or floating-point ABI.
      // Explicit -fsigned-char/-funsigned-char remain part of this evidence.
      auto normalized = normalize(flag.str());
      normalized = replaceAll(std::move(normalized), headers.Sysroot, "$SYSROOT");
      normalized = replaceAll(std::move(normalized), headers.ResourceDir, "$RESOURCE");
      result.push_back(std::move(normalized));
    }
    return result;
  }

public:
  CaptureConsumer(clang::CompilerInstance &compiler, fs::path lane,
                  fs::path metadata, std::string profile)
      : compiler(compiler), dependencies(std::make_shared<AllDependencies>()),
        lane(std::move(lane)), metadata(std::move(metadata)),
        output(absolutePath(compiler.getFrontendOpts().OutputFile)), profile(std::move(profile)),
        recordedDriverCommand(compiler.getInvocation().getCodeGenOpts().RecordCommandLine) {
    compiler.getInvocation().getCodeGenOpts().RecordCommandLine.clear();
    fs::create_directories(this->metadata);
    if (compiler.getFrontendOpts().UseTemporary)
      throw std::runtime_error("native SDK capture requires stock Clang -fno-temp-file");
    // A PID alone can be reused during a long build. Reserve a unique capture
    // so an archived object's immutable journal cannot later be overwritten.
    llvm::SmallString<256> reserved;
    int descriptor;
    auto pattern = this->metadata / (digest(output.string()) + ".%%%%%%.bc");
    if (auto error = llvm::sys::fs::createUniqueFile(pattern.string(), descriptor, reserved))
      throw std::runtime_error("cannot reserve native capture: " + error.message());
    close(descriptor);
    capture = reserved.str().str();
    setenv("SELA_CAPTURE_PATH", capture.c_str(), 1);
    dependencies->attachToPreprocessor(compiler.getPreprocessor());
  }

  void HandleTranslationUnit(clang::ASTContext &) override {
    if (compiler.getDiagnostics().hasErrorOccurred()) return;
    try {
      llvm::json::Array recordedDependencies;
      std::set<std::string> seen;
      auto record = [&](llvm::StringRef name) -> llvm::Error {
        fs::path path = absolutePath(name.str());
        if (!seen.insert(path.string()).second) return llvm::Error::success();
        if (auto file = compiler.getFileManager().getOptionalFileRef(path.string())) {
          // File contents live in SourceManager's per-file cache, independently
          // of the macro-expansion/location table. Ask for that buffer directly:
          // translateFile scans every source location for each dependency and
          // needlessly ties provenance to the complete expansion inventory.
          // In particular, do not reopen a consumed file to hash newer bytes.
          auto buffer = compiler.getSourceManager().getMemoryBufferForFileOrNone(*file);
          if (!buffer) return fail("cannot hash the compiler's consumed input: " + path.string());
          recordedDependencies.push_back(llvm::json::Object{
              {"path", path.string()}, {"sha256", digest(buffer->getBuffer())}});
          return llvm::Error::success();
        }
        auto bytes = read(path);
        if (!bytes) return bytes.takeError();
        recordedDependencies.push_back(llvm::json::Object{
            {"path", path.string()}, {"sha256", digest(*bytes)}});
        return llvm::Error::success();
      };
      auto source = compiler.getFrontendOpts().Inputs.front().getFile();
      if (auto error = record(source)) { diagnose(compiler, std::move(error)); return; }
      for (const auto &path : dependencies->getDependencies())
        if (auto error = record(path)) { diagnose(compiler, std::move(error)); return; }
      const fs::path immutableJournal = capture.string() + ".compile.json";
      auto contents = jsonText(llvm::json::Object{
          {"source", normalize(absolutePath(source.str()).string())},
          {"key", normalize(output.string())}, {"flags", semanticFlags()},
          {"optimization", optimization(compiler.getCodeGenOpts())},
          {"profile", profile}, {"capture", capture.string()},
          {"dependencies", std::move(recordedDependencies)}});
      if (auto error = write(immutableJournal, contents)) {
        diagnose(compiler, std::move(error)); return;
      }
      setenv("SELA_CAPTURE_RECORD", immutableJournal.c_str(), 1);
    } catch (const std::exception &error) {
      diagnose(compiler, fail(error.what()));
    }
  }
};

class NativeCaptureAction final : public clang::PluginASTAction {
public:
  ActionType getActionType() override { return AddBeforeMainAction; }
  bool ParseArgs(const clang::CompilerInstance &,
                 const std::vector<std::string> &) override { return true; }
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance &compiler, llvm::StringRef) override {
    const char *metadata = std::getenv("SELA_BUILD_METADATA");
    const char *lane = std::getenv("SELA_BUILD_LANE");
    const char *profile = std::getenv("SELA_BUILD_PROFILE");
    if (!metadata || !*metadata || !lane || !profile ||
        compiler.getFrontendOpts().ProgramAction != clang::frontend::EmitObj ||
        compiler.getFrontendOpts().OutputFile.empty() ||
        compiler.getFrontendOpts().OutputFile == "-")
      return std::make_unique<clang::ASTConsumer>();
    try {
      return std::make_unique<CaptureConsumer>(compiler, absolutePath(lane),
                                               absolutePath(metadata), profile);
    } catch (const std::exception &error) {
      diagnose(compiler, fail(error.what()));
      return std::make_unique<clang::ASTConsumer>();
    }
  }
};

class CaptureFinishConsumer final : public clang::ASTConsumer {
  clang::CompilerInstance &compiler;
  fs::path output, metadata, lane;

public:
  CaptureFinishConsumer(clang::CompilerInstance &compiler, fs::path metadata,
                        fs::path lane)
      : compiler(compiler),
        output(absolutePath(compiler.getFrontendOpts().OutputFile)),
        metadata(std::move(metadata)), lane(std::move(lane)) {}

  void HandleTranslationUnit(clang::ASTContext &) override {
    if (compiler.getDiagnostics().hasErrorOccurred()) return;
    try {
      // Pinned Clang 18.1.3 calls BackendConsumer::HandleTranslationUnit
      // synchronously before AddAfterMainAction consumers. EmitBackendOutput
      // owns and destroys its output stream before returning. Native SDK
      // -fno-temp-file makes those finished bytes accessible at the requested
      // output path (the default temporary is renamed only later).
      if (compiler.getFrontendOpts().UseTemporary) {
        diagnose(compiler, fail("native capture cannot finalize temporary output")); return;
      }
      // Compile-only configure checks may intentionally discard output.
      // They never supply an object to a captured publication link.
      std::error_code ec;
      if (fs::exists(output, ec) && !fs::is_regular_file(output, ec)) return;
      const char *capture = std::getenv("SELA_CAPTURE_PATH");
      if (!capture || !*capture) {
        diagnose(compiler, fail("native capture completion has no capture path")); return;
      }
      const fs::path record = std::string(capture) + ".compile.json";
      if (absolutePath(record).parent_path() != metadata) {
        diagnose(compiler, fail("native capture completion journal escapes metadata")); return;
      }
      auto journal = readJson(record);
      if (!journal) { diagnose(compiler, journal.takeError()); return; }
      auto *item = journal->getAsObject();
      const auto key = replaceAll(output.string(), lane.string(), "$PRIVATE");
      if (!item || item->getString("capture") != capture ||
          item->getString("key") != key || !item->getString("capture_sha256") ||
          item->get("native_sha256")) {
        diagnose(compiler, fail("invalid provisional native capture journal")); return;
      }
      auto captured = read(capture);
      if (!captured) { diagnose(compiler, captured.takeError()); return; }
      if (item->getString("capture_sha256") != digest(*captured)) {
        diagnose(compiler, fail("native capture changed before codegen completion")); return;
      }
      auto native = read(output);
      if (!native) { diagnose(compiler, native.takeError()); return; }
      const auto nativeName = output.string();
      auto object = llvm::object::ObjectFile::createObjectFile(
          llvm::MemoryBufferRef(*native, nativeName));
      if (!object) { diagnose(compiler, object.takeError()); return; }
      unsigned markers = 0;
      const std::string expected = record.string() + '\0';
      for (const auto &section : (*object)->sections()) {
        auto name = section.getName();
        if (!name) { diagnose(compiler, name.takeError()); return; }
        if (*name != ".sela.capture") continue;
        auto contents = section.getContents();
        if (!contents) { diagnose(compiler, contents.takeError()); return; }
        if (*contents != expected || ++markers != 1) {
          diagnose(compiler, fail("completed object has invalid capture provenance")); return;
        }
      }
      if (markers != 1) {
        diagnose(compiler, fail("completed object is missing its capture provenance")); return;
      }
      (*item)["native_sha256"] = digest(*native);
      const fs::path staged = record.string() + ".finalizing";
      if (auto error = write(staged, jsonText(std::move(*journal)))) {
        diagnose(compiler, std::move(error)); return;
      }
      fs::rename(staged, record);
    } catch (const std::exception &error) {
      diagnose(compiler, fail(error.what()));
    }
  }
};

class NativeCaptureFinishAction final : public clang::PluginASTAction {
public:
  ActionType getActionType() override { return AddAfterMainAction; }
  bool ParseArgs(const clang::CompilerInstance &,
                 const std::vector<std::string> &) override { return true; }
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance &compiler, llvm::StringRef) override {
    const char *metadata = std::getenv("SELA_BUILD_METADATA");
    const char *lane = std::getenv("SELA_BUILD_LANE");
    if (!metadata || !*metadata || !lane ||
        !std::getenv("SELA_BUILD_PROFILE") ||
        compiler.getFrontendOpts().ProgramAction != clang::frontend::EmitObj ||
        compiler.getFrontendOpts().OutputFile.empty() ||
        compiler.getFrontendOpts().OutputFile == "-")
      return std::make_unique<clang::ASTConsumer>();
    try {
      return std::make_unique<CaptureFinishConsumer>(compiler,
          absolutePath(metadata), absolutePath(lane));
    } catch (const std::exception &error) {
      diagnose(compiler, fail(error.what()));
      return std::make_unique<clang::ASTConsumer>();
    }
  }
};

static clang::FrontendPluginRegistry::Add<SelaAction> producer(
    "sela", "emit a standalone architecture-neutral Sela object");
static clang::FrontendPluginRegistry::Add<NativeCaptureAction> capture(
    "selacapture", "observe private native SDK build captures");
static clang::FrontendPluginRegistry::Add<NativeCaptureFinishAction> finish(
    "selacapturefinish", "bind completed native objects to private captures");
} // namespace
