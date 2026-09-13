#include "sela/Producer/Partitions.h"
#include "sela/Producer/LLVM.h"
#include "sela/Targets.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <filesystem>
#include <map>
#include <set>

namespace sela {
namespace {
llvm::Error fail(const llvm::Twine &text) {
  return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), text);
}
using NativeModules = std::vector<std::unique_ptr<llvm::Module>>;
using Ownership = std::map<std::string, size_t>;
llvm::Expected<NativeModules> load(llvm::ArrayRef<std::string> paths,
                                 llvm::LLVMContext &context, Ownership &owners) {
  NativeModules modules;
  if (paths.empty() || paths.size() > 256) return fail("invalid grouped native capture inventory");
  for (const auto &path : paths) {
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseIRFile(path, diagnostic, context);
    if (!module || llvm::verifyModule(*module)) return fail("invalid grouped native capture: " + path);
    unsigned definitions = 0;
    for (const auto &value : module->global_values()) {
      if (value.isDeclaration() || value.hasLocalLinkage() || value.hasAvailableExternallyLinkage()) continue;
      if (!value.hasExternalLinkage() || value.getName().empty() ||
          !owners.emplace(value.getName().str(), modules.size()).second)
        return fail("ambiguous grouped native definition: " + value.getName());
      ++definitions;
    }
    if (!definitions) return fail("grouped native unit lacks a unique external definition: " + path);
    modules.push_back(std::move(module));
  }
  return modules;
}
bool scalar(llvm::Type *type) {
  return type->isVoidTy() || type->isIntegerTy() || type->isPointerTy() ||
         type->isFloatTy() || type->isDoubleTy();
}
llvm::Error qualifySplit(const llvm::Module &module) {
  if (!module.global_empty() || !module.alias_empty() || !module.ifunc_empty() ||
      !module.getModuleInlineAsm().empty())
    return fail("regrouped native units require explicit global/local identity support");
  for (const auto &function : module) {
    if (function.getName().starts_with("llvm.dbg.")) continue;
    if (function.isDeclaration() || !function.hasExternalLinkage() || function.isVarArg() ||
        !scalar(function.getReturnType()) || function.hasComdat() || function.hasSection())
      return fail("regrouped native units currently require self-contained external scalar definitions: " + function.getName());
    for (const auto &argument : function.args())
      if (!scalar(argument.getType())) return fail("regrouped aggregate ABI needs explicit ownership");
    for (const auto &instruction : llvm::instructions(function))
      if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
          alloca && !scalar(alloca->getAllocatedType()))
        return fail("regrouped aggregate storage needs explicit ownership");
  }
  return llvm::Error::success();
}
llvm::Expected<std::unique_ptr<llvm::Module>> slice(
    const llvm::Module &module, const std::set<std::string> &definitions) {
  auto result = llvm::CloneModule(module);
  std::vector<llvm::Function *> removed;
  for (auto &function : *result)
    if (!function.isDeclaration() && !definitions.count(function.getName().str()))
      removed.push_back(&function);
  for (auto *function : removed) function->dropAllReferences();
  for (auto *function : removed) {
    if (!function->use_empty())
      return fail("cross-fragment native definition reference needs an explicit shared identity: " + function->getName());
    function->eraseFromParent();
  }
  if (llvm::verifyModule(*result)) return fail("invalid private native definition slice");
  return result;
}
std::string evidenceText(const llvm::Module &module) {
  auto copy = llvm::CloneModule(module);
  llvm::StripDebugInfo(*copy);
  copy->setModuleIdentifier("sela-partition-proof");
  copy->setSourceFileName("sela-partition-proof");
  if (auto *ident = copy->getNamedMetadata("llvm.ident")) copy->eraseNamedMetadata(ident);
  std::vector<llvm::Function *> debug;
  for (auto &function : *copy)
    if (function.getName().starts_with("llvm.dbg.")) debug.push_back(&function);
  for (auto *function : debug) function->eraseFromParent();
  std::string text;
  llvm::raw_string_ostream stream(text);
  copy->print(stream, nullptr);
  return text;
}
llvm::Error provePartition(const llvm::Module &original,
                          llvm::ArrayRef<std::unique_ptr<llvm::Module>> fragments) {
  std::unique_ptr<llvm::Module> reconstructed;
  for (const auto &fragment : fragments) {
    auto copy = llvm::CloneModule(*fragment);
    if (!reconstructed) reconstructed = std::move(copy);
    else if (llvm::Linker::linkModules(*reconstructed, std::move(copy)))
      return fail("cannot reconstruct original native translation unit from private slices");
  }
  if (!reconstructed || llvm::verifyModule(*reconstructed) ||
      evidenceText(original) != evidenceText(*reconstructed))
    return fail("private partition does not exactly reconstruct the original native translation unit");
  return llvm::Error::success();
}
llvm::Error writeCapture(const llvm::Module &module, const std::filesystem::path &path) {
  std::error_code error;
  llvm::raw_fd_ostream stream(path.string(), error, llvm::sys::fs::OF_None);
  if (error) return llvm::errorCodeToError(error);
  llvm::WriteBitcodeToFile(module, stream);
  stream.flush();
  if (stream.has_error()) return fail("cannot write private native partition capture");
  return llvm::Error::success();
}
}
llvm::Expected<MergedPartitions> mergeProfilePartitions(
    llvm::ArrayRef<ProfilePartitionInput> observations,
    llvm::StringRef privateDirectory) {
  if (observations.empty() || observations.size() > 256) return fail("invalid grouped target observation inventory");
  struct Profile {
    std::unique_ptr<llvm::LLVMContext> context;
    NativeModules modules;
    Ownership owners;
  };
  std::vector<Profile> profiles;
  std::set<std::string> targets;
  for (const auto &observation : observations) {
    if (!sela::targets::find(observation.target) || !targets.insert(observation.target).second)
      return fail("unknown or duplicate grouped target observation");
    auto context = std::make_unique<llvm::LLVMContext>();
    Ownership owners;
    auto modules = load(observation.captures, *context, owners);
    if (!modules) return modules.takeError();
    if (!profiles.empty()) {
      if (owners.size() != profiles.front().owners.size())
        return fail("target-conditioned definition presence needs additional common IR semantics");
      for (const auto &[name, unit] : profiles.front().owners)
        if (!owners.count(name)) return fail("unmatched grouped native definition: " + name);
    }
    profiles.push_back({std::move(context), std::move(*modules), std::move(owners)});
  }
  struct Fragment { std::vector<size_t> owners; std::set<std::string> definitions; };
  std::vector<Fragment> fragments;
  std::map<std::vector<size_t>, size_t> indices;
  MergedPartitions result;
  for (size_t i = 0; i < profiles.size(); ++i)
    result.unitsByTarget[observations[i].target].resize(profiles[i].modules.size());
  // Use physical definition order, not symbol-name sorting, for native layout.
  auto &first = profiles.front();
  for (size_t i = 0; i < first.modules.size(); ++i)
    for (const auto &value : first.modules[i]->global_values()) {
      if (!first.owners.count(value.getName().str()) || first.owners.at(value.getName().str()) != i) continue;
      std::vector<size_t> owners;
      for (const auto &profile : profiles) owners.push_back(profile.owners.at(value.getName().str()));
      auto [found, inserted] = indices.emplace(owners, fragments.size());
      if (inserted) {
        fragments.push_back({owners, {}});
      }
      fragments[found->second].definitions.insert(value.getName().str());
    }
  for (size_t p = 0; p < profiles.size(); ++p) {
    const auto &profile = profiles[p];
    for (size_t i = 0; i < profile.modules.size(); ++i) {
      std::set<size_t> seen;
      for (const auto &value : profile.modules[i]->global_values()) {
        if (!profile.owners.count(value.getName().str()) || profile.owners.at(value.getName().str()) != i) continue;
        std::vector<size_t> owners;
        for (const auto &other : profiles) owners.push_back(other.owners.at(value.getName().str()));
        size_t index = indices.at(owners);
        if (seen.insert(index).second) result.unitsByTarget[observations[p].target][i].push_back(index);
      }
    }
  }
  const auto directory = std::filesystem::path(privateDirectory.str());
  std::filesystem::create_directories(directory);
  std::vector<std::vector<std::string>> fragmentPaths(profiles.size(), std::vector<std::string>(fragments.size()));
  for (size_t p = 0; p < profiles.size(); ++p) {
    auto &modules = profiles[p].modules;
    const auto &units = result.unitsByTarget[observations[p].target];
    const auto &originalPaths = observations[p].captures;
    auto &paths = fragmentPaths[p];
    for (size_t unit = 0; unit < units.size(); ++unit) {
      if (units[unit].size() == 1) {
        paths[units[unit].front()] = originalPaths[unit];
        continue;
      }
      if (auto error = qualifySplit(*modules[unit])) return error;
      NativeModules slices;
      for (size_t index : units[unit]) {
        auto fragment = slice(*modules[unit], fragments[index].definitions);
        if (!fragment) return fragment.takeError();
        auto path = directory / (observations[p].target + "-" + std::to_string(index) + ".bc");
        if (auto error = writeCapture(**fragment, path)) return error;
        paths[index] = path.string();
        slices.push_back(std::move(*fragment));
      }
      if (auto error = provePartition(*modules[unit], slices)) return error;
    }
  }
  for (size_t i = 0; i < fragments.size(); ++i) {
    auto output = directory / ("fragment-" + std::to_string(i) + ".selabc");
    std::vector<CaptureObservation> captures;
    for (size_t p = 0; p < profiles.size(); ++p) captures.push_back({observations[p].target, fragmentPaths[p][i]});
    if (auto error = mergeProfiles(captures, output.string())) return error;
    result.fragments.push_back(output.string());
  }
  return result;
}
}
