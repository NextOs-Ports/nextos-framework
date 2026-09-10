#!/usr/bin/env bash
# Build and audit the public BYO-data PortMaster package.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
VERSION=$(tr -d '\r\n' < "$ROOT/version.txt")
case "$VERSION" in
  ''|*[!0-9A-Za-z._-]*)
    printf '%s\n' 'invalid version.txt' >&2
    exit 1
    ;;
esac

if [ "${PF2_SKIP_BUILD:-0}" != 1 ]; then
  "$ROOT/build_universal.sh"
fi

BIN=$ROOT/pf2-universal
[ -x "$BIN" ] || { printf '%s\n' 'pf2-universal is missing' >&2; exit 1; }

python3 "$ROOT/nxextract.py" recipe-check --recipe "$ROOT/extractor.json"
python3 "$ROOT/tests/test_import_owned_save.py"
bash "$ROOT/tests/test_nxextract_runtime_env.sh"
bash -n "$ROOT/pf2-nextos.sh" "$ROOT/run-extractor.sh" \
  "$ROOT/nxextract-runtime-env.sh" "$ROOT/import-owned-save.sh"
sh -n "$ROOT/Prizefighters 2.sh"

MAX_GLIBC=$(readelf --version-info "$BIN" |
  grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1)
[ -n "$MAX_GLIBC" ] || { printf '%s\n' 'cannot read binary glibc ABI' >&2; exit 1; }
version_number=${MAX_GLIBC#GLIBC_}
major=${version_number%%.*}
minor=${version_number#*.}
minor=${minor%%.*}
if [ "$major" -gt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -gt 30 ]; }; then
  printf 'binary requires %s; release ceiling is GLIBC_2.30\n' "$MAX_GLIBC" >&2
  exit 1
fi

TLS_FILESZ=$(readelf -lW "$BIN" | awk '$1 == "TLS" { value=$5 } END { print value }')
PAD_LAYOUT=$(readelf -sW "$BIN" | awk '$4 == "TLS" && $8 == "g_bionic_guard_pad" { value=$2 ":" $3 } END { print value }')
[ "$TLS_FILESZ" = 0x000100 ] && [ "$PAD_LAYOUT" = 0000000000000000:256 ] || {
  printf 'audited TLS layout changed: template=%s pad=%s\n' "$TLS_FILESZ" "$PAD_LAYOUT" >&2
  exit 1
}

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/pf2-package.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT
PACKAGE_ROOT=$STAGE/package
# Canonical PortMaster ZIP layout, the same one every other published NextOS
# port uses: the visible launcher at the root and the game folder beside it.
# The user unzips into roms/ports/, so muOS/ArkOS/ROCKNIX/Knulli see both in
# the folder they scan, and the NextOS installer is what copies the .sh into
# ports_scripts/.  Wrapping the release in ports/ + ports_scripts/ was what
# made v1.0.2 invisible on muOS.
GAME=$PACKAGE_ROOT/pf2
mkdir -p "$GAME/tools/patches" "$GAME/tools/vendor" "$GAME/licenses" \
  "$GAME/docs/images" "$GAME/gamedata/owned-android-save"

install -m 0755 "$BIN" "$GAME/pf2"
install -m 0755 "$ROOT/pf2-nextos.sh" "$GAME/pf2-nextos.sh"
install -m 0755 "$ROOT/run-extractor.sh" "$GAME/run-extractor.sh"
install -m 0755 "$ROOT/nxextract-runtime-env.sh" "$GAME/nxextract-runtime-env.sh"
install -m 0755 "$ROOT/import-owned-save.sh" "$GAME/import-owned-save.sh"
install -m 0755 "$ROOT/nxextract.py" "$GAME/nxextract.py"
install -m 0755 "$ROOT/nxextract-ui" "$GAME/nxextract-ui"
install -m 0755 "$ROOT/tools/prepare_pf2_data.py" "$GAME/tools/prepare_pf2_data.py"
install -m 0755 "$ROOT/tools/pf2_transpile_shaders.py" "$GAME/tools/pf2_transpile_shaders.py"
install -m 0755 "$ROOT/tools/import_owned_android_save.py" "$GAME/tools/import_owned_android_save.py"
install -m 0755 "$ROOT/tools/liblz4.so.1" "$GAME/tools/liblz4.so.1"
install -m 0644 "$ROOT/tools/patches/"*.xormask "$GAME/tools/patches/"
cp -a "$ROOT/tools/vendor/python" "$GAME/tools/vendor/python"
find "$GAME/tools/vendor/python" -type f -name '*.py[co]' -delete
find "$GAME/tools/vendor/python" -depth -type d -name __pycache__ -empty -delete
# UnityPy ships optional FMOD binaries for desktop audio export.  The PF2
# shader preparer never imports them, so keep proprietary/foreign executables
# out of this small ARM64 runtime package.
rm -rf -- "$GAME/tools/vendor/python/UnityPy/lib"

install -m 0644 "$ROOT/extractor.json" "$GAME/extractor.json"
install -m 0644 "$ROOT/version.txt" "$GAME/version.txt"
install -m 0644 "$ROOT/README.md" "$GAME/README.md"
install -m 0644 "$ROOT/INSTALLATION.md" "$GAME/INSTALLATION.md"
install -m 0644 "$ROOT/NOTICE.md" "$GAME/NOTICE.md"
install -m 0644 "$ROOT/CHANGELOG.md" "$GAME/CHANGELOG.md"
install -m 0644 "$ROOT/LICENSE" "$GAME/LICENSE"
install -m 0644 "$ROOT/docs/OWNED-SAVE-IMPORT.md" "$GAME/docs/OWNED-SAVE-IMPORT.md"
install -m 0644 "$ROOT/docs/PROVENANCE.md" "$GAME/docs/PROVENANCE.md"
install -m 0644 "$ROOT/docs/images/"*.png "$GAME/docs/images/"
install -m 0644 "$ROOT/licenses/"*.txt "$GAME/licenses/"
install -m 0644 "$ROOT/package/GAMEDATA.txt" "$GAME/gamedata/COLOQUE-O-XAPK-AQUI.txt"
install -m 0644 "$ROOT/package/OWNED-SAVE.txt" "$GAME/gamedata/owned-android-save/LEIA-ME.txt"
# The launcher ships in BOTH ports/ (PortMaster family: muOS, ArkOS, ROCKNIX,
# Knulli) and ports_scripts/ (NextOS/EmuELEC, where ES only scans that folder).
# Shipping it only in ports_scripts/ made muOS users see no entry at all, and
# moving just the .sh by hand left the resolver without the pf2 folder.
install -m 0755 "$ROOT/Prizefighters 2.sh" \
  "$PACKAGE_ROOT/Prizefighters 2.sh"
install -m 0644 "$ROOT/package/LEIA-ME.txt" "$PACKAGE_ROOT/LEIA-ME.txt"

sh -n "$PACKAGE_ROOT/Prizefighters 2.sh"
[ "$(wc -c < "$PACKAGE_ROOT/Prizefighters 2.sh")" -le 3072 ] ||
  { printf 'visible PortMaster launcher is no longer thin\n' >&2; exit 1; }

# Explicitly reject game content, personal data and accidental diagnostics.
for forbidden in \
  libmain.so libil2cpp.so libunity.so libpairipcore.so \
  lib_burst_generated.so data.unity3d global-metadata.dat \
  shared-preferences.bin; do
  if find "$PACKAGE_ROOT" -type f -name "$forbidden" -print -quit | grep -q .; then
    printf 'forbidden game/personal file entered package: %s\n' "$forbidden" >&2
    exit 1
  fi
done
if find "$PACKAGE_ROOT" -type f \( \
  -iname '*.apk' -o -iname '*.apkm' -o -iname '*.apks' -o \
  -iname '*.xapk' -o -iname '*.obb' -o -iname '*.log' -o \
  -iname '*.ppm' -o -iname '*.dll' -o -iname '*.dylib' \) \
  -print -quit | grep -q .; then
  printf '%s\n' 'forbidden package, log or capture entered release' >&2
  exit 1
fi
for forbidden_dir in "$GAME/assets" "$GAME/lib" "$GAME/home" "$GAME/.nxextract"; do
  [ ! -e "$forbidden_dir" ] || {
    printf 'forbidden runtime tree entered package: %s\n' "$forbidden_dir" >&2
    exit 1
  }
done

# Audit every executable object, not only the loader.  Exactly these three
# project/runtime ELFs are allowed in the BYO-data ZIP.
while IFS= read -r -d '' candidate; do
  kind=$(file -b "$candidate")
  case "$kind" in
    *ELF*)
      relative=${candidate#"$PACKAGE_ROOT/"}
      case "$relative" in
        pf2/pf2|pf2/nxextract-ui|pf2/tools/liblz4.so.1) ;;
        *)
          printf 'unexpected ELF entered package: %s\n' "$relative" >&2
          exit 1
          ;;
      esac
      elf_glibc=$(readelf --version-info "$candidate" 2>/dev/null |
        grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1)
      [ -n "$elf_glibc" ] || {
        printf 'cannot determine glibc ABI for %s\n' "$relative" >&2
        exit 1
      }
      elf_version=${elf_glibc#GLIBC_}
      elf_major=${elf_version%%.*}
      elf_minor=${elf_version#*.}
      elf_minor=${elf_minor%%.*}
      if [ "$elf_major" -gt 2 ] || {
        [ "$elf_major" -eq 2 ] && [ "$elf_minor" -gt 30 ]
      }; then
        printf '%s requires %s; release ceiling is GLIBC_2.30\n' \
          "$relative" "$elf_glibc" >&2
        exit 1
      fi
      ;;
    *PE32*|*Mach-O*)
      printf 'foreign executable entered package: %s\n' \
        "${candidate#"$PACKAGE_ROOT/"}" >&2
      exit 1
      ;;
  esac
done < <(find "$PACKAGE_ROOT" -type f -print0)

mkdir -p "$ROOT/.build"
OUTPUT=$ROOT/.build/Prizefighters.2.NextOS-v$VERSION.zip
TEMP_ZIP=$STAGE/Prizefighters.2.NextOS-v$VERSION.zip
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}
export TZ=UTC
find "$PACKAGE_ROOT" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
(
  cd "$PACKAGE_ROOT"
  find LEIA-ME.txt 'Prizefighters 2.sh' pf2 -print | LC_ALL=C sort |
    zip -X -9 -q "$TEMP_ZIP" -@
)
mv -f -- "$TEMP_ZIP" "$OUTPUT"
HASH=$(sha256sum "$OUTPUT" | awk '{print $1}')
TEMP_HASH=$STAGE/Prizefighters.2.NextOS-v$VERSION.zip.sha256
printf '%s  %s\n' "$HASH" "$(basename "$OUTPUT")" > "$TEMP_HASH"
mv -f -- "$TEMP_HASH" "$OUTPUT.sha256"

unzip -tq "$OUTPUT" >/dev/null
printf 'PACKAGE OK: %s\n' "$OUTPUT"
printf 'SHA-256: %s\n' "$HASH"
printf 'binary ABI: %s | TLS %s %s\n' "$MAX_GLIBC" "$TLS_FILESZ" "$PAD_LAYOUT"
