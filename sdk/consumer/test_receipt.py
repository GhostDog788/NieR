#!/usr/bin/env python3
"""Exercise consumer SDK state guards without compiling or changing an SDK."""
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from receipt import (atomic_write, claim_profile, elf_dynamic, invalidate,
                     runtime_libraries, validate_build_roots, validate_linkage,
                     validate_tool_linkage)


class TemporarySDKTestCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="sela-consumer-receipt-")
        self.addCleanup(self.temporary.cleanup)
        self.sdk = Path(self.temporary.name)

    @staticmethod
    def snapshot(root):
        return {str(path.relative_to(root)): path.read_bytes()
                for path in root.rglob("*") if path.is_file()}


class ReceiptStateTests(TemporarySDKTestCase):
    def test_fresh_profile_claim_is_readable_and_idempotent(self):
        claim_profile(self.sdk, "i686")
        marker = self.sdk / "consumer-profile"
        self.assertEqual(marker.read_bytes(), b"i686\n")
        self.assertEqual(marker.stat().st_mode & 0o777, 0o644)
        original = self.snapshot(self.sdk)
        claim_profile(self.sdk, "i686")
        self.assertEqual(self.snapshot(self.sdk), original)

    def test_other_profile_rejection_preserves_existing_readiness(self):
        claim_profile(self.sdk, "i686")
        for name in ("consumer-sdk.json", "sdk-lock.sha256", "development.cmake"):
            (self.sdk / name).write_text("retained " + name + "\n")
        original = self.snapshot(self.sdk)
        with self.assertRaisesRegex(ValueError, "another profile"):
            claim_profile(self.sdk, "x86_64")
        self.assertEqual(self.snapshot(self.sdk), original)

    def test_other_width_files_are_rejected_even_with_matching_marker(self):
        for relative in ("sysroots/x86_64-linux-gnu",
                         "host/usr/lib/x86_64-linux-gnu",
                         "native-runtime/usr/lib/x86_64-linux-gnu"):
            with self.subTest(relative=relative):
                candidate = self.sdk / relative.split("/")[0]
                candidate.mkdir()
                claim_profile(candidate, "i686")
                (candidate / relative).mkdir(parents=True)
                original = self.snapshot(candidate)
                with self.assertRaisesRegex(ValueError, "other profile's files"):
                    claim_profile(candidate, "i686")
                self.assertEqual(self.snapshot(candidate), original)

    def test_unidentified_nonempty_destination_is_not_adopted(self):
        (self.sdk / "user-file").write_text("do not replace\n")
        original = self.snapshot(self.sdk)
        with self.assertRaisesRegex(ValueError, "Nonempty output"):
            claim_profile(self.sdk, "i686")
        self.assertEqual(self.snapshot(self.sdk), original)
        self.assertFalse((self.sdk / "consumer-profile").exists())

    def test_unambiguous_in_progress_profile_can_be_adopted(self):
        (self.sdk / "sysroots/i686-linux-gnu").mkdir(parents=True)
        claim_profile(self.sdk, "i686")
        self.assertEqual((self.sdk / "consumer-profile").read_text(), "i686\n")

    def test_profile_marker_symlink_is_never_followed(self):
        target = self.sdk / "user-marker"
        target.write_text("i686\n")
        marker = self.sdk / "consumer-profile"
        marker.symlink_to(target.name)
        with self.assertRaisesRegex(ValueError, "must not be a symlink"):
            claim_profile(self.sdk, "i686")
        self.assertTrue(marker.is_symlink())
        self.assertEqual(target.read_text(), "i686\n")

    def test_invalidation_archives_all_readiness_markers(self):
        payloads = {}
        for name in ("consumer-sdk.json", "sdk-lock.sha256", "development.cmake"):
            payloads[name] = ("previous " + name + "\n").encode()
            (self.sdk / name).write_bytes(payloads[name])
        invalidate(self.sdk)
        for name, payload in payloads.items():
            self.assertFalse((self.sdk / name).exists())
            digest = hashlib.sha256(payload).hexdigest()
            self.assertEqual((self.sdk / (name + ".previous-" + digest)).read_bytes(), payload)
        archived = self.snapshot(self.sdk)
        invalidate(self.sdk)
        self.assertEqual(self.snapshot(self.sdk), archived)

    def test_invalidation_rejects_completion_symlink(self):
        target = self.sdk / "user-completion"
        target.write_text("do not replace\n")
        completion = self.sdk / "consumer-sdk.json"
        completion.symlink_to(target.name)
        with self.assertRaisesRegex(ValueError, "must not be a symlink"):
            invalidate(self.sdk)
        self.assertTrue(completion.is_symlink())
        self.assertEqual(target.read_text(), "do not replace\n")

    def test_atomic_state_replacement_uses_readable_permissions(self):
        state = self.sdk / "state"
        state.write_bytes(b"old")
        state.chmod(0o600)
        atomic_write(state, b"new")
        self.assertEqual(state.read_bytes(), b"new")
        self.assertEqual(state.stat().st_mode & 0o777, 0o644)
        self.assertEqual({path.name for path in self.sdk.iterdir()}, {"state"})


