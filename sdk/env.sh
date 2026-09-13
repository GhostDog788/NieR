# Source this file from Bash. No host installation or shell startup-file edits.
_sela_repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export SELA_SDK_ROOT=${SELA_SDK_ROOT:-$_sela_repo_root/.sdk}
export SELA_LLVM_ROOT="$SELA_SDK_ROOT/host/usr/lib/llvm-18"
export LLVM_DIR="$SELA_LLVM_ROOT/lib/cmake/llvm"
export MLIR_DIR="$SELA_LLVM_ROOT/lib/cmake/mlir"
export Clang_DIR="$SELA_LLVM_ROOT/lib/cmake/clang"
export SELA_SYSROOT_X86_64="$SELA_SDK_ROOT/sysroots/x86_64-linux-gnu"
export SELA_SYSROOT_I686="$SELA_SDK_ROOT/sysroots/i686-linux-gnu"
export PATH="$SELA_LLVM_ROOT/bin:$SELA_SDK_ROOT/host/usr/bin:$PATH"
export LD_LIBRARY_PATH="$SELA_LLVM_ROOT/lib:$SELA_SDK_ROOT/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CMAKE_PREFIX_PATH="$SELA_SDK_ROOT/host/usr:$SELA_LLVM_ROOT${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
unset _sela_repo_root
