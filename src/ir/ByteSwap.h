#pragma once

#include "llvm/Support/Error.h"

namespace llvm { class Module; }
namespace nier::detail {

// Producer-only canonicalization of a proved full-width scalar byte reversal.
// Accepts a closed bounded mask/shift/or DAG fed by one argument, directly or
// through simple loads of one nonescaping, single-store argument alloca. A
// temporary DAG is checked by the pinned LLVM bit-provenance recognizer before
// changing the input module. Failed/partial/flagged/escaping idioms are unchanged.
// The alloca/store and unrelated instructions remain; no body/name replacement
// or general mem2reg is performed. Input LLVM IR must already be well formed.
llvm::Expected<unsigned> normalizeNativeByteSwaps(llvm::Module &module);

} // namespace nier::detail
