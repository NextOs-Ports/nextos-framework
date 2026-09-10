#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V3 audit (blocker 4): the owner-file materialization helpers in the canonical
# launcher template must be atomic and symlink-safe on the target, the .new
# candidate and the receipt/pin. This gate extracts the two helper functions
# straight from the template and proves a planted symlink at the destination or
# the temp path is never written through, and the last valid file survives.
set -euo pipefail
TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
TEMPLATE="$TEST_DIR/../templates/launcher.sh.in"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nx-owner-atomic.XXXXXX")
cleanup() { case "$WORK" in "${TMPDIR:-/tmp}"/nx-owner-atomic.*) rm -rf -- "$WORK";; esac; }
trap cleanup EXIT INT TERM
fail() { printf 'owner-materialize-atomic: %s\n' "$*" >&2; exit 1; }

# Extract the two functions verbatim from the template.
awk '/^nxbootstrap_atomic_install\(\) \{/{c=1}
     c{print}
     /^\}/{if(c==2){exit} }
     /^nxbootstrap_atomic_write_line\(\) \{/{c=2}' "$TEMPLATE" > "$WORK/helpers.sh"
grep -q "nxbootstrap_atomic_install" "$WORK/helpers.sh" || fail "install helper not extracted"
grep -q "nxbootstrap_atomic_write_line" "$WORK/helpers.sh" || fail "write-line helper not extracted"
# shellcheck disable=SC1090
. "$WORK/helpers.sh"

# --- happy path: install creates a real regular 0644 file ---
printf 'default-bytes\n' > "$WORK/src"
nxbootstrap_atomic_install "$WORK/src" "$WORK/target" || fail "install failed on clean path"
[ -f "$WORK/target" ] && [ ! -L "$WORK/target" ] || fail "target not a regular file"
[ "$(cat "$WORK/target")" = "default-bytes" ] || fail "target content wrong"
mode=$(stat -c '%a' "$WORK/target"); [ "$mode" = "644" ] || fail "target mode $mode != 644"

# --- symlink at the DESTINATION must not be written through ---
printf 'SECRET-PRESERVED\n' > "$WORK/outside"
ln -s "$WORK/outside" "$WORK/link_target"
if nxbootstrap_atomic_install "$WORK/src" "$WORK/link_target"; then
  # mv replaces the symlink inode with a regular file; the pointee must be intact
  [ "$(cat "$WORK/outside")" = "SECRET-PRESERVED" ] || fail "wrote THROUGH dest symlink"
  [ -L "$WORK/link_target" ] && fail "dest symlink survived as symlink after replace"
fi
[ "$(cat "$WORK/outside")" = "SECRET-PRESERVED" ] || fail "outside file was clobbered"

# --- TOCTOU regression guard (auditoria V3, ponto 6): the install helper must
# fill the temp in the SAME exclusive (set -C / O_EXCL) open, never
# create-empty-then-reopen with an unguarded "cat > tmp". ---
grep -Fq 'set -C; cat "$src" > "$tmp"' "$WORK/helpers.sh" \
  || fail "install content write is not in a single set -C open (reopen TOCTOU)"
if grep -Fq ': > "$tmp"' "$WORK/helpers.sh"; then
  fail "install still create-empties the temp then reopens (TOCTOU pattern)"
fi

# --- symlinked destination DIRECTORY is refused ---
mkdir "$WORK/realdir"; ln -s "$WORK/realdir" "$WORK/linkdir"
if nxbootstrap_atomic_install "$WORK/src" "$WORK/linkdir/x"; then
  fail "install accepted a symlinked destination directory"
fi

# --- REAL collision at the temp path: plant a symlink at the EXACT temp name
# the helper opens (its $$ plus a seeded $RANDOM make the path predictable) and
# prove the single exclusive write refuses it, never writing through to the
# pointee. This is the collision the earlier "we cannot know $RANDOM" note
# skipped. ---
printf 'TEMP-POINTEE-SECRET\n' > "$WORK/temp_pointee"
RANDOM=4242; predicted=$RANDOM
ln -s "$WORK/temp_pointee" "$WORK/.tcol.nxtmp.$$.$predicted"
RANDOM=4242
if nxbootstrap_atomic_install "$WORK/src" "$WORK/tcol"; then
  [ "$(cat "$WORK/temp_pointee")" = "TEMP-POINTEE-SECRET" ] \
    || fail "wrote THROUGH a symlink planted at the temp path (TOCTOU)"
fi
[ "$(cat "$WORK/temp_pointee")" = "TEMP-POINTEE-SECRET" ] \
  || fail "temp-path symlink pointee was clobbered"

# --- .new candidate: symlink there must not be written through ---
printf 'NEW-SECRET\n' > "$WORK/outside2"
ln -s "$WORK/outside2" "$WORK/target.new"
# emulate the launcher guard: it only writes .new when it is neither a file nor
# a symlink; prove the guard by checking the helper still refuses to follow it.
if nxbootstrap_atomic_install "$WORK/src" "$WORK/target.new"; then
  [ "$(cat "$WORK/outside2")" = "NEW-SECRET" ] || fail ".new symlink written through"
fi
[ "$(cat "$WORK/outside2")" = "NEW-SECRET" ] || fail ".new pointee clobbered"

# --- pin line: atomic write, symlink-safe ---
nxbootstrap_atomic_write_line "abc123" "$WORK/pin.sha256" || fail "pin write failed"
[ "$(cat "$WORK/pin.sha256")" = "abc123" ] || fail "pin content wrong"
printf 'PIN-SECRET\n' > "$WORK/outside3"; ln -s "$WORK/outside3" "$WORK/pin_link"
if nxbootstrap_atomic_write_line "zzz" "$WORK/pin_link"; then
  [ "$(cat "$WORK/outside3")" = "PIN-SECRET" ] || fail "pin symlink written through"
fi

# --- interruption safety: a failing install leaves the previous file intact ---
printf 'PREVIOUS-VALID\n' > "$WORK/keep"
# a source that does not exist must fail without touching the destination
if nxbootstrap_atomic_install "$WORK/does-not-exist" "$WORK/keep"; then
  fail "install claimed success on a missing source"
fi
[ "$(cat "$WORK/keep")" = "PREVIOUS-VALID" ] || fail "failed install clobbered the last valid file"

printf 'owner-materialize-atomic: PASS (atomic + symlink-safe target/.new/pin, last-valid preserved)\n'
