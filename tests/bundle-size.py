#!/usr/bin/env python3
"""Bounded synthetic tests for the read-only bundle-size reporter."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/bundle-size.py"
SPEC = importlib.util.spec_from_file_location("bundle_size", SCRIPT)
REPORTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORTER)


class BundleSizeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nier-bundle-size-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "bundle"
        self.root.mkdir()

    def put(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def snapshot(self):
        result = {}
        for name, info in REPORTER.walk(self.root):
            path = self.root / name
            content = os.readlink(path) if path.is_symlink() else (
                hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None)
            result[name] = (info.st_mode, info.st_mtime_ns, info.st_ino, content)
        return result

    def test_accounting_aliases_comparison_and_no_mutation(self):
        original = self.put("bin/nierc", b"compiler")
        os.link(original, self.root / "bin/hard-alias")
        (self.root / "bin/symbolic-alias").symlink_to("nierc")
        (self.root / "external").symlink_to("/definitely/not/a/bundle/file")
        self.put("sdk/host/usr/lib/llvm-18/bin/opt", b"tool")
        self.put("sdk/sysroots/x86_64-linux-gnu/usr/lib/libc.a", b"runtime")
        self.put("LICENSE", b"license")
        before = self.snapshot()
        report = REPORTER.measure(self.root, compressed=True)
        self.assertEqual(report["regular_file_bytes"], 26)
        self.assertEqual(sum(g["regular_file_bytes"] for g in report["groups"].values()), 26)
        self.assertEqual(report["groups"]["nierc"]["regular_file_bytes"], 8)
        self.assertEqual(sum(g["allocated_bytes"] for g in report["groups"].values()) + report["non_file_allocated_bytes"], report["allocated_bytes"])
        self.assertEqual(report["counts"], {"unique_regular_files": 4, "hardlink_aliases": 1, "symlinks": 2})
        self.assertGreater(report["compressed_tar_gzip1_bytes"], 0)
        self.assertEqual(report, REPORTER.measure(self.root, compressed=True))
        self.assertEqual(before, self.snapshot())
        baseline = Path(self.temporary.name) / "baseline"
        baseline.mkdir()
        (baseline / "LICENSE").write_bytes(b"old")
        output = subprocess.check_output([sys.executable, "-B", str(SCRIPT), str(self.root),
                                          "--compare", str(baseline), "--json", "--compressed"], text=True)
        parsed = json.loads(output)
        self.assertEqual(parsed["schema_version"], 1)
        self.assertEqual(parsed["delta"]["regular_file_bytes"], 23)
        self.assertEqual(before, self.snapshot())

    def test_compression_ignores_mtime_and_location(self):
        self.put("bin/nierc", b"compiler" * 20)
        before = REPORTER.measure(self.root, compressed=True)["compressed_tar_gzip1_bytes"]
        os.utime(self.root / "bin/nierc", (1234, 1234))
        copied = Path(self.temporary.name) / "elsewhere"
        shutil.copytree(self.root, copied)
        self.assertEqual(before, REPORTER.measure(copied, compressed=True)["compressed_tar_gzip1_bytes"])

    def test_dependency_grouping(self):
        prefix = "sdk/host/usr/lib/llvm-18/lib/"
        dependencies = {"bin/nierc": ["libarchive.so.13", "libz.so.1"],
                        prefix + "libarchive.so.13": ["libxml2.so.2", "libz.so.1"],
                        prefix + "libxml2.so.2": ["libicuuc.so.74"],
                        prefix + "libicuuc.so.74": ["libicudata.so.74"],
                        prefix + "libicudata.so.74": [], prefix + "libz.so.1": [],
                        prefix + "unused.so": []}
        for name in dependencies:
            self.put(name, b"fixture")

        def inspect(path):
            return {"needed": dependencies[path.relative_to(self.root).as_posix()],
                    "soname": None, "sections": dict.fromkeys(REPORTER.SECTION_KEYS, 0)}

        with patch.object(REPORTER, "elf_info", side_effect=inspect):
            report = REPORTER.measure(self.root)
        self.assertEqual(report["groups"]["compiler_libraries.xml_icu"]["file_count"], 3)
        self.assertEqual(report["groups"]["compiler_libraries.archive"]["file_count"], 1)
        self.assertEqual(report["groups"]["compiler_libraries.compression"]["file_count"], 1)
        self.assertEqual(report["groups"]["compiler_libraries.unreferenced"]["file_count"], 1)
        self.assertEqual(report["archive_only_dependency_subset"]["regular_file_bytes"], 28)
        self.assertNotIn(prefix + "libz.so.1", report["archive_only_dependency_subset"]["files"])

    @unittest.skipUnless(shutil.which("readelf") and shutil.which("true"), "needs readelf and an ELF executable")
    def test_real_elf_is_inspected_not_executed(self):
        source = Path(shutil.which("true"))
        if source.read_bytes()[:4] != b"\x7fELF":
            self.skipTest("true is not ELF")
        executable = self.put("bin/nierc", source.read_bytes())
        executable.chmod(0o600)
        before = self.snapshot()
        report = REPORTER.measure(self.root)
        self.assertGreater(report["sections"]["code"], 0)
        self.assertGreater(report["sections"]["unwind"], 0)
        self.assertIsInstance(report["files"]["bin/nierc"]["elf"]["needed"], list)
        self.assertEqual(before, self.snapshot())

    def test_invalid_input_fails_without_writing(self):
        fifo = self.root / "fifo"
        os.mkfifo(fifo)
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(self.root), "--json"],
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 1)
        self.assertIn("unsupported special file", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_section_accounting_excludes_nobits_and_dynamic_symbols(self):
        fixture = self.put("bin/nierc", b"\x7fELFfixture")
        sections = """
  [ 1] .text PROGBITS 00000100 000100 000010 00 AX 0 0 16
  [ 2] .symtab SYMTAB 00000000 000200 000018 18 4 1 8
  [ 3] .strtab STRTAB 00000000 000300 000007 00 0 0 1
  [ 4] .eh_frame PROGBITS 00000400 000400 000008 00 A 0 0 8
  [ 5] .debug_info PROGBITS 00000000 000500 000009 00 0 0 1
  [ 6] .bss NOBITS 00000600 000600 001000 00 WA 0 0 16
  [ 7] .dynsym DYNSYM 00000700 000700 000018 18 A 8 1 8
 0x0000000000000001 (NEEDED) Shared library: [libfixture.so.1]
 0x000000000000000e (SONAME) Library soname: [nierc-fixture]
"""
        with patch.object(REPORTER.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, sections, "")):
            result = REPORTER.elf_info(fixture)
        self.assertEqual(result["sections"], {"code": 16, "symbols": 31, "unwind": 8,
                                              "debug": 9, "other_file_sections": 24})
        self.assertEqual(result["needed"], ["libfixture.so.1"])
        self.assertEqual(result["soname"], "nierc-fixture")

    def test_non_dot_prefixed_elf_sections_are_counted(self):
        fixture = self.put("bin/nierc", b"\x7fELFfixture")
        sections = """
  [ 0]              NULL     00000000 000000 000000 00 0 0 0
  [ 1] custom_code  PROGBITS 00000100 000100 000010 00 AX 0 0 16
  [ 2] custom_data  PROGBITS 00000200 000200 000007 00 A 0 0 1
  [ 3] custom_bss   NOBITS   00000300 000300 001000 00 WA 0 0 16
"""
        with patch.object(REPORTER.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, sections, "")):
            result = REPORTER.elf_info(fixture)
        self.assertEqual(result["sections"], {"code": 16, "symbols": 0, "unwind": 0,
                                              "debug": 0, "other_file_sections": 7})


if __name__ == "__main__":
    unittest.main()
