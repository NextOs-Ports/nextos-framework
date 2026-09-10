#!/bin/bash
# NEXTOS_SETTINGS/2 owner file: the sealed adapter-env.sh only READS
# NEXTOSSETTINGS.txt (the generated launcher seeds it from the generator's /2
# template), parses `video.aspect` with the strict grammar, reports file:line
# diagnostics and exports NX_VIDEO_ASPECT. Runs the real adapter-env.sh against
# a fake GAMEDIR with a /2 seed shaped like the generator output.
set -euo pipefail
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$TEST_DIR/../.." && pwd -P)
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
GAMEDIR="$WORK/tearscape"
mkdir -p "$GAMEDIR/lib" "$GAMEDIR/game/addons/crt" "$GAMEDIR/defaults"
: > "$GAMEDIR/lib/libEGL.so"; : > "$GAMEDIR/lib/libGLESv2.so"
printf 'shader_type canvas_item;\n' > "$GAMEDIR/game/addons/crt/crt.gdshader"
printf '# NEXTOS_SETTINGS/2\nlanguage=auto\nvideo.authority=nextos\nvideo.output_size=display\nvideo.aspect=auto\nvideo.filter=engine\nvideo.invalid_policy=package_default\n' > "$GAMEDIR/defaults/NEXTOSSETTINGS.txt"
OWNER="$GAMEDIR/NEXTOSSETTINGS.txt"

