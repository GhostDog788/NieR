#!/usr/bin/env python3
"""Exercise archive admission through the actual (possibly packaged) nierc.

Usage: python3 tests/consumer-package-boundary.py NIER_COMPILER VALID_ARTIFACT
The caller supplies an independent producer's valid artifact and any SDK
environment needed by the compiler. Only private temporary fixtures are written.
This complements package_tests, whose full libarchive also creates gzip fixtures;
it specifically covers the shipped compiler's separately linked archive reader.
"""

import argparse
import gzip
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


MAX_BYTES = 64 * 1024 * 1024
MAX_FILES = 512
TIMEOUT_SECONDS = 20
BLOCK_BYTES = 512


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").digest()


def header(name, size=0, kind=tarfile.REGTYPE, linkname=""):
    entry = tarfile.TarInfo(name)
    entry.size = size
    entry.type = kind
    entry.linkname = linkname
    entry.mode = 0o644
    entry.mtime = 0
    return entry.tobuf(format=tarfile.PAX_FORMAT)


def archive(path, entries):
    """Write simple PAX tar entries without allocating declared-size payloads.

    Entries contain a serialized header and their actual body. Some rejection
    fixtures intentionally declare more bytes than they supply: the size guard
    must reject their headers before attempting to read or allocate that body.
    """
    with path.open("wb") as output:
        for entry_header, body in entries:
            output.write(entry_header)
            output.write(body)
            output.write(b"\0" * (-len(body) % BLOCK_BYTES))
        output.write(b"\0" * (2 * BLOCK_BYTES))
    return path


class BoundaryChecks:
    def __init__(self, compiler, work):
        self.compiler = compiler
        self.work = work
        self.environment = dict(os.environ, LC_ALL="C", TMPDIR=str(work))
        self.count = 0

    def inspect(self, label, path, expected=None):
        try:
            result = subprocess.run(
                [str(self.compiler), "inspect", str(path)],
                env=self.environment, capture_output=True, text=True,
                errors="replace", timeout=TIMEOUT_SECONDS, check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise AssertionError(f"{label}: compiler did not finish within "
                                 f"{TIMEOUT_SECONDS} seconds") from error
        output = result.stdout + result.stderr
        if expected is None:
            passed = result.returncode == 0 and "native validation passed" in output
        else:
            # A crash, loader failure, or later manifest rejection must not
            # masquerade as successful rejection by the archive reader.
            passed = (result.returncode == 1 and "nierc: " in result.stderr and
                      any(message.lower() in result.stderr.lower()
                          for message in expected))
        if not passed:
            raise AssertionError(f"{label}: unexpected exit {result.returncode}\n"
                                 f"{output}")
        self.count += 1
        print(f"PASS: {label}", flush=True)

    def bad_archive(self, label, entries, expected):
        path = self.work / f"case-{self.count}.nier"
        self.inspect(label, archive(path, entries), expected)


def run(compiler, valid_artifact, work):
    tests = BoundaryChecks(compiler, work)
    original_digest = digest(valid_artifact)
    tests.inspect("valid artifact", valid_artifact)

    for name in ("/absolute", "../escape", "modules/../escape",
                 "./manifest.json", "modules//0.nierbc", "x" * 257):
        tests.bad_archive(f"invalid path {name[:40]!r}", [(header(name), b"")],
                          ("invalid archive entry:",))
    for label, kind, linkname in (
        ("symlink", tarfile.SYMTYPE, "../escape"),
        ("hardlink", tarfile.LNKTYPE, "manifest.json"),
        ("directory", tarfile.DIRTYPE, ""),
        ("FIFO member", tarfile.FIFOTYPE, ""),
    ):
        tests.bad_archive(label, [(header("member", kind=kind, linkname=linkname), b"")],
                          ("invalid archive entry:",))

    duplicate = (header("manifest.json", 2), b"{}")
    tests.bad_archive("duplicate member", [duplicate, duplicate],
                      ("archive size/count limit or duplicate entry:",))
    entries = [(header(f"member-{index}"), b"") for index in range(MAX_FILES)]
    # All 512 entries must reach manifest validation, rather than an off-by-one
    # archive count rejection. The separate valid fixture covers full admission.
    tests.bad_archive("exact member limit reaches manifest validation", entries,
                      ("missing or oversized manifest",))
    tests.bad_archive("member count overflow", entries + [(header("extra"), b"")],
                      ("archive size/count limit or duplicate entry:",))
    tests.bad_archive("individual declared size overflow",
                      [(header("large", MAX_BYTES + 1), b"")],
                      ("archive size/count limit or duplicate entry:",))
    tests.bad_archive("aggregate declared size overflow",
                      [(header("first", 1), b"x"),
                       (header("second", MAX_BYTES), b"")],
                      ("archive size/count limit or duplicate entry:",))

    short_body = work / "truncated-body.nier"
    short_body.write_bytes(header("payload", 4096) + b"x" * 17)
    tests.inspect("truncated body", short_body, ("truncated archive entry:",))
    short_header = work / "truncated-header.nier"
    short_header.write_bytes(header("payload", 4096)[:100])
    tests.inspect("truncated header", short_header,
                  ("unrecognized archive format", "truncated archive"))
    bad_checksum = work / "bad-checksum.nier"
    damaged = bytearray(header("payload"))
    damaged[0] ^= 1
    bad_checksum.write_bytes(damaged + b"\0" * (2 * BLOCK_BYTES))
    tests.inspect("invalid tar checksum", bad_checksum,
                  ("unrecognized archive format", "damaged tar archive"))

    oversized = work / "oversized-outer.nier"
    # Sparse file: no 65 MiB fixture allocation or write, and rejection must
    # happen during the bounded regular-file check before the compiler reads it.
    with oversized.open("wb") as output:
        output.truncate(MAX_BYTES + 1024 * 1024 + 1)
    tests.inspect("outer file size overflow", oversized, ("size limit exceeded",))
    compressed = work / "compressed.nier"
    with valid_artifact.open("rb") as source, compressed.open("wb") as output:
        with gzip.GzipFile(fileobj=output, mode="wb", mtime=0) as writer:
            shutil.copyfileobj(source, writer, length=64 * 1024)
    tests.inspect("compressed valid artifact", compressed, ("unrecognized archive format",))

    fifo = work / "input.fifo"
    os.mkfifo(fifo)
    tests.inspect("FIFO input rejects without blocking", fifo, ("input is not a regular file:",))
    if digest(valid_artifact) != original_digest:
        raise AssertionError("compiler inspection modified the original artifact")
    print(f"Consumer archive boundary: {tests.count} checks passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("valid_artifact", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    valid_artifact = args.valid_artifact.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="nier-package-boundary-") as temporary:
        run(compiler, valid_artifact, Path(temporary))


if __name__ == "__main__":
    main()