class BuildCacheTests(TemporarySDKTestCase):
    @staticmethod
    def write_cache(path, values):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// Fake CMake cache for root-reuse validation\n" +
                        "".join(f"{key}:STRING={value}\n" for key, value in values.items()))

    def fixture(self):
        work, destination, publisher = (self.sdk / name for name in ("work", "sdk", "publisher"))
        destination.mkdir()
        for name in ("consumer-sdk.json", "sdk-lock.sha256", "development.cmake"):
            (destination / name).write_text("retained " + name + "\n")
        compiler = publisher / "host/usr/lib/llvm-18/bin"
        compilers = {"CMAKE_C_COMPILER": str(compiler / "clang"),
                     "CMAKE_CXX_COMPILER": str(compiler / "clang++")}
        self.write_cache(work / "build-i686/CMakeCache.txt",
                         dict(compilers, SELA_SDK_ROOT=str(destination),
                              SELA_BUILD_SDK_ROOT=str(publisher), SELA_DEVICE_TARGET="i686"))
        self.write_cache(work / "build-native-generators/CMakeCache.txt",
                         dict(compilers, CMAKE_PREFIX_PATH=str(publisher / "host/usr")))
        return work, destination, publisher

    def test_absent_caches_need_no_migration_or_writes(self):
        validate_build_roots(self.sdk / "work", self.sdk / "sdk", "i686", self.sdk / "publisher")
        self.assertEqual(list(self.sdk.iterdir()), [])

    def test_matching_roots_preserve_all_state(self):
        work, destination, publisher = self.fixture()
        original = self.snapshot(self.sdk)
        validate_build_roots(work, destination, "i686", publisher)
        self.assertEqual(self.snapshot(self.sdk), original)

    def test_changed_output_root_preserves_old_readiness(self):
        work, destination, publisher = self.fixture()
        original = self.snapshot(self.sdk)
        with self.assertRaisesRegex(ValueError, "SELA_SDK_ROOT"):
            validate_build_roots(work, destination / "different", "i686", publisher)
        self.assertEqual(self.snapshot(self.sdk), original)

    def test_changed_build_host_root_preserves_old_readiness(self):
        work, destination, publisher = self.fixture()
        original = self.snapshot(self.sdk)
        with self.assertRaisesRegex(ValueError, "Source build cache"):
            validate_build_roots(work, destination, "i686", publisher / "different")
        self.assertEqual(self.snapshot(self.sdk), original)

    def test_native_generator_reuse_is_checked_for_a_new_profile(self):
        work, destination, publisher = self.fixture()
        original = self.snapshot(self.sdk)
        with self.assertRaisesRegex(ValueError, "build-native-generators"):
            validate_build_roots(work, destination, "x86_64", publisher / "different")
        self.assertEqual(self.snapshot(self.sdk), original)

    def test_missing_required_cache_key_is_not_guessed(self):
        work, destination, publisher = self.fixture()
        cache = work / "build-i686/CMakeCache.txt"
        cache.write_text("\n".join(line for line in cache.read_text().splitlines()
                                  if not line.startswith("SELA_BUILD_SDK_ROOT:")) + "\n")
        original = self.snapshot(self.sdk)
        with self.assertRaisesRegex(ValueError, "SELA_BUILD_SDK_ROOT"):
            validate_build_roots(work, destination, "i686", publisher)
        self.assertEqual(self.snapshot(self.sdk), original)

    def test_duplicate_required_cache_key_is_rejected(self):
        work, destination, publisher = self.fixture()
        cache = work / "build-i686/CMakeCache.txt"
        cache.write_text(cache.read_text() + "SELA_DEVICE_TARGET:STRING=i686\n")
        with self.assertRaisesRegex(ValueError, "Duplicate source build cache key"):
            validate_build_roots(work, destination, "i686", publisher)


