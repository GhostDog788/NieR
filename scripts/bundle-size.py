#!/usr/bin/env python3
"""Read-only, dependency-aware accounting for an installed Sela compiler bundle.

Uses readelf (never executes inspected binaries). Regular-file bytes and section
totals count each inode once; symlinks are not followed. Allocated bytes include
directory and symlink blocks and are filesystem-dependent. Compression is an
optional reproducible tar/gzip-1 stream to a counter, not a file on disk.
"""
import argparse
import gzip
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tarfile


TOOLS = ("opt", "llc", "ld.lld", "llvm-ar")
GROUPS = ("selac", *("llvm_tools." + tool for tool in TOOLS), "shared_llvm",
          "compiler_libraries.xml_icu", "compiler_libraries.archive", "compiler_libraries.compression",
          "compiler_libraries.other", "compiler_libraries.unreferenced",
          "native_runtime", "compiler_rt", "metadata")
SECTION_KEYS = ("code", "symbols", "unwind", "debug", "other_file_sections")


def elf_info(path):
    with path.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            return None
    result = subprocess.run(["readelf", "--wide", "--sections", "--dynamic", str(path)],
                            text=True, capture_output=True, check=True,
                            env=dict(os.environ, LC_ALL="C"))
    sections = dict.fromkeys(SECTION_KEYS, 0)
    for line in result.stdout.splitlines():
        match = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+"
                         r"[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+"
                         r"[0-9a-fA-F]+\s+([a-zA-Z]*)\s*\d+\s+\d+\s+\d+\s*$", line)
        if not match:
            continue
        name, section_type, size_hex, flags = match.groups()
        if section_type == "NOBITS":
            continue
        if "X" in flags:
            group = "code"
        elif name in (".symtab", ".strtab"):
            group = "symbols"
        elif name.startswith((".eh_frame", ".gcc_except_table", ".ARM.exidx", ".ARM.extab")):
            group = "unwind"
        elif name.startswith((".debug", ".zdebug")):
            group = "debug"
        else:
            group = "other_file_sections"
        sections[group] += int(size_hex, 16)
    soname = re.search(r"\(SONAME\).*\[([^]]+)\]", result.stdout)
    return {"needed": sorted(set(re.findall(r"\(NEEDED\).*\[([^]]+)\]", result.stdout))),
            "soname": soname.group(1) if soname else None, "sections": sections}


def walk(root):
    """Sorted lstat inventory, including root, without traversing symlinks."""
    entries = []

    def visit(path):
        info = path.lstat()
        entries.append((path.relative_to(root).as_posix(), info))
        if stat.S_ISDIR(info.st_mode):
            for child in sorted(path.iterdir()):
                visit(child)
        elif not (stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode)):
            raise ValueError("unsupported special file: " + str(path))

    visit(root)
    return entries


def dependency_sets(files, aliases):
    libraries = {}
    for name, record in files.items():
        if name.startswith("sdk/host/") and record.get("elf"):
            libraries[Path(name).name] = name
            if record["elf"]["soname"]:
                libraries[record["elf"]["soname"]] = name
    for alias, canonical in aliases.items():
        if alias.startswith("sdk/host/") and canonical in files and files[canonical].get("elf"):
            libraries[Path(alias).name] = canonical
    roots = [name for name in files if name == "bin/selac" or
             name.startswith("sdk/host/") and "/bin/" in name and Path(name).name in TOOLS]

    def closure(block_archive):
        found = set()
        pending = list(roots)
        while pending:
            name = pending.pop()
            if name in found or block_archive and Path(name).name.startswith("libarchive"):
                continue
            found.add(name)
            for needed in (files[name].get("elf") or {}).get("needed", []):
                if needed in libraries:
                    pending.append(libraries[needed])
        return found

    reachable = closure(False)
    return reachable, reachable - closure(True)


