#!/usr/bin/env bash
# Validate and atomically bundle the public-quality BYO-data release.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPOSITORY_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
FRAMEWORK_ROOT=${NEXTOS_FRAMEWORK_ROOT:-}
[[ -n $FRAMEWORK_ROOT && -d $FRAMEWORK_ROOT ]] || {
  printf '%s\n' \
    'set NEXTOS_FRAMEWORK_ROOT to the pinned NextOS framework source tree' >&2
  exit 1
}
FRAMEWORK_ROOT=$(CDPATH= cd -- "$FRAMEWORK_ROOT" && pwd -P)

NXRELEASE="$FRAMEWORK_ROOT/nxrelease/nxrelease.py"
NXRELEASE_VERSION=0.2.15
NXRELEASE_SHA256=5294ef2aafaffa892d59ee855328524a132429b20de4932aaf4ce50e76307bd1
NXGENERATOR_ROOT="$FRAMEWORK_ROOT/nxgenerator"
NXBOOTSTRAP_ROOT="$FRAMEWORK_ROOT/nxbootstrap"
NXSPLASH_ROOT="$FRAMEWORK_ROOT/nxsplash"
NXEXTRACT_ROOT="$FRAMEWORK_ROOT/../suportando_outros_devices/extrator-universal"
MANIFEST="$REPOSITORY_ROOT/nxrelease.json"
DESTINATION=${1:-"$REPOSITORY_ROOT/dist/v1.2.2"}
ARCHIVE_NAME=hitmango.zip

fail() {
  printf 'hitmango package error: %s\n' "$*" >&2
  exit 1
}

require_pinned_file() {
  local input_path=$1 expected_sha256=$2 label=$3
  [[ -f $input_path && ! -L $input_path ]] ||
    fail "$label is missing or unsafe"
  [[ $(sha256sum -- "$input_path" | awk '{print $1}') == "$expected_sha256" ]] ||
    fail "$label SHA-256 drifted"
}

[[ -f $NXRELEASE && -f $MANIFEST ]] || fail 'release tool or manifest missing'
require_pinned_file "$NXRELEASE" "$NXRELEASE_SHA256" 'NXRelease'
[[ $(python3 -B "$NXRELEASE" --version) == "nxrelease $NXRELEASE_VERSION" ]] ||
  fail 'NXRelease version drifted'
[[ $(<"$NXGENERATOR_ROOT/VERSION") == 0.2.10 ]] ||
  fail 'NXGenerator version drifted'
require_pinned_file \
  "$NXGENERATOR_ROOT/nxgenerator.py" \
  552a024506ce95b66a4e71adeeba3aad8a81951c8f027ccd49a8899518916382 \
  'NXGenerator'
[[ $(<"$NXBOOTSTRAP_ROOT/VERSION") == 0.6.16 ]] ||
  fail 'NXBootstrap version drifted'
require_pinned_file \
  "$NXBOOTSTRAP_ROOT/tools/generate-port.py" \
  3accd0e4bf598b641850ebccb07ebebebe5aa8dd92781b5576c69f428412ce74 \
  'NXBootstrap generator'
require_pinned_file \
  "$NXBOOTSTRAP_ROOT/templates/launcher.sh.in" \
  235c4172b3b4ba829056ee951d8602712e506765b98b85c802d12675f68bda07 \
  'NXBootstrap launcher template'
[[ $(<"$NXSPLASH_ROOT/VERSION") == 0.1.2 ]] || fail 'NXSplash version drifted'
require_pinned_file \
  "$NXSPLASH_ROOT/release/manifest-v1.json" \
  14c9a0ec823eceb6b53620e443821375c0d6c2dbe4004470bcee514bb8e3d118 \
  'NXSplash release manifest'
require_pinned_file \
  "$NXSPLASH_ROOT/release/aarch64/nxsplash-nextos" \
  d85d896a906a778c9af250e5617d45d085a98b18552cb0254addbbc626036c97 \
  'NXSplash AArch64 artifact'
require_pinned_file \
  "$REPOSITORY_ROOT/hitmango/nxsplash-nextos" \
  d85d896a906a778c9af250e5617d45d085a98b18552cb0254addbbc626036c97 \
  'vendored NXSplash AArch64 artifact'
