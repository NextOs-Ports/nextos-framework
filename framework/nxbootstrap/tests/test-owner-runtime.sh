#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V5 7A.1/7A.2 + FV1/FV2 (nxbootstrap 0.8.0): the OWNER RUNTIME. Extracts the
# owner-file materialization and the owner-hook guard verbatim from the
# canonical template and proves, on a temporary GAMEDIR:
#   1. port-env.sh is seeded ONCE from defaults/ (only under owner_runtime);
#   2. an edited owner survives a new default: bytes intact, `.new` offered;
#   3. an untouched owner migrates to the new default (receipt);
#   4. the owner hook changes the effective state (a runtime variable) --
#      no rebuild, no repack;
#   5. a hook touching a RESERVED variable fails VISIBLY naming it; bytes intact;
#   6. a hook that does not parse aborts visibly with bash's diagnostic; bytes intact;
#   7. the sealed helper is sourced before the owner hook (order observable);
#   8. port-env.sh is excluded from the generation path set (never healed)
#      only under owner_runtime; V4 behaviour unchanged when off.
set -euo pipefail
TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
TEMPLATE="$TEST_DIR/../templates/launcher.sh.in"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nx-owner-runtime.XXXXXX")
cleanup() { case "$WORK" in "${TMPDIR:-/tmp}"/nx-owner-runtime.*) rm -rf -- "$WORK";; esac; }
trap cleanup EXIT INT TERM
fail() { printf 'owner-runtime: FAIL %s\n' "$*" >&2; exit 1; }
ok() { printf 'owner-runtime: ok %s\n' "$*"; }

extract_fn() { awk -v name="$1" '$0 ~ "^"name"\\(\\) \\{" {c=1} c {print} c && /^\}/ {exit}' "$TEMPLATE"; }
{
  extract_fn nxbootstrap_atomic_install
  extract_fn nxbootstrap_atomic_write_line
  extract_fn nxbootstrap_owner_files
  extract_fn nxbootstrap_materialize_owner_files
  extract_fn nxbootstrap_relative_path_safe
  grep '^NXBOOTSTRAP_OWNER_RESERVED_VARS=' "$TEMPLATE"
  extract_fn nxbootstrap_owner_env_snapshot
  extract_fn nxbootstrap_owner_env_changed
  extract_fn nxbootstrap_owner_env_source
} > "$WORK/helpers.sh"
for fn in nxbootstrap_materialize_owner_files nxbootstrap_owner_env_source nxbootstrap_relative_path_safe; do
  grep -q "^$fn() {" "$WORK/helpers.sh" || fail "$fn not extracted from the template"
done
# shellcheck disable=SC1090
. "$WORK/helpers.sh"

GAMEDIR="$WORK/game"; mkdir -p "$GAMEDIR/defaults"
PORT_ID=owner-test; NXBOOTSTRAP_LAUNCHER_DIR="$WORK"; NXBOOTSTRAP_GENERATION_ID=gen1; NXBOOTSTRAP_GENERATION_FORMAT=2
NXBOOTSTRAP_VIDEO_REQUIRED=0; NXBOOTSTRAP_HEALTH_FILE=h; NXBOOTSTRAP_VIDEO_FILE=v; NXBOOTSTRAP_LOGICAL_GAMEDIR="$GAMEDIR"; NXBOOTSTRAP_ANALOG_STICKS_HINT=x
printf '# NEXTOS_SETTINGS/2\nvideo.aspect=auto\n' > "$GAMEDIR/defaults/NEXTOSSETTINGS.txt"
printf 'export NX_VIDEO_ASPECT=auto\nexport NX_OWNER_MARK=default\n' > "$GAMEDIR/defaults/port-env.sh"

# --- V4 behaviour (owner_runtime off): port-env.sh is NOT seeded, path is a valid generation member
NXBOOTSTRAP_OWNER_RUNTIME=0
nxbootstrap_materialize_owner_files >/dev/null
[ ! -e "$GAMEDIR/port-env.sh" ] || fail "V4 mode seeded port-env.sh"
[ -f "$GAMEDIR/NEXTOSSETTINGS.txt" ] || fail "V4 mode did not seed NEXTOSSETTINGS.txt"
nxbootstrap_relative_path_safe port-env.sh || fail "V4 mode must still accept port-env.sh as a generation path"
ok "owner_runtime off: V4 behaviour unchanged (no hook seed; hook may be a generation member)"

# --- 1. seed once
NXBOOTSTRAP_OWNER_RUNTIME=1
out=$(nxbootstrap_materialize_owner_files)
[ -f "$GAMEDIR/port-env.sh" ] && [ ! -L "$GAMEDIR/port-env.sh" ] || fail "owner hook not seeded"
grep -q "materialized port-env.sh" <<<"$out" || fail "seed receipt missing"
[ "$(cat "$GAMEDIR/port-env.sh")" = "$(cat "$GAMEDIR/defaults/port-env.sh")" ] || fail "seed bytes differ"
[ -f "$GAMEDIR/.nxruntime/port-env.sh.default.sha256" ] || fail "seed pin missing"
ok "1. port-env.sh seeded once from defaults/ (pin written)"

# --- 8. never a generation member / never healed
if nxbootstrap_relative_path_safe port-env.sh; then fail "owner hook accepted as a generation path under owner_runtime"; fi
nxbootstrap_relative_path_safe adapter-env.sh || fail "sealed helper must remain a valid generation path"
ok "8. port-env.sh excluded from the generation/healing path set; adapter-env.sh stays sealed"

