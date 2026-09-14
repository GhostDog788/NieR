#include "sela/IR/CompilationUnits.h"
#include "Internal.h"
#include "sela/Targets.h"
#include "sela/IR/Domains.h"
#include "mlir/IR/Builders.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <set>

namespace sela {
llvm::Error lowerCompilationUnit(llvm::ArrayRef<llvm::StringRef> fragments,
                                llvm::StringRef profile,
                                llvm::StringRef llvmIROutput) {
  auto fail = [](llvm::StringRef text) {
    return llvm::createStringError(std::make_error_code(std::errc::invalid_argument), text);
  };
  if (fragments.empty() || fragments.size() > 512 ||
      !targets::find(profile))
    return fail("invalid Sela compilation-unit contract");
  mlir::MLIRContext context;
  llvm::LLVMContext nativeContext;
  mlir::OwningOpRef<mlir::ModuleOp> combined;
  std::map<std::string, mlir::Operation *> symbols;
  llvm::SmallVector<mlir::Attribute> flags;
  for (auto path : fragments) {
    auto source = readModuleStructure(path, context);
    if (!source) return source.takeError();
    auto selected = ir::specializeDomains(**source, *targets::find(profile));
    if (!selected) return selected.takeError();
    if (!combined) {
      combined = mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));
      combined->getOperation()->setAttrs((*selected)->getOperation()->getAttrs());
    }
    if (auto moduleFlags = (*selected)->getOperation()->getAttrOfType<mlir::ArrayAttr>("sela.module_flags"))
      for (auto flag : moduleFlags)
        if (!llvm::is_contained(flags, flag)) flags.push_back(flag);
    for (auto &operation : (*selected)->getBody()->getOperations()) {
      auto identity = operation.getAttrOfType<mlir::StringAttr>("id");
      if (!identity) return fail("fragment definition has no explicit symbol identity");
      auto found = symbols.find(identity.getValue().str());
      if (found == symbols.end()) {
        auto *copy = operation.clone();
        combined->getBody()->push_back(copy);
        symbols[identity.getValue().str()] = copy;
        continue;
      }
      auto *previous = found->second;
      auto isDeclaration = [](mlir::Operation *value) {
        auto declaration = value->getAttrOfType<mlir::BoolAttr>("declaration");
        return declaration && declaration.getValue();
      };
      if (previous->getName() != operation.getName() ||
          (!isDeclaration(previous) && !isDeclaration(&operation)))
        return fail("fragment group has conflicting or duplicate native definitions");
      for (auto attribute : {"type", "element", "native_abi", "variadic"})
        if (previous->getAttr(attribute) != operation.getAttr(attribute))
          return fail("fragment symbol declarations have incompatible native types");
      if (isDeclaration(previous) && isDeclaration(&operation)) {
        if (previous->getAttrs() != operation.getAttrs())
          return fail("fragment group has inconsistent symbol declarations");
      } else if (isDeclaration(previous)) {
        auto *copy = operation.clone();
        previous->getBlock()->getOperations().insert(previous->getIterator(), copy);
        previous->erase();
        found->second = copy;
      }
    }
  }
  combined->getOperation()->setAttr("sela.module_flags", mlir::ArrayAttr::get(&context, flags));
  // Lower once, after the complete Sela symbol inventory has been assembled.
  // NativeLowering allocates all symbols before emitting initializers/bodies,
  // so private symbols keep their identities and no LLVM linker renames them.
  auto result = detail::lowerModule(*combined, nativeContext, profile);
  if (!result) return result.takeError();
  if (llvm::verifyModule(**result)) return fail("reconstructed Sela translation unit is invalid");
  std::error_code error;
  llvm::raw_fd_ostream output(llvmIROutput, error, llvm::sys::fs::OF_Text);
  if (error) return llvm::errorCodeToError(error);
  (*result)->print(output, nullptr);
  output.flush();
  if (output.has_error()) return fail("cannot write reconstructed native translation unit");
  return llvm::Error::success();
}
}
