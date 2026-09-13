#!/usr/bin/env python3
"""Bounded synthetic tests: no SDK, compiler build, network, or host credentials."""
import importlib.util
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("ci_evidence", Path(__file__).resolve().parents[1] / "scripts/ci-evidence.py")
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="sela-ci-test-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "sela-ci-job"
        self.root.mkdir()
        self.output = self.base / "diagnostics.tar.gz"

    def file(self, relative, contents=b"diagnostic"):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def collected(self, **kwargs):
        report = evidence.collect(self.root, self.output, **kwargs)
        with tarfile.open(self.output) as archive:
            self.assertTrue(all(member.isfile() and not member.name.startswith("/") and ".." not in Path(member.name).parts
                                for member in archive))
            entries = {member.name: archive.extractfile(member).read() for member in archive}
        self.assertEqual(json.loads(entries.pop("evidence-manifest.json")), report)
        return entries, report

    def test_reports_journals_logs_and_build_failure(self):
        for path in ("logs/build.log", "sela-corpus-X/qualification.txt",
                     "sela-private-X/build-armv7/native.cfg",
                     "sela-private-X/build-armv7/metadata/input.compile.json",
                     "sela-private-X/build-armv7/metadata/input.bc",
                     "sela-consumer-vm-X/serial.log", "sela-consumer-vm-X/host-settings.txt"):
            self.file(path)
        build = self.base / "build"
        (build / "Testing/Temporary").mkdir(parents=True)
        (build / "Testing/Temporary/LastTest.log").write_text("failed test")
        entries, _ = self.collected(build_roots=[build])
        self.assertEqual(len(entries), 8)
        self.assertEqual(entries["build-0/Testing/Temporary/LastTest.log"], b"failed test")

    def test_no_sdk_sources_or_vm_payload_copy(self):
        for path in ("sela-private-X/source/secret.log", "sela-corpus-X/sources/source.log",
                     "sela-consumer-vm-X/root/opt/sela/metadata/tool.json",
                     "unrelated/logs/token.log", "sela-private-X/build/metadata/native.o"):
            self.file(path)
        entries, _ = self.collected()
        self.assertEqual(entries, {})

    def test_file_directory_and_hard_links_are_not_followed(self):
        outside = self.base / "outside"
        outside.mkdir()
        secret = outside / "secret.log"
        secret.write_bytes(b"must not archive")
        logs = self.root / "logs"
        logs.mkdir()
        (logs / "linked.log").symlink_to(secret)
        (logs / "linked-directory").symlink_to(outside, target_is_directory=True)
        os.link(secret, logs / "hardlinked.log")
        os.mkfifo(logs / "named-pipe.log")
        entries, report = self.collected()
        self.assertEqual(entries, {})
        self.assertEqual(report["omitted_count"], 3)

    def test_ancestor_symlink_build_root_is_rejected(self):
        outside = self.base / "outside/child"
        outside.mkdir(parents=True)
        (outside / "CMakeCache.txt").write_text("must not archive")
        alias = self.base / "alias"
        alias.symlink_to(outside.parent, target_is_directory=True)
        entries, report = self.collected(build_roots=[alias / "child"])
        self.assertEqual(entries, {})
        self.assertEqual(report["omitted_count"], 1)

    def test_tail_limits_are_explicit_and_binaries_are_not_truncated(self):
        self.file("logs/build.log", b"prefix failure")
        self.file("sela-private-X/metadata/capture.bc", b"123456789")
        entries, report = self.collected(max_file_bytes=7, max_bytes=9)
        self.assertEqual(entries, {"temp/logs/build.log": b"failure"})
        self.assertEqual(report["included"][0]["tail_offset"], 7)
        self.assertEqual(report["omitted_count"], 1)

    def test_failed_boot_image_is_optional_and_bounded(self):
        self.file("sela-consumer-vm-X/initramfs.cpio.gz", b"boot image")
        entries, _ = self.collected(failed=True)
        self.assertIn("temp/sela-consumer-vm-X/initramfs.cpio.gz", entries)

    def test_success_does_not_duplicate_boot_image(self):
        self.file("sela-consumer-vm-X/initramfs.cpio.gz", b"boot image")
        entries, _ = self.collected()
        self.assertEqual(entries, {})

    def test_failed_boot_image_has_separate_limit_and_never_partial_bytes(self):
        self.file("sela-consumer-vm-X/initramfs.cpio.gz", b"123456789")
        entries, report = self.collected(failed=True, max_file_bytes=2, max_boot_bytes=10)
        self.assertEqual(entries["temp/sela-consumer-vm-X/initramfs.cpio.gz"], b"123456789")
        self.assertEqual(report["included"][0]["tail_offset"], 0)

    def test_failed_boot_image_excess_is_reported_not_truncated(self):
        self.file("sela-consumer-vm-X/initramfs.cpio.gz", b"123456789")
        entries, report = self.collected(failed=True, max_boot_bytes=8)
        self.assertEqual(entries, {})
        self.assertEqual(report["omitted_count"], 1)

    def test_existing_output_and_broad_root_reject(self):
        self.output.write_bytes(b"preserve")
        with self.assertRaises(FileExistsError):
            evidence.collect(self.root, self.output)
        self.assertEqual(self.output.read_bytes(), b"preserve")
        with self.assertRaises(ValueError):
            evidence.collect(self.base / "not-job-scoped", self.output)


if __name__ == "__main__":
    unittest.main()
