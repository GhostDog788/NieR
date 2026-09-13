#!/usr/bin/env python3
"""Exercise consumer SDK state guards without compiling or changing an SDK."""
import hashlib
from pathlib import Path
import tempfile
import unittest

from receipt import atomic_write, claim_profile, invalidate, validate_build_roots


class TemporarySDKTestCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nier-consumer-receipt-")
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
                         dict(compilers, NIER_SDK_ROOT=str(destination),
                              NIER_BUILD_SDK_ROOT=str(publisher), NIER_DEVICE_TARGET="i686"))
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
        with self.assertRaisesRegex(ValueError, "NIER_SDK_ROOT"):
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
                                  if not line.startswith("NIER_BUILD_SDK_ROOT:")) + "\n")
        original = self.snapshot(self.sdk)
        with self.assertRaisesRegex(ValueError, "NIER_BUILD_SDK_ROOT"):
            validate_build_roots(work, destination, "i686", publisher)
        self.assertEqual(self.snapshot(self.sdk), original)

    def test_duplicate_required_cache_key_is_rejected(self):
        work, destination, publisher = self.fixture()
        cache = work / "build-i686/CMakeCache.txt"
        cache.write_text(cache.read_text() + "NIER_DEVICE_TARGET:STRING=i686\n")
        with self.assertRaisesRegex(ValueError, "Duplicate source build cache key"):
            validate_build_roots(work, destination, "i686", publisher)


if __name__ == "__main__":
    unittest.main()
