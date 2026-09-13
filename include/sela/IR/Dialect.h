#pragma once

#include "sela/IR/Overlap.h"

#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/Types.h"
#include "llvm/ADT/Hashing.h"
#include <tuple>

namespace sela::ir {

namespace detail {
struct ArrayTypeStorage : public mlir::TypeStorage {
  using KeyTy = std::tuple<mlir::Type, uint64_t, uint64_t>;
  mlir::Type element;
  uint64_t count64, count32;
  ArrayTypeStorage(const KeyTy &key) : element(std::get<0>(key)), count64(std::get<1>(key)), count32(std::get<2>(key)) {}
  bool operator==(const KeyTy &key) const { return key == KeyTy(element, count64, count32); }
  static llvm::hash_code hashKey(const KeyTy &key) {
    return llvm::hash_combine(std::get<0>(key), std::get<1>(key), std::get<2>(key));
  }
  static ArrayTypeStorage *construct(mlir::TypeStorageAllocator &allocator, const KeyTy &key) {
    return new (allocator.allocate<ArrayTypeStorage>()) ArrayTypeStorage(key);
  }
};
struct RecordTypeStorage : public mlir::TypeStorage {
  using KeyTy = std::tuple<llvm::StringRef, bool, llvm::ArrayRef<mlir::Type>>;
  llvm::StringRef identity;
  bool packed;
  llvm::ArrayRef<mlir::Type> fields;
  RecordTypeStorage(llvm::StringRef identity, bool packed, llvm::ArrayRef<mlir::Type> fields)
      : identity(identity), packed(packed), fields(fields) {}
  bool operator==(const KeyTy &key) const {
    return identity == std::get<0>(key) && packed == std::get<1>(key) && fields == std::get<2>(key);
  }
  static llvm::hash_code hashKey(const KeyTy &key) {
    return llvm::hash_combine(std::get<0>(key), std::get<1>(key), llvm::hash_combine_range(std::get<2>(key).begin(), std::get<2>(key).end()));
  }
  static RecordTypeStorage *construct(mlir::TypeStorageAllocator &allocator, const KeyTy &key) {
    return new (allocator.allocate<RecordTypeStorage>()) RecordTypeStorage(
        allocator.copyInto(std::get<0>(key)), std::get<1>(key), allocator.copyInto(std::get<2>(key)));
  }
};
}

class ArrayType : public mlir::Type::TypeBase<ArrayType, mlir::Type, detail::ArrayTypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "sela.array";
  static ArrayType get(mlir::MLIRContext *context, mlir::Type element, uint64_t count) {
    return Base::get(context, element, count, count);
  }
  static ArrayType getForWordWidths(mlir::MLIRContext *context, mlir::Type element,
                                   uint64_t count64, uint64_t count32) {
    return Base::get(context, element, count64, count32);
  }
  mlir::Type getElementType() const { return getImpl()->element; }
  uint64_t getNumElements(bool word64 = true) const { return word64 ? getImpl()->count64 : getImpl()->count32; }
};

class RecordType : public mlir::Type::TypeBase<RecordType, mlir::Type, detail::RecordTypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "sela.record";
  static RecordType get(mlir::MLIRContext *context, llvm::StringRef identity,
                        bool packed, llvm::ArrayRef<mlir::Type> fields) {
    return Base::get(context, identity, packed, fields);
  }
  llvm::StringRef getIdentity() const { return getImpl()->identity; }
  bool isPacked() const { return getImpl()->packed; }
  llvm::ArrayRef<mlir::Type> getFields() const { return getImpl()->fields; }
};

class WordType : public mlir::Type::TypeBase<WordType, mlir::Type,
                                             mlir::TypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "sela.word";
};

class PointerType : public mlir::Type::TypeBase<PointerType, mlir::Type,
                                                mlir::TypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "sela.ptr";
};

// Storage for the target native variadic argument cursor. Its layout is owned
// by the target ABI, never by the source language or a producer plugin.
class VaListType : public mlir::Type::TypeBase<VaListType, mlir::Type, mlir::TypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "sela.va_list";
};

// These are real registered dialect operations. They deliberately use MLIR's
// generic assembly format; no private parser or opaque payload is involved.
#define SELA_SIMPLE_OP(CLASS, NAME, ...)                                         \
  class CLASS : public mlir::Op<CLASS, __VA_ARGS__> {                            \
  public:                                                                     \
    using Op::Op;                                                             \
    static llvm::StringRef getOperationName() { return NAME; }                  \
    static llvm::ArrayRef<llvm::StringRef> getAttributeNames() { return {}; }    \
  }

SELA_SIMPLE_OP(FunctionOp, "sela.func", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::OneRegion);
SELA_SIMPLE_OP(GlobalOp, "sela.global", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(ConstantOp, "sela.constant", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(AddressOp, "sela.address", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(AllocaOp, "sela.alloca", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(LoadOp, "sela.load", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(VaArgOp, "sela.va_arg", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(VaForwardOp, "sela.va_forward", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(StoreOp, "sela.store", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(CallOp, "sela.call", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::VariadicResults, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(IndirectCallOp, "sela.call_indirect", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::VariadicResults, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(BinaryOp, "sela.binary", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(CastOp, "sela.cast", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(CompareOp, "sela.compare", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(AddressIndexOp, "sela.gep", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(SelectOp, "sela.select", mlir::OpTrait::NOperands<3>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(NegateOp, "sela.fneg", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(ByteSwapOp, "sela.bswap", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(ReturnOp, "sela.return", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::IsTerminator);
SELA_SIMPLE_OP(BranchOp, "sela.br", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::OneSuccessor, mlir::OpTrait::IsTerminator);
SELA_SIMPLE_OP(CondBranchOp, "sela.cond_br", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::NSuccessors<2>::Impl, mlir::OpTrait::IsTerminator);
SELA_SIMPLE_OP(SwitchOp, "sela.switch", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::VariadicSuccessors, mlir::OpTrait::IsTerminator);
SELA_SIMPLE_OP(UnreachableOp, "sela.unreachable", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::IsTerminator);

#undef SELA_SIMPLE_OP

class SelaDialect : public mlir::Dialect {
public:
  explicit SelaDialect(mlir::MLIRContext *context);
  static llvm::StringRef getDialectNamespace() { return "sela"; }
  mlir::Type parseType(mlir::DialectAsmParser &parser) const override;
  void printType(mlir::Type type,
                 mlir::DialectAsmPrinter &printer) const override;
};

} // namespace sela::ir
