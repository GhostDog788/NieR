#!/usr/bin/env python3
"""Bounded VM metadata/recipe tests; no downloads, builds, guests or global writes."""
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("relocate_ctest", ROOT / "tests/vm/relocate-ctest.py")
relocator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relocator)


class VMHarness(unittest.TestCase):
    def test_actual_vm_resource_block_selects_corpus_default_and_honors_overrides(self):
        script = (ROOT / "tests/consumer-vm.sh").read_text()
        start = script.index("memory=${SELA_VM_RAM_MIB")
        resources = script[start:script.index("accelerator=${SELA_VM_ACCEL", start)]
        # Execute the runner's actual resource block only: no package staging,
        # kernel download, guest launch, or substitute timeout implementation.
        command = 'fixtures=$1\n' + resources + '\nprintf "%s:%s" "$memory" "$deadline"\n'
        with tempfile.TemporaryDirectory(prefix="sela-vm-resources-") as temporary:
            fixtures = Path(temporary)
            for corpus in (False, True):
                if corpus:
                    (fixtures / "corpus.list").write_text("zlib\n")
                for override in (None, "", "30", "900", "1800", "3600", "0", "29", "3601", "invalid"):
                    with self.subTest(corpus=corpus, override=override):
                        environment = {key: value for key, value in os.environ.items()
                                       if key not in ("SELA_VM_TIMEOUT", "SELA_VM_RAM_MIB")}
                        if override is not None:
                            environment["SELA_VM_TIMEOUT"] = override
                        result = subprocess.run(["bash", "-euo", "pipefail", "-c", command,
                                                 "bash", str(fixtures)], env=environment,
                                                capture_output=True, text=True, timeout=5)
                        if override in ("0", "29", "3601", "invalid"):
                            self.assertNotEqual(result.returncode, 0)
                        else:
                            expected = override or ("3600" if corpus else "900")
                            self.assertEqual(result.returncode, 0, result.stderr)
                            self.assertEqual(result.stdout, "3072:" + expected)

    def test_publication_guard_detects_changed_or_missing_inputs(self):
        with tempfile.TemporaryDirectory(prefix="sela-publication-inputs-") as temporary:
            work = Path(temporary)
            tools, sdk = work / "tools with spaces", work / "sdk"
            paths = [tools / name for name in ("sela.cfg", "sela-build", "sela-native-ld", "sela-ld", "libsela-clang.so", "sela-capture.so")]
            paths += [sdk / "host/usr/lib/llvm-18/bin" / name for name in ("clang", "ld.lld")]
            paths += [sdk / "sdk-lock.sha256", work / "targets.json"]
            for path in paths:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("original input\n")
            receipt = work / "inputs.sha256"
            helper = ROOT / "tests/publication-inputs.sh"
            subprocess.run(["bash", "-c", 'source "$1"; sela_record_publication_inputs "$2" "$3" "$4" "$5"', "bash", str(helper), str(receipt), str(tools / "sela.cfg"), str(sdk), str(work / "targets.json")], check=True, timeout=10)
            original_receipt = receipt.read_bytes()
            check = ["bash", "-c", 'source "$1"; sela_verify_publication_inputs "$2"', "bash", str(helper), str(receipt)]
            self.assertEqual(subprocess.run(check, capture_output=True, timeout=10).returncode, 0)
            paths[1].write_text("different compiler\n")
            changed = subprocess.run(check, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("mixed checkpoint cannot be qualified", changed.stderr)
            paths[1].write_text("original input\n")
            paths[-1].unlink()
            self.assertNotEqual(subprocess.run(check, capture_output=True, timeout=10).returncode, 0)
            self.assertEqual(receipt.read_bytes(), original_receipt)

    def test_vm_packages_cover_every_registered_device(self):
        targets = {target["id"]: target for target in json.loads((ROOT / "sdk/targets.json").read_text())["targets"]}
        seen = set()
        for line in (ROOT / "tests/vm/packages.lock").read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            target, role, package, arch, version, digest, url = line.split()
            self.assertIn(target, targets)
            self.assertEqual(arch, targets[target]["packageArch"])
            self.assertIn(role, ("kernel", "busybox", "headers"))
            self.assertNotIn((target, role), seen)
            seen.add((target, role))
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
            self.assertTrue(url.startswith("https://snapshot."))
            self.assertIn("20260910T000000Z", url)
            self.assertTrue(package and version)
        expected = {(target, role) for target in targets for role in ("kernel", "busybox")}
        expected.update((target, "headers") for target in targets if target != "i686")
        self.assertEqual(seen, expected)

    def test_test_tools_have_independent_per_architecture_closures(self):
        rows = {}
        for line in (ROOT / "tests/vm/test-tools.lock").read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            package, arch, version, digest, url = line.split()
            self.assertNotIn((arch, package), rows)
            rows[(arch, package)] = version
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
            self.assertTrue(url.startswith("https://snapshot.ubuntu.com/"))
        for arch in ("amd64", "i386", "armhf", "arm64"):
            for package in ("cmake", "make", "libarchive13t64", "libstdc++6", "libgcc-s1", "libcurl4t64"):
                self.assertIn((arch, package), rows)
        self.assertIn(("all", "cmake-data"), rows)

    def test_no_emulator_preserves_commands_and_properties(self):
        original = 'add_test([=[parse]=] "/native/build/tests/parse" "argument")\nset_tests_properties([=[parse]=] PROPERTIES TIMEOUT "30")\n'
        expected = original.replace("/native/build", "@SELA_CORPUS_DESTINATION@")
        self.assertEqual(relocator.relocate(original, "/native/build", []), expected)

    def test_exact_cross_emulator_is_removed_only_from_test_command(self):
        emulator = ["/usr/bin/env", "-u", "LD_LIBRARY_PATH", "-u", "LD_PRELOAD", "/usr/bin/qemu-arm"]
        prefix = " ".join('"' + part + '"' for part in emulator)
        original = 'add_test([=[parse]=] ' + prefix + ' "/native/build/tests/parse" "arg")\n'
        original += 'set_tests_properties([=[parse]=] PROPERTIES WILL_FAIL "TRUE")\n'
        expected = original.replace(prefix + " ", "").replace("/native/build", "@SELA_CORPUS_DESTINATION@")
        self.assertEqual(relocator.relocate(original, "/native/build", emulator), expected)

    def test_unknown_emulator_position_is_not_silently_rewritten(self):
        with self.assertRaisesRegex(ValueError, "Unrelocated"):
            relocator.relocate('add_test(test "/usr/bin/qemu-arm" "/another/test")\n', "/native/build", ["/usr/bin/qemu-arm"])

    def test_cli_preserves_original_files(self):
        with tempfile.TemporaryDirectory(prefix="sela-relocate-test-") as temporary:
            build, output = Path(temporary) / "build", Path(temporary) / "output"
            build.mkdir()
            (build / "CMakeCache.txt").write_text("CMAKE_CROSSCOMPILING_EMULATOR:STRING=/usr/bin/env;-u;LD_LIBRARY_PATH\n")
            sources = {}
            for directory in (".", "tests", "fuzzing"):
                path = build / directory / "CTestTestfile.cmake"
                path.parent.mkdir(exist_ok=True)
                sources[path] = ('add_test([=[original]=] "/usr/bin/env" "-u" "LD_LIBRARY_PATH" "' + str(build) + '/test")\n')
                path.write_text(sources[path])
            subprocess.run(["python3", "-B", str(ROOT / "tests/vm/relocate-ctest.py"), str(build), str(output)], check=True, timeout=10)
            for path, original in sources.items():
                self.assertEqual(path.read_text(), original)
                result = (output / path.relative_to(build)).read_text()
                self.assertIn('add_test([=[original]=] "@SELA_CORPUS_DESTINATION@/test")', result)
                self.assertNotIn("/usr/bin/env", result)

    def test_guest_metadata_comes_from_registry_and_is_posix_readable(self):
        with tempfile.TemporaryDirectory(prefix="sela-target-fixtures-") as temporary:
            fixtures = Path(temporary)
            subprocess.run(["python3", "-B", str(ROOT / "tests/vm/stage-target-metadata.py"), str(fixtures)], check=True, timeout=10)
            targets = json.loads((ROOT / "sdk/targets.json").read_text())["targets"]
            self.assertEqual((fixtures / "targets.list").read_text().splitlines(), [target["id"] for target in targets])
            for target in targets:
                env = fixtures / "target-metadata" / (target["id"] + ".env")
                actual = subprocess.check_output(["sh", "-c", '. "$1"; printf "%s:%s:%s" "$SELA_TARGET_ID" "$SELA_TARGET_ELF_CLASS" "$SELA_TARGET_ELF_MACHINE"', "sh", str(env)], text=True, timeout=10)
                self.assertEqual(actual, f'{target["id"]}:{target["elfClass"]}:{target["elfMachine"]}')


if __name__ == "__main__":
    unittest.main()