[[ $(<"$NXEXTRACT_ROOT/VERSION") == 1.2.10 ]] || fail 'NXExtract version drifted'
require_pinned_file \
  "$NXEXTRACT_ROOT/nxextract.py" \
  b1b46ecdf1336b1412d7d3a3d291220aca4834a47730a5545afb382dae6036b5 \
  'NXExtract engine'
require_pinned_file \
  "$NXEXTRACT_ROOT/run-extractor.sh" \
  c931427c7226d22d7e30eee8549b50f0621dca1c9d0336634aca08631f454d7a \
  'NXExtract runner'
require_pinned_file \
  "$NXEXTRACT_ROOT/nxextract-runtime-env.sh" \
  332919a9960d4317563b647f9932d1a4367da147a425fe2f78eafd706f01563f \
  'NXExtract runtime helper'
require_pinned_file \
  "$NXEXTRACT_ROOT/ui/release/aarch64/nxextract-ui" \
  7ca901d8515ab9a084be81e05888e1fd03cec80fb03896df6331c1c95698ef56 \
  'NXExtract UI AArch64 artifact'
require_pinned_file \
  "$REPOSITORY_ROOT/hitmango/nxextract/nxextract.py" \
  b1b46ecdf1336b1412d7d3a3d291220aca4834a47730a5545afb382dae6036b5 \
  'vendored NXExtract engine'
require_pinned_file \
  "$REPOSITORY_ROOT/hitmango/nxextract/run-extractor.sh" \
  c931427c7226d22d7e30eee8549b50f0621dca1c9d0336634aca08631f454d7a \
  'vendored NXExtract runner'
require_pinned_file \
  "$REPOSITORY_ROOT/hitmango/nxextract/nxextract-runtime-env.sh" \
  332919a9960d4317563b647f9932d1a4367da147a425fe2f78eafd706f01563f \
  'vendored NXExtract runtime helper'
require_pinned_file \
  "$REPOSITORY_ROOT/hitmango/nxextract/nxextract-ui" \
  7ca901d8515ab9a084be81e05888e1fd03cec80fb03896df6331c1c95698ef56 \
  'vendored NXExtract UI AArch64 artifact'

python3 -B "$SCRIPT_DIR/check-installation.py" \
  "$REPOSITORY_ROOT/hitmango/extractor.json" \
  "$REPOSITORY_ROOT/hitmango/nxport.json" \
  "$REPOSITORY_ROOT/hitmango/INSTALLATION.md" \
  "$MANIFEST"

[[ ! -e $DESTINATION && ! -L $DESTINATION ]] ||
  fail "destination already exists: $DESTINATION"
mkdir -p -- "$(dirname -- "$DESTINATION")"

WORK_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/hitmango-package.XXXXXX")
cleanup() {
  case $WORK_ROOT in
    "${TMPDIR:-/tmp}"/hitmango-package.*)
      [[ -d $WORK_ROOT ]] && rm -rf -- "$WORK_ROOT"
      ;;
    *) printf 'refusing unsafe cleanup target: %s\n' "$WORK_ROOT" >&2 ;;
  esac
}
trap cleanup EXIT INT TERM

python3 -B "$NXRELEASE" validate --manifest "$MANIFEST"
python3 -B "$NXRELEASE" bundle \
  --manifest "$MANIFEST" \
  --stage "$WORK_ROOT/stage" \
  --destination "$DESTINATION" \
  --archive-name "$ARCHIVE_NAME" \
  --max-glibc 2.27
python3 -B "$NXRELEASE" verify \
  --archive "$DESTINATION/$ARCHIVE_NAME" \
  --sha256-file "$DESTINATION/$ARCHIVE_NAME.sha256" \
  --max-glibc 2.27

printf 'HITMAN GO BYO RELEASE: %s\n' "$DESTINATION/$ARCHIVE_NAME"
printf '%s\n' 'profile=universal-portmaster proprietary_payload=0 compatible_apk_recipe=content-pinned'
sha256sum -- "$DESTINATION/$ARCHIVE_NAME"
