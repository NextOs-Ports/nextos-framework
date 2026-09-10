#!/bin/sh
# Prepare a supported standalone Huntdown Android APK without redistributing it.
set -eu

usage() {
  cat <<'EOF'
Usage:
  prepare_data.sh OWNER_APK OUTPUT_DIR

The APK must come from the user's own installation. This host helper accepts a
standalone APK; the public NXExtract path also accepts split APK sets, APKM,
APKS and XAPK. Compatibility is decided from package/ABI structure and one
correlated internal payload profile, never from the container filename or its
whole-file SHA-256.
EOF
}

[ "$#" -eq 2 ] || { usage >&2; exit 2; }
apk=$1
output_dir=${2%/}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

[ -f "$apk" ] || { echo "APK not found: $apk" >&2; exit 1; }
[ -n "$output_dir" ] && [ "$output_dir" != "/" ] ||
  { echo "Refusing unsafe output directory: $output_dir" >&2; exit 1; }
if [ -e "$output_dir" ]; then
  if [ ! -d "$output_dir" ] ||
     find "$output_dir" -mindepth 1 -print -quit | grep . >/dev/null; then
    echo "Output already exists and is not empty: $output_dir" >&2
    echo "Choose a fresh directory; an existing working payload is never overwritten." >&2
    exit 1
  fi
fi
for tool in sha256sum unzip python3; do
  command -v "$tool" >/dev/null 2>&1 ||
    { echo "Missing required host tool: $tool" >&2; exit 1; }
done

actual_sha256=$(sha256sum "$apk" | awk '{print $1}')

output_parent=$(dirname -- "$output_dir")
mkdir -p "$output_parent"
members=$(mktemp "${TMPDIR:-/tmp}/huntdown-members.XXXXXX")
work_dir=$(mktemp -d "$output_parent/.huntdown-data.XXXXXX")
trap 'rm -f "$members"; rm -rf "$work_dir"' EXIT HUP INT TERM
unzip -Z1 "$apk" > "$members"

for member in \
  lib/arm64-v8a/libunity.so \
  lib/arm64-v8a/libil2cpp.so \
  lib/arm64-v8a/libmain.so \
  assets/bin/Data/data.unity3d \
  assets/bin/Data/datapack.unity3d \
  assets/bin/Data/Managed/Metadata/global-metadata.dat \
  assets/bin/Data/resources.resource; do
  grep -Fx "$member" "$members" >/dev/null ||
    { echo "APK is missing required member: $member" >&2; exit 1; }
done

mkdir -p "$work_dir/root"
unzip -q "$apk" 'assets/*' -d "$work_dir/apk"
cp -a "$work_dir/apk/assets/." "$work_dir/root/"
unzip -q -j "$apk" \
  lib/arm64-v8a/libunity.so \
  lib/arm64-v8a/libil2cpp.so \
  lib/arm64-v8a/libmain.so \
  -d "$work_dir/root"

# Unity treats this fused install-time pack like a split APK. Rebuilding the
# one-member ZIP preserves the native PAD flow used by UnityPlayer.
python3 "$script_dir/build_unity_asset_pack.py" \
  "$work_dir/root/bin/Data/datapack.unity3d" \
  "$work_dir/root/UnityDataAssetPack.apk"

for required in \
  "$work_dir/root/libunity.so" \
  "$work_dir/root/libil2cpp.so" \
  "$work_dir/root/libmain.so" \
  "$work_dir/root/bin/Data/data.unity3d" \
  "$work_dir/root/bin/Data/datapack.unity3d" \
  "$work_dir/root/bin/Data/Managed/Metadata/global-metadata.dat" \
  "$work_dir/root/UnityDataAssetPack.apk"; do
  [ -s "$required" ] || { echo "Prepared file is empty: $required" >&2; exit 1; }
done

"$script_dir/verify_payload.sh" "$work_dir/root"
[ -d "$output_dir" ] && rmdir "$output_dir"
mv "$work_dir/root" "$output_dir"

echo "Huntdown data prepared in: $output_dir"
echo "APK SHA-256: $actual_sha256"
echo "Game files remain local and are ignored by Git."
