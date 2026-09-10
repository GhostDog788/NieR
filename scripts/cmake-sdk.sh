#!/usr/bin/env bash
# CMake Tools probes CMake before loading a preset's environment. A desktop
# launch therefore needs the SDK libraries even for the initial --version call.
set -euo pipefail
nier_repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$nier_repo_root/sdk/env.sh"
nier_cmake="$NIER_SDK_ROOT/host/usr/bin/cmake"
if [[ ! -x "$nier_cmake" ]]; then
  echo 'Nier SDK is missing. Run bash scripts/bootstrap-sdk.sh from the repository root.' >&2
  exit 1
fi
exec "$nier_cmake" "$@"
