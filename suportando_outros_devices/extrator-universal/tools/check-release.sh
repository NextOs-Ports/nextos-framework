#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

# readelf/grep contracts below match untranslated tool output.
export LC_ALL=C LANG=C

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$PROJECT_ROOT"

require_ui=0
if [ "${1:-}" = --require-ui ]; then
  require_ui=1
  shift
fi
if [ "$#" -ne 0 ]; then
  printf 'usage: %s [--require-ui]\n' "${0##*/}" >&2
  exit 2
fi

fail() {
  printf 'NXExtract release gate failed: %s\n' "$*" >&2
  exit 1
}

require_release_file() {
  local path=$1 link_count

  [ ! -L "$path" ] || fail "release member is a symlink: $path"
  [ -f "$path" ] || fail "release member is not a regular file: $path"
  [ -s "$path" ] || fail "release member is empty: $path"
  link_count=$(stat -c '%h' -- "$path" 2>/dev/null) ||
    fail "cannot inspect release member: $path"
  [ "$link_count" = 1 ] ||
    fail "release member has $link_count hard links: $path"
}

required_release_files=(
  LICENSE
  VERSION
  CHANGELOG.md
  README.md
  README.en.md
  docs/terminal-result-schema-v1.json
  nxextract.py
  nxextract-runtime-env.sh
  run-extractor.sh
  examples/recipe-minimal.json
  tests/make_device_fixture.py
  tests/fake-sdl-provider.c
  tests/test_nxextract.py
  tests/test_elf_gate.sh
  tests/test_runtime_env.sh
  tests/test_visual_identity.sh
  tools/check-glibc.sh
  tools/check-nxextract-pin.sh
  tools/check-release.sh
  ui/nxextract_ui.c
  ui/schema-release-v1.json
  ui/tools/build-release.sh
  ui/tools/release-manifest.py
  ui/release/manifest-v1.json
  ui/release/aarch64/nxextract-ui
  ui/release/armv7/nxextract-ui
  ui/release/x86_64/nxextract-ui
  ui/release/i386/nxextract-ui
  ui/Dockerfile.glibc230
  ui/build-ui.sh
  ui/build-compat-container.sh
)
for release_file in "${required_release_files[@]}"; do
  require_release_file "$release_file"
done

export PYTHONDONTWRITEBYTECODE=1
python3 -B -c 'import ast, pathlib; [ast.parse(pathlib.Path(p).read_bytes(), p, feature_version=(3, 7)) for p in ("nxextract.py", "tests/make_device_fixture.py", "tests/test_nxextract.py")]'
python3 -B ui/tools/release-manifest.py --verify
python_test_count=$(python3 -B -c 'import unittest; print(unittest.defaultTestLoader.discover("tests", pattern="test_nxextract.py").countTestCases())')
python3 -B -m unittest -v tests/test_nxextract.py
bash tests/test_runtime_env.sh
bash tests/test_visual_identity.sh
bash tests/test_pin_gate.sh
bash tests/test_elf_gate.sh
python3 -m json.tool examples/recipe-minimal.json >/dev/null
python3 -m json.tool docs/terminal-result-schema-v1.json >/dev/null
python3 -B nxextract.py recipe-check \
  --recipe examples/recipe-minimal.json \
  >/dev/null
project_version="$(tr -d '[:space:]' <VERSION)"
cli_version="$(python3 -B nxextract.py --version)"
test "$cli_version" = "NXExtract $project_version"
require_release_file "docs/releases/$project_version.md"
grep -Fqx "# NXExtract $project_version" "docs/releases/$project_version.md"
grep -Fqx "## $project_version" CHANGELOG.md

mapfile -d '' proprietary_archives < <(
  find . -type f \
    \( -iname '*.apk' -o -iname '*.apkm' -o -iname '*.apks' \
       -o -iname '*.xapk' -o -iname '*.obb' -o -iname '*.zip' \) \
    -print0
)
[ "${#proprietary_archives[@]}" -eq 0 ] ||
  fail "source release contains APK/OBB/ZIP payload data"

mapfile -d '' linked_members < <(
  find . -type l -not -path './.git/*' -print0
)
[ "${#linked_members[@]}" -eq 0 ] ||
  fail "source release contains a symbolic link: ${linked_members[0]}"

while IFS= read -r -d '' release_member; do
  [ -s "$release_member" ] || fail "source release contains an empty file: $release_member"
  member_links=$(stat -c '%h' -- "$release_member" 2>/dev/null) ||
    fail "cannot inspect source release member: $release_member"
  [ "$member_links" = 1 ] ||
    fail "source release member has $member_links hard links: $release_member"
done < <(
  find . -type f \
    -not -path './.git/*' \
    -not -path '*/__pycache__/*' \
    -print0
)

mapfile -d '' special_members < <(
  find . \
    -not -path './.git' \
    -not -path './.git/*' \
    -not -type d -not -type f -not -type l \
    -print0
)
[ "${#special_members[@]}" -eq 0 ] ||
  fail "source release contains a special filesystem object: ${special_members[0]}"

