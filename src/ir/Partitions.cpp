#include "nier/Producer/Partitions.h"
#include "nier/Producer/LLVM.h"
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

namespace nier {
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
  copy->setModuleIdentifier("nier-partition-proof");
  copy->setSourceFileName("nier-partition-proof");
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
    llvm::ArrayRef<std::string> x64Captures, llvm::ArrayRef<std::string> i686Captures,
    llvm::StringRef privateDirectory) {
  llvm::LLVMContext leftContext, rightContext;
  Ownership leftOwners, rightOwners;
  auto left = load(x64Captures, leftContext, leftOwners);
  if (!left) return left.takeError();
  auto right = load(i686Captures, rightContext, rightOwners);
  if (!right) return right.takeError();
  if (leftOwners.size() != rightOwners.size())
    return fail("target-conditioned definition presence needs additional common IR semantics");
  for (const auto &[name, unit] : leftOwners)
    if (!rightOwners.count(name)) return fail("unmatched grouped native definition: " + name);
  struct Fragment { size_t left, right; std::set<std::string> definitions; };
  std::vector<Fragment> fragments;
  std::map<std::pair<size_t, size_t>, size_t> indices;
  MergedPartitions result;
  result.x64Units.resize(left->size()); result.i686Units.resize(right->size());
  // Use physical definition order, not symbol-name sorting, for native layout.
  for (size_t i = 0; i < left->size(); ++i)
    for (const auto &value : (*left)[i]->global_values()) {
      if (!leftOwners.count(value.getName().str()) || leftOwners.at(value.getName().str()) != i) continue;
      const size_t peer = rightOwners.at(value.getName().str());
      auto [found, inserted] = indices.emplace(std::make_pair(i, peer), fragments.size());
      if (inserted) {
        result.x64Units[i].push_back(fragments.size());
        fragments.push_back({i, peer, {}});
      }
      fragments[found->second].definitions.insert(value.getName().str());
    }
  for (size_t i = 0; i < right->size(); ++i) {
    std::set<size_t> seen;
    for (const auto &value : (*right)[i]->global_values()) {
      if (!rightOwners.count(value.getName().str()) || rightOwners.at(value.getName().str()) != i) continue;
      size_t index = indices.at({leftOwners.at(value.getName().str()), i});
      if (seen.insert(index).second) result.i686Units[i].push_back(index);
    }
  }
  const auto directory = std::filesystem::path(privateDirectory.str());
  std::filesystem::create_directories(directory);
  std::vector<std::string> leftPaths(fragments.size()), rightPaths(fragments.size());
  for (bool x64 : {true, false}) {
    auto &modules = x64 ? *left : *right;
    const auto &units = x64 ? result.x64Units : result.i686Units;
    const auto &originalPaths = x64 ? x64Captures : i686Captures;
    auto &paths = x64 ? leftPaths : rightPaths;
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
        auto path = directory / ((x64 ? "x64-" : "i686-") + std::to_string(index) + ".bc");
        if (auto error = writeCapture(**fragment, path)) return error;
        paths[index] = path.string();
        slices.push_back(std::move(*fragment));
      }
      if (auto error = provePartition(*modules[unit], slices)) return error;
    }
  }
  for (size_t i = 0; i < fragments.size(); ++i) {
    auto output = directory / ("fragment-" + std::to_string(i) + ".nierbc");
    if (auto error = mergeProfiles(leftPaths[i], rightPaths[i], output.string())) return error;
    result.fragments.push_back(output.string());
  }
  return result;
}
}