def classify(name, reachable):
    if name == "bin/selac":
        return "selac"
    if name.startswith("sdk/host/"):
        if "/bin/" in name and Path(name).name in TOOLS:
            return "llvm_tools." + Path(name).name
        if "/lib/clang/" in name:
            return "compiler_rt"
        if Path(name).name.startswith("libLLVM"):
            return "shared_llvm"
        if Path(name).name.startswith(("libxml", "libicu")):
            return "compiler_libraries.xml_icu"
        if Path(name).name.startswith("libarchive"):
            return "compiler_libraries.archive"
        if Path(name).name.startswith(("libz.", "libzstd", "liblzma", "liblz4", "libbz2")):
            return "compiler_libraries.compression"
        return "compiler_libraries.other" if name in reachable else "compiler_libraries.unreferenced"
    return "native_runtime" if name.startswith("sdk/sysroots/") else "metadata"


class ByteCounter:
    def __init__(self):
        self.size = 0

    def write(self, data):
        self.size += len(data)
        return len(data)

    def flush(self):
        pass


def compressed_bytes(root, entries):
    sink = ByteCounter()
    hardlinks = {}
    with gzip.GzipFile(filename="", mode="wb", fileobj=sink, compresslevel=1, mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode="w|", format=tarfile.PAX_FORMAT) as archive:
            for name, info in entries:
                item = tarfile.TarInfo(name)
                item.mode = stat.S_IMODE(info.st_mode)
                # TarInfo defaults give uid/gid/mtime zero and empty user names.
                if stat.S_ISDIR(info.st_mode):
                    item.type = tarfile.DIRTYPE
                elif stat.S_ISLNK(info.st_mode):
                    item.type = tarfile.SYMTYPE
                    item.linkname = os.readlink(root / name)
                else:
                    inode = (info.st_dev, info.st_ino)
                    if inode in hardlinks:
                        item.type = tarfile.LNKTYPE
                        item.linkname = hardlinks[inode]
                    else:
                        hardlinks[inode] = name
                        item.size = info.st_size
                        with (root / name).open("rb") as stream:
                            archive.addfile(item, stream)
                        continue
                archive.addfile(item)
    return sink.size


def measure(bundle, compressed=False):
    root = Path(bundle).resolve(strict=True)
    if not root.is_dir():
        raise ValueError("bundle must be a directory: " + str(root))
    entries = walk(root)
    files, aliases, symlinks, inodes = {}, {}, {}, {}
    allocated = non_file_allocated = 0
    seen_allocations = set()
    # Prefer public compiler/tool names if an inode also has an arbitrary alias.
    # Their entire cost remains attributed to that role, not to metadata.
    def role_order(entry):
        name = entry[0]
        role = 0 if name == "bin/selac" else (1 if name.startswith("sdk/host/") and
               "/bin/" in name and Path(name).name in TOOLS else 2)
        return role, name

    for name, info in sorted(entries, key=role_order):
        inode = (info.st_dev, info.st_ino)
        blocks = info.st_blocks * 512
        if inode not in seen_allocations:
            allocated += blocks
            if not stat.S_ISREG(info.st_mode):
                non_file_allocated += blocks
            seen_allocations.add(inode)
        if stat.S_ISLNK(info.st_mode):
            symlinks[name] = os.readlink(root / name)
            # Resolve only lexically; never follow an external alias to read data.
            target = os.path.normpath(os.path.join(os.path.dirname(name), symlinks[name]))
            if not os.path.isabs(target) and target != ".." and not target.startswith("../"):
                aliases[name] = target
        elif stat.S_ISREG(info.st_mode):
            if inode in inodes:
                aliases[name] = inodes[inode]
                continue
            inodes[inode] = name
            files[name] = {"regular_file_bytes": info.st_size, "allocated_bytes": blocks,
                           "elf": elf_info(root / name)}
    # Canonicalize alias chains without following symlinks in the filesystem.
    for name in list(aliases):
        target, seen = aliases[name], {name}
        while target in aliases and target not in seen:
            seen.add(target)
            target = aliases[target]
        aliases[name] = target
    reachable, archive_only = dependency_sets(files, aliases)
    groups = {name: {"regular_file_bytes": 0, "allocated_bytes": 0, "file_count": 0}
              for name in GROUPS}
    sections = dict.fromkeys(SECTION_KEYS, 0)
    for name, record in files.items():
        group = classify(name, reachable)
        record["group"] = group
        for key in ("regular_file_bytes", "allocated_bytes"):
            groups[group][key] += record[key]
        groups[group]["file_count"] += 1
        for key, value in (record["elf"] or {}).get("sections", {}).items():
            sections[key] += value
    report = {"path": str(root), "regular_file_bytes": sum(x["regular_file_bytes"] for x in files.values()),
              "allocated_bytes": allocated, "non_file_allocated_bytes": non_file_allocated,
              "groups": groups, "sections": sections, "files": files, "aliases": aliases,
              "archive_only_dependency_subset": {
                  "regular_file_bytes": sum(files[name]["regular_file_bytes"] for name in archive_only),
                  "files": sorted(archive_only)},
              "symlinks": symlinks, "counts": {"unique_regular_files": len(files),
                  "hardlink_aliases": len(aliases.keys() - symlinks.keys()), "symlinks": len(symlinks)}}
    if compressed:
        report["compressed_tar_gzip1_bytes"] = compressed_bytes(root, entries)
    return report