run_env() {
	( cd "$GAMEDIR" && env -u NX_VIDEO_ASPECT -u NX_VIDEO_AUTHORITY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$WORK/none" \
		GAMEDIR="$GAMEDIR" bash -c '. "$1"; printf "ASPECT_ENV=%s\nAUTHORITY_ENV=%s\n" "$NX_VIDEO_ASPECT" "$NX_VIDEO_AUTHORITY"' _ "$PORT_DIR/adapter-env.sh" )
}
fail() { printf 'NEXTOSSETTINGS HOST: FAIL: %s\n' "$1" >&2; exit 1; }

# 0. the port ships NO settings file of its own: the generator renders the /2 seed
[ ! -e "$PORT_DIR/defaults/NEXTOSSETTINGS.txt" ] || fail "the port must not ship its own NEXTOSSETTINGS.txt (generator seeds /2)"
python3 - "$PORT_DIR" <<'PY'
import json, sys
v = json.load(open(sys.argv[1] + "/nxproject.json"))["video"]
assert v["authority"] == "nextos" and v["aspect"] == "auto"
assert v["aspect_policies"] == ["auto", "engine", "preserve", "stretch"], v
assert v["auto_algorithm"] == "stretch", v
PY

# 1. no owner file yet (launcher not run): auto, said so
out=$(run_env)
grep -q '^NEXTOSSETTINGS.txt: absent; using video.aspect=auto$' <<<"$out" || fail "absent diagnostic missing"
grep -q '^ASPECT_ENV=auto$' <<<"$out" || fail "absent default is not auto"
grep -q '^AUTHORITY_ENV=nextos$' <<<"$out" || fail "absent authority is not nextos"

# 2. seeded owner file (what the launcher does) -> auto, bytes untouched
cp "$GAMEDIR/defaults/NEXTOSSETTINGS.txt" "$OWNER"
before=$(sha256sum "$OWNER")
out=$(run_env)
grep -q '^SETTINGS OWNER: NEXTOSSETTINGS.txt video.aspect=auto$' <<<"$out" || fail "owner line missing"
grep -q '^SETTINGS OWNER: NEXTOSSETTINGS.txt video.authority=nextos$' <<<"$out" || fail "authority owner line missing"
[ "$before" = "$(sha256sum "$OWNER")" ] || fail "owner bytes changed"

# 3. owner edit honoured
sed -i 's/^video.aspect=auto$/video.aspect=preserve/' "$OWNER"
out=$(run_env); grep -q '^ASPECT_ENV=preserve$' <<<"$out" || fail "preserve not parsed"
sed -i 's/^video.aspect=preserve$/video.aspect=stretch/' "$OWNER"
out=$(run_env); grep -q '^ASPECT_ENV=stretch$' <<<"$out" || fail "stretch not parsed"
sed -i 's/^video.aspect=stretch$/video.aspect=engine/' "$OWNER"
out=$(run_env); grep -q '^ASPECT_ENV=engine$' <<<"$out" || fail "engine not parsed"

# 4. /2 tokens this engine cannot honour and invalid values: bytes kept, file:line named, fallback auto
sed -i 's/^video.aspect=engine$/video.aspect=crop/' "$OWNER"
before=$(sha256sum "$OWNER")
out=$(run_env)
grep -q "^NEXTOSSETTINGS.txt:[0-9][0-9]*: video.aspect='crop' is not supported by this engine (fixed 640x360 viewport); using auto$" <<<"$out" || fail "unsupported token diagnostic missing"
grep -q '^ASPECT_ENV=auto$' <<<"$out" || fail "unsupported token did not fall back"
[ "$before" = "$(sha256sum "$OWNER")" ] || fail "unsupported token rewrote the owner file"
sed -i 's/^video.aspect=crop$/video.aspect=bogus/' "$OWNER"
out=$(run_env)
grep -q "^NEXTOSSETTINGS.txt:[0-9][0-9]*: video.aspect='bogus' is not auto|engine|preserve|stretch; using auto$" <<<"$out" || fail "invalid value diagnostic missing"
grep -q '^ASPECT_ENV=auto$' <<<"$out" || fail "invalid value did not fall back"
# 4b. legacy 0.2.17 spelling is understood and diagnosed
printf '# NEXTOS_SETTINGS/2\naspect=native\n' > "$OWNER"
out=$(run_env)
grep -q "^NEXTOSSETTINGS.txt:2: legacy key 'aspect'; the /2 key is video.aspect$" <<<"$out" || fail "legacy key diagnostic missing"
grep -q "^NEXTOSSETTINGS.txt:2: legacy token 'native'; the /2 token is engine$" <<<"$out" || fail "legacy token diagnostic missing"
grep -q '^ASPECT_ENV=engine$' <<<"$out" || fail "legacy native did not map to engine"

# 5. /1 keys accepted silently; unknown key and spaced line diagnosed with their line numbers
printf '# NEXTOS_SETTINGS/2\nlanguage=auto\nquality=auto\nvideo.authority=nextos\nvideo.filter=nearest\nvideo.output_size=display\nfilter=nearest\nvideo.aspect = preserve\nvideo.aspect=preserve\n' > "$OWNER"
out=$(run_env)
grep -q "^NEXTOSSETTINGS.txt:7: unknown key 'filter'; ignored$" <<<"$out" || fail "unknown key diagnostic missing"
grep -q '^NEXTOSSETTINGS.txt:8: key/value outside \[A-Za-z0-9._-\] (no spaces allowed); ignored$' <<<"$out" || fail "spaced line diagnostic missing"
grep -q "language\|quality\|video.filter\|video.output_size" <<<"$(grep -i 'unknown key' <<<"$out")" && fail "/2 keys reported as unknown"
grep -q '^ASPECT_ENV=preserve$' <<<"$out" || fail "valid line after diagnostics not applied"

# 5b. authority is part of the same typed owner contract; invalid is visible
sed -i 's/^video.authority=nextos$/video.authority=engine/' "$OWNER"
out=$(run_env); grep -q '^AUTHORITY_ENV=engine$' <<<"$out" || fail "engine authority not parsed"
sed -i 's/^video.authority=engine$/video.authority=device-square/' "$OWNER"
out=$(run_env)
grep -q "video.authority='device-square' is not nextos|engine|synchronized; using nextos" <<<"$out" || fail "invalid authority diagnostic missing"
grep -q '^AUTHORITY_ENV=nextos$' <<<"$out" || fail "invalid authority did not fall back"

# 6. CRLF and a line without '=' are tolerated
printf '# NEXTOS_SETTINGS/2\r\nvideo.aspect=stretch\r\njunk\r\n' > "$OWNER"
out=$(run_env)
grep -q '^ASPECT_ENV=stretch$' <<<"$out" || fail "CRLF value not parsed"
grep -q '^NEXTOSSETTINGS.txt:3: not a key=value line; ignored$' <<<"$out" || fail "junk line diagnostic missing"

# 7. inherited NX_VIDEO_ASPECT wins over the file
out=$( cd "$GAMEDIR" && env -u WAYLAND_DISPLAY NX_VIDEO_ASPECT=engine NX_VIDEO_AUTHORITY=nextos XDG_RUNTIME_DIR="$WORK/none" GAMEDIR="$GAMEDIR" \
	bash -c '. "$1"; printf "ASPECT_ENV=%s\n" "$NX_VIDEO_ASPECT"' _ "$PORT_DIR/adapter-env.sh" )
grep -q '^ASPECT_ENV=engine$' <<<"$out" || fail "inherited value lost"

# 8. symlinked owner file fails closed
rm -f "$OWNER"; ln -s /etc/hostname "$OWNER"
if run_env >/dev/null 2>&1; then fail "symlink accepted"; fi

printf 'NEXTOSSETTINGS HOST: PASS\n'
