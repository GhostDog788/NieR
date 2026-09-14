#pragma once

#include "sela/IR/Overlap.h"

#include "mlir/IR/Dialect.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/Types.h"
#include "llvm/ADT/Hashing.h"
#include <tuple>

namespace sela::ir {

namespace detail {
struct ArrayTypeStorage : public mlir::TypeStorage {
  using KeyTy = std::tuple<mlir::Type, mlir::Attribute>;
  mlir::Type element;
  mlir::Attribute count;
  ArrayTypeStorage(const KeyTy &key) : element(std::get<0>(key)), count(std::get<1>(key)) {}
  bool operator==(const KeyTy &key) const { return key == KeyTy(element, count); }
  static llvm::hash_code hashKey(const KeyTy &key) {
    return llvm::hash_combine(std::get<0>(key), std::get<1>(key));
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
struct ChoiceTypeStorage : public mlir::TypeStorage {
  using KeyTy = mlir::Attribute;
  mlir::Attribute cases;
  explicit ChoiceTypeStorage(mlir::Attribute cases) : cases(cases) {}
  bool operator==(KeyTy key) const { return cases == key; }
  static llvm::hash_code hashKey(KeyTy key) { return llvm::hash_value(key.getAsOpaquePointer()); }
  static ChoiceTypeStorage *construct(mlir::TypeStorageAllocator &allocator, KeyTy key) {
    return new (allocator.allocate<ChoiceTypeStorage>()) ChoiceTypeStorage(key);
  }
};
}

class ArrayType : public mlir::Type::TypeBase<ArrayType, mlir::Type, detail::ArrayTypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "sela.array";
  static ArrayType get(mlir::MLIRContext *context, mlir::Type element, uint64_t count) {
    return Base::get(context, element, mlir::IntegerAttr::get(mlir::IntegerType::get(context, 64), count));
  }
  static ArrayType get(mlir::MLIRContext *context, mlir::Type element, mlir::Attribute count) {
    return Base::get(context, element, count);
  }
  mlir::Type getElementType() const { return getImpl()->element; }
  mlir::Attribute getCount() const { return getImpl()->count; }
  // Only valid after public target specialization.
  uint64_t getNumElements() const { return mlir::cast<mlir::IntegerAttr>(getCount()).getValue().getZExtValue(); }
};

// A local type relationship, never a native module or source-language type.
class ChoiceType : public mlir::Type::TypeBase<ChoiceType, mlir::Type, detail::ChoiceTypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "sela.choice";
  static ChoiceType get(mlir::MLIRContext *context, mlir::Attribute cases) {
    return Base::get(context, cases);
  }
  mlir::Attribute getCases() const { return getImpl()->cases; }
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
// Native ABI value used to forward a variadic cursor to another function. Its
// representation is ABI-owned and is not universally a pointer.
class VaListArgumentType : public mlir::Type::TypeBase<VaListArgumentType, mlir::Type, mlir::TypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "sela.va_list_argument";
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
SELA_SIMPLE_OP(ExtractValueOp, "sela.extract_value", mlir::OpTrait::OneOperand,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(InsertValueOp, "sela.insert_value", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(ExtractElementOp, "sela.extract_element", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(InsertElementOp, "sela.insert_element", mlir::OpTrait::NOperands<3>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(ShuffleOp, "sela.shuffle", mlir::OpTrait::NOperands<2>::Impl,
              mlir::OpTrait::OneResult, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(IntrinsicOp, "sela.intrinsic", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::VariadicResults, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(InlineAsmOp, "sela.inline_asm", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::VariadicResults, mlir::OpTrait::ZeroRegions);
SELA_SIMPLE_OP(InlineAsmBranchOp, "sela.inline_asm_br", mlir::OpTrait::VariadicOperands,
              mlir::OpTrait::VariadicResults, mlir::OpTrait::ZeroRegions,
              mlir::OpTrait::VariadicSuccessors, mlir::OpTrait::IsTerminator);
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
