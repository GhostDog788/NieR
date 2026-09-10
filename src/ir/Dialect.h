#pragma once

#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/Types.h"

namespace aot::ir {

class WordType : public mlir::Type::TypeBase<WordType, mlir::Type,
                                             mlir::TypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "aot.word";
};

class PointerType : public mlir::Type::TypeBase<PointerType, mlir::Type,
                                                mlir::TypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "aot.ptr";
};

// These are real registered dialect operations. They deliberately use MLIR's
// generic assembly format; no private parser or opaque payload is involved.
#define AOT_SIMPLE_OP(CLASS, NAME, ...)                                         \
  class CLASS : public mlir::Op<CLASS, __VA_ARGS__> {                            \
  public:                                                                     \
    using Op::Op;                                                             \
    static llvm::StringRef getOperationName() { return NAME; }                  \
    static llvm::ArrayRef<llvm::StringRef> getAttributeNames() { return {}; }    \
  }

AOT_SIMPLE_OP(FunctionOp, "aot.func", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::OneRegion);
AOT_SIMPLE_OP(GlobalOp, "aot.global", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions);
AOT_SIMPLE_OP(ConstantOp, "aot.constant", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
AOT_SIMPLE_OP(AddressOp, "aot.address", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
AOT_SIMPLE_OP(AllocaOp, "aot.alloca", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
AOT_SIMPLE_OP(LoadOp, "aot.load", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
AOT_SIMPLE_OP(StoreOp, "aot.store", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions);
AOT_SIMPLE_OP(CallOp, "aot.call", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::VariadicResults, mlir::OpTrait::ZeroRegions);
AOT_SIMPLE_OP(BinaryOp, "aot.binary", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
AOT_SIMPLE_OP(CastOp, "aot.cast", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
AOT_SIMPLE_OP(CompareOp, "aot.compare", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
AOT_SIMPLE_OP(ReturnOp, "aot.return", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::IsTerminator);

#undef AOT_SIMPLE_OP

class AOTDialect : public mlir::Dialect {
public:
  explicit AOTDialect(mlir::MLIRContext *context);
  static llvm::StringRef getDialectNamespace() { return "aot"; }
  mlir::Type parseType(mlir::DialectAsmParser &parser) const override;
  void printType(mlir::Type type,
                 mlir::DialectAsmPrinter &printer) const override;
};

} // namespace aot::ir
