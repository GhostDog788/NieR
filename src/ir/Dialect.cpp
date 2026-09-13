#include "sela/IR/Dialect.h"
#include "mlir/IR/DialectImplementation.h"

namespace sela::ir {

SelaDialect::SelaDialect(mlir::MLIRContext *context)
    : Dialect(getDialectNamespace(), context, mlir::TypeID::get<SelaDialect>()) {
  addTypes<WordType, PointerType, ArrayType, RecordType, VaListType, VaListArgumentType, OverlapType, ChoiceType>();
  addOperations<FunctionOp, GlobalOp, ConstantOp, AddressOp, AllocaOp, LoadOp,
                StoreOp, CallOp, IndirectCallOp, BinaryOp, CastOp, CompareOp, ReturnOp,
                BranchOp, CondBranchOp, SwitchOp, UnreachableOp, AddressIndexOp,
                SelectOp, NegateOp, ByteSwapOp, VaArgOp, VaForwardOp>();
}

mlir::Type SelaDialect::parseType(mlir::DialectAsmParser &parser) const {
  llvm::StringRef keyword;
  if (parser.parseKeyword(&keyword))
    return {};
  if (keyword == "word")
    return WordType::get(getContext());
  if (keyword == "ptr")
    return PointerType::get(getContext());
  if (keyword == "va_list")
    return VaListType::get(getContext());
  if (keyword == "va_list_argument")
    return VaListArgumentType::get(getContext());
  if (keyword == "array") {
    uint64_t count;
    mlir::Type element;
    if (parser.parseLess() || parser.parseInteger(count) || parser.parseComma() ||
        parser.parseType(element) || parser.parseGreater()) return {};
    return ArrayType::get(getContext(), element, count);
  }
  if (keyword == "target_array") {
    mlir::Attribute count;
    mlir::Type element;
    if (parser.parseLess() || parser.parseAttribute(count) || parser.parseComma() || parser.parseType(element) ||
        parser.parseGreater()) return {};
    return ArrayType::get(getContext(), element, count);
  }
  if (keyword == "choice") {
    mlir::Attribute cases;
    if (parser.parseLess() || parser.parseAttribute(cases) || parser.parseGreater()) return {};
    return ChoiceType::get(getContext(), cases);
  }
  if (keyword == "overlap") {
    std::string identity;
    llvm::SmallVector<mlir::Type> alternatives;
    mlir::ArrayAttr domains;
    if (parser.parseLess() || parser.parseString(&identity) || parser.parseComma() || parser.parseLSquare()) return {};
    if (mlir::failed(parser.parseOptionalRSquare())) {
      do {
        mlir::Type type;
        if (parser.parseType(type)) return {};
        alternatives.push_back(type);
      } while (mlir::succeeded(parser.parseOptionalComma()));
      if (parser.parseRSquare()) return {};
    }
    if (parser.parseComma() || parser.parseAttribute(domains)) return {};
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
  parser.emitError(parser.getCurrentLocation(), "unknown sela type: ") << keyword;
  return {};
}

void SelaDialect::printType(mlir::Type type,
                            mlir::DialectAsmPrinter &printer) const {
  if (mlir::isa<WordType>(type))
    printer << "word";
  else if (mlir::isa<PointerType>(type))
    printer << "ptr";
  else if (mlir::isa<VaListType>(type))
    printer << "va_list";
  else if (mlir::isa<VaListArgumentType>(type))
    printer << "va_list_argument";
  else if (auto array = mlir::dyn_cast<ArrayType>(type)) {
    if (mlir::isa<mlir::IntegerAttr>(array.getCount()))
      printer << "array<" << array.getNumElements() << ", " << array.getElementType() << ">";
    else printer << "target_array<" << array.getCount() << ", " << array.getElementType() << ">";
  } else if (auto choice = mlir::dyn_cast<ChoiceType>(type)) {
    printer << "choice<" << choice.getCases() << ">";
  } else if (auto overlap = mlir::dyn_cast<OverlapType>(type)) {
    printer << "overlap<\"" << overlap.getIdentity() << "\", [";
    llvm::interleaveComma(overlap.getAlternatives(), printer, [&](mlir::Type alternative) { printer << alternative; });
    printer << "], " << overlap.getDomains() << ">";
  } else if (auto record = mlir::dyn_cast<RecordType>(type)) {
    printer << "record<\"" << record.getIdentity() << "\", " << unsigned(record.isPacked()) << ", [";
    llvm::interleaveComma(record.getFields(), printer, [&](mlir::Type field) { printer << field; });
    printer << "]>";
  }
  else
    llvm_unreachable("unregistered sela type");
}

} // namespace sela::ir
