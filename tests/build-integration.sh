#!/usr/bin/env bash
set -euo pipefail
nierc=$1
export NIER_SDK_ROOT=$2
clang_config=$3
publisher_bin=$(dirname -- "$clang_config")
build_tool="$publisher_bin/nier-build"
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-build-test-XXXXXX")
for fixture in build generated file-identity; do
  for system in make cmake; do
    artifact="$test_work/$fixture-$system.nier"
    if test "$system" = make; then
      make -f "$test_root/sdk/share/nier/Nier.mk" \
        NIER_BUILD_TOOL="$build_tool" \
        NIER_SOURCE_DIR="$test_root/tests/fixtures/$fixture" \
        NIER_NATIVE_OUTPUT=hello NIER_TARGETS=hello NIER_ARTIFACT="$artifact"
    else
      "$NIER_SDK_ROOT/host/usr/bin/cmake" \
        -S "$test_root/tests/fixtures/sdk-integration" -B "$test_work/$fixture-coordinator" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$NIER_SDK_ROOT/host/usr/bin/ninja" \
        -DNIER_INTEGRATION_FILE="$test_root/sdk/share/nier/Nier.cmake" \
        -DNIER_BUILD_TOOL="$build_tool" \
        -DAPPLICATION_SOURCE="$test_root/tests/fixtures/$fixture" \
        -DAPPLICATION_ARTIFACT="$artifact"
      "$NIER_SDK_ROOT/host/usr/bin/cmake" --build "$test_work/$fixture-coordinator" --target published
    fi
    "$nierc" "$artifact" -o "$test_work/$fixture-$system-native"
    if test "$fixture" = generated; then
      test "$(env -u LD_LIBRARY_PATH "$test_work/$fixture-$system-native")" = 'Generated width=8'
      # The native generator stays outside the selected published link graph.
      test "$(tar -tf "$artifact" | wc -l)" -eq 2
    elif test "$fixture" = file-identity; then
      test "$(env -u LD_LIBRARY_PATH "$test_work/$fixture-$system-native")" = '/nier/source/main.c'
    else
      test "$(env -u LD_LIBRARY_PATH "$test_work/$fixture-$system-native")" = 'Hello from a normal build: 8'
    fi
  done
done
# A normal existing build's selected shared link remains its own Nier artifact.
make -f "$test_root/sdk/share/nier/Nier.mk" NIER_BUILD_TOOL="$build_tool" \
  NIER_SOURCE_DIR="$test_root/tests/fixtures/shared-build" NIER_NATIVE_OUTPUT=libnierfixture.so \
  NIER_TARGETS=libnierfixture.so NIER_ARTIFACT="$test_work/shared.nier"
"$nierc" "$test_work/shared.nier" -o "$test_work/libnierfixture.so"
readelf -d "$test_work/libnierfixture.so" | grep -q 'SONAME.*libnierfixture.so'
native_lib="$NIER_SDK_ROOT/sysroots/x86_64-linux-gnu/usr/lib/x86_64-linux-gnu"
"$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" \
  --target=x86_64-unknown-linux-gnu --sysroot="$NIER_SDK_ROOT/sysroots/x86_64-linux-gnu" \
  -resource-dir="$NIER_SDK_ROOT/host/usr/lib/llvm-18/lib/clang/18" \
  --rtlib=compiler-rt --unwindlib=none --ld-path="$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/ld.lld" \
  "$test_root/tests/fixtures/shared-build/probe.c" -L"$test_work" -lnierfixture \
  -Wl,--dynamic-linker,"$native_lib/ld-linux-x86-64.so.2" \
  -Wl,-rpath,"$test_work:$native_lib" -Wl,-z,nodefaultlib -o "$test_work/shared-probe"
env -u LD_LIBRARY_PATH "$test_work/shared-probe"
readelf --version-info "$test_work/libnierfixture.so" | grep -q NIER_FIXTURE_1
# Publish the dependent application independently: its artifact declares the
# selected native SONAME, not a publisher path or another packed native binary.
make -f "$test_root/sdk/share/nier/Nier.mk" NIER_BUILD_TOOL="$build_tool" \
  NIER_SOURCE_DIR="$test_root/tests/fixtures/shared-build" NIER_NATIVE_OUTPUT=probe \
  NIER_TARGETS=probe NIER_ARTIFACT="$test_work/shared-app.nier"
