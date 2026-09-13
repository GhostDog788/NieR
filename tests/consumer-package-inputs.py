#!/usr/bin/env python3
"""Black-box rejection tests for consumer package inputs, without SDK rebuilds.

Usage: python3 tests/consumer-package-inputs.py BUILD_DIR SDK_ROOT

Only private fixtures are modified. Synthetic receipts and small tool placeholders
exercise the real packager's input guards. Copied native ELF files exercise its
actual readelf checks; a test-only installer copies them into its private stage.
These are rejection tests, not evidence that a synthetic SDK can ship a compiler.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


LLVM_ROOT = Path("host/usr/lib/llvm-18")
RPATH = "$ORIGIN/../sdk/host/usr/lib/llvm-18/lib"
SONAME = "libLLVMx"
TIMEOUT_SECONDS = 20


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def readelf(path, option):
    return subprocess.check_output(
        ["readelf", option, str(path)], text=True,
        env=dict(os.environ, LC_ALL="C"), timeout=TIMEOUT_SECONDS,
    )


def dynamic_strings(path, tag):
    return re.findall(r"\(" + tag + r"\).*\[([^\]]+)\]", readelf(path, "-d"))


def replace_dynamic_string(path, old, new):
    """Mutate only a private ELF fixture's existing dynstr slot, without relinking."""
    if len(new) > len(old):
        raise AssertionError("test replacement does not fit existing ELF string")
    section = re.search(r"\]\s+\.dynstr\s+STRTAB\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)",
                        readelf(path, "-SW"))
    if not section:
        raise AssertionError("fixture ELF lacks .dynstr")
    offset, size = (int(value, 16) for value in section.groups())
    contents = bytearray(path.read_bytes())
    strings = contents[offset:offset + size]
    needle = old.encode() + b"\0"
    if strings.count(needle) != 1:
        raise AssertionError("fixture dynamic string is not unique")
    start = offset + strings.index(needle)
    contents[start:start + len(needle)] = new.encode().ljust(len(needle), b"\0")
    path.write_bytes(contents)


class Fixture:
    def __init__(self, work, compiler, library, repository, profile, shared=False):
        self.work = work
        self.build = work / "build"
        self.sdk = work / "sdk"
        self.output = work / "package"
        self.compiler = self.build / "fixture-nierc"
        self.shared = shared
        self.profile = profile
        self.build.mkdir()
        (self.sdk / LLVM_ROOT / "bin").mkdir(parents=True)
        (self.sdk / LLVM_ROOT / "lib").mkdir()
        shutil.copy2(compiler, self.compiler)
        paths = dynamic_strings(self.compiler, "RPATH")
        if len(paths) != 1:
            raise AssertionError("source nierc must have one build/install RPATH")
        replace_dynamic_string(self.compiler, paths[0], RPATH)

        # The real packager invokes this only after receipt/linkage admission.
        # It deliberately does not invoke real CMake or alter the source build.
        installer = work / "installer/bin/cmake"
        installer.parent.mkdir(parents=True)
        installer.write_text(
            "#!/usr/bin/env python3\n"
            "from pathlib import Path\nimport shutil, sys\n"
            "build = Path(sys.argv[sys.argv.index('--install') + 1])\n"
            "stage = Path(sys.argv[sys.argv.index('--prefix') + 1])\n"
            "(stage / 'bin').mkdir()\n"
            "shutil.copy2(build / 'fixture-nierc', stage / 'bin/nierc')\n"
        )
        installer.chmod(0o755)
        build_sdk = work / "build-sdk"
        strip = build_sdk / LLVM_ROOT / "bin/llvm-strip"
        strip.parent.mkdir(parents=True)
        strip.write_text("#!/bin/sh\nexit 99\n")
        strip.chmod(0o755)
        self.cache = {
            "NIER_DEVICE_TARGET:STRING": profile,
            "NIER_LLVM_COMPONENT_LINKING:BOOL": "OFF" if shared else "ON",
            "NIER_USE_CONSUMER_SDK:BOOL": "ON",
            "CMAKE_COMMAND:INTERNAL": str(installer),
            "NIER_BUILD_SDK_ROOT:PATH": str(build_sdk),
        }
        tools = {}
        for name in ("opt", "llc", "llvm-ar", "lld"):
            path = self.sdk / LLVM_ROOT / "bin" / name
            path.write_bytes(("receipt-only fixture: " + name + "\n").encode())
            tools[name] = digest(path)
        (self.sdk / LLVM_ROOT / "bin/ld.lld").symlink_to("lld")
        self.receipt = {
            "format": 1, "profile": profile, "targets": ["X86"],
            "linkage": "shared" if shared else "static-components",
            "inputs": {name: digest(repository / "sdk/consumer" / name)
                       for name in ("source.lock", "packages.lock")},
            "tools": tools, "runtime_libraries": {},
        }
        if shared:
            # A copied small DSO supplies genuine ELF SONAME metadata. This is
            # test data, never executed or represented as an LLVM implementation.
            relative = LLVM_ROOT / "lib/libLLVM-fixture.so"
            self.library = self.sdk / relative
            shutil.copy2(library, self.library)
            old_soname = dynamic_strings(self.library, "SONAME")
            if len(old_soname) != 1:
                raise AssertionError("source library must have one SONAME")
            replace_dynamic_string(self.library, old_soname[0], SONAME)
            (self.library.parent / SONAME).symlink_to(self.library.name)
            self.receipt["runtime_libraries"][SONAME] = {
                "file": str(relative), "sha256": digest(self.library),
            }
        self.save()

    def save(self):
        identity = {key: value for key, value in self.receipt.items()
                    if key not in ("sdk_identity", "tools", "runtime_libraries")}
        self.receipt["sdk_identity"] = hashlib.sha256(canonical(identity)).hexdigest()
        self.write_receipt()
        (self.sdk / "sdk-lock.sha256").write_text(self.receipt["sdk_identity"] + "\n")
        (self.build / "CMakeCache.txt").write_text(
            "".join(f"{key}={value}\n" for key, value in self.cache.items()))

    def write_receipt(self):
        (self.sdk / "consumer-sdk.json").write_text(json.dumps(self.receipt))

    def reject(self, packager, expected):
        result = subprocess.run(
            ["bash", str(packager), str(self.build), str(self.output), str(self.sdk)],
            capture_output=True, text=True, errors="replace", check=False,
            timeout=TIMEOUT_SECONDS, env=dict(os.environ, LC_ALL="C"),
        )
        if result.returncode <= 0 or expected not in result.stderr:
            raise AssertionError(f"expected {expected!r}, exit {result.returncode}\n"
                                 f"{result.stdout}{result.stderr}")


