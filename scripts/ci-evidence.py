#!/usr/bin/env python3
"""Collect bounded, job-scoped CI diagnostics without following file links."""
import argparse
import io
import json
import os
from pathlib import Path
import stat
import tarfile


TEMP_PREFIXES = ("sela-private-", "sela-corpus-", "sela-multi-consumer-",
                 "sela-consumer-vm-", "sela-consumer-test-", "sela-xxhash-")
REPORTS = {"qualification.txt", "publication-inputs.sha256", "host-settings.txt",
           "CMakeCache.txt", "CMakeConfigureLog.yaml", "native.cfg", ".ninja_log",
           "receipt-token", "trace-policy.txt"}
PRUNE = {"source", "sources", "root", "consumer-fixtures", "fixtures",
         "node_modules", ".git", "downloads", "sysroots"}


def rooted_open(root_fd, relative):
    """Resolve each component using directory descriptors, never symlinks."""
    parts = Path(relative).parts
    if not parts or any(part in ("", ".", "..") for part in parts) or Path(relative).is_absolute():
        raise ValueError("unsafe relative evidence path")
    directory = os.dup(root_fd)
    try:
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=directory)
            os.close(directory)
            directory = child
        return os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    finally:
        os.close(directory)


def candidates(root, failed, build=False):
    """Do not traverse source trees or copied VM/compiler payloads."""
    visited = 0
    for directory, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = sorted(name for name in dirs if name not in PRUNE
                         and not (Path(directory) / name).is_symlink())
        if not build and Path(directory) == root:
            dirs[:] = [name for name in dirs if name == "logs" or name.startswith(TEMP_PREFIXES)]
        visited += len(dirs)
        if visited > 100000:
            yield None, "scan-limit"
            return
        for name in sorted(files):
            visited += 1
            if visited > 100000:
                yield None, "scan-limit"
                return
            relative = (Path(directory) / name).relative_to(root)
            if build:
                selected = name in REPORTS or ("Testing" in relative.parts and name.endswith((".log", ".txt", ".xml")))
            else:
                first = relative.parts[0]
                if first != "logs" and not first.startswith(TEMP_PREFIXES):
                    continue
                selected = name in REPORTS or name.endswith((".log", ".trace", ".tsv"))
                if "metadata" in relative.parts and name.endswith((".json", ".bc")):
                    selected = True
                if failed and first.startswith("sela-consumer-vm-") and len(relative.parts) == 2 and name == "initramfs.cpio.gz":
                    selected = True
            if selected:
                # Logs/reports first, journals next, optional replay/boot bytes last.
                priority = 2 if name.endswith((".bc", ".cpio.gz")) else 1 if name.endswith(".json") else 0
                yield (priority, relative), None


def collect(temp_root, output, build_roots=(), failed=False,
            max_bytes=256 * 1024 * 1024, max_file_bytes=16 * 1024 * 1024,
            max_boot_bytes=128 * 1024 * 1024):
    temp_root = Path(temp_root)
    if temp_root.is_symlink() or not temp_root.name.startswith("sela-ci-"):
        raise ValueError("expected the dedicated sela-ci-* job directory")
    roots = [("temp", temp_root)] + [(f"build-{index}", Path(path)) for index, path in enumerate(build_roots)]
    descriptors = []
    report = {"failed_job": failed, "max_bytes": max_bytes, "max_file_bytes": max_file_bytes,
              "max_boot_bytes": max_boot_bytes,
              "included": [], "omitted": [], "omitted_count": 0, "bytes": 0,
              "build_roots": [str(path) for path in build_roots]}

    def omit(path, reason):
        report["omitted_count"] += 1
        if len(report["omitted"]) < 1000:
            report["omitted"].append({"path": path, "reason": reason})

    try:
        selected = []
        for label, root in roots:
            try:
                filesystem = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
                try:
                    descriptor = rooted_open(filesystem, root.absolute().relative_to("/"))
                finally:
                    os.close(filesystem)
                if not stat.S_ISDIR(os.fstat(descriptor).st_mode):
                    os.close(descriptor)
                    raise ValueError("evidence root is not a directory")
            except (OSError, ValueError) as error:
                omit(label, str(error))
                continue
            descriptors.append(descriptor)
            for entry, warning in candidates(root, failed, build=label != "temp"):
                if warning:
                    omit(label, warning)
                else:
                    priority, relative = entry
                    selected.append((priority, label, relative, descriptor))
        output = Path(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        # Exclusive creation preserves an earlier attempt; no extraction or
        # archive symlink/hardlink entries are ever generated.
        with output.open("xb") as destination, tarfile.open(fileobj=destination, mode="w:gz", dereference=False) as archive:
            for _, label, relative, descriptor in sorted(selected):
                name = f"{label}/{relative.as_posix()}"
                try:
                    fd = rooted_open(descriptor, relative)
                    with os.fdopen(fd, "rb") as source:
                        info = os.fstat(source.fileno())
                        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                            omit(name, "not a singly linked regular file")
                            continue
                        binary = str(relative).endswith((".bc", ".cpio.gz"))
                        remaining = max_bytes - report["bytes"]
                        file_limit = max_boot_bytes if str(relative).endswith(".cpio.gz") else max_file_bytes
                        limit = min(file_limit, remaining)
                        if limit <= 0 or (binary and info.st_size > limit):
                            omit(name, "byte limit; binary evidence is never truncated")
                            continue
                        # Preserve the failure tail of large text logs; the
                        # manifest explicitly records any truncation.
                        length = min(info.st_size, limit)
                        offset = info.st_size - length
                        source.seek(offset)
                        payload = source.read(length)
                        header = tarfile.TarInfo(name)
                        header.size = len(payload)
                        header.mode = 0o600
                        archive.addfile(header, io.BytesIO(payload))
                        report["bytes"] += len(payload)
                        report["included"].append({"path": name, "original_bytes": info.st_size,
                                                   "included_bytes": len(payload), "tail_offset": offset})
                except (OSError, ValueError) as error:
                    omit(name, str(error))
            payload = json.dumps(report, sort_keys=True, indent=2).encode()
            header = tarfile.TarInfo("evidence-manifest.json")
            header.size = len(payload)
            header.mode = 0o600
            archive.addfile(header, io.BytesIO(payload))
        return report
    finally:
        for descriptor in descriptors:
            os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--temp-root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--build-dir", action="append", default=[])
    parser.add_argument("--failed", action="store_true")
    args = parser.parse_args()
    report = collect(args.temp_root, args.output, args.build_dir, args.failed)
    print(f"CI diagnostics: {args.output}; {len(report['included'])} files, {report['bytes']} bytes; "
          f"{report['omitted_count']} omissions recorded in evidence-manifest.json")


if __name__ == "__main__":
    main()