tar -xOf "$test_work/shared-app.nier" manifest.json | grep -q ':libnierfixture.so'
"$nierc" "$test_work/shared-app.nier" --library-dir "$test_work" -o "$test_work/shared-app"
readelf -d "$test_work/shared-app" | grep -q 'NEEDED.*libnierfixture.so'
env -u LD_LIBRARY_PATH LD_DEBUG=libs "$test_work/shared-app" 2> "$test_work/loader.log"
grep -F "calling init: $test_work/libnierfixture.so" "$test_work/loader.log"
# Static-library outputs preserve all native member occurrences, including
# duplicate basenames, instead of eagerly linking or deduplicating them. Both
# members define archive_pick; only the first may be extracted. The second also
# has an unresolved external, so eager linking or replacing the first fails.
make -f "$test_root/sdk/share/nier/Nier.mk" NIER_BUILD_TOOL="$build_tool" \
  NIER_SOURCE_DIR="$test_root/tests/fixtures/static-build" NIER_NATIVE_OUTPUT=empty.a \
  NIER_TARGETS=empty.a NIER_ARTIFACT="$test_work/static-empty.nier"
"$nierc" "$test_work/static-empty.nier" -o "$test_work/static-empty.a"
test -f "$test_work/static-empty.a"
test -z "$("$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar" t "$test_work/static-empty.a")"
for system in make cmake; do
  artifact="$test_work/static-$system.nier"
  if test "$system" = make; then
    make -f "$test_root/sdk/share/nier/Nier.mk" NIER_BUILD_TOOL="$build_tool" \
      NIER_SOURCE_DIR="$test_root/tests/fixtures/static-build" \
      NIER_NATIVE_OUTPUT=libduplicate.a NIER_TARGETS=libduplicate.a NIER_ARTIFACT="$artifact"
    member=member.o
  else
    "$NIER_SDK_ROOT/host/usr/bin/cmake" \
      -S "$test_root/tests/fixtures/sdk-integration" -B "$test_work/static-coordinator" -G Ninja \
      -DCMAKE_MAKE_PROGRAM="$NIER_SDK_ROOT/host/usr/bin/ninja" \
      -DNIER_INTEGRATION_FILE="$test_root/sdk/share/nier/Nier.cmake" \
      -DNIER_BUILD_TOOL="$build_tool" \
      -DAPPLICATION_SOURCE="$test_root/tests/fixtures/static-build" \
      -DAPPLICATION_NATIVE_OUTPUT=libduplicate.a -DAPPLICATION_TARGET=duplicate \
      -DAPPLICATION_ARTIFACT="$artifact"
    "$NIER_SDK_ROOT/host/usr/bin/cmake" --build "$test_work/static-coordinator" --target published
    member=member.c.o
  fi
  "$nierc" "$artifact" -o "$test_work/static-$system.a"
  test "$("$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/llvm-ar" t "$test_work/static-$system.a")" = "$(printf '%s\n%s' "$member" "$member")"
  "$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" \
    --target=x86_64-unknown-linux-gnu --sysroot="$NIER_SDK_ROOT/sysroots/x86_64-linux-gnu" \
    -resource-dir="$NIER_SDK_ROOT/host/usr/lib/llvm-18/lib/clang/18" \
    --rtlib=compiler-rt --unwindlib=none --ld-path="$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/ld.lld" \
    "$test_root/tests/fixtures/static-build/probe.c" "$test_work/static-$system.a" \
    -Wl,--dynamic-linker,"$native_lib/ld-linux-x86-64.so.2" \
    -Wl,-rpath,"$native_lib" -Wl,-z,nodefaultlib -o "$test_work/static-$system-probe"
  env -u LD_LIBRARY_PATH "$test_work/static-$system-probe"
  "$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/clang" --config="$clang_config" \
    "$test_root/tests/fixtures/static-build/probe.c" -l:"static-$system.a" \
    -o "$test_work/static-$system-nier-probe.nier"
  "$nierc" "$test_work/static-$system-nier-probe.nier" --library-dir "$test_work" \
    -o "$test_work/static-$system-nier-probe"
  env -u LD_LIBRARY_PATH "$test_work/static-$system-nier-probe"
done
for selected in ordinary group whole; do
  make -f "$test_root/sdk/share/nier/Nier.mk" NIER_BUILD_TOOL="$build_tool" \
    NIER_SOURCE_DIR="$test_root/tests/fixtures/static-build" NIER_NATIVE_OUTPUT="$selected" \
    NIER_TARGETS="$selected" NIER_ARTIFACT="$test_work/duplicates-$selected.nier"
  "$nierc" "$test_work/duplicates-$selected.nier" -o "$test_work/duplicates-$selected"
  env -u LD_LIBRARY_PATH "$test_work/duplicates-$selected"
  count=$(tar -tf "$test_work/duplicates-$selected.nier" | grep -c '^modules/')
  if test "$selected" = whole; then test "$count" -eq 3; else test "$count" -eq 2; fi
