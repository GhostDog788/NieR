#!/usr/bin/env bash
# CMake Tools can discover tests without passing the selected preset's
# environment. Load the SDK libraries before starting stock CTest as well.
set -euo pipefail
sela_repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$sela_repo_root/sdk/env.sh"
sela_ctest="$SELA_SDK_ROOT/host/usr/bin/ctest"
if [[ ! -x "$sela_ctest" ]]; then
  echo 'Sela SDK is missing. Run bash scripts/bootstrap-sdk.sh from the repository root.' >&2
  exit 1
fi
exec "$sela_ctest" "$@"
