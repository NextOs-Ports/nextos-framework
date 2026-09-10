#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V3-UPDATE-01: the launcher keeps two game-dir identities. The LOGICAL root is
# the path the frontend invoked (/roms/ports/<id>) and is the lexical identity
# handed to the guest; the PHYSICAL root (pwd -P) is for framework I/O. `pwd -P`
# must never silently replace /roms with /storage/roms in the guest argument,
# and two distinct trees holding the same port must not run a stale copy.
#
# This gate extracts the resolution block VERBATIM from the canonical template,
# substitutes the port id, wraps it in a runner placed at the launcher's real
# location (so `readlink -f "$0"` is honest), and drives it over real symlinked
# and distinct trees.
set -euo pipefail
TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
TEMPLATE="$TEST_DIR/../templates/launcher.sh.in"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nx-gamedir.XXXXXX")
cleanup() { case "$WORK" in "${TMPDIR:-/tmp}"/nx-gamedir.*) rm -rf -- "$WORK";; esac; }
trap cleanup EXIT INT TERM
fail() { printf 'gamedir-logical-physical: %s\n' "$*" >&2; exit 1; }

# Extract from the LOGICAL_GAMEDIR line through PHYSICAL_GAMEDIR; port id -> demo.
awk '/NXBOOTSTRAP_LOGICAL_GAMEDIR="\/\$directory\/ports\/@PORT_ID@"/{c=1}
     c{print}
     /NXBOOTSTRAP_PHYSICAL_GAMEDIR="\$GAMEDIR"/{exit}' "$TEMPLATE" \
  | sed 's/@PORT_ID@/demo/g' > "$WORK/resolve.inc"
grep -q "NXBOOTSTRAP_LOGICAL_GAMEDIR" "$WORK/resolve.inc" || fail "block not extracted"
grep -q "NXBOOTSTRAP_PHYSICAL_GAMEDIR" "$WORK/resolve.inc" || fail "block truncated"

# A runner that IS the launcher (readlink -f "$0" == this file): $1 = directory.
make_runner() { # $1 destination path for the runner
  {
    echo '#!/usr/bin/env bash'
    echo 'set -euo pipefail'
    echo 'directory="$1"'
    cat "$WORK/resolve.inc"
    echo 'echo "LOGICAL=$NXBOOTSTRAP_LOGICAL_GAMEDIR"'
    echo 'echo "PHYSICAL=$NXBOOTSTRAP_PHYSICAL_GAMEDIR"'
    echo 'echo "REASON=$NXBOOTSTRAP_GAMEDIR_REASON"'
  } > "$1"
  chmod +x "$1"
}

# The block prepends "/" to $directory (CFW roots are like "roms"): pass WORK
# without its leading slash so "/$directory" rebuilds the absolute sandbox path.
WORK_REL=${WORK#/}

# ---- Scenario 1: symlinked root /roms -> /storage/roms ----
mkdir -p "$WORK/storage/roms/ports/demo"
ln -s "$WORK/storage/roms" "$WORK/roms"
make_runner "$WORK/roms/ports/demo.sh"
out=$("$WORK/roms/ports/demo.sh" "$WORK_REL/roms" 2>"$WORK/e1") || { cat "$WORK/e1" >&2; fail "s1 crashed"; }
logical=$(sed -n 's/^LOGICAL=//p' <<<"$out")
physical=$(sed -n 's/^PHYSICAL=//p' <<<"$out")
[ "$logical" = "$WORK/roms/ports/demo" ] || fail "s1 logical swapped away from /roms: $logical"
[ "$physical" = "$WORK/storage/roms/ports/demo" ] || fail "s1 physical not canonical: $physical"
[ "$logical" -ef "$physical" ] || fail "s1 logical/physical not same inode"
grep -q "NXU0007" "$WORK/e1" && fail "s1 (symlink) must NOT raise NXU0007"

# ---- Scenario 2: two DISTINCT trees both hold the port ----
mkdir -p "$WORK/frontendtree/ports/demo" "$WORK/invokedtree/ports/demo"
make_runner "$WORK/invokedtree/ports/demo.sh"
out=$("$WORK/invokedtree/ports/demo.sh" "$WORK_REL/frontendtree" 2>"$WORK/e2") || {
  cat "$WORK/e2" >&2; fail "s2 crashed"; }
logical=$(sed -n 's/^LOGICAL=//p' <<<"$out")
grep -q "NXU0007" "$WORK/e2" || fail "s2 distinct trees did not raise NXU0007"
[ "$logical" -ef "$WORK/invokedtree/ports/demo" ] || \
  fail "s2 did not prefer the invoked launcher's tree: $logical"

# ---- Scenario 3: launcher path WITH SPACES ----
mkdir -p "$WORK/with space/roms/ports/demo"
make_runner "$WORK/with space/roms/ports/demo.sh"
out=$("$WORK/with space/roms/ports/demo.sh" "nope" 2>"$WORK/e3") || {
  cat "$WORK/e3" >&2; fail "s3 (spaces) crashed"; }
logical=$(sed -n 's/^LOGICAL=//p' <<<"$out")
[ "$logical" -ef "$WORK/with space/roms/ports/demo" ] || fail "s3 spaces mishandled: $logical"

printf 'gamedir-logical-physical: PASS (symlink keeps logical, distinct trees -> NXU0007+invoked, spaces ok)\n'
