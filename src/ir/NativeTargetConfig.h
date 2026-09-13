#pragma once

// Each native implementation is compiled independently. Public word-domain
// data remain shared; these constants select executable native compiler code.
#if SELA_NATIVE_WORD_BITS == 64
#define SELA_NATIVE_NAMESPACE native64
#elif SELA_NATIVE_WORD_BITS == 32
#define SELA_NATIVE_NAMESPACE native32
#else
#error "Compile native Sela sources with SELA_NATIVE_WORD_BITS=32 or 64"
#endif

#include "llvm/ADT/StringRef.h"
namespace sela::detail::SELA_NATIVE_NAMESPACE {
inline constexpr bool x64 = SELA_NATIVE_WORD_BITS == 64;
#if SELA_NATIVE_WORD_BITS == 64
inline constexpr llvm::StringRef TargetID = "x86_64";
inline constexpr llvm::StringRef TargetTriple = "x86_64-unknown-linux-gnu";
inline constexpr llvm::StringRef TargetLayout =
    "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";
#else
inline constexpr llvm::StringRef TargetID = "i686";
inline constexpr llvm::StringRef TargetTriple = "i686-unknown-linux-gnu";
inline constexpr llvm::StringRef TargetLayout =
    "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128";
#endif
} // namespace sela::detail::SELA_NATIVE_NAMESPACE