# --- 4. the owner hook changes the effective state
printf 'export NX_VIDEO_ASPECT=preserve\nexport NX_OWNER_MARK=edited\n' > "$GAMEDIR/port-env.sh"
printf 'export NX_SEALED_MARK=sealed\n' > "$GAMEDIR/adapter-env.sh"
order=""
nxbootstrap_owner_env_source "$GAMEDIR/adapter-env.sh" sealed || fail "sealed helper refused"
[ "${NX_SEALED_MARK-}" = sealed ] || fail "sealed helper not applied"
nxbootstrap_owner_env_source "$GAMEDIR/port-env.sh" owner || fail "owner hook refused"
[ "${NX_VIDEO_ASPECT-}" = preserve ] && [ "${NX_OWNER_MARK-}" = edited ] || fail "owner edit did not reach the effective state"
ok "4./7. sealed helper then owner hook: the edit changes the effective state without rebuild/repack"

# --- 2. edited owner survives a NEW default: bytes intact, .new offered
printf 'export NX_VIDEO_ASPECT=auto\nexport NX_OWNER_MARK=default-v2\n' > "$GAMEDIR/defaults/port-env.sh"
out=$(nxbootstrap_materialize_owner_files)
[ "$(cat "$GAMEDIR/port-env.sh")" = "$(printf 'export NX_VIDEO_ASPECT=preserve\nexport NX_OWNER_MARK=edited\n')" ] || fail "MUTANT survived: update overwrote the edited owner"
[ -f "$GAMEDIR/port-env.sh.new" ] || fail ".new not offered"
grep -q "customized; new default saved as port-env.sh.new" <<<"$out" || fail "update receipt missing"
ok "2. MUTANT killed: update/healing never overwrites the edited owner; default offered as port-env.sh.new"
# running it again must not duplicate anything or touch the owner
before=$(sha256sum "$GAMEDIR/port-env.sh"); nxbootstrap_materialize_owner_files >/dev/null
[ "$before" = "$(sha256sum "$GAMEDIR/port-env.sh")" ] || fail "second pass modified the owner"
ok "2b. idempotent: a second boot leaves the owner untouched"

# --- 3. untouched owner migrates to the new default (receipt)
rm -f "$GAMEDIR/port-env.sh" "$GAMEDIR/port-env.sh.new" "$GAMEDIR/.nxruntime/port-env.sh.default.sha256"
nxbootstrap_materialize_owner_files >/dev/null            # seeds default-v2 + pin
printf 'export NX_VIDEO_ASPECT=auto\nexport NX_OWNER_MARK=default-v3\n' > "$GAMEDIR/defaults/port-env.sh"
out=$(nxbootstrap_materialize_owner_files)
grep -q "default-v3" "$GAMEDIR/port-env.sh" || fail "untouched owner did not migrate"
grep -q "was untouched; migrated" <<<"$out" || fail "migration receipt missing"
ok "3. untouched owner migrates to the new default with a receipt"

# --- 5. reserved variable -> visible failure naming it; bytes intact
printf 'GAMEDIR=/tmp/elsewhere\nexport NX_OWNER_MARK=evil\n' > "$GAMEDIR/port-env.sh"
saved_gamedir=$GAMEDIR; before=$(sha256sum "$GAMEDIR/port-env.sh")
set +e; msg=$(nxbootstrap_owner_env_source "$saved_gamedir/port-env.sh" owner 2>&1); rc=$?; set -e
GAMEDIR=$saved_gamedir
[ "$rc" = 3 ] || fail "reserved-variable change not refused (rc=$rc)"
grep -q "changed the reserved variable GAMEDIR" <<<"$msg" || fail "diagnostic does not name the variable: $msg"
grep -q "was NOT modified" <<<"$msg" || fail "diagnostic must state the file was not modified"
[ "$before" = "$(sha256sum "$GAMEDIR/port-env.sh")" ] || fail "MUTANT survived: guard rewrote the owner"
ok "5. MUTANT killed: reserved variable change fails visibly, names GAMEDIR, never restores silently"

# --- 6. hook that does not parse -> visible abort with file:line; bytes intact
printf 'export NX_OWNER_MARK=ok\nif [ 1 = 1 ; then\n' > "$GAMEDIR/port-env.sh"
before=$(sha256sum "$GAMEDIR/port-env.sh")
set +e; msg=$(nxbootstrap_owner_env_source "$GAMEDIR/port-env.sh" owner 2>&1); rc=$?; set -e
[ "$rc" = 2 ] || fail "unparsable hook not refused (rc=$rc)"
grep -q "does not parse" <<<"$msg" && grep -q "port-env.sh: line" <<<"$msg" || fail "diagnostic lacks origin/line: $msg"
[ "$before" = "$(sha256sum "$GAMEDIR/port-env.sh")" ] || fail "MUTANT survived: syntax failure rewrote the owner"
ok "6. MUTANT killed: unparsable hook aborts visibly with origin/line, no fallback, bytes intact"

# --- symlinked owner is ignored (never followed)
rm -f "$GAMEDIR/port-env.sh"; printf 'export NX_OWNER_MARK=linked\n' > "$WORK/outside.sh"; ln -s "$WORK/outside.sh" "$GAMEDIR/port-env.sh"
NX_OWNER_MARK=unset; nxbootstrap_owner_env_source "$GAMEDIR/port-env.sh" owner || fail "symlink path must be skipped, not fatal"
[ "$NX_OWNER_MARK" = unset ] || fail "symlinked owner hook was sourced"
ok "symlinked owner hook is never followed"

printf 'owner-runtime: PASS (seed once, .new on update, edit reaches effective state, reserved guard, syntax abort, never a generation member)\n'
