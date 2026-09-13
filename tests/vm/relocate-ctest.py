#!/usr/bin/env python3
"""Relocate unmodified upstream CTest recipes for native guest execution.

Only the build root and exact configured cross-emulator command are changed.
All test names, arguments, properties and test inventory remain intact.
"""
import argparse
from pathlib import Path
import re


def relocate(text, build, emulator):
    prefix = " ".join('"' + part.replace("\\", "\\\\").replace('"', '\\"') + '"'
                      for part in emulator)
    if prefix:
        text = re.sub(r'(add_test\([^\n]*? )' + re.escape(prefix) + r' (?="' + re.escape(build) + r'/)',
                      r'\1', text)
    text = text.replace(build, "@SELA_CORPUS_DESTINATION@")
    if prefix and prefix in text:
        raise ValueError("Unrelocated cross-emulator command in upstream CTest recipe")
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    emulator = []
    for line in (args.build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith("CMAKE_CROSSCOMPILING_EMULATOR:"):
            emulator = line.split("=", 1)[1].split(";")
    for directory in (Path("."), Path("tests"), Path("fuzzing")):
        source = args.build / directory / "CTestTestfile.cmake"
        destination = args.output / directory / source.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(relocate(source.read_text(), str(args.build), emulator))


if __name__ == "__main__":
    main()
