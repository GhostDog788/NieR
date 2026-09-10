# Source this file from Bash. No host installation or shell startup-file edits.
_nier_repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export NIER_SDK_ROOT=${NIER_SDK_ROOT:-$_nier_repo_root/.sdk}
export NIER_LLVM_ROOT="$NIER_SDK_ROOT/host/usr/lib/llvm-18"
export LLVM_DIR="$NIER_LLVM_ROOT/lib/cmake/llvm"
export MLIR_DIR="$NIER_LLVM_ROOT/lib/cmake/mlir"
export Clang_DIR="$NIER_LLVM_ROOT/lib/cmake/clang"
export NIER_SYSROOT_X86_64="$NIER_SDK_ROOT/sysroots/x86_64-linux-gnu"
export NIER_SYSROOT_I686="$NIER_SDK_ROOT/sysroots/i686-linux-gnu"
export PATH="$NIER_LLVM_ROOT/bin:$NIER_SDK_ROOT/host/usr/bin:$PATH"
export LD_LIBRARY_PATH="$NIER_LLVM_ROOT/lib:$NIER_SDK_ROOT/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CMAKE_PREFIX_PATH="$NIER_SDK_ROOT/host/usr:$NIER_LLVM_ROOT${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
unset _nier_repo_root
