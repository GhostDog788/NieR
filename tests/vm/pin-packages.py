#!/usr/bin/env python3
"""Refresh test-only pins from signed Ubuntu snapshot metadata (stdout only).

Only metadata downloads are cached. This is not run by builds or VM acceptance.
The historical Debian i686 kernel pin is deliberately retained separately.
"""
import argparse
import hashlib
import lzma
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "sdk"))
from targets import load  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("vm", "tools"))
    parser.add_argument("cache", type=Path)
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    mirror = "https://snapshot.ubuntu.com/ubuntu/20260910T000000Z"
    requested = {}
    for line in (ROOT / "tests/vm/test-tools.lock").read_text().splitlines():
        fields = line.split()
        if fields and fields[0] != "#" and fields[1] in ("i386", "all"):
            requested[fields[0]] = fields[2]
    records = {}
    wanted = set(requested) | {"linux-image-6.8.0-139-generic", "linux-headers-6.8.0-139-generic", "busybox-static"}
    wanted.update("binutils-" + target["gccTriple"].replace("x86_64", "x86-64") for target in load().values())
    for suite in ("noble", "noble-updates"):
        release = args.cache / (suite + "-InRelease")
        fetch(mirror + "/dists/" + suite + "/InRelease", release)
        subprocess.run(["gpgv", "--keyring", "/usr/share/keyrings/ubuntu-archive-keyring.gpg",
                        str(release)], check=True, stdout=sys.stderr)
        hashes = {}
        recording = False
        for line in release.read_text().splitlines():
            if line == "SHA256:":
                recording = True
            elif recording and line.startswith(" "):
                digest, size, relative = line.split()
                hashes[relative] = (digest, int(size))
            elif recording:
                break
        for target in load().values():
            arch = target["packageArch"]
            for component in ("main", "universe"):
                relative = component + "/binary-" + arch + "/Packages.xz"
                path = args.cache / (suite + "-" + component + "-" + arch + "-Packages.xz")
                data = fetch(mirror + "/dists/" + suite + "/" + relative, path)
                digest, size = hashes[relative]
                if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
                    raise ValueError("Unauthenticated index: " + str(path))
                for paragraph in lzma.decompress(data).decode().split("\n\n"):
                    fields = dict(line.split(": ", 1) for line in paragraph.splitlines()
                                  if ": " in line and not line.startswith(" "))
                    if (fields.get("Architecture") not in (arch, "all") or
                            fields.get("Package") not in wanted):
                        continue
                    key = (arch, fields.get("Package"), fields.get("Version"))
                    records[key] = fields
    if args.kind == "vm":
        print("# Target role package architecture version SHA256 HTTPS-URL")
        print("# Ubuntu pins: signed noble/noble-updates snapshot 20260910T000000Z.")
        print("# i686 kernel: retained Debian bookworm 6.1.176-1 exact package pin.")
        for target in load().values():
            arch = target["packageArch"]
            if target["id"] == "i686":
                print("i686 kernel linux-image-6.1.0-50-686-pae i386 6.1.176-1 "
                      "0d6241268f9344a998c3339c11e527ea8048d409d6c770f67c573707606a3f2e "
                      "https://snapshot.debian.org/archive/debian/20260910T000000Z/pool/main/l/"
                      "linux-signed-i386/linux-image-6.1.0-50-686-pae_6.1.176-1_i386.deb")
            else:
                package = "linux-image-6.8.0-139-generic"
                emit(records[(arch, package, "6.8.0-139.139")], mirror, target["id"] + " kernel")
                emit(records[(arch, "linux-headers-6.8.0-139-generic", "6.8.0-139.139")],
                     mirror, target["id"] + " headers")
            emit(records[(arch, "busybox-static", "1:1.36.1-6ubuntu3.1")], mirror,
                 target["id"] + " busybox")
    else:
        print("# Test-only package architecture version SHA256 HTTPS-URL")
        print("# Exact versions retained from i386; all architectures independently pinned.")
        print("# Signed Ubuntu noble/noble-updates snapshot 20260910T000000Z.")
        seen = set()
        for target in load().values():
            arch = target["packageArch"]
            for name, version in sorted(requested.items()):
                if name == "binutils-i686-linux-gnu":
                    name = "binutils-" + target["gccTriple"].replace("x86_64", "x86-64")
                fields = records[(arch, name, version)]
                identity = (fields["Package"], fields["Architecture"])
                if identity not in seen:
                    emit(fields, mirror)
                    seen.add(identity)


def fetch(url, path):
    if not path.is_file():
        subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location",
                        "--retry", "3", "--connect-timeout", "20", "--max-time", "180",
                        url, "-o", str(path)], check=True)
    return path.read_bytes()


def emit(fields, mirror, prefix=""):
    print(*([prefix] if prefix else []), fields["Package"], fields["Architecture"],
          fields["Version"], fields["SHA256"], mirror + "/" + fields["Filename"])


if __name__ == "__main__":
    main()
