#!/usr/bin/env python3
"""Cheap registry checks, including the actual pinned Clang target contract."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from targets import cpp, get, load


class TargetRegistryTests(unittest.TestCase):
    def test_generated_cpp_matches_metadata(self):
        repository = Path(__file__).resolve().parent.parent
        self.assertEqual((repository / "src/targets/Targets.cpp").read_text(), cpp())

    def test_same_width_architectures_have_distinct_abis_and_machines(self):
        targets = list(load().values())
        self.assertEqual({value["id"] for value in targets}, {"x86_64", "i686", "armv7", "aarch64"})
        self.assertEqual(len({value["elfMachine"] for value in targets}), len(targets))
        self.assertEqual(len({value["abi"] for value in targets}), len(targets))
        self.assertEqual(get("i686")["wordBits"], get("armv7")["wordBits"])
        self.assertEqual(get("x86_64")["wordBits"], get("aarch64")["wordBits"])

    def test_unknown_target_is_not_inferred_from_width(self):
        for value in ("32", "64", "arm", "mips", "riscv64"):
            with self.assertRaisesRegex(ValueError, "Unsupported target"):
                get(value)

    def test_arm_eabi_and_float_flags_distinguish_objects_from_linked_products(self):
        registry = Path(__file__).with_name("targets.py")
        with tempfile.TemporaryDirectory(prefix="sela-arm-elf-flags-") as temporary:
            path = Path(temporary) / "header.elf"
            for kind, flags, expected in ((2, 0x05000400, True), (3, 0x05000400, True),
                                          (1, 0x05000000, True), (1, 0x05000400, True),
                                          (2, 0x05000000, False), (3, 0x05000200, False),
                                          (2, 0x05000600, False), (2, 0x04000400, False),
                                          (2, 0x05800400, False),
                                          (1, 0x05000200, False)):
                with self.subTest(kind=kind, flags=hex(flags)):
                    header = bytearray(52)
                    header[:7] = b"\x7fELF\x01\x01\x01"
                    header[16:18] = kind.to_bytes(2, "little")
                    header[18:20] = (40).to_bytes(2, "little")
                    header[36:40] = flags.to_bytes(4, "little")
                    path.write_bytes(header)
                    result = subprocess.run(["python3", "-B", str(registry), "check-elf", "armv7", str(path)],
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode == 0, expected, result.stderr)
                    if not expected: self.assertIn("ARM EABI5 hard-float", result.stderr)

    def test_pinned_clang_layout_cpu_features_and_char_policy(self):
        repository = Path(__file__).resolve().parent.parent
        sdk = Path(os.environ.get("SELA_SDK_ROOT", repository / ".sdk"))
        clang = sdk / "host/usr/lib/llvm-18/bin/clang"
        self.assertTrue(clang.is_file(), "Bootstrap the pinned SDK before checking target metadata")
        from targets import host_id
        environment = dict(os.environ)
        environment["LD_LIBRARY_PATH"] = str(clang.parent.parent / "lib") + ":" + str(sdk / "host/usr/lib" / get(host_id())["multiarch"])
        for target in load().values():
            with self.subTest(target=target["id"]):
                command = [str(clang), "--target=" + target["triple"], *target["clangArgs"],
                           *target["publicationClangArgs"], "-emit-llvm", "-S", "-x", "c", "-", "-o", "-"]
                result = subprocess.run(command, input="int character_signed(void) { return (char)-1 < 0; }\n",
                                        capture_output=True, text=True, check=True, timeout=20, env=environment)
                for field, pattern in (("layout", r'target datalayout = "([^"]+)"'),
                                       ("triple", r'target triple = "([^"]+)"'),
                                       ("cpu", r'"target-cpu"="([^"]+)"'),
                                       ("features", r'"target-features"="([^"]+)"')):
                    self.assertEqual(re.search(pattern, result.stdout).group(1), target[field])
                self.assertIn("ret i32 " + ("1" if target["plainCharSigned"] else "0"), result.stdout)


if __name__ == "__main__":
    unittest.main()
