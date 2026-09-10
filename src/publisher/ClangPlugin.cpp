#include "nier/Artifact/Artifact.h"
#include "nier/IR/Compiler.h"
#include "nier/Producer/LLVM.h"
#include "nier/Producer/Partitions.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Basic/CodeGenOptions.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <unistd.h>

using namespace nier::driver;

namespace {
void diagnose(clang::CompilerInstance &compiler, llvm::Error error) {
  unsigned id = compiler.getDiagnostics().getCustomDiagID(
      clang::DiagnosticsEngine::Error, "Nier: %0");
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
// It does not inspect the AST or implement a C-to-Nier lowering.
llvm::Error captureSource(const clang::CompilerInvocation &original,
                          const Sdk &sdk, llvm::StringRef profile,
                          const fs::path &capture, const fs::path &object,
                          bool preserveDependencies,
                          const fs::path &dependencyOutput = {}) {
  clang::CompilerInvocation invocation(original);
  const auto &originalTarget = original.getTargetOpts();
  if ((!originalTarget.CPU.empty() && originalTarget.CPU != "x86-64" &&
       originalTarget.CPU != "i686") ||
      (!originalTarget.TuneCPU.empty() && originalTarget.TuneCPU != "generic") ||
      !originalTarget.FeaturesAsWritten.empty() || !originalTarget.ABI.empty() ||
      !originalTarget.FPMath.empty())
    return fail("explicit CPU/features/ABI tuning is not qualified by the neutral source producer");
  if (!original.getCodeGenOpts().PassPlugins.empty())
    return fail("additional LLVM pass plugins are not qualified by the source producer");
  clearFrontendPlugins(invocation);
  invocation.getFrontendOpts().OutputFile = object.string();
  if (!preserveDependencies)
    invocation.getDependencyOutputOpts() = clang::DependencyOutputOptions();
  else if (!dependencyOutput.empty())
    invocation.getDependencyOutputOpts().OutputFile = dependencyOutput.string();
  auto &target = invocation.getTargetOpts();
  target.Triple = profile == "x86_64" ? "x86_64-unknown-linux-gnu"
                                      : "i686-unknown-linux-gnu";
  target.CPU = profile == "x86_64" ? "x86-64" : "i686";
  target.TuneCPU = "generic";
  target.FeaturesAsWritten.clear();
  target.Features.clear();
  target.FeatureMap.clear();
  target.ABI.clear();
  target.FPMath.clear();

  auto &headers = invocation.getHeaderSearchOpts();
  const std::string oldSysroot = headers.Sysroot;
  const std::string oldResource = headers.ResourceDir;
  headers.Sysroot = sdk.sysroot(profile).string();
  headers.ResourceDir = (sdk.root / "host/usr/lib/llvm-18/lib/clang/18").string();
  // Keep explicit project include paths; regenerate only the configured SDK
  // search entries for the selected native profile. Host system paths are not
  // silently accepted as an application's target SDK.
  auto &entries = headers.UserEntries;
  const std::set<std::string> standardSdkEntries = {
      oldResource + "/include", oldSysroot + "/usr/local/include",
      oldSysroot + "/include", oldSysroot + "/usr/include",
      oldSysroot + "/usr/include/x86_64-linux-gnu",
      oldSysroot + "/usr/include/i386-linux-gnu"};
  for (const auto &entry : entries) {
    llvm::StringRef path(entry.Path);
    if (path.starts_with("/usr/include") || path.starts_with("/usr/local/include") ||
        path.starts_with("/usr/lib/gcc") || path.starts_with("/include"))
      return fail("host system include paths are not a target SDK; use the supplied nier.cfg");
    if (!oldSysroot.empty() && oldSysroot != "/" &&
        path.starts_with(oldSysroot + "/") && !standardSdkEntries.count(path.str()))
      return fail("custom profile-specific SDK include paths require the paired native-build integration");
  }
  entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto &entry) {
    return standardSdkEntries.count(entry.Path) != 0;
  }), entries.end());
  headers.UseStandardSystemIncludes = false;
  headers.AddPath(headers.ResourceDir + "/include", clang::frontend::System,
                  false, true);
  const std::string nativeTriple = profile == "x86_64" ? "x86_64-linux-gnu"
                                                        : "i386-linux-gnu";
  headers.AddPath((sdk.sysroot(profile) / "usr/include" / nativeTriple).string(),
                  clang::frontend::ExternCSystem, false, true);
  headers.AddPath((sdk.sysroot(profile) / "usr/include").string(),
                  clang::frontend::ExternCSystem, false, true);

  auto &codegen = invocation.getCodeGenOpts();
  codegen.PassPlugins = {NIER_CAPTURE_PLUGIN};
  codegen.setDebugInfo(llvm::codegenoptions::FullDebugInfo);
  std::vector<std::string> command{sdk.tool("clang").string(), "-cc1"};
  auto flags = invocation.getCC1CommandLine();
  command.insert(command.end(), flags.begin(), flags.end());
  if (auto error = run(command, {}, {{"NIER_CAPTURE_PATH", capture.string()},
                                    {"NIER_BUILD_METADATA", ""}}))
    return error;
  if (!fs::is_regular_file(capture))
    return fail("stock Clang did not run the qualified LLVM capture hook");
  return llvm::Error::success();
}

