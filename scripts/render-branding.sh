#!/usr/bin/env bash
# Render the editable, self-contained social card for GitHub's upload field.
set -euo pipefail
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
command -v ffmpeg >/dev/null || { printf 'Install FFmpeg with the librsvg decoder to render the social card.\n' >&2; exit 1; }
command -v python3 >/dev/null || { printf 'Install Python 3 to embed the presentation assets.\n' >&2; exit 1; }
# Keep the card self-contained, using the exact supplied PNG bytes each time.
python3 - "$repository/assets" <<'PY'
import base64
from pathlib import Path
import re
import sys

assets = Path(sys.argv[1])
card = assets / "social-preview.svg"
svg = card.read_text()
for element, name in (("sela-symbol", "sela.png"), ("sela-wordmark", "sela-word.png")):
    data = base64.b64encode((assets / name).read_bytes()).decode("ascii")
    pattern = rf'(<image id="{element}"[^>]*xlink:href=")[^"]*(")'
    svg, count = re.subn(pattern, lambda match: match[1] + "data:image/png;base64," + data + match[2], svg)
    if count != 1:
        raise SystemExit(f"Expected exactly one {element} image in the social card")
card.write_text(svg)
PY
ffmpeg -hide_banner -loglevel error -y \
  -i "$repository/assets/social-preview.svg" -frames:v 1 -update 1 \
  "$repository/assets/social-preview.png"
printf 'Rendered assets/social-preview.png (1280 x 640).\n'
