# Source this file from Bash. No host installation or shell startup-file edits.
_aot_repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export AOT_SDK_ROOT=${AOT_SDK_ROOT:-$_aot_repo_root/.sdk}
export AOT_LLVM_ROOT="$AOT_SDK_ROOT/host/usr/lib/llvm-18"
export LLVM_DIR="$AOT_LLVM_ROOT/lib/cmake/llvm"
export MLIR_DIR="$AOT_LLVM_ROOT/lib/cmake/mlir"
export AOT_SYSROOT_X86_64="$AOT_SDK_ROOT/sysroots/x86_64-linux-gnu"
export AOT_SYSROOT_I686="$AOT_SDK_ROOT/sysroots/i686-linux-gnu"
export PATH="$AOT_LLVM_ROOT/bin:$AOT_SDK_ROOT/host/usr/bin:$PATH"
export LD_LIBRARY_PATH="$AOT_LLVM_ROOT/lib:$AOT_SDK_ROOT/host/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CMAKE_PREFIX_PATH="$AOT_SDK_ROOT/host/usr:$AOT_LLVM_ROOT${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
unset _aot_repo_root
