#!/usr/bin/env bash
# CMake Tools probes CMake before loading a preset's environment. A desktop
# launch therefore needs the SDK libraries even for the initial --version call.
set -euo pipefail
sela_repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$sela_repo_root/sdk/env.sh"
sela_cmake="$SELA_SDK_ROOT/host/usr/bin/cmake"
if [[ ! -x "$sela_cmake" ]]; then
  echo 'Sela SDK is missing. Run bash scripts/bootstrap-sdk.sh from the repository root.' >&2
  exit 1
fi
exec "$sela_cmake" "$@"
