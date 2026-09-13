#pragma once

#include "mlir/IR/Types.h"
#include "llvm/ADT/Hashing.h"
#include <tuple>

namespace sela::ir {
namespace detail {
struct OverlapTypeStorage : mlir::TypeStorage {
  using KeyTy = std::tuple<llvm::StringRef, llvm::ArrayRef<mlir::Type>, llvm::ArrayRef<unsigned>>;
  llvm::StringRef identity;
  llvm::ArrayRef<mlir::Type> alternatives;
  llvm::ArrayRef<unsigned> domains;
  OverlapTypeStorage(llvm::StringRef id, llvm::ArrayRef<mlir::Type> types, llvm::ArrayRef<unsigned> masks)
      : identity(id), alternatives(types), domains(masks) {}
  bool operator==(const KeyTy &key) const {
    return identity == std::get<0>(key) && alternatives == std::get<1>(key) && domains == std::get<2>(key);
  }
  static llvm::hash_code hashKey(const KeyTy &key) {
    return llvm::hash_combine(std::get<0>(key),
        llvm::hash_combine_range(std::get<1>(key).begin(), std::get<1>(key).end()),
        llvm::hash_combine_range(std::get<2>(key).begin(), std::get<2>(key).end()));
  }
  static OverlapTypeStorage *construct(mlir::TypeStorageAllocator &allocator, const KeyTy &key) {
    return new (allocator.allocate<OverlapTypeStorage>()) OverlapTypeStorage(
        allocator.copyInto(std::get<0>(key)), allocator.copyInto(std::get<1>(key)), allocator.copyInto(std::get<2>(key)));
  }
};
}

// Unnamed overlapping storage alternatives, in semantic declaration order.
// Domain bits are native word64=1, word32=2; 3 means both. These are semantic
// target constraints, not source-language tags or original LLVM payloads.
class OverlapType : public mlir::Type::TypeBase<OverlapType, mlir::Type, detail::OverlapTypeStorage> {
public:
  using Base::Base;
  static constexpr llvm::StringLiteral name = "sela.overlap";
  static OverlapType get(mlir::MLIRContext *context, llvm::StringRef identity,
                        llvm::ArrayRef<mlir::Type> alternatives, llvm::ArrayRef<unsigned> domains) {
    return Base::get(context, identity, alternatives, domains);
  }
  llvm::StringRef getIdentity() const { return getImpl()->identity; }
  llvm::ArrayRef<mlir::Type> getAlternatives() const { return getImpl()->alternatives; }
  llvm::ArrayRef<unsigned> getDomains() const { return getImpl()->domains; }
};
}