bash -n \
  nxextract-runtime-env.sh \
  run-extractor.sh \
  tests/test_runtime_env.sh \
  tests/test_visual_identity.sh \
  tests/test_pin_gate.sh \
  tests/test_elf_gate.sh \
  ui/build-ui.sh \
  ui/build-compat-container.sh \
  ui/tools/build-release.sh \
  tools/check-glibc.sh \
  tools/check-nxextract-pin.sh \
  tools/check-release.sh

cc \
  -std=gnu11 \
  -D_GNU_SOURCE \
  -Wall \
  -Wextra \
  -Wformat=2 \
  -Wshadow \
  -Wstrict-prototypes \
  -fsyntax-only \
  ui/nxextract_ui.c

for ui_token in \
  'dlopen(names[index], RTLD_NOW | RTLD_LOCAL)' \
  'SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC' \
  'SDL_RENDERER_SOFTWARE' \
  'driver_is_invisible(driver)' \
  'SDL_CreateSoftwareRenderer' \
  'renderer=fbdev path=/dev/fb0' \
  'publish_ready(ready_path, "fbdev")' \
  'enable_portable_provider_retry()' \
  '"libEGL.so"' \
  '"libGLESv2.so"' \
  'NXEXTRACT_ALLOW_TTY_UI' \
  'readiness intentionally withheld' \
  'headless extraction'; do
  grep -Fq "$ui_token" ui/nxextract_ui.c ||
    fail "UI source lost required negotiation token: $ui_token"
done
grep -Fq -- '--require-ui' run-extractor.sh ||
  fail "packaged runner no longer requires visible UI readiness"
grep -Fq 'mandatory setup UI graphical renderer confirmed' nxextract.py ||
  fail "engine no longer verifies UI readiness before extraction"
grep -Fq 'READY_TIMEOUT_SECONDS = 40.0' nxextract.py ||
  fail "engine lost the 40-second fail-closed graphical readiness boundary"
grep -Fq 'identity, not compatibility' nxextract.py ||
  fail "engine lost the flexible APK-container recipe gate"
grep -Fq 'container extraction requires input.packages' nxextract.py ||
  fail "engine lost the package identity gate for APK containers"
if grep -Eq -- '(^|[[:space:]])-l(SDL|EGL|GLES|GL)([[:space:]]|$)' \
     ui/build-compat-container.sh; then
  fail "UI compatibility build links a private graphics/SDL provider"
fi

ui_binary=ui/build/nxextract-ui
if [ "$require_ui" -eq 1 ]; then
  ui_binary=ui/release/aarch64/nxextract-ui
fi
if [ -e "$ui_binary" ] || [ -L "$ui_binary" ]; then
  require_release_file "$ui_binary"
  [ -x "$ui_binary" ] || fail "UI ELF is not executable: $ui_binary"

  ui_header=$(readelf -hW "$ui_binary") || fail "cannot read UI ELF header"
  ui_programs=$(readelf -lW "$ui_binary") || fail "cannot read UI program headers"
  ui_dynamic=$(readelf -dW "$ui_binary") || fail "cannot read UI dynamic section"
  grep -qE 'Class:[[:space:]]+ELF64' <<<"$ui_header" ||
    fail "UI is not ELF64"
  grep -qE 'Type:[[:space:]]+DYN' <<<"$ui_header" ||
    fail "UI is not a PIE/DYN ELF"
  grep -qE 'Machine:[[:space:]]+AArch64' <<<"$ui_header" ||
    fail "UI is not AArch64"
  grep -Fq '[Requesting program interpreter: /lib/ld-linux-aarch64.so.1]' \
    <<<"$ui_programs" || fail "UI has the wrong PT_INTERP"
  grep -qE 'GNU_STACK[[:space:]].*[[:space:]]RW[[:space:]]' \
    <<<"$ui_programs" || fail "UI stack is executable or GNU_STACK is missing"
  grep -q 'GNU_RELRO' <<<"$ui_programs" || fail "UI lacks GNU_RELRO"
  grep -qE '\(FLAGS\).*BIND_NOW' <<<"$ui_dynamic" || fail "UI lacks BIND_NOW"
  grep -qE '\(FLAGS_1\).*PIE' <<<"$ui_dynamic" || fail "UI lacks PIE flag"
  if grep -qE '\((RPATH|RUNPATH)\)' <<<"$ui_dynamic"; then
    fail "UI contains RPATH/RUNPATH"
  fi
  ui_needed=$(
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' <<<"$ui_dynamic" |
      sort
  )
  [ "$ui_needed" = $'libc.so.6\nlibdl.so.2' ] ||
    fail "UI DT_NEEDED is not exactly libc.so.6 + libdl.so.2: $ui_needed"
fi

release_elfs=()
while IFS= read -r -d '' candidate; do
  magic=$(od -An -tx1 -N4 -- "$candidate" 2>/dev/null | tr -d ' \n')
  [ "$magic" = 7f454c46 ] && release_elfs+=("$candidate")
done < <(
  find . -type f \
    -not -path './.git/*' \
    -not -path '*/__pycache__/*' \
    -print0
)
if [ "${#release_elfs[@]}" -gt 0 ]; then
  tools/check-glibc.sh "${release_elfs[@]}"
fi

printf 'NXExtract release checks passed: version=%s tests=%s elfs=%s ui_required=%s\n' \
  "$project_version" "$python_test_count" "${#release_elfs[@]}" "$require_ui"
