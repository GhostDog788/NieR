#include "Dialect.h"
#include "mlir/IR/DialectImplementation.h"

namespace aot::ir {

AOTDialect::AOTDialect(mlir::MLIRContext *context)
    : Dialect(getDialectNamespace(), context, mlir::TypeID::get<AOTDialect>()) {
  addTypes<WordType, PointerType>();
  addOperations<FunctionOp, GlobalOp, ConstantOp, AddressOp, AllocaOp, LoadOp,
                StoreOp, CallOp, BinaryOp, CastOp, CompareOp, ReturnOp>();
}

mlir::Type AOTDialect::parseType(mlir::DialectAsmParser &parser) const {
  llvm::StringRef keyword;
  if (parser.parseKeyword(&keyword))
    return {};
  if (keyword == "word")
    return WordType::get(getContext());
  if (keyword == "ptr")
    return PointerType::get(getContext());
  parser.emitError(parser.getCurrentLocation(), "unknown aot type: ") << keyword;
  return {};
}

void AOTDialect::printType(mlir::Type type,
                            mlir::DialectAsmPrinter &printer) const {
  if (mlir::isa<WordType>(type))
    printer << "word";
  else if (mlir::isa<PointerType>(type))
    printer << "ptr";
  else
    llvm_unreachable("unregistered aot type");
}

} // namespace aot::ir
