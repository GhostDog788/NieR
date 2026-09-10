#include "nier/IR/Dialect.h"
#include "mlir/IR/DialectImplementation.h"

namespace nier::ir {

NIERDialect::NIERDialect(mlir::MLIRContext *context)
    : Dialect(getDialectNamespace(), context, mlir::TypeID::get<NIERDialect>()) {
  addTypes<WordType, PointerType, ArrayType, RecordType, VaListType, OverlapType>();
  addOperations<FunctionOp, GlobalOp, ConstantOp, AddressOp, AllocaOp, LoadOp,
                StoreOp, CallOp, IndirectCallOp, BinaryOp, CastOp, CompareOp, ReturnOp,
                BranchOp, CondBranchOp, SwitchOp, UnreachableOp, AddressIndexOp,
                SelectOp, NegateOp, ByteSwapOp, VaArgOp, VaForwardOp>();
}

mlir::Type NIERDialect::parseType(mlir::DialectAsmParser &parser) const {
  llvm::StringRef keyword;
  if (parser.parseKeyword(&keyword))
    return {};
  if (keyword == "word")
    return WordType::get(getContext());
  if (keyword == "ptr")
    return PointerType::get(getContext());
  if (keyword == "va_list")
    return VaListType::get(getContext());
  if (keyword == "array") {
    uint64_t count;
    mlir::Type element;
    if (parser.parseLess() || parser.parseInteger(count) || parser.parseComma() ||
        parser.parseType(element) || parser.parseGreater()) return {};
    return ArrayType::get(getContext(), element, count);
  }
  if (keyword == "word_array") {
    uint64_t count64, count32;
    mlir::Type element;
    if (parser.parseLess() || parser.parseInteger(count64) || parser.parseComma() ||
        parser.parseInteger(count32) || parser.parseComma() || parser.parseType(element) ||
        parser.parseGreater()) return {};
    return ArrayType::getForWordWidths(getContext(), element, count64, count32);
  }
  if (keyword == "overlap") {
    std::string identity;
    llvm::SmallVector<mlir::Type> alternatives;
    llvm::SmallVector<unsigned> domains;
    if (parser.parseLess() || parser.parseString(&identity) || parser.parseComma() || parser.parseLSquare()) return {};
    if (mlir::failed(parser.parseOptionalRSquare())) {
      do {
        mlir::Type type;
        if (parser.parseType(type)) return {};
        alternatives.push_back(type);
      } while (mlir::succeeded(parser.parseOptionalComma()));
      if (parser.parseRSquare()) return {};
    }
    if (parser.parseComma() || parser.parseLSquare()) return {};
    if (mlir::failed(parser.parseOptionalRSquare())) {
      do {
        unsigned domain;
        if (parser.parseInteger(domain)) return {};
        domains.push_back(domain);
      } while (mlir::succeeded(parser.parseOptionalComma()));
      if (parser.parseRSquare()) return {};
    }
    if (parser.parseGreater()) return {};
    return OverlapType::get(getContext(), identity, alternatives, domains);
  }
  if (keyword == "record") {
    std::string identity;
    unsigned packed;
    llvm::SmallVector<mlir::Type> fields;
    if (parser.parseLess() || parser.parseString(&identity) || parser.parseComma() ||
        parser.parseInteger(packed) || packed > 1 || parser.parseComma() || parser.parseLSquare()) return {};
    if (mlir::failed(parser.parseOptionalRSquare())) {
      do {
        mlir::Type field;
        if (parser.parseType(field)) return {};
        fields.push_back(field);
      } while (mlir::succeeded(parser.parseOptionalComma()));
      if (parser.parseRSquare()) return {};
    }
    if (parser.parseGreater()) return {};
    return RecordType::get(getContext(), identity, packed, fields);
  }
  parser.emitError(parser.getCurrentLocation(), "unknown nier type: ") << keyword;
  return {};
}

void NIERDialect::printType(mlir::Type type,
                            mlir::DialectAsmPrinter &printer) const {
  if (mlir::isa<WordType>(type))
    printer << "word";
  else if (mlir::isa<PointerType>(type))
    printer << "ptr";
  else if (mlir::isa<VaListType>(type))
    printer << "va_list";
  else if (auto array = mlir::dyn_cast<ArrayType>(type)) {
    if (array.getNumElements(true) == array.getNumElements(false))
      printer << "array<" << array.getNumElements() << ", " << array.getElementType() << ">";
    else printer << "word_array<" << array.getNumElements(true) << ", " << array.getNumElements(false) << ", " << array.getElementType() << ">";
  } else if (auto overlap = mlir::dyn_cast<OverlapType>(type)) {
    printer << "overlap<\"" << overlap.getIdentity() << "\", [";
    llvm::interleaveComma(overlap.getAlternatives(), printer, [&](mlir::Type alternative) { printer << alternative; });
    printer << "], [";
    llvm::interleaveComma(overlap.getDomains(), printer);
    printer << "]>";
  } else if (auto record = mlir::dyn_cast<RecordType>(type)) {
    printer << "record<\"" << record.getIdentity() << "\", " << unsigned(record.isPacked()) << ", [";
    llvm::interleaveComma(record.getFields(), printer, [&](mlir::Type field) { printer << field; });
    printer << "]>";
  }
  else
    llvm_unreachable("unregistered nier type");
}

} // namespace nier::ir