done
# The ambiguous-definition --whole-archive link itself is invalid natively;
# witnessing must preserve stock LLD's error, not select a convenient member.
if make -f "$test_root/sdk/share/nier/Nier.mk" NIER_BUILD_TOOL="$build_tool" \
    NIER_SOURCE_DIR="$test_root/tests/fixtures/static-build" NIER_NATIVE_OUTPUT=whole-native-error \
    NIER_TARGETS=whole-native-error NIER_ARTIFACT="$test_work/whole-native-error.nier" \
    > "$test_work/whole-native-error.log" 2>&1; then
  echo 'An invalid native whole-archive link unexpectedly published' >&2
  exit 1
fi
grep -q 'duplicate symbol: archive_pick' "$test_work/whole-native-error.log"
test ! -e "$test_work/whole-native-error.nier"
for selected in archive thin whole group renamed hash-override; do
  make -f "$test_root/sdk/share/nier/Nier.mk" NIER_BUILD_TOOL="$build_tool" \
    NIER_SOURCE_DIR="$test_root/tests/fixtures/build-rejections" NIER_NATIVE_OUTPUT="$selected" \
    NIER_TARGETS="$selected" NIER_ARTIFACT="$test_work/$selected.nier"
  "$nierc" "$test_work/$selected.nier" -o "$test_work/$selected-native"
  env -u LD_LIBRARY_PATH "$test_work/$selected-native"
  if test "$selected" = hash-override; then
    readelf -d "$test_work/$selected-native" > "$test_work/hash-override.dynamic"
    grep -q '(GNU_HASH)' "$test_work/hash-override.dynamic"
    if grep -q '(HASH)' "$test_work/hash-override.dynamic"; then
      echo 'An overridden native hash setting leaked into destination semantics' >&2
      exit 1
    fi
  fi
  count=$(tar -tf "$test_work/$selected.nier" | grep -c '^modules/')
  if test "$selected" = whole; then test "$count" -eq 3; else test "$count" -eq 2; fi
done
make -f "$test_root/sdk/share/nier/Nier.mk" NIER_BUILD_TOOL="$build_tool" \
  NIER_SOURCE_DIR="$test_root/tests/fixtures/source-selection" NIER_NATIVE_OUTPUT=hello \
  NIER_TARGETS=hello NIER_ARTIFACT="$test_work/source-selection.nier"
"$nierc" "$test_work/source-selection.nier" -o "$test_work/source-selection"
env -u LD_LIBRARY_PATH "$test_work/source-selection"
# Normal SDK rebuilds replace the prior valid artifact atomically; failed
# private builds must not damage it. No migration/old-format reader is involved.
before=$(sha256sum "$test_work/build-make.nier")
make -f "$test_root/sdk/share/nier/Nier.mk" NIER_BUILD_TOOL="$build_tool" \
  NIER_SOURCE_DIR="$test_root/tests/fixtures/build" NIER_NATIVE_OUTPUT=hello \
  NIER_TARGETS=hello NIER_ARTIFACT="$test_work/build-make.nier"
test "$(sha256sum "$test_work/build-make.nier")" = "$before"
sed 's@^--ld-path=.*@--ld-path=/bin/false@' "$clang_config" > "$test_work/failing-link.cfg"
if "$build_tool" --system make --source "$test_root/tests/fixtures/build" \
  --output hello --target hello --artifact "$test_work/build-make.nier" \
  --clang-config "$test_work/failing-link.cfg"; then
  echo 'An injected final Clang linker failure unexpectedly succeeded' >&2
  exit 1
fi
test "$(sha256sum "$test_work/build-make.nier")" = "$before"
if make -f "$test_root/sdk/share/nier/Nier.mk" NIER_BUILD_TOOL="$build_tool" \
  NIER_SOURCE_DIR="$test_root/tests/fixtures/build" NIER_NATIVE_OUTPUT=hello \
  NIER_TARGETS=nier-intentionally-missing-target NIER_ARTIFACT="$test_work/build-make.nier"; then
  echo 'An invalid private build unexpectedly succeeded' >&2
  exit 1
fi
test "$(sha256sum "$test_work/build-make.nier")" = "$before"
test ! -e "$test_root/tests/fixtures/build/hello"
test ! -e "$test_root/tests/fixtures/build/hello.o"
test ! -e "$test_root/tests/fixtures/build/CMakeCache.txt"
test ! -e "$test_root/tests/fixtures/generated/generated.h"
test ! -e "$test_root/tests/fixtures/generated/generator"
printf 'Stock-Clang Make/CMake, generators/probes, archives, source selection and versioned native DSO dependencies passed.\n'
