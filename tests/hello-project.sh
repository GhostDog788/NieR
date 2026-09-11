#!/usr/bin/env bash
set -euo pipefail

nierc=$(realpath -- "$1")
export NIER_SDK_ROOT
NIER_SDK_ROOT=$(realpath -- "$2")
build_tool=$(realpath -- "$3")
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
examples="$test_root/examples/hello"
guide="$test_root/docs/guides/02-toolchain-users/hello-project-walkthrough.md"
test_work=$(mktemp -d "${TMPDIR:-/tmp}/nier-hello-project-XXXXXX")

# Only the project paths contain spaces. This does not expand the qualified
# SDK/scratch-path contract used by native configure scripts.
starter="$test_work/starter project"
solution="$test_work/solution project"
practice="$test_work/practice project"
clang="$NIER_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
cmake="$NIER_SDK_ROOT/host/usr/bin/cmake"
ninja="$NIER_SDK_ROOT/host/usr/bin/ninja"

snapshot_examples() {
  (
    cd -- "$examples"
    find . -printf '%P\t%y\t%l\n' | LC_ALL=C sort
    find . -type f -print0 | LC_ALL=C sort -z |
      xargs -0 --no-run-if-empty sha256sum
  )
}

snapshot_examples > "$test_work/examples.before"
finish() {
  local status=$?
  trap - EXIT
  if ! snapshot_examples > "$test_work/examples.after" ||
     ! cmp "$test_work/examples.before" "$test_work/examples.after"; then
    printf 'ERROR: the walkthrough changed the repository examples\n' >&2
    status=1
  fi
  printf 'Hello project test workspace: %s\n' "$test_work"
  exit "$status"
}
trap finish EXIT

# The baseline projects must differ only by the two integration files. Ignore
# existing local build outputs, which are not part of either source project.
for file in main.c hello.c hello.h Makefile CMakeLists.txt .gitignore; do
  cmp "$examples/hello/$file" "$examples/hello-nier/$file"
done
test ! -e "$examples/hello/nier"
test -f "$examples/hello-nier/nier/Makefile"
test -f "$examples/hello-nier/nier/CMakeLists.txt"
diff -ru --exclude=build --exclude=nier "$examples/hello" "$examples/hello-nier"

mkdir "$starter" "$solution" "$practice"
for file in main.c hello.c hello.h Makefile CMakeLists.txt .gitignore; do
  cp -- "$examples/hello/$file" "$starter/$file"
  cp -- "$examples/hello-nier/$file" "$solution/$file"
  cp -- "$examples/hello/$file" "$practice/$file"
done
cp -R -- "$examples/hello-nier/nier" "$solution/nier"
mkdir "$practice/nier"
diff -ru --exclude=build "$examples/hello" "$starter"
diff -ru --exclude=build "$examples/hello-nier" "$solution"

# Reconstruct the exercise from its prose, not from the checked-in solution.
# Requiring exactly one complete named block also catches heading/fence drift.
extract_config() {
  local heading=$1 language=$2 destination=$3
  awk -v heading="$heading" -v opening="\`\`\`$language" '
    $0 == heading { headings++; waiting = 1; next }
    waiting && /^```/ {
      if ($0 != opening) exit 1
      waiting = 0; inside = 1; next
    }
    inside && /^```$/ { inside = 0; complete++; next }
    inside { print }
    END { if (headings != 1 || complete != 1 || waiting || inside) exit 1 }
  ' "$guide" > "$destination"
  test -s "$destination"
}
extract_config '### Create `nier/Makefile`' make "$practice/nier/Makefile"
extract_config '### Create `nier/CMakeLists.txt`' cmake "$practice/nier/CMakeLists.txt"
diff -ru --exclude=build "$solution" "$practice"

check_output() {
  local executable=$1 greeting=$2
  printf '%s\n' "$greeting" > "$test_work/expected.stdout"
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$executable" > "$test_work/actual.stdout"
  cmp "$test_work/expected.stdout" "$test_work/actual.stdout"
}

native_builds() {
  local project=$1
  (
    cd -- "$project"
    make CC="$clang"
  )
  check_output "$project/build/native-make/hello" 'Hello world'
  "$cmake" -S "$project" -B "$project/build/native-cmake" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_C_COMPILER="$clang"
  "$cmake" --build "$project/build/native-cmake"
  check_output "$project/build/native-cmake/hello" 'Hello world'
}

publish_make() {
  local project=$1
  (
    cd -- "$project"
    make -f nier/Makefile NIER_ROOT="$test_root" NIER_BUILD_TOOL="$build_tool"
  )
}

configure_publication() {
  local project=$1
  "$cmake" -S "$project/nier" -B "$project/build/nier-cmake" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja" -DNIER_ROOT="$test_root" \
    -DNIER_BUILD_TOOL="$build_tool"
}

publish_cmake() {
  "$cmake" --build "$1/build/nier-cmake" --target publish
}

consume() {
  local project=$1 system=$2 greeting=$3
  local artifact="$project/build/nier-$system/hello.nier"
  local executable="$project/build/nier-$system/hello-native"
  test -f "$artifact"
  "$nierc" inspect "$artifact"
  "$nierc" "$artifact" --sdk "$NIER_SDK_ROOT" -o "$executable"
  check_output "$executable" "$greeting"
}

native_builds "$starter"
native_builds "$solution"
for project in "$solution" "$practice"; do
  publish_make "$project"
  consume "$project" make 'Hello world'
  configure_publication "$project"
  publish_cmake "$project"
  consume "$project" cmake 'Hello world'
done

# Request publication again without touching/reconfiguring either coordinator.
# Existing artifacts and old executables must not hide the changed source.
diff -ru --exclude=build "$examples/hello" "$starter"
diff -ru --exclude=build "$examples/hello-nier" "$solution"
diff -ru --exclude=build "$solution" "$practice"
cp -- "$practice/build/nier-make/hello.nier" "$test_work/make.before.nier"
cp -- "$practice/build/nier-cmake/hello.nier" "$test_work/cmake.before.nier"
sed 's/Hello world/Hello Nier rebuild/' "$practice/hello.c" > "$practice/hello.c.updated"
mv -- "$practice/hello.c.updated" "$practice/hello.c"
grep -q 'Hello Nier rebuild' "$practice/hello.c"
publish_make "$practice"
publish_cmake "$practice"
for system in make cmake; do
  if cmp -s "$test_work/$system.before.nier" "$practice/build/nier-$system/hello.nier"; then
    printf 'ERROR: %s publication ignored the source change\n' "$system" >&2
    exit 1
  fi
  consume "$practice" "$system" 'Hello Nier rebuild'
done

printf 'Standalone Hello projects, guide reconstruction, native builds, both publication paths, and on-demand rebuilds passed.\n'