def difference(current, baseline):
    keys = ("regular_file_bytes", "allocated_bytes", "non_file_allocated_bytes")
    result = {key: current[key] - baseline[key] for key in keys}
    if "compressed_tar_gzip1_bytes" in current:
        result["compressed_tar_gzip1_bytes"] = current["compressed_tar_gzip1_bytes"] - baseline["compressed_tar_gzip1_bytes"]
    result["groups"] = {name: {key: current["groups"][name][key] - baseline["groups"][name][key]
                               for key in current["groups"][name]} for name in GROUPS}
    result["sections"] = {key: current["sections"][key] - baseline["sections"][key] for key in SECTION_KEYS}
    result["archive_only_dependency_subset_bytes"] = (current["archive_only_dependency_subset"]["regular_file_bytes"] -
                                                      baseline["archive_only_dependency_subset"]["regular_file_bytes"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", metavar="BUNDLE")
    parser.add_argument("--compare", metavar="BASELINE")
    parser.add_argument("--json", action="store_true", help="emit deterministic machine-readable JSON")
    parser.add_argument("--compressed", action="store_true", help="also measure a deterministic tar/gzip-1 stream")
    args = parser.parse_args()
    try:
        output = {"schema_version": 1, "bundle": measure(args.bundle, args.compressed)}
        if args.compare:
            output["baseline"] = measure(args.compare, args.compressed)
            output["delta"] = difference(output["bundle"], output["baseline"])
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, "bundle-size: " + str(error) + "\n")
    if args.json:
        print(json.dumps(output, sort_keys=True, indent=2))
        return
    bundle = output["bundle"]
    print("Bundle:", bundle["path"])
    if "baseline" in output:
        print("Baseline:", output["baseline"]["path"])
    print(f"{'Component':42} {'File bytes':>14} {'MiB':>10}" + (f" {'Delta bytes':>14}" if args.compare else ""))
    for name, values in bundle["groups"].items():
        delta = f" {output['delta']['groups'][name]['regular_file_bytes']:+14,d}" if args.compare else ""
        print(f"{name:42} {values['regular_file_bytes']:14,d} {values['regular_file_bytes'] / 1048576:10.2f}{delta}")
    for label, key in (("TOTAL unique regular files", "regular_file_bytes"),
                       ("Allocated disk bytes (includes directories)", "allocated_bytes"),
                       ("Deterministic tar/gzip-1 download", "compressed_tar_gzip1_bytes")):
        if key in bundle:
            delta = f" {output['delta'][key]:+14,d}" if args.compare else ""
            print(f"{label:42} {bundle[key]:14,d} {bundle[key] / 1048576:10.2f}{delta}")
    print("ELF sections (part of file totals, not additional bytes):")
    for key, value in bundle["sections"].items():
        print(f"  {key:38} {value:14,d} {value / 1048576:10.2f}")
    print("Symbols = .symtab + .strtab; not a prediction of strip savings.")
    print("ELF sections exclude static archive members, file headers/padding, and NOBITS.")
    archive_bytes = bundle["archive_only_dependency_subset"]["regular_file_bytes"]
    print(f"Archive-exclusive dependency subset: {archive_bytes:,} bytes ({archive_bytes / 1048576:.2f} MiB), already included above.")
    print("This subset uses compiler DT_NEEDED closure; unreferenced does not mean safe to delete.")
    print("Symlinks are not followed; hardlinked data is counted once. Disk allocation is filesystem-dependent.")


if __name__ == "__main__":
    main()
