#include "nier/Artifact/Artifact.h"
#include "nier/IR/Compiler.h"
#include "nier/IR/Dialect.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/Support/raw_ostream.h"

using namespace nier::driver;
namespace {
llvm::Error inspect(const fs::path &input, const fs::path &dump) {
  auto files = readPackage(input);
  if (!files) return files.takeError();
  auto manifest = validatePackage(*files);
  if (!manifest) return manifest.takeError();
  auto &record = *manifest->getAsObject();
  auto *modules = record.getArray("modules");
  if (!modules || modules->size() != 1 || files->size() != 2)
    return fail("conditional fixture must contain one shared NieR module and its manifest only");
  auto path = (*modules)[0].getAsObject()->getString("path");
  if (!path || !path->ends_with(".nierbc")) return fail("conditional fixture is not NieR bytecode");
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  auto bytecode = scratch->path / "fixture.nierbc";
  if (auto error = write(bytecode, files->at(path->str()))) return error;
  mlir::MLIRContext context;
  context.getOrLoadDialect<nier::ir::NIERDialect>();
  auto module = nier::readModule(bytecode.string(), context, nier::supportedNativeTargets());
  if (!module) return module.takeError();
  unsigned definitions = 0, chosenBodies = 0, dispatches = 0;
  bool validNamespaces = true, wideBlock = false, narrowBlock = false;
  bool wideCase = false, secondWideCase = false, narrowCase = false;
  (*module)->walk([&](mlir::Operation *operation) {
    auto name = operation->getName().getStringRef();
    if (name != "builtin.module" && !name.starts_with("nier.")) validNamespaces = false;
    if (name != "nier.func") return;
    auto declaration = operation->getAttrOfType<mlir::BoolAttr>("declaration");
    if (declaration && !declaration.getValue()) ++definitions;
    auto identity = operation->getAttrOfType<mlir::StringAttr>("id");
    if (!identity || identity.getValue() != "choose" || !declaration || declaration.getValue()) return;
    ++chosenBodies;
    auto blocks = operation->getAttrOfType<mlir::ArrayAttr>("block_domains");
    if (!blocks || operation->getNumRegions() != 1 ||
        blocks.size() != operation->getRegion(0).getBlocks().size()) return;
    for (auto domain : blocks) {
      auto value = mlir::dyn_cast<mlir::IntegerAttr>(domain);
      if (!value) continue;
      wideBlock |= value.getInt() == 1;
      narrowBlock |= value.getInt() == 2;
    }
    operation->walk([&](mlir::Operation *nested) {
      if (nested->getName().getStringRef() != "nier.switch") return;
      ++dispatches;
      auto cases = nested->getAttrOfType<mlir::ArrayAttr>("cases");
      auto domains = nested->getAttrOfType<mlir::ArrayAttr>("case_domains");
      if (!cases || !domains || cases.size() != domains.size()) return;
      for (unsigned index = 0; index < cases.size(); ++index) {
        auto label = mlir::dyn_cast<mlir::IntegerAttr>(cases[index]);
        auto domain = mlir::dyn_cast<mlir::IntegerAttr>(domains[index]);
        if (!label || !domain) continue;
        wideCase |= label.getInt() == 8 && domain.getInt() == 1;
        secondWideCase |= label.getInt() == 16 && domain.getInt() == 1;
        narrowCase |= label.getInt() == 4 && domain.getInt() == 2;
      }
    });
  });
  if (!validNamespaces || definitions != 3 || chosenBodies != 1 || dispatches != 1 ||
      !wideBlock || !narrowBlock || !wideCase || !secondWideCase || !narrowCase)
    return fail("conditional fixture lacks one shared body with both proved native-word masks");
  std::string readable;
  llvm::raw_string_ostream stream(readable);
  (*module)->print(stream);
  stream.flush();
  return write(dump, readable);
}
}
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  if (auto error = inspect(argv[1], argv[2])) {
    llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "conditional artifact: ");
    return 1;
  }
  return 0;
}
