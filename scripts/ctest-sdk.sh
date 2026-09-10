#!/usr/bin/env bash
# CMake Tools can discover tests without passing the selected preset's
# environment. Load the SDK libraries before starting stock CTest as well.
set -euo pipefail
nier_repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$nier_repo_root/sdk/env.sh"
nier_ctest="$NIER_SDK_ROOT/host/usr/bin/ctest"
if [[ ! -x "$nier_ctest" ]]; then
  echo 'Nier SDK is missing. Run bash scripts/bootstrap-sdk.sh from the repository root.' >&2
  exit 1
fi
exec "$nier_ctest" "$@"