class LinkageReceiptTests(TemporarySDKTestCase):
    def configuration(self, linkage):
        dynamic = "ON" if linkage == "shared" else "OFF"
        values = {"LLVM_BUILD_LLVM_DYLIB": dynamic, "LLVM_LINK_LLVM_DYLIB": dynamic,
                  "BUILD_SHARED_LIBS": "OFF", "LLVM_TARGETS_TO_BUILD": "X86",
                  "LLVM_DYLIB_COMPONENTS": "all"}
        build = self.sdk / "build"
        BuildCacheTests.write_cache(build / "CMakeCache.txt", values)
        return build, values

    def test_both_linkage_configurations_are_verified_without_writes(self):
        for linkage in ("shared", "static-components"):
            with self.subTest(linkage=linkage):
                build, expected = self.configuration(linkage)
                original = self.snapshot(self.sdk)
                self.assertEqual(validate_linkage(build, linkage), expected)
                self.assertEqual(self.snapshot(self.sdk), original)

    def test_shared_request_cannot_claim_a_static_build(self):
        build, _ = self.configuration("static-components")
        with self.assertRaisesRegex(ValueError, "LLVM_BUILD_LLVM_DYLIB"):
            validate_linkage(build, "shared")

    def test_foreign_backends_or_component_policy_are_not_silently_accepted(self):
        for key, value in (("LLVM_TARGETS_TO_BUILD", "X86;AArch64"),
                           ("LLVM_DYLIB_COMPONENTS", "Core;Support"),
                           ("BUILD_SHARED_LIBS", "ON")):
            with self.subTest(key=key):
                build, values = self.configuration("shared")
                values[key] = value
                BuildCacheTests.write_cache(build / "CMakeCache.txt", values)
                with self.assertRaisesRegex(ValueError, key):
                    validate_linkage(build, "shared")

    def test_missing_linkage_cache_values_are_not_guessed(self):
        build, values = self.configuration("shared")
        del values["LLVM_LINK_LLVM_DYLIB"]
        BuildCacheTests.write_cache(build / "CMakeCache.txt", values)
        with self.assertRaisesRegex(ValueError, "LLVM_LINK_LLVM_DYLIB"):
            validate_linkage(build, "shared")

    def test_invalid_linkage_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "LLVM linkage"):
            validate_linkage(self.sdk, "whatever")

    def library(self):
        directory = self.sdk / "host/usr/lib/llvm-18/lib"
        directory.mkdir(parents=True)
        library = directory / "libLLVM.so.18.1"
        library.write_bytes(b"retained shared LLVM fixture")
        (directory / "libLLVM.so").symlink_to(library.name)
        return library

    def test_shared_runtime_receipt_binds_soname_native_abi_and_bytes(self):
        library = self.library()
        original = self.snapshot(self.sdk)
        with patch("receipt.elf_dynamic", return_value=([], library.name)) as inspect:
            recorded = runtime_libraries(self.sdk, "i686", "shared")
        inspect.assert_called_once_with(library, "i686")
        self.assertEqual(recorded, {library.name: {
            "file": "host/usr/lib/llvm-18/lib/" + library.name,
            "sha256": hashlib.sha256(library.read_bytes()).hexdigest()}})
        self.assertEqual(self.snapshot(self.sdk), original)

    def test_static_completion_does_not_claim_stale_shared_sdk_files(self):
        self.library()
        with patch("receipt.elf_dynamic") as inspect:
            self.assertEqual(runtime_libraries(self.sdk, "x86_64", "static-components"), {})
        inspect.assert_not_called()

    def test_shared_runtime_may_not_escape_sdk(self):
        library = self.library()
        link = library.parent / "libLLVM.so"
        link.unlink()
        outside = self.sdk / "host-library"
        outside.write_bytes(b"unrelated")
        link.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "inside the consumer SDK"):
            runtime_libraries(self.sdk, "x86_64", "shared")

    def test_shared_runtime_requires_usable_soname(self):
        self.library()
        for soname in (None, "../libLLVM.so.18.1", "libOther.so.18.1"):
            with self.subTest(soname=soname):
                with patch("receipt.elf_dynamic", return_value=([], soname)):
                    with self.assertRaisesRegex(ValueError, "LLVM SONAME"):
                        runtime_libraries(self.sdk, "x86_64", "shared")

    def test_shared_runtime_requires_matching_soname_alias(self):
        library = self.library()
        (library.parent / "libLLVM.so.99").write_bytes(b"different LLVM library")
        with patch("receipt.elf_dynamic", return_value=([], "libLLVM.so.99")):
            with self.assertRaisesRegex(ValueError, "SONAME alias"):
                runtime_libraries(self.sdk, "x86_64", "shared")

    def test_shared_tools_must_use_the_recorded_llvm_soname(self):
        libraries = {"libLLVM.so.18.1": {}}
        with patch("receipt.elf_dynamic", return_value=(["libLLVM.so.18.1", "libz.so.1"], None)) as inspect:
            validate_tool_linkage(self.sdk, "x86_64", "shared", libraries)
        self.assertEqual(inspect.call_count, 4)
        for needed in ([], ["libLLVM.so.17.1"], ["libLLVM.so.18.1", "libLLVM.so.17.1"]):
            with self.subTest(needed=needed):
                with patch("receipt.elf_dynamic", return_value=(needed, None)):
                    with self.assertRaisesRegex(ValueError, "LLVM dependencies"):
                        validate_tool_linkage(self.sdk, "x86_64", "shared", libraries)

    def test_static_tools_may_not_claim_a_shared_dependency(self):
        with patch("receipt.elf_dynamic", return_value=(["libLLVM.so.18.1"], None)):
            with self.assertRaisesRegex(ValueError, "LLVM dependencies"):
                validate_tool_linkage(self.sdk, "i686", "static-components", {})

    def test_elf_inspection_rejects_wrong_abi_before_running_readelf(self):
        library = self.sdk / "wrong-abi"
        header = bytearray(20)
        header[:6] = b"\x7fELF\x02\x01"
        header[18:20] = (62).to_bytes(2, "little")
        library.write_bytes(header)
        with patch("receipt.subprocess.run") as inspect:
            with self.assertRaisesRegex(ValueError, "Wrong native ELF ABI"):
                elf_dynamic(library, "i686")
        inspect.assert_not_called()


if __name__ == "__main__":
    unittest.main()
