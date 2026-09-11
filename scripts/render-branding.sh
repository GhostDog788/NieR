#!/usr/bin/env bash
# Render the editable, self-contained social card for GitHub's upload field.
set -euo pipefail
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
command -v ffmpeg >/dev/null || { printf 'Install FFmpeg with the librsvg decoder to render the social card.\n' >&2; exit 1; }
ffmpeg -hide_banner -loglevel error -y \
  -i "$repository/assets/social-preview.svg" -frames:v 1 -update 1 \
  "$repository/assets/social-preview.png"
printf 'Rendered assets/social-preview.png (1280 x 640).\n'
