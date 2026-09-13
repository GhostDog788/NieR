#!/usr/bin/env python3
"""Emit ARM package rows from authenticated Ubuntu snapshot indexes.

Maintainer-only lock refresh: downloads metadata into a private cache, verifies
InRelease signatures and index hashes, and emits rows without editing a lock.
"""
import argparse
import hashlib
import lzma
from pathlib import Path
import subprocess
import sys

from targets import get


def fetch(url, output):
    if not output.is_file():
        subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location", "--retry", "3",
                        "--connect-timeout", "20", "--max-time", "180", url, "-o", str(output)], check=True)
    return output.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache", type=Path)
    parser.add_argument("--mirror", default="https://snapshot.ubuntu.com/ubuntu/20260910T000000Z")
    parser.add_argument("--keyring", default="/usr/share/keyrings/ubuntu-archive-keyring.gpg")
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    sdk = Path(__file__).resolve().parent
    requested = {}
    for line in (sdk / "consumer/packages.lock").read_text().splitlines():
        if line.startswith("x86_64 "):
            fields = line.split()
            requested[fields[1]] = fields[3]
    # Stock compiler-rt's architecture packages supply the matching C builtins.
    for line in (sdk / "packages.lock").read_text().splitlines():
        if line.startswith("host libclang-rt-18-dev "):
            requested["libclang-rt-18-dev"] = line.split()[3]
    # LLVM's ARM atomics may require libatomic, a native GCC runtime library.
    requested["libatomic1"] = requested["libstdc++6"]
    found = {}
    for suite in ("noble", "noble-updates"):
        release_path = args.cache / (suite + "-InRelease")
        release = fetch(args.mirror + "/dists/" + suite + "/InRelease", release_path)
        subprocess.run(["gpgv", "--keyring", args.keyring, str(release_path)], check=True, stdout=sys.stderr)
        hashes = {}
        recording = False
        for line in release.decode().splitlines():
            if line == "SHA256:":
                recording = True
            elif recording and line.startswith(" "):
                digest, size, path = line.split()
                hashes[path] = (digest, int(size))
            elif recording:
                break
        for profile in ("armv7", "aarch64"):
            arch = get(profile)["packageArch"]
            for component in ("main", "universe"):
                relative = component + "/binary-" + arch + "/Packages.xz"
                data = fetch(args.mirror + "/dists/" + suite + "/" + relative,
                             args.cache / (suite + "-" + component + "-" + arch + "-Packages.xz"))
                expected, size = hashes[relative]
                if len(data) != size or hashlib.sha256(data).hexdigest() != expected:
                    raise ValueError("Unauthenticated index: " + relative)
                for paragraph in lzma.decompress(data).decode().split("\n\n"):
                    fields = dict(line.split(": ", 1) for line in paragraph.splitlines()
                                  if ": " in line and not line.startswith(" "))
                    name = fields.get("Package")
                    if name not in requested or fields.get("Architecture") != arch:
                        continue
                    # Keep matching source versions where available. Clang-rt
                    # uses the stock Ubuntu LLVM18 package version.
                    key = (profile, name)
                    if fields["Version"] == requested[name]:
                        found[key] = fields
    print("# ARM rows from signed Ubuntu noble/noble-updates snapshot 20260910T000000Z.")
    for profile in ("armv7", "aarch64"):
        for name in requested:
            if (profile, name) not in found:
                raise ValueError("Pinned package version absent: " + profile + " " + name + " " + requested[name])
            fields = found[profile, name]
            print(profile, name, fields["Architecture"], fields["Version"], fields["SHA256"], fields["Filename"])


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print("pin target packages: " + str(error), file=sys.stderr)
        sys.exit(1)
