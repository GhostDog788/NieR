#!/usr/bin/env bash
set -euo pipefail
selac=$1
export SELA_SDK_ROOT=$2
clang_config=$3
publisher_bin=$(dirname -- "$clang_config")
build_tool="$publisher_bin/sela-build"
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-build-test-XXXXXX")
# Empty/invalid QEMU environment values are not equivalent to unset values.
# The publisher's child-only execution environment must remove them while
# leaving this caller's environment unchanged.
export QEMU_SET_ENV=sela-deliberately-invalid QEMU_UNSET_ENV=
for fixture in build generated file-identity; do
  for system in make cmake; do
    artifact="$test_work/$fixture-$system.sela"
    if test "$system" = make; then
      make -f "$test_root/sdk/share/sela/Sela.mk" \
        SELA_BUILD_TOOL="$build_tool" \
        SELA_SOURCE_DIR="$test_root/tests/fixtures/$fixture" \
        SELA_NATIVE_OUTPUT=hello SELA_BUILD_TARGETS=hello SELA_ARTIFACT="$artifact"
    else
      "$SELA_SDK_ROOT/host/usr/bin/cmake" \
        -S "$test_root/tests/fixtures/sdk-integration" -B "$test_work/$fixture-coordinator" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$SELA_SDK_ROOT/host/usr/bin/ninja" \
        -DSELA_INTEGRATION_FILE="$test_root/sdk/share/sela/Sela.cmake" \
        -DSELA_BUILD_TOOL="$build_tool" \
        -DAPPLICATION_SOURCE="$test_root/tests/fixtures/$fixture" \
        -DAPPLICATION_ARTIFACT="$artifact"
      "$SELA_SDK_ROOT/host/usr/bin/cmake" --build "$test_work/$fixture-coordinator" --target published
    fi
    "$selac" "$artifact" -o "$test_work/$fixture-$system-native"
    if test "$fixture" = generated; then
      test "$(env -u LD_LIBRARY_PATH "$test_work/$fixture-$system-native")" = 'Generated width=8'
      # The native generator stays outside the selected published link graph.
      test "$(tar -tf "$artifact" | wc -l)" -eq 2
    elif test "$fixture" = file-identity; then
      test "$(env -u LD_LIBRARY_PATH "$test_work/$fixture-$system-native")" = '/sela/source/main.c'
    else
      test "$(env -u LD_LIBRARY_PATH "$test_work/$fixture-$system-native")" = 'Hello from a normal build: 8'
    fi
  done
done
# ARM's native stack-protector guard comes from its managed dynamic loader.
# That genuine DT_NEEDED edge must not become an architecture-named artifact
# library or be confused with an uncaptured project DSO.
"$build_tool" --system make --source "$test_root/tests/fixtures/build" \
  --output hello --build-target hello --cflag -fstack-protector-all \
  --clang-config "$clang_config" --artifact "$test_work/stack-protected.sela" \
  --keep-private > "$test_work/stack-protected.log" 2>&1
protected_private=$(sed -n 's/^Private build evidence: //p' "$test_work/stack-protected.log")
readelf -d "$protected_private/build-armv7/source/hello" | grep -F 'Shared library: [ld-linux-armhf.so.3]'
tar -xOf "$test_work/stack-protected.sela" manifest.json | python3 -c \
  'import json,sys; assert json.load(sys.stdin)["libraries"] == []'
"$selac" "$test_work/stack-protected.sela" -o "$test_work/stack-protected-native"
test "$(env -u LD_LIBRARY_PATH "$test_work/stack-protected-native")" = 'Hello from a normal build: 8'
# A normal existing build's selected shared link remains its own Sela artifact.
make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
  SELA_SOURCE_DIR="$test_root/tests/fixtures/shared-build" SELA_NATIVE_OUTPUT=libselafixture.so \
  SELA_BUILD_TARGETS=libselafixture.so SELA_ARTIFACT="$test_work/shared.sela"
"$selac" "$test_work/shared.sela" -o "$test_work/libselafixture.so"
readelf -d "$test_work/libselafixture.so" | grep -q 'SONAME.*libselafixture.so'
native_lib="$SELA_SDK_ROOT/sysroots/x86_64-linux-gnu/usr/lib/x86_64-linux-gnu"
"$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" \
  --target=x86_64-unknown-linux-gnu --sysroot="$SELA_SDK_ROOT/sysroots/x86_64-linux-gnu" \
  -resource-dir="$SELA_SDK_ROOT/host/usr/lib/llvm-18/lib/clang/18" \
  --rtlib=compiler-rt --unwindlib=none --ld-path="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/ld.lld" \
  "$test_root/tests/fixtures/shared-build/probe.c" -L"$test_work" -lselafixture \
  -Wl,--dynamic-linker,"$native_lib/ld-linux-x86-64.so.2" \
  -Wl,-rpath,"$test_work:$native_lib" -Wl,-z,nodefaultlib -o "$test_work/shared-probe"
