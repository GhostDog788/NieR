#!/usr/bin/env bash
# Re-record the real README demo using the already-built development checkout.
set -euo pipefail
nier_demo_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$nier_demo_repo/sdk/env.sh"
exec python3 "$nier_demo_repo/scripts/record-hello-demo.py" "$@"