class NierAction final : public clang::PluginASTAction {
  std::string mode = "source", peer, requestedOptimization, sdkArgument;
  std::vector<std::string> groupLeft, groupRight;
  bool keepWork = false;
  clang::DependencyOutputOptions requestedDependencies;

  llvm::Error emit(clang::CompilerInstance &compiler) {
    const auto &invocation = compiler.getInvocation();
    const auto &frontend = invocation.getFrontendOpts();
    if (frontend.OutputFile.empty() || frontend.OutputFile == "-")
      return fail("publication requires a file output (-o)");
    if (sameFile(frontend.OutputFile, getCurrentFile().str()) ||
        (!peer.empty() && sameFile(frontend.OutputFile, peer)))
      return fail("the Nier output must not overwrite a compiler input");
    for (const auto *inputs : {&groupLeft, &groupRight})
      for (const auto &input : *inputs)
        if (sameFile(frontend.OutputFile, input)) return fail("grouped output aliases a native capture");
    auto scratch = Scratch::create();
    if (!scratch) return scratch.takeError();
    scratch->keep = keepWork;
    if (keepWork) llvm::errs() << "Private Nier producer workspace: " << scratch->path.string() << '\n';
    fs::path left, right;
    std::string dependencyText;
    std::string opt = optimization(invocation.getCodeGenOpts());
    if (mode == "group") {
      if (getCurrentFileKind().getLanguage() != clang::Language::LLVM_IR ||
          groupLeft.empty() || groupRight.empty() ||
          !sameFile(getCurrentFile().str(), groupLeft.front()))
        return fail("grouped capture mode requires ordered left/right LLVM captures");
      if (!requestedOptimization.empty()) opt = requestedOptimization;
      auto partition = nier::mergeProfilePartitions(groupLeft, groupRight, scratch->path.string());
      if (!partition) return partition.takeError();
      std::vector<ArtifactModule> modules;
      for (const auto &path : partition->fragments) {
        auto bytes = read(path);
        if (!bytes) return bytes.takeError();
        modules.push_back({std::move(*bytes), opt});
      }
      CompilationPlan plan;
      for (const auto &unit : partition->x64Units) plan["x86_64"].push_back({unit, opt});
      for (const auto &unit : partition->i686Units) plan["i686"].push_back({unit, opt});
      auto artifact = createArtifact("object", modules, {}, {}, {"x86_64", "i686"}, {}, plan);
      if (!artifact) return artifact.takeError();
      return writePackage(absolutePath(frontend.OutputFile), *artifact);
    }
    if (mode == "pair") {
      if (getCurrentFileKind().getLanguage() != clang::Language::LLVM_IR || peer.empty())
        return fail("paired capture mode requires -x ir and peer=<i686 LLVM capture>");
      left = absolutePath(getCurrentFile().str());
      right = absolutePath(peer);
      if (!requestedOptimization.empty()) opt = requestedOptimization;
    } else {
      if (getCurrentFileKind().getLanguage() != clang::Language::C ||
          getCurrentFileKind().isPreprocessed())
        return fail("the current source producer accepts ordinary C input, not preprocessed/other-language input");
      const char *sdkEnvironment = std::getenv("NIER_SDK_ROOT");
      Sdk sdk{absolutePath(!sdkArgument.empty() ? sdkArgument :
                       sdkEnvironment ? sdkEnvironment : NIER_DEFAULT_SDK)};
      if (auto error = sdk.validate(true)) return error;
      left = scratch->path / "x86_64.bc";
      right = scratch->path / "i686.bc";
      clang::CompilerInvocation sourceInvocation(invocation);
      sourceInvocation.getDependencyOutputOpts() = requestedDependencies;
      const bool emitDependencies = !requestedDependencies.OutputFile.empty();
      const fs::path deps64 = emitDependencies ? scratch->path / "x86_64.d" : fs::path();
      const fs::path deps32 = emitDependencies ? scratch->path / "i686.d" : fs::path();
      if (auto error = captureSource(sourceInvocation, sdk, "x86_64", left,
                                    scratch->path / "x86_64.o", true, deps64)) return error;
      if (auto error = captureSource(sourceInvocation, sdk, "i686", right,
                                    scratch->path / "i686.o", emitDependencies, deps32)) return error;
      if (emitDependencies) {
        auto first = read(deps64), second = read(deps32);
        if (!first) return first.takeError();
        if (!second) return second.takeError();
        // Make combines prerequisites from repeated rules for the same target.
        // Preserve stock Clang's escaping and include both profile-specific
        // header sets without implementing a second Make dependency parser.
        dependencyText = *first + *second;
      }
    }
    const fs::path common = scratch->path / "unit.nierbc";
    if (auto error = nier::mergeProfiles(left.string(), right.string(), common.string()))
      return error;
    auto bytes = read(common);
    if (!bytes) return bytes.takeError();
    auto artifact = createArtifact("object", {{std::move(*bytes), opt}});
    if (!artifact) return artifact.takeError();
    if (!dependencyText.empty()) {
      if (requestedDependencies.OutputFile == "-") llvm::outs() << dependencyText;
      else if (auto error = write(requestedDependencies.OutputFile, dependencyText)) return error;
    }
    return writePackage(absolutePath(frontend.OutputFile), *artifact);
  }

public:
  // Explicit -plugin nier still selects this as the main frontend action.
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
      else if (arg.consume_front("peer=")) peer = arg.str();
      else if (arg.consume_front("left=")) groupLeft.push_back(arg.str());
      else if (arg.consume_front("right=")) groupRight.push_back(arg.str());
      else if (arg.consume_front("optimization=")) requestedOptimization = arg.str();
      else if (arg.consume_front("sdk=")) sdkArgument = arg.str();
      else if (arg == "keep-work") keepWork = true;
      else {
        unsigned id = compiler.getDiagnostics().getCustomDiagID(
            clang::DiagnosticsEngine::Error, "unknown Nier plugin argument: %0");
        compiler.getDiagnostics().Report(id) << argument;
        return false;
      }
    }
    if (mode != "source" && mode != "pair" && mode != "group") {
      unsigned id = compiler.getDiagnostics().getCustomDiagID(
          clang::DiagnosticsEngine::Error, "unsupported Nier producer mode: %0");
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
  std::string profile;

  std::string normalize(std::string text) const {
    return replaceAll(std::move(text), lane.string(), "$PRIVATE");
  }

  llvm::json::Array semanticFlags() const {
    const auto &invocation = compiler.getInvocation();
    const auto &headers = invocation.getHeaderSearchOpts();
    const std::set<std::string> optionWithValue = {
        "-o", "-triple", "-target-cpu", "-tune-cpu", "-target-feature",
        "-isysroot", "-resource-dir", "-internal-isystem", "-internal-externc-isystem",
        "-main-file-name", "-dumpdir", "-load", "-plugin", "-dependency-file",
        "-MT", "-fdebug-compilation-dir", "-fcoverage-compilation-dir"};
    llvm::json::Array result;
    auto flags = invocation.getCC1CommandLine();
    const std::string source = compiler.getFrontendOpts().Inputs.front().getFile().str();
    for (size_t index = 0; index < flags.size(); ++index) {
      llvm::StringRef flag(flags[index]);
      if (optionWithValue.count(flag.str())) { ++index; continue; }
      if (flag == source || flag == "-disable-free" ||
          flag.starts_with("-fpass-plugin=") || flag.starts_with("-plugin-arg-")) {
        if (flag.starts_with("-plugin-arg-")) ++index;
        continue;
      }
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
        output(absolutePath(compiler.getFrontendOpts().OutputFile)), profile(std::move(profile)) {
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
    setenv("NIER_CAPTURE_PATH", capture.c_str(), 1);
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
          auto id = compiler.getSourceManager().translateFile(*file);
          if (id.isValid()) {
            bool invalid = false;
            auto buffer = compiler.getSourceManager().getBufferData(id, &invalid);
            if (invalid) return fail("cannot hash the compiler's consumed input: " + path.string());
            recordedDependencies.push_back(llvm::json::Object{
                {"path", path.string()}, {"sha256", digest(buffer)}});
            return llvm::Error::success();
          }
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
      setenv("NIER_CAPTURE_RECORD", immutableJournal.c_str(), 1);
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
    const char *metadata = std::getenv("NIER_BUILD_METADATA");
    const char *lane = std::getenv("NIER_BUILD_LANE");
    const char *profile = std::getenv("NIER_BUILD_PROFILE");
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
      const char *capture = std::getenv("NIER_CAPTURE_PATH");
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
        if (*name != ".nier.capture") continue;
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
    const char *metadata = std::getenv("NIER_BUILD_METADATA");
    const char *lane = std::getenv("NIER_BUILD_LANE");
    if (!metadata || !*metadata || !lane ||
        !std::getenv("NIER_BUILD_PROFILE") ||
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

static clang::FrontendPluginRegistry::Add<NierAction> producer(
    "nier", "emit a standalone architecture-neutral Nier object");
static clang::FrontendPluginRegistry::Add<NativeCaptureAction> capture(
    "niercapture", "observe private native SDK build captures");
static clang::FrontendPluginRegistry::Add<NativeCaptureFinishAction> finish(
    "niercapturefinish", "bind completed native objects to private captures");
} // namespace
