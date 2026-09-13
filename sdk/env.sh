# Source this file from Bash. No host installation or shell startup-file edits.
_sela_repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export SELA_SDK_ROOT=${SELA_SDK_ROOT:-$_sela_repo_root/.sdk}
export SELA_LLVM_ROOT="$SELA_SDK_ROOT/host/usr/lib/llvm-18"
export LLVM_DIR="$SELA_LLVM_ROOT/lib/cmake/llvm"
export MLIR_DIR="$SELA_LLVM_ROOT/lib/cmake/mlir"
export Clang_DIR="$SELA_LLVM_ROOT/lib/cmake/clang"
export SELA_BUILD_HOST=$(python3 "$_sela_repo_root/sdk/targets.py" host)
_sela_host_multiarch=$(python3 "$_sela_repo_root/sdk/targets.py" get "$SELA_BUILD_HOST" multiarch)
while IFS= read -r _sela_target; do
  _sela_sysroot=$(python3 "$_sela_repo_root/sdk/targets.py" get "$_sela_target" sysrootTriple)
  export "SELA_SYSROOT_${_sela_target^^}=$SELA_SDK_ROOT/sysroots/$_sela_sysroot"
done < <(python3 "$_sela_repo_root/sdk/targets.py" list)
export PATH="$SELA_LLVM_ROOT/bin:$SELA_SDK_ROOT/host/usr/bin:$PATH"
export LD_LIBRARY_PATH="$SELA_LLVM_ROOT/lib:$SELA_SDK_ROOT/host/usr/lib/$_sela_host_multiarch${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CMAKE_PREFIX_PATH="$SELA_SDK_ROOT/host/usr:$SELA_LLVM_ROOT${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
unset _sela_repo_root _sela_host_multiarch _sela_target _sela_sysroot
