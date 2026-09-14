#include "sela/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/IntrinsicsAArch64.h"

namespace sela::ir {
using O = IntrinsicOverload;
using E = IntrinsicEffects;
llvm::ArrayRef<IntrinsicSpec> intrinsicRegistry() {
  static const IntrinsicSpec registry[] = {
    {"assume", llvm::Intrinsic::assume, O::None, 1, E::Assumption, {}},
    {"expect", llvm::Intrinsic::expect, O::Result, 2, E::Pure, {}},
    {"prefetch", llvm::Intrinsic::prefetch, O::FirstOperand, 4, E::Prefetch, {}},
    {"funnel_shift_left", llvm::Intrinsic::fshl, O::Result, 3, E::Pure, {}},
    {"funnel_shift_right", llvm::Intrinsic::fshr, O::Result, 3, E::Pure, {}},
    {"unsigned_min", llvm::Intrinsic::umin, O::Result, 2, E::Pure, {}},
    {"unsigned_max", llvm::Intrinsic::umax, O::Result, 2, E::Pure, {}},
    {"signed_min", llvm::Intrinsic::smin, O::Result, 2, E::Pure, {}},
    {"signed_max", llvm::Intrinsic::smax, O::Result, 2, E::Pure, {}},
    {"count_leading_zeros", llvm::Intrinsic::ctlz, O::Result, 2, E::Pure, {}},
    {"count_trailing_zeros", llvm::Intrinsic::cttz, O::Result, 2, E::Pure, {}},
    {"population_count", llvm::Intrinsic::ctpop, O::Result, 1, E::Pure, {}},
    {"reverse_bits", llvm::Intrinsic::bitreverse, O::Result, 1, E::Pure, {}},
    {"vector_reduce_add", llvm::Intrinsic::vector_reduce_add, O::FirstOperand, 1, E::Pure, {}},
    {"vector_reduce_multiply", llvm::Intrinsic::vector_reduce_mul, O::FirstOperand, 1, E::Pure, {}},
    {"vector_reduce_and", llvm::Intrinsic::vector_reduce_and, O::FirstOperand, 1, E::Pure, {}},
    {"vector_reduce_or", llvm::Intrinsic::vector_reduce_or, O::FirstOperand, 1, E::Pure, {}},
    {"vector_reduce_xor", llvm::Intrinsic::vector_reduce_xor, O::FirstOperand, 1, E::Pure, {}},
    {"unsigned_add_overflow", llvm::Intrinsic::uadd_with_overflow, O::FirstOperand, 2, E::Pure, {}},
    {"unsigned_subtract_overflow", llvm::Intrinsic::usub_with_overflow, O::FirstOperand, 2, E::Pure, {}},
    {"unsigned_multiply_overflow", llvm::Intrinsic::umul_with_overflow, O::FirstOperand, 2, E::Pure, {}},
    {"signed_add_overflow", llvm::Intrinsic::sadd_with_overflow, O::FirstOperand, 2, E::Pure, {}},
    {"signed_subtract_overflow", llvm::Intrinsic::ssub_with_overflow, O::FirstOperand, 2, E::Pure, {}},
    {"signed_multiply_overflow", llvm::Intrinsic::smul_with_overflow, O::FirstOperand, 2, E::Pure, {}},
    {"x86_ternary_logic_i32x16", llvm::Intrinsic::x86_avx512_pternlog_d_512, O::None, 4, E::Pure, "X86"},
    {"x86_sse2_shift_right_i64x2", llvm::Intrinsic::x86_sse2_psrli_q, O::None, 2, E::Pure, "X86"},
    {"x86_sse2_shift_left_i64x2", llvm::Intrinsic::x86_sse2_pslli_q, O::None, 2, E::Pure, "X86"},
    {"x86_avx2_shift_right_i64x4", llvm::Intrinsic::x86_avx2_psrli_q, O::None, 2, E::Pure, "X86"},
    {"x86_avx2_shift_left_i64x4", llvm::Intrinsic::x86_avx2_pslli_q, O::None, 2, E::Pure, "X86"},
    {"x86_avx512_shift_right_i64x8", llvm::Intrinsic::x86_avx512_psrli_q_512, O::None, 2, E::Pure, "X86"},
    {"x86_avx512_shift_left_i64x8", llvm::Intrinsic::x86_avx512_pslli_q_512, O::None, 2, E::Pure, "X86"},
    {"aarch64_unsigned_widening_multiply", llvm::Intrinsic::aarch64_neon_umull, O::Result, 2, E::Pure, "AArch64"},
  };
  return registry;
}
const IntrinsicSpec *findIntrinsic(llvm::StringRef name) {
  for (const auto &entry : intrinsicRegistry()) if (entry.name == name) return &entry;
  return nullptr;
}
const IntrinsicSpec *findIntrinsic(llvm::Intrinsic::ID id) {
  for (const auto &entry : intrinsicRegistry()) if (entry.llvmID == id) return &entry;
  return nullptr;
}
} // namespace sela::ir
