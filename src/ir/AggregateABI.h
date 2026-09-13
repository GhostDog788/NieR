#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <optional>

namespace sela::detail {

enum class NativeABIKind { Direct, Coerce, Expand, Indirect };

struct NativeABIPiece {
  llvm::Type *type = nullptr;
  uint64_t offset = 0;
};

struct NativeABIValue {
  NativeABIKind kind = NativeABIKind::Direct;
  llvm::Type *storageType = nullptr;
  uint64_t storageSize = 0;
  llvm::Align storageAlignment{1};
  // For byval/sret only. May exceed natural storage alignment (SysV64 byval).
  llvm::Align abiAlignment{1};
  llvm::SmallVector<NativeABIPiece, 2> pieces;
  // Parameter positions in nativeType. For a direct result nativeBegin is
  // unused; an indirect result refers to the hidden sret parameter.
  unsigned nativeBegin = 0;
  unsigned nativeCount = 0;
  bool byVal = false;
  bool sRet = false;
};

struct NativeABISignature {
  llvm::FunctionType *nativeType = nullptr;
  NativeABIValue result;
  llvm::SmallVector<NativeABIValue, 8> parameters;
  std::optional<unsigned> sretIndex;
  unsigned remainingGP = 0;
  unsigned remainingSSE = 0;
};

llvm::Expected<llvm::DataLayout> nativeABIDataLayout(bool x64);

// Ordinary Linux SysV C calling convention, with the pinned baseline target
// layouts. orderedRecords is an explicit semantic assertion: every listed
// type is an ordered, non-overlapping, naturally aligned record, not merely a
// storage representation for a union, bitfield, complex value, C++ class or
// over-aligned source type. Nested records must also be listed. The producer
// must prove that assertion and exact native inverse correspondence; LLVM
// struct spelling and debuginfo are neither consulted nor sufficient here.
//
// Signedness/extension and other semantic attributes remain the caller's
// separate contract. This classifies only fixed parameters in logical;
// aggregate operands in a variadic tail need separate call-site admission.
llvm::Expected<NativeABISignature> classifyNativeABI(
    llvm::FunctionType *logical, bool x64,
    llvm::ArrayRef<llvm::StructType *> orderedRecords);

enum class NativeABIRecordKind { Ordered, Union };
struct NativeABIFieldLayout {
  llvm::Type *type = nullptr;
  uint64_t bitOffset = 0;
  // Present only for an integer bitfield. An unnamed bitfield is padding,
  // contributes no ABI data, and may have zero width.
  std::optional<uint64_t> bitWidth;
  bool padding = false;
};
struct NativeABIRecordLayout {
  llvm::StructType *storageType = nullptr;
  NativeABIRecordKind kind = NativeABIRecordKind::Ordered;
  uint64_t sizeBytes = 0;
  llvm::Align alignment{1};
  llvm::SmallVector<NativeABIFieldLayout, 4> fields;
};

// Explicit selected-target semantic layouts, independent of the LLVM storage
// carrier. Union alternatives overlap at bit offset zero; ordered fields may
// be packed or bitfields but cannot overlap each other's named data. Every
// nested semantic record needs a descriptor. Source over-alignment and packing
// are expressed by explicit alignment/offsets, not LLVM struct spelling.
// Producers must prove these descriptors and the complete native inverse;
// the classifier never derives union membership or bitfields from a carrier.
llvm::Expected<NativeABISignature> classifyNativeLayoutABI(
    llvm::FunctionType *logical, bool x64,
    llvm::ArrayRef<NativeABIRecordLayout> records);

// Load/store precisely the declared native pieces in existing aggregate
// storage. No allocation, padding initialization, aliasing wrapper, memcpy or
// indirect-value copy is invented. The caller proves the complete storage
// extent and baseAlignment; emitted memory alignment is conservative at each
// offset. Validation precedes all IR emission. Indirect descriptors reject.
llvm::Expected<llvm::SmallVector<llvm::Value *, 2>> loadNativeABIPieces(
    llvm::IRBuilderBase &builder, const NativeABIValue &value,
    llvm::Value *storage, llvm::Align baseAlignment);
llvm::Error storeNativeABIPieces(
    llvm::IRBuilderBase &builder, const NativeABIValue &value,
    llvm::Value *storage, llvm::Align baseAlignment,
    llvm::ArrayRef<llvm::Value *> pieces);

} // namespace sela::detail
