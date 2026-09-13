#!/usr/bin/env bash
set -euo pipefail

selac=$(realpath -- "$1")
export SELA_SDK_ROOT
SELA_SDK_ROOT=$(realpath -- "$2")
build_tool=$(realpath -- "$3")
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
examples="$test_root/examples/hello"
guide="$test_root/docs/guides/02-toolchain-users/hello-project-walkthrough.md"
test_work=$(mktemp -d "${TMPDIR:-/tmp}/sela-hello-project-XXXXXX")

# Only the project paths contain spaces. This does not expand the qualified
# SDK/scratch-path contract used by native configure scripts.
starter="$test_work/starter project"
solution="$test_work/solution project"
practice_make="$test_work/practice make project"
practice_cmake="$test_work/practice cmake project"
clang="$SELA_SDK_ROOT/host/usr/lib/llvm-18/bin/clang"
cmake="$SELA_SDK_ROOT/host/usr/bin/cmake"
ninja="$SELA_SDK_ROOT/host/usr/bin/ninja"

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
  cmp "$examples/hello/$file" "$examples/hello-sela/$file"
done
test ! -e "$examples/hello/sela"
test -f "$examples/hello-sela/sela/Makefile"
test -f "$examples/hello-sela/sela/CMakeLists.txt"
diff -ru --exclude=build --exclude=sela "$examples/hello" "$examples/hello-sela"

mkdir "$starter" "$solution" "$practice_make" "$practice_cmake"
for file in main.c hello.c hello.h Makefile CMakeLists.txt .gitignore; do
  cp -- "$examples/hello/$file" "$starter/$file"
  cp -- "$examples/hello-sela/$file" "$solution/$file"
  cp -- "$examples/hello/$file" "$practice_make/$file"
  cp -- "$examples/hello/$file" "$practice_cmake/$file"
done
cp -R -- "$examples/hello-sela/sela" "$solution/sela"
mkdir "$practice_make/sela" "$practice_cmake/sela"
diff -ru --exclude=build "$examples/hello" "$starter"
diff -ru --exclude=build "$examples/hello-sela" "$solution"

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
extract_config '### Create `sela/Makefile`' make "$practice_make/sela/Makefile"
extract_config '### Create `sela/CMakeLists.txt`' cmake "$practice_cmake/sela/CMakeLists.txt"

check_adapter() {
  local project=$1 chosen=$2 unchosen=$3
  cmp "$solution/sela/$chosen" "$project/sela/$chosen"
  test ! -e "$project/sela/$unchosen"
}

check_practice() {
  local project=$1 chosen=$2 unchosen=$3
  for file in main.c hello.c hello.h Makefile CMakeLists.txt .gitignore; do
    cmp "$solution/$file" "$project/$file"
  done
  check_adapter "$project" "$chosen" "$unchosen"
  diff -ru --exclude=build --exclude=sela "$solution" "$project"
  diff -ru --exclude="$unchosen" "$solution/sela" "$project/sela"
}
check_practice "$practice_make" Makefile CMakeLists.txt
check_practice "$practice_cmake" CMakeLists.txt Makefile

check_output() {
  local executable=$1 greeting=$2
  printf '%s\n' "$greeting" > "$test_work/expected.stdout"
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$executable" > "$test_work/actual.stdout"
  cmp "$test_work/expected.stdout" "$test_work/actual.stdout"
}

native_make() {
  local project=$1
  (
    cd -- "$project"
    make CC="$clang"
  )
  check_output "$project/build/native-make/hello" 'Hello world'
}

native_cmake() {
  local project=$1
  "$cmake" -S "$project" -B "$project/build/native-cmake" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_C_COMPILER="$clang"
  "$cmake" --build "$project/build/native-cmake"
  check_output "$project/build/native-cmake/hello" 'Hello world'
}

publish_make() {
  local project=$1
  (
    cd -- "$project"
    make -f sela/Makefile SELA_ROOT="$test_root" SELA_BUILD_TOOL="$build_tool"
  )
}

configure_publication() {
  local project=$1
  "$cmake" -S "$project/sela" -B "$project/build/sela-cmake" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja" -DSELA_ROOT="$test_root" \
    -DSELA_BUILD_TOOL="$build_tool"
}

publish_cmake() {
  "$cmake" --build "$1/build/sela-cmake" --target publish
}

consume() {
  local project=$1 system=$2 greeting=$3
  local artifact="$project/build/sela-$system/hello.sela"
  local executable="$project/build/sela-$system/hello-native"
  test -f "$artifact"
  "$selac" inspect "$artifact"
  "$selac" "$artifact" --sdk "$SELA_SDK_ROOT" -o "$executable"
  check_output "$executable" "$greeting"
}

for project in "$starter" "$solution"; do
  native_make "$project"
  native_cmake "$project"
done
publish_make "$solution"
consume "$solution" make 'Hello world'
configure_publication "$solution"
publish_cmake "$solution"
consume "$solution" cmake 'Hello world'

# Each exercise starts from its own copy with only its chosen adapter. Neither
# branch may obtain configuration or previous outputs from the other branch.
for system in make cmake; do
  if test "$system" = make; then
    project=$practice_make
    chosen=Makefile
    unchosen=CMakeLists.txt
    other_system=cmake
  else
    project=$practice_cmake
    chosen=CMakeLists.txt
    unchosen=Makefile
    other_system=make
  fi
  "native_$system" "$project"
  if test "$system" = cmake; then configure_publication "$project"; fi
  "publish_$system" "$project"
  consume "$project" "$system" 'Hello world'
  check_practice "$project" "$chosen" "$unchosen"

  # Request publication again without modifying or reconfiguring the adapter.
  # Existing artifacts and old executables must not hide the changed source.
  cp -- "$project/build/sela-$system/hello.sela" "$test_work/$system.before.sela"
  greeting="Hello Sela $system rebuild"
  sed "s/Hello world/$greeting/" "$project/hello.c" > "$project/hello.c.updated"
  mv -- "$project/hello.c.updated" "$project/hello.c"
  grep -q "$greeting" "$project/hello.c"
  "publish_$system" "$project"
  if cmp -s "$test_work/$system.before.sela" "$project/build/sela-$system/hello.sela"; then
    printf 'ERROR: %s publication ignored the source change\n' "$system" >&2
    exit 1
  fi
  consume "$project" "$system" "$greeting"
  check_adapter "$project" "$chosen" "$unchosen"
  test ! -e "$project/build/sela-$other_system"
done
diff -ru --exclude=build "$examples/hello" "$starter"
diff -ru --exclude=build "$examples/hello-sela" "$solution"

printf 'Standalone Hello projects, independent Make-only/CMake-only guide exercises, native builds, publication, and on-demand rebuilds passed.\n'
