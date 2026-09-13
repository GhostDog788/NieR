#!/usr/bin/env python3
"""Separate source/configuration identity from successful SDK completion."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def read_cache(path, keys):
    if path.is_symlink() or not path.is_file():
        raise ValueError("Source build cache must be a regular file: " + str(path))
    values = {}
    for line in path.read_text().splitlines():
        if line.startswith(("#", "//")):
            continue
        declaration, separator, value = line.partition("=")
        key, type_separator, _ = declaration.partition(":")
        if not separator or not type_separator or key not in keys:
            continue
        if key in values:
            raise ValueError("Duplicate source build cache key: " + key)
        values[key] = value
    return values


def validate_linkage(build, linkage):
    """Record the actual stock LLVM configuration, not only a requested flag."""
    if linkage not in ("shared", "static-components"):
        raise ValueError("LLVM linkage must be shared or static-components")
    dynamic = "ON" if linkage == "shared" else "OFF"
    expected = {"LLVM_BUILD_LLVM_DYLIB": dynamic, "LLVM_LINK_LLVM_DYLIB": dynamic,
                "BUILD_SHARED_LIBS": "OFF", "LLVM_TARGETS_TO_BUILD": "X86",
                "LLVM_DYLIB_COMPONENTS": "all"}
    actual = read_cache(build / "CMakeCache.txt", expected)
    for key, value in expected.items():
        if actual.get(key) != value:
            raise ValueError(f"Consumer LLVM build has {key}={actual.get(key)!r}, expected {value!r}")
    return actual


def elf_dynamic(path, profile):
    """Inspect either native ABI without executing the input file."""
    expected_class, expected_machine = (2, 62) if profile == "x86_64" else (1, 3)
    with path.open("rb") as stream:
        header = stream.read(20)
    if (len(header) != 20 or header[:4] != b"\x7fELF" or header[4] != expected_class or
            header[5] != 1 or int.from_bytes(header[18:20], "little") != expected_machine):
        raise ValueError("Wrong native ELF ABI: " + str(path))
    result = subprocess.run(["readelf", "--dynamic", str(path)], check=True,
                            capture_output=True, text=True, env=dict(os.environ, LC_ALL="C"))
    needed = re.findall(r"\(NEEDED\).*\[([^\]]+)\]", result.stdout)
    sonames = re.findall(r"\(SONAME\).*\[([^\]]+)\]", result.stdout)
    if len(sonames) > 1:
        raise ValueError("Multiple ELF SONAME entries: " + str(path))
    return needed, sonames[0] if sonames else None


def runtime_libraries(destination, profile, linkage):
    """Bind the shared LLVM runtime's native ABI, SONAME and bytes to completion."""
    if linkage == "static-components":
        return {}
    if linkage != "shared":
        raise ValueError("LLVM linkage must be shared or static-components")
    directory = destination / "host/usr/lib/llvm-18/lib"
    library = (directory / "libLLVM.so").resolve(strict=True)
    if library.parent != directory.resolve():
        raise ValueError("Shared LLVM library must resolve inside the consumer SDK library directory")
    _, soname = elf_dynamic(library, profile)
    if not soname or not re.fullmatch(r"libLLVM[A-Za-z0-9_.+-]*\.so(?:\.[A-Za-z0-9_.+-]+)?", soname):
        raise ValueError("Missing or invalid LLVM SONAME: " + str(library))
    if (directory / soname).resolve(strict=True) != library:
        raise ValueError("LLVM SONAME alias does not resolve to the recorded runtime library")
    return {soname: {"file": str(library.relative_to(destination)), "sha256": sha256(library)}}


def validate_tool_linkage(destination, profile, linkage, libraries):
    expected = set(libraries) if linkage == "shared" else set()
    for name in ("opt", "llc", "llvm-ar", "lld"):
        needed, _ = elf_dynamic(destination / "host/usr/lib/llvm-18/bin" / name, profile)
        actual = {library for library in needed if library.startswith("libLLVM")}
        if actual != expected:
            raise ValueError(f"Consumer SDK tool {name} has LLVM dependencies {sorted(actual)}, expected {sorted(expected)}")


def atomic_write(path, content):
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(content)
        os.fchmod(stream.fileno(), 0o644)
    os.replace(temporary, path)


def invalidate(destination):
    for name in ("consumer-sdk.json", "sdk-lock.sha256", "development.cmake"):
        previous = destination / name
        if previous.is_symlink():
            raise ValueError("SDK state file must not be a symlink: " + name)
        if previous.exists():
            retained = destination / (name + ".previous-" + sha256(previous))
            os.replace(previous, retained)


def claim_profile(destination, profile):
    if profile not in ("x86_64", "i686"):
        raise ValueError("unsupported device profile")
    marker = destination / "consumer-profile"
    if marker.is_symlink():
        raise ValueError("SDK profile marker must not be a symlink")
    if marker.exists() and marker.read_text().strip() != profile:
        raise ValueError("SDK belongs to another profile; choose a fresh output directory")
    opposite = "x86_64" if profile == "i686" else "i686"
    opposite_library = "x86_64-linux-gnu" if opposite == "x86_64" else "i386-linux-gnu"
    forbidden = (destination / "sysroots" / (opposite + "-linux-gnu"),
                 destination / "host/usr/lib" / opposite_library,
                 destination / "native-runtime/usr/lib" / opposite_library)
    if any(path.exists() or path.is_symlink() for path in forbidden):
        raise ValueError("SDK contains the other profile's files; choose a fresh output directory")
    if not marker.exists():
        # Adopt only the already-created, single-profile development layout.
        # An arbitrary nonempty directory is not a consumer SDK destination.
        existing = {path.name for path in destination.iterdir()} - {"bootstrap.lock"}
        if existing and not (destination / "sysroots" / (profile + "-linux-gnu")).is_dir():
            raise ValueError("Nonempty output is not an identifiable single-profile consumer SDK")
        atomic_write(marker, (profile + "\n").encode())