env -u LD_LIBRARY_PATH "$test_work/shared-probe"
readelf --version-info "$test_work/libselafixture.so" | grep -q SELA_FIXTURE_1
# Publish the dependent application independently: its artifact declares the
# selected native SONAME, not a publisher path or another packed native binary.
make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
  SELA_SOURCE_DIR="$test_root/tests/fixtures/shared-build" SELA_NATIVE_OUTPUT=probe \
  SELA_BUILD_TARGETS=probe SELA_ARTIFACT="$test_work/shared-app.sela"
tar -xOf "$test_work/shared-app.sela" manifest.json | grep -q ':libselafixture.so'
"$selac" "$test_work/shared-app.sela" --library-dir "$test_work" -o "$test_work/shared-app"
readelf -d "$test_work/shared-app" | grep -q 'NEEDED.*libselafixture.so'
env -u LD_LIBRARY_PATH LD_DEBUG=libs "$test_work/shared-app" 2> "$test_work/loader.log"
grep -F "calling init: $test_work/libselafixture.so" "$test_work/loader.log"
# Static-library outputs preserve all native member occurrences, including
# duplicate basenames, instead of eagerly linking or deduplicating them. Both
# members define archive_pick; only the first may be extracted. The second also
# has an unresolved external, so eager linking or replacing the first fails.
make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
  SELA_SOURCE_DIR="$test_root/tests/fixtures/static-build" SELA_NATIVE_OUTPUT=empty.a \
  SELA_BUILD_TARGETS=empty.a SELA_ARTIFACT="$test_work/static-empty.sela"
"$selac" "$test_work/static-empty.sela" -o "$test_work/static-empty.a"
test -f "$test_work/static-empty.a"
test -z "$("$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar" t "$test_work/static-empty.a")"
for system in make cmake; do
  artifact="$test_work/static-$system.sela"
  if test "$system" = make; then
    make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
      SELA_SOURCE_DIR="$test_root/tests/fixtures/static-build" \
      SELA_NATIVE_OUTPUT=libduplicate.a SELA_BUILD_TARGETS=libduplicate.a SELA_ARTIFACT="$artifact"
    member=member.o
  else
    "$SELA_SDK_ROOT/host/usr/bin/cmake" \
      -S "$test_root/tests/fixtures/sdk-integration" -B "$test_work/static-coordinator" -G Ninja \
      -DCMAKE_MAKE_PROGRAM="$SELA_SDK_ROOT/host/usr/bin/ninja" \
      -DSELA_INTEGRATION_FILE="$test_root/sdk/share/sela/Sela.cmake" \
      -DSELA_BUILD_TOOL="$build_tool" \
      -DAPPLICATION_SOURCE="$test_root/tests/fixtures/static-build" \
      -DAPPLICATION_NATIVE_OUTPUT=libduplicate.a -DAPPLICATION_TARGET=duplicate \
      -DAPPLICATION_ARTIFACT="$artifact"
    "$SELA_SDK_ROOT/host/usr/bin/cmake" --build "$test_work/static-coordinator" --target published
    member=member.c.o
  fi
  "$selac" "$artifact" -o "$test_work/static-$system.a"
  test "$("$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar" t "$test_work/static-$system.a")" = "$(printf '%s\n%s' "$member" "$member")"
  "$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" \
    --target=x86_64-unknown-linux-gnu --sysroot="$SELA_SDK_ROOT/sysroots/x86_64-linux-gnu" \
    -resource-dir="$SELA_SDK_ROOT/host/usr/lib/llvm-18/lib/clang/18" \
    --rtlib=compiler-rt --unwindlib=none --ld-path="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/ld.lld" \
    "$test_root/tests/fixtures/static-build/probe.c" "$test_work/static-$system.a" \
    -Wl,--dynamic-linker,"$native_lib/ld-linux-x86-64.so.2" \
    -Wl,-rpath,"$native_lib" -Wl,-z,nodefaultlib -o "$test_work/static-$system-probe"
  env -u LD_LIBRARY_PATH "$test_work/static-$system-probe"
  "$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" --config="$clang_config" \
    "$test_root/tests/fixtures/static-build/probe.c" -l:"static-$system.a" \
    -o "$test_work/static-$system-sela-probe.sela"
  "$selac" "$test_work/static-$system-sela-probe.sela" --library-dir "$test_work" \
    -o "$test_work/static-$system-sela-probe"
  env -u LD_LIBRARY_PATH "$test_work/static-$system-sela-probe"
