#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V5 E6a (nxbootstrap 0.8.1): the owner file lifecycle proven on the REAL
# rendered launcher, not on functions extracted from the template.
#
# test-owner-runtime.sh proves the MODEL: it lifts the helpers out of
# templates/launcher.sh.in and drives them directly. That can never catch a
# launcher that renders the helpers but never calls them, calls them in the
# wrong phase, or calls them with a GAMEDIR that is not the one the game
# receives. This gate renders a launcher with tools/generate-port.py, puts it
# in a PortMaster fixture tree and BOOTS it four times, reading only what a
# device would see: the launcher log and the bytes on disk.
#
#   boot 1  absent owner  -> seeded ONCE from defaults/, pin written
#   boot 2  owner edited + a NEW default -> owner bytes intact, `.new` offered
#   boot 3  a THIRD default -> `.new` REFRESHED (the E6a defect: before 0.8.1
#           the second offer was frozen for good and the owner never saw v3)
#   boot 4  owner edited the `.new` itself -> preserved and named, never
#           written over; the live owner file still untouched
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_ROOT=$(cd -- "$TEST_DIR/.." && pwd -P)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nx-owner-e6a.XXXXXX")
cleanup() {
  case "$WORK" in "${TMPDIR:-/tmp}"/nx-owner-e6a.*) rm -rf -- "$WORK" ;; esac
}
trap cleanup EXIT INT TERM
fail() {
  printf 'owner-e6a-real: FAIL %s\n' "$*" >&2
  if [ -f "$GAMEDIR/log.txt" ]; then
    printf -- '--- launcher log ---\n' >&2
    sed -n '1,120p' "$GAMEDIR/log.txt" >&2 || true
  fi
  exit 1
}
ok() { printf 'owner-e6a-real: ok %s\n' "$*"; }

# ------------------------------------------------------------------ fixture
# generation_runtime carries real bytes: build the runtime root FIRST and pin
# the real digests, exactly like a port does.
PORT_ID=e6aport
RUNTIME_ROOT=$WORK/runtime-root
mkdir -p "$RUNTIME_ROOT/bin"
cat > "$RUNTIME_ROOT/bin/e6a-loader" <<STUB
#!/bin/sh
printf 'E6A_MARK=%s\n' "\${E6A_MARK-__unset__}" >> "$WORK/markers"
exit 0
STUB
chmod 0755 "$RUNTIME_ROOT/bin/e6a-loader"
# The sealed helper is a normal generation member and is sourced BEFORE the
# owner hook, so the value it sets has to lose to the owner's.
printf 'E6A_MARK=sealed\nexport E6A_MARK\n' > "$RUNTIME_ROOT/adapter-env.sh"
# The generation store pins the exact mode of every runtime member, so the
# fixture must set it instead of inheriting the caller's umask: under the
# battery's run-logged.sh (umask 0077) these were born 0600 and the generator
# refused them, which is the store doing its job.
chmod 0644 "$RUNTIME_ROOT/adapter-env.sh"
chmod 0755 "$RUNTIME_ROOT/bin/e6a-loader"
sha_of() { set -- $(sha256sum "$1"); printf '%s' "$1"; }
LOADER_SHA=$(sha_of "$RUNTIME_ROOT/bin/e6a-loader")
ADAPTER_SHA=$(sha_of "$RUNTIME_ROOT/adapter-env.sh")