def validate_build_roots(work, destination, profile, publisher):
    """Reject caches whose compiler/_INIT paths belong to different roots."""
    if profile not in ("x86_64", "i686"):
        raise ValueError("unsupported device profile")
    compiler = publisher / "host/usr/lib/llvm-18/bin"
    expected_compilers = {"CMAKE_C_COMPILER": str(compiler / "clang"),
                          "CMAKE_CXX_COMPILER": str(compiler / "clang++")}
    target = dict(expected_compilers, NIER_SDK_ROOT=str(destination),
                  NIER_BUILD_SDK_ROOT=str(publisher), NIER_DEVICE_TARGET=profile)
    native = dict(expected_compilers, CMAKE_PREFIX_PATH=str(publisher / "host/usr"))
    for build, expected in ((work / ("build-" + profile), target),
                            (work / "build-native-generators", native)):
        cache = build / "CMakeCache.txt"
        if not cache.exists() and not cache.is_symlink():
            continue
        values = read_cache(cache, expected)
        for key, value in expected.items():
            if values.get(key) != value:
                raise ValueError(
                    f"Source build cache {cache} has {key}={values.get(key)!r}, expected {value!r}; "
                    "reuse the original SDK/build-host roots or choose a fresh build cache/checkout")


def main():
    phase = sys.argv[1]
    destination = Path(sys.argv[2]).resolve()
    if phase == "claim" and len(sys.argv) == 4:
        claim_profile(destination, sys.argv[3])
        return
    if phase == "invalidate" and len(sys.argv) == 3:
        invalidate(destination)
        return
    if phase == "check-build" and len(sys.argv) == 6:
        validate_build_roots(Path(sys.argv[3]).resolve(), destination,
                             sys.argv[4], Path(sys.argv[5]).resolve())
        return
    if phase not in ("prepare", "complete") or len(sys.argv) != 7:
        raise ValueError("usage: receipt.py claim SDK PROFILE | invalidate SDK | check-build SDK WORK PROFILE PUBLISHER_SDK | prepare|complete SDK BUILD PROFILE PUBLISHER_SDK shared|static-components")
    build, profile, publisher = Path(sys.argv[3]).resolve(), sys.argv[4], Path(sys.argv[5]).resolve()
    if profile not in ("x86_64", "i686"):
        raise ValueError("unsupported device profile")
    linkage = sys.argv[6]
    llvm_configuration = validate_linkage(build, linkage)
    definition = Path(__file__).resolve().parent
    repository = definition.parent.parent
    inputs = {name: sha256(definition / name) for name in
              ("source.lock", "packages.lock", "toolchain.cmake", "mlir32-probe.cpp", "abi-probe.cpp", "receipt.py")}
    inputs["bootstrap-consumer-sdk.sh"] = sha256(repository / "scripts/bootstrap-consumer-sdk.sh")
    inputs["publisher-sdk-lock"] = (publisher / "sdk-lock.sha256").read_text().strip()
    identity = {"format": 1, "profile": profile, "llvm_version": "18.1.3", "targets": ["X86"],
                "linkage": linkage, "llvm_configuration": llvm_configuration, "inputs": inputs}
    identity_hash = hashlib.sha256(canonical(identity)).hexdigest()
    if phase == "prepare":
        invalidate(destination)
        atomic_write(destination / "sdk-lock.sha256", (identity_hash + "\n").encode())
        cache = (f'set(LLVM_DIR "{build}/lib/cmake/llvm" CACHE PATH "Consumer LLVM build" FORCE)\n'
                 f'set(MLIR_DIR "{build}/lib/cmake/mlir" CACHE PATH "Consumer MLIR build" FORCE)\n'
                 f'set(NIER_BUILD_SDK_ROOT "{publisher}" CACHE PATH "Pinned build-host SDK")\n')
        atomic_write(destination / "development.cmake", cache.encode())
        print(f"Consumer development libraries ready ({profile}, {linkage}); complete SDK receipt is still pending.")
    else:
        if (destination / "sdk-lock.sha256").read_text().strip() != identity_hash:
            raise ValueError("SDK build definition changed while building; rerun the bootstrap")
        tools = {name: sha256(destination / "host/usr/lib/llvm-18/bin" / name)
                 for name in ("opt", "llc", "llvm-ar", "lld")}
        libraries = runtime_libraries(destination, profile, linkage)
        validate_tool_linkage(destination, profile, linkage, libraries)
        receipt = dict(identity, sdk_identity=identity_hash, tools=tools, runtime_libraries=libraries)
        atomic_write(destination / "consumer-sdk.json", (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode())


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, IndexError, subprocess.CalledProcessError) as error:
        print(f"consumer receipt: {error}", file=sys.stderr)
        sys.exit(1)