done
for selected in ordinary group whole; do
  make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
    SELA_SOURCE_DIR="$test_root/tests/fixtures/static-build" SELA_NATIVE_OUTPUT="$selected" \
    SELA_BUILD_TARGETS="$selected" SELA_ARTIFACT="$test_work/duplicates-$selected.sela"
  "$selac" "$test_work/duplicates-$selected.sela" -o "$test_work/duplicates-$selected"
  env -u LD_LIBRARY_PATH "$test_work/duplicates-$selected"
  count=$(tar -tf "$test_work/duplicates-$selected.sela" | grep -c '^modules/')
  if test "$selected" = whole; then test "$count" -eq 3; else test "$count" -eq 2; fi
done
# The ambiguous-definition --whole-archive link itself is invalid natively;
# witnessing must preserve stock LLD's error, not select a convenient member.
if make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
    SELA_SOURCE_DIR="$test_root/tests/fixtures/static-build" SELA_NATIVE_OUTPUT=whole-native-error \
    SELA_BUILD_TARGETS=whole-native-error SELA_ARTIFACT="$test_work/whole-native-error.sela" \
    > "$test_work/whole-native-error.log" 2>&1; then
  echo 'An invalid native whole-archive link unexpectedly published' >&2
  exit 1
fi
grep -q 'duplicate symbol: archive_pick' "$test_work/whole-native-error.log"
test ! -e "$test_work/whole-native-error.sela"
for selected in archive thin whole group renamed hash-override; do
  make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
    SELA_SOURCE_DIR="$test_root/tests/fixtures/build-rejections" SELA_NATIVE_OUTPUT="$selected" \
    SELA_BUILD_TARGETS="$selected" SELA_ARTIFACT="$test_work/$selected.sela"
  "$selac" "$test_work/$selected.sela" -o "$test_work/$selected-native"
  env -u LD_LIBRARY_PATH "$test_work/$selected-native"
  if test "$selected" = hash-override; then
    readelf -d "$test_work/$selected-native" > "$test_work/hash-override.dynamic"
    grep -q '(GNU_HASH)' "$test_work/hash-override.dynamic"
    if grep -q '(HASH)' "$test_work/hash-override.dynamic"; then
      echo 'An overridden native hash setting leaked into destination semantics' >&2
      exit 1
    fi
  fi
  count=$(tar -tf "$test_work/$selected.sela" | grep -c '^modules/')
  if test "$selected" = whole; then test "$count" -eq 3; else test "$count" -eq 2; fi
done
make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
  SELA_SOURCE_DIR="$test_root/tests/fixtures/source-selection" SELA_NATIVE_OUTPUT=hello \
  SELA_BUILD_TARGETS=hello SELA_ARTIFACT="$test_work/source-selection.sela"
"$selac" "$test_work/source-selection.sela" -o "$test_work/source-selection"
env -u LD_LIBRARY_PATH "$test_work/source-selection"
# Normal SDK rebuilds replace the prior valid artifact atomically; failed
# private builds must not damage it. No migration/old-format reader is involved.
before=$(sha256sum "$test_work/build-make.sela")
make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
  SELA_SOURCE_DIR="$test_root/tests/fixtures/build" SELA_NATIVE_OUTPUT=hello \
  SELA_BUILD_TARGETS=hello SELA_ARTIFACT="$test_work/build-make.sela"
test "$(sha256sum "$test_work/build-make.sela")" = "$before"
sed 's@^--ld-path=.*@--ld-path=/bin/false@' "$clang_config" > "$test_work/failing-link.cfg"
if "$build_tool" --system make --source "$test_root/tests/fixtures/build" \
  --output hello --build-target hello --artifact "$test_work/build-make.sela" \
  --clang-config "$test_work/failing-link.cfg"; then
  echo 'An injected final Clang linker failure unexpectedly succeeded' >&2
  exit 1
fi
test "$(sha256sum "$test_work/build-make.sela")" = "$before"
if make -f "$test_root/sdk/share/sela/Sela.mk" SELA_BUILD_TOOL="$build_tool" \
  SELA_SOURCE_DIR="$test_root/tests/fixtures/build" SELA_NATIVE_OUTPUT=hello \
  SELA_BUILD_TARGETS=sela-intentionally-missing-target SELA_ARTIFACT="$test_work/build-make.sela"; then
  echo 'An invalid private build unexpectedly succeeded' >&2
  exit 1
fi
test "$(sha256sum "$test_work/build-make.sela")" = "$before"
test ! -e "$test_root/tests/fixtures/build/hello"
test ! -e "$test_root/tests/fixtures/build/hello.o"
test ! -e "$test_root/tests/fixtures/build/CMakeCache.txt"
test ! -e "$test_root/tests/fixtures/generated/generated.h"
test ! -e "$test_root/tests/fixtures/generated/generator"
test "$QEMU_SET_ENV" = sela-deliberately-invalid
test -z "$QEMU_UNSET_ENV"
printf 'Stock-Clang Make/CMake, generators/probes, archives, source selection and versioned native DSO dependencies passed.\n'