def run(build, sdk, repository):
    compiler = build / "nierc"
    with compiler.open("rb") as source:
        elf = source.read(20)
    abi = (elf[4], int.from_bytes(elf[18:20], "little"))
    if elf[:4] != b"\x7fELF" or abi not in ((2, 62), (1, 3)):
        raise AssertionError("source compiler is not a supported native ELF")
    profile = "x86_64" if abi == (2, 62) else "i686"
    multiarch = "x86_64-linux-gnu" if profile == "x86_64" else "i386-linux-gnu"
    library = (sdk / "host/usr/lib" / multiarch / "libz.so.1").resolve(strict=True)
    originals = {path: digest(path) for path in (compiler, library)}
    packager = repository / "scripts/package-consumer.sh"
    count = 0

    def check(label, mutate, expected, shared=False, existing=None):
        nonlocal count
        with tempfile.TemporaryDirectory(prefix="nier-package-inputs-") as temporary:
            fixture = Fixture(Path(temporary), compiler, library, repository, profile, shared)
            if existing == "directory":
                fixture.output.mkdir()
                marker = fixture.output / "keep"
                marker.write_bytes(b"existing user package\n")
            elif existing == "file":
                marker = fixture.output
                marker.write_bytes(b"existing user file\n")
            elif existing == "symlink":
                marker = fixture.work / "user-target"
                marker.write_bytes(b"existing symlink target\n")
                fixture.output.symlink_to(marker.name)
            if existing:
                before = digest(marker)
            mutate(fixture)
            fixture.reject(packager, expected)
            if existing:
                if digest(marker) != before or (existing == "symlink" and not fixture.output.is_symlink()):
                    raise AssertionError("existing output or its target was changed")
            elif fixture.output.exists() or fixture.output.is_symlink():
                raise AssertionError("invalid input published an output package")
            count += 1
            print(f"PASS: {label}", flush=True)

    def corrupt_receipt(fixture):
        fixture.receipt["profile"] = "changed-without-new-identity"
        fixture.write_receipt()

    def corrupt_tool(fixture):
        (fixture.sdk / LLVM_ROOT / "bin/opt").write_bytes(b"corrupted tool\n")

    def linkage_mismatch(fixture):
        fixture.cache["NIER_LLVM_COMPONENT_LINKING:BOOL"] = "ON" if fixture.shared else "OFF"
        fixture.save()

    def corrupt_library(fixture):
        with fixture.library.open("ab") as output:
            output.write(b"corrupted DSO")

    def wrong_soname(fixture):
        record = fixture.receipt["runtime_libraries"].pop(SONAME)
        fixture.receipt["runtime_libraries"]["libLLVMwrong"] = record
        (fixture.library.parent / "libLLVMwrong").symlink_to(fixture.library.name)
        fixture.save()

    def wrong_machine(fixture):
        data = bytearray(fixture.compiler.read_bytes())
        data[18:20] = (183).to_bytes(2, "little")  # EM_AARCH64 in a private fixture.
        fixture.compiler.write_bytes(data)

    def unexpected_dependency(fixture):
        needed = dynamic_strings(fixture.compiler, "NEEDED")
        if not needed:
            raise AssertionError("fixture requires a dynamic dependency")
        replace_dynamic_string(fixture.compiler, needed[0], SONAME)

    check("corrupted receipt", corrupt_receipt, "Consumer SDK receipt mismatch")
    check("corrupted tool", corrupt_tool, "Consumer SDK tool changed: opt")
    check("static SDK / shared compiler mismatch", linkage_mismatch,
          "Compiler LLVM linkage does not match its consumer SDK")
    check("shared SDK / static compiler mismatch", linkage_mismatch,
          "Compiler LLVM linkage does not match its consumer SDK", shared=True)
    check("corrupted shared DSO hash", corrupt_library,
          "Consumer SDK LLVM runtime changed:", shared=True)
    check("wrong shared SONAME record", wrong_soname,
          "LLVM runtime SONAME does not match its receipt", shared=True)
    check("wrong native ELF machine", wrong_machine, "Wrong ELF machine in")
    check("unexpected LLVM dependency", unexpected_dependency,
          "Unexpected LLVM runtime dependency:")
    for kind in ("directory", "file", "symlink"):
        check(f"existing output {kind} preserved", lambda fixture: None,
              "Consumer bundle output must not already exist:", existing=kind)
    for path, expected in originals.items():
        if digest(path) != expected:
            raise AssertionError("test modified an original compiler/SDK file: " + str(path))
    print(f"Consumer package input guards: {count} checks passed ({profile}).")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("sdk_root", type=Path)
    args = parser.parse_args()
    run(args.build_dir.resolve(strict=True), args.sdk_root.resolve(strict=True),
        Path(__file__).resolve().parent.parent)


if __name__ == "__main__":
    main()