cat > "$WORK/nxport.json" <<JSON
{
  "schema_version": 3,
  "id": "$PORT_ID",
  "title": "E6a Port",
  "launcher_name": "E6a Port.sh",
  "architecture": "aarch64",
  "executable": "bin/e6a-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "no", "version": "1.3.0"},
  "required_files": ["bin/e6a-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log",
  "owner_runtime": "1",
  "generation_runtime": [
    {"role": "executable", "path": "bin/e6a-loader", "mode": "0755",
     "sha256": "$LOADER_SHA"},
    {"role": "runtime-hook", "path": "adapter-env.sh", "mode": "0644",
     "sha256": "$ADAPTER_SHA"}
  ]
}
JSON
python3 -B "$PROJECT_ROOT/tools/generate-port.py" "$WORK/nxport.json" \
  --output "$WORK/generated" --runtime-root "$RUNTIME_ROOT" \
  >"$WORK/generate.log" 2>&1 ||
  { sed -n '1,40p' "$WORK/generate.log" >&2
    printf 'owner-e6a-real: FAIL generator refused the E6a manifest\n' >&2
    exit 1; }

ROOT=$WORK/real
PORTS=$ROOT/roms/ports
mkdir -p "$PORTS"
cp "$WORK/generated/E6a Port.sh" "$PORTS/"
GAMEDIR=$PORTS/$PORT_ID
cp -a "$WORK/generated/$PORT_ID" "$GAMEDIR"
mkdir -p "$GAMEDIR/defaults"
[ -x "$GAMEDIR/bin/e6a-loader" ] ||
  { printf 'owner-e6a-real: FAIL generated port lacks the executable\n' >&2; exit 1; }
# The real nxsplash is an aarch64 binary; this host runs the sealed stub the
# nxbootstrap suite already uses, so the boot reaches the owner phase.
cp "$PROJECT_ROOT/tests/splash-stub.sh" "$GAMEDIR/nxsplash-nextos"
chmod 0755 "$GAMEDIR/nxsplash-nextos"

XDG=$WORK/xdg
mkdir -p "$XDG/PortMaster"
cat > "$XDG/PortMaster/control.txt" <<CONTROL
# E6a fixture control.txt
directory="${ROOT#/}/roms"
ESUDO=""
CUR_TTY=/dev/null
CFW_NAME=e6afix
get_controls() { :; }
pm_platform_helper() { :; }
pm_finish() { :; }
CONTROL

RUNTIME=$WORK/runtime
mkdir -m 0700 "$RUNTIME"
LAUNCHER="$PORTS/E6a Port.sh"

boot() {
  env -i PATH="$PATH" HOME="$WORK" TMPDIR="${TMPDIR:-/tmp}" \
    XDG_DATA_HOME="$XDG" XDG_RUNTIME_DIR="$RUNTIME" \
    bash "$LAUNCHER" </dev/null >"$WORK/boot.out" 2>&1 || true
  cat "$GAMEDIR/log.txt" 2>/dev/null > "$WORK/boot.log" || : > "$WORK/boot.log"
  cat "$WORK/boot.out" >> "$WORK/boot.log"
}
mark_of() { grep -o 'E6A_MARK=[a-z0-9-]*' "$1" 2>/dev/null | tail -1; }

# ------------------------------------------------------- boot 1: seed ONCE
printf '# NEXTOS_SETTINGS/2\nvideo.aspect=auto\n' \
  > "$GAMEDIR/defaults/NEXTOSSETTINGS.txt"
printf 'E6A_MARK=default-v1\nexport E6A_MARK\n' > "$GAMEDIR/defaults/port-env.sh"
boot
[ -f "$GAMEDIR/port-env.sh" ] && [ ! -L "$GAMEDIR/port-env.sh" ] ||
  fail 'boot 1: the REAL launcher did not seed port-env.sh'
grep -q 'OWNER FILE: materialized port-env.sh from defaults/' "$WORK/boot.log" ||
  fail 'boot 1: the launcher printed no seed receipt'
cmp -s "$GAMEDIR/port-env.sh" "$GAMEDIR/defaults/port-env.sh" ||
  fail 'boot 1: seeded bytes differ from the seed'
[ -f "$GAMEDIR/.nxruntime/port-env.sh.default.sha256" ] ||
  fail 'boot 1: the default pin was not written'
[ "$(mark_of "$WORK/markers")" = "E6A_MARK=default-v1" ] ||
  fail "boot 1: the game did not receive the seeded value ($(mark_of "$WORK/markers"))"
ok '1. the REAL launcher seeds port-env.sh once from defaults/ and the game receives it'

# ------------------------------------------ boot 2: owner edit + new default
printf 'E6A_MARK=owner-edit\nexport E6A_MARK\n' > "$GAMEDIR/port-env.sh"
owner_sha=$(sha256sum "$GAMEDIR/port-env.sh")
printf 'E6A_MARK=default-v2\nexport E6A_MARK\n' > "$GAMEDIR/defaults/port-env.sh"
boot
[ "$owner_sha" = "$(sha256sum "$GAMEDIR/port-env.sh")" ] ||
  fail 'boot 2: MUTANT survived -- the real update overwrote the owner bytes'
[ -f "$GAMEDIR/port-env.sh.new" ] ||
  fail 'boot 2: the real launcher offered no port-env.sh.new'
grep -q 'new default saved as port-env.sh.new' "$WORK/boot.log" ||
  fail 'boot 2: no .new receipt in the launcher log'
grep -q 'default-v2' "$GAMEDIR/port-env.sh.new" ||
  fail 'boot 2: .new does not carry the new default'
[ "$(mark_of "$WORK/markers")" = "E6A_MARK=owner-edit" ] ||
  fail 'boot 2: the owner edit did not reach the effective state (no rebuild, no repack)'
ok '2. MUTANT killed: a real update keeps the owner bytes, offers .new, and the edit reaches the game'

# ------------------------------------------ boot 3: THIRD default (the E6a bug)
printf 'E6A_MARK=default-v3\nexport E6A_MARK\n' > "$GAMEDIR/defaults/port-env.sh"
boot
[ "$owner_sha" = "$(sha256sum "$GAMEDIR/port-env.sh")" ] ||
  fail 'boot 3: the owner bytes moved'
grep -q 'default-v3' "$GAMEDIR/port-env.sh.new" ||
  fail 'boot 3: MUTANT survived -- .new is frozen on the previous default, so the owner never sees v3'
grep -q 'port-env.sh.new refreshed to the newest default' "$WORK/boot.log" ||
  fail 'boot 3: the refresh happened without a receipt'
ok '3. MUTANT killed: a THIRD default refreshes .new (before 0.8.1 the offer froze on v2 for good)'

# --------------------------------- boot 4: the owner edited the .new itself
printf 'E6A_MARK=owner-wrote-here\nexport E6A_MARK\n' > "$GAMEDIR/port-env.sh.new"
new_sha=$(sha256sum "$GAMEDIR/port-env.sh.new")
printf 'E6A_MARK=default-v4\nexport E6A_MARK\n' > "$GAMEDIR/defaults/port-env.sh"
boot
[ "$new_sha" = "$(sha256sum "$GAMEDIR/port-env.sh.new")" ] ||
  fail 'boot 4: MUTANT survived -- the refresh wrote over bytes the owner put in .new'
grep -q 'port-env.sh.new was edited; kept as it is' "$WORK/boot.log" ||
  fail 'boot 4: an edited .new was skipped without naming it'
[ "$owner_sha" = "$(sha256sum "$GAMEDIR/port-env.sh")" ] ||
  fail 'boot 4: the live owner file moved'
ok '4. MUTANT killed: bytes the owner put in .new are preserved and named, never written over'

# ------------------- boot 5 (0.8.2, review 2 F6): a `.new` from a 0.8.0 launcher
# has no `.new.sha256` pin. It must not be mistaken for an owner edit: an
# unpinned offer equal to the previous default is migrated; unknown bytes are
# set aside as `.new.unpinned` and the newest default is offered with a pin.
rm -f "$GAMEDIR/.nxruntime/port-env.sh.new.sha256"
printf 'E6A_MARK=default-v4\nexport E6A_MARK\n' > "$GAMEDIR/port-env.sh.new"   # = previous default, unpinned
sha256sum "$GAMEDIR/defaults/port-env.sh" | cut -d' ' -f1 > "$GAMEDIR/.nxruntime/port-env.sh.default.sha256"
printf 'E6A_MARK=default-v5\nexport E6A_MARK\n' > "$GAMEDIR/defaults/port-env.sh"
boot
grep -q 'E6A_MARK=default-v5' "$GAMEDIR/port-env.sh.new" ||
  fail 'boot 5: MUTANT survived -- an unpinned .new equal to the previous default was left frozen (the 0.8.0->0.8.1 migration gap)'
[ -f "$GAMEDIR/.nxruntime/port-env.sh.new.sha256" ] ||
  fail 'boot 5: the migrated offer was not pinned'
grep -q 'offer without pin, from an older launcher' "$WORK/boot.log" ||
  fail 'boot 5: no migration receipt'
[ "$owner_sha" = "$(sha256sum "$GAMEDIR/port-env.sh")" ] ||
  fail 'boot 5: the live owner file moved'
ok '5. MUTANT killed: an unpinned .new (older launcher) is migrated instead of being frozen as "edited"'
# boot 6: unpinned .new with UNKNOWN bytes -> set aside as .new.unpinned, newest default offered
rm -f "$GAMEDIR/.nxruntime/port-env.sh.new.sha256"
printf 'E6A_MARK=who-knows\nexport E6A_MARK\n' > "$GAMEDIR/port-env.sh.new"
printf 'E6A_MARK=default-v6\nexport E6A_MARK\n' > "$GAMEDIR/defaults/port-env.sh"
boot
grep -q 'E6A_MARK=who-knows' "$GAMEDIR/port-env.sh.new.unpinned" ||
  fail 'boot 6: the unknown unpinned bytes were not preserved as .new.unpinned'
grep -q 'E6A_MARK=default-v6' "$GAMEDIR/port-env.sh.new" ||
  fail 'boot 6: the newest default was not offered'
ok '6. unknown unpinned .new preserved as .new.unpinned; newest default offered with a pin'

# ------------------------------------------------ the owner file is not healed
grep -q '"port-env.sh"' "$GAMEDIR/nxport.json" &&
  fail 'the packaged manifest lists port-env.sh (it would be healed)'
ok 'port-env.sh is absent from the rendered manifest: outside the healing closure'

printf 'owner-e6a-real: PASS (6 real boots: seed once, .new on update, .new refreshed on a third default, owner-edited .new preserved, unpinned .new migrated, unknown unpinned set aside)\n'
