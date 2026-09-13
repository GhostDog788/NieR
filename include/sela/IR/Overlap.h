#pragma once

#include "mlir/IR/Types.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/ADT/Hashing.h"
#include <tuple>

namespace sela::ir {
namespace detail {
struct OverlapTypeStorage : mlir::TypeStorage {
  using KeyTy = std::tuple<llvm::StringRef, llvm::ArrayRef<mlir::Type>, mlir::ArrayAttr>;
  llvm::StringRef identity;
  llvm::ArrayRef<mlir::Type> alternatives;
  mlir::ArrayAttr domains;
  OverlapTypeStorage(llvm::StringRef id, llvm::ArrayRef<mlir::Type> types, mlir::ArrayAttr masks)
      : identity(id), alternatives(types), domains(masks) {}
  bool operator==(const KeyTy &key) const {
    return identity == std::get<0>(key) && alternatives == std::get<1>(key) && domains == std::get<2>(key);
  }
  static llvm::hash_code hashKey(const KeyTy &key) {
    return llvm::hash_combine(std::get<0>(key),
        llvm::hash_combine_range(std::get<1>(key).begin(), std::get<1>(key).end()),
        std::get<2>(key));
  }
  static OverlapTypeStorage *construct(mlir::TypeStorageAllocator &allocator, const KeyTy &key) {
    return new (allocator.allocate<OverlapTypeStorage>()) OverlapTypeStorage(
        allocator.copyInto(std::get<0>(key)), allocator.copyInto(std::get<1>(key)), std::get<2>(key));
  }
};
}

// Unnamed overlapping storage alternatives, in semantic declaration order.
// Each alternative has an explicit set of qualified target identities. These
// are semantic constraints, not source-language tags or original LLVM payloads.
class OverlapType : public mlir::Type::TypeBase<OverlapType, mlir::Type, detail::OverlapTypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "sela.overlap";
  static OverlapType get(mlir::MLIRContext *context, llvm::StringRef identity,
                        llvm::ArrayRef<mlir::Type> alternatives, mlir::ArrayAttr domains) {
    return Base::get(context, identity, alternatives, domains);
  }
  llvm::StringRef getIdentity() const { return getImpl()->identity; }
  llvm::ArrayRef<mlir::Type> getAlternatives() const { return getImpl()->alternatives; }
  mlir::ArrayAttr getDomains() const { return getImpl()->domains; }
};
}
