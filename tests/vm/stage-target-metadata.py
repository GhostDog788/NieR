#!/usr/bin/env python3
"""Stage registry-derived, POSIX-readable native fixture metadata."""
from pathlib import Path
import shlex
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "sdk"))
from targets import load, variable  # noqa: E402


def stage(fixtures):
    targets = load()
    metadata = fixtures / "target-metadata"
    metadata.mkdir()
    (fixtures / "targets.list").write_text("".join(target + "\n" for target in targets))
    for target in targets.values():
        text = "".join(variable(field) + "=" + shlex.quote(str(target[field])) + "\n"
                       for field in ("id", "elfClass", "elfMachine", "sysrootTriple", "multiarch", "loader"))
        (metadata / (target["id"] + ".env")).write_text(text)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: stage-target-metadata.py FIXTURE_DIRECTORY")
    stage(Path(sys.argv[1]))
