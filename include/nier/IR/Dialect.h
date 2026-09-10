#pragma once

#include "nier/IR/Overlap.h"

#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/Types.h"
#include "llvm/ADT/Hashing.h"
#include <tuple>

namespace nier::ir {

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
  static constexpr llvm::StringLiteral name = "nier.array";
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
  static constexpr llvm::StringLiteral name = "nier.record";
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
  static constexpr llvm::StringLiteral name = "nier.word";
};

class PointerType : public mlir::Type::TypeBase<PointerType, mlir::Type,
                                                mlir::TypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "nier.ptr";
};

// Storage for the target native variadic argument cursor. Its layout is owned
// by the target ABI, never by the source language or a producer plugin.
class VaListType : public mlir::Type::TypeBase<VaListType, mlir::Type, mlir::TypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "nier.va_list";
};

// These are real registered dialect operations. They deliberately use MLIR's
// generic assembly format; no private parser or opaque payload is involved.
#define NIER_SIMPLE_OP(CLASS, NAME, ...)                                         \
  class CLASS : public mlir::Op<CLASS, __VA_ARGS__> {                            \
  public:                                                                     \
    using Op::Op;                                                             \
    static llvm::StringRef getOperationName() { return NAME; }                  \
    static llvm::ArrayRef<llvm::StringRef> getAttributeNames() { return {}; }    \
  }

NIER_SIMPLE_OP(FunctionOp, "nier.func", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::OneRegion);
NIER_SIMPLE_OP(GlobalOp, "nier.global", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(ConstantOp, "nier.constant", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(AddressOp, "nier.address", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(AllocaOp, "nier.alloca", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(LoadOp, "nier.load", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(VaArgOp, "nier.va_arg", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(VaForwardOp, "nier.va_forward", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(StoreOp, "nier.store", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(CallOp, "nier.call", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::VariadicResults, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(IndirectCallOp, "nier.call_indirect", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::VariadicResults, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(BinaryOp, "nier.binary", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(CastOp, "nier.cast", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(CompareOp, "nier.compare", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(AddressIndexOp, "nier.gep", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(SelectOp, "nier.select", mlir::OpTrait::NOperands<3>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(NegateOp, "nier.fneg", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(ByteSwapOp, "nier.bswap", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
NIER_SIMPLE_OP(ReturnOp, "nier.return", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::IsTerminator);
NIER_SIMPLE_OP(BranchOp, "nier.br", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::OneSuccessor, mlir::OpTrait::IsTerminator);
NIER_SIMPLE_OP(CondBranchOp, "nier.cond_br", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::NSuccessors<2>::Impl, mlir::OpTrait::IsTerminator);
NIER_SIMPLE_OP(SwitchOp, "nier.switch", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::VariadicSuccessors, mlir::OpTrait::IsTerminator);
NIER_SIMPLE_OP(UnreachableOp, "nier.unreachable", mlir::OpTrait::ZeroOperands,
              mlir::OpTrait::ZeroResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::IsTerminator);

#undef NIER_SIMPLE_OP

class NIERDialect : public mlir::Dialect {
public:
  explicit NIERDialect(mlir::MLIRContext *context);
  static llvm::StringRef getDialectNamespace() { return "nier"; }
  mlir::Type parseType(mlir::DialectAsmParser &parser) const override;
  void printType(mlir::Type type,
                 mlir::DialectAsmPrinter &printer) const override;
};

} // namespace nier::ir
