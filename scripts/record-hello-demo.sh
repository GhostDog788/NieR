#!/usr/bin/env bash
# Re-record the real README demo using the already-built development checkout.
set -euo pipefail
sela_demo_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$sela_demo_repo/sdk/env.sh"
exec python3 "$sela_demo_repo/scripts/record-hello-demo.py" "$@"
