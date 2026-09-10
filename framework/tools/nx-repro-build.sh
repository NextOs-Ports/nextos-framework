#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V3-REPRO-01: prove the framework's own generated artifacts are
# byte-reproducible from clean sources.
#
# Method: two detached git worktrees of the CURRENT HEAD commit, created at
# two temp paths of DIFFERENT depth (path independence is part of the proof).
# In each, under a controlled environment, nxgenerator is run over both public
# example manifests into per-worktree output directories.  The two output
# trees are then byte-compared recursively, including file modes.  Any
# divergence fails unless the exact path is excused (with written
# justification) in framework/tools/repro-exceptions.txt.
#
# The ELF double cross-build leg (reference loaders built twice and compared
# byte-identical) already exists in framework/nxabi/tools/nx-abi-gate.sh and
# is NOT duplicated here: pass --with-elf to run it, otherwise it stays
# delegated to the nxabi gate in the battery.
#
# On success, BUILD-PROVENANCE.json (schema nx-build-provenance-v1) is written
# into --out DIR via framework/tools/nx-build-provenance.py.
#
# Test hook: NXREPRO_TEST_MUTATE=1 appends one byte to one generated file in
# tree B before comparison; the comparison must then FAIL naming the file.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)
EXCEPTIONS_FILE="$SCRIPT_DIR/repro-exceptions.txt"
PROVENANCE_TOOL="$SCRIPT_DIR/nx-build-provenance.py"
ABI_GATE="$REPO_ROOT/framework/nxabi/tools/nx-abi-gate.sh"

EXAMPLES=(
  "framework/nxgenerator/examples/nxproject-aarch64.example.json"
  "framework/nxgenerator/examples/nxproject-armv7.example.json"
)
EXAMPLE_NAMES=(aarch64 armv7)

# Controlled build environment (recorded in the provenance).
REPRO_SOURCE_DATE_EPOCH=1786492800
REPRO_UMASK=022
REPRO_PATH=/usr/bin:/bin

usage() {
  printf 'usage: %s --out ABSOLUTE_DIR [--with-elf]\n' "${0##*/}" >&2
  exit 2
}

OUT_DIR=
WITH_ELF=0
while (( $# > 0 )); do
  case $1 in
    --out) [[ -n ${2:-} ]] || usage; OUT_DIR=$2; shift 2 ;;
    --with-elf) WITH_ELF=1; shift ;;
    -h|--help) usage ;;
    *) printf 'nx-repro-build: unknown argument: %s\n' "$1" >&2; usage ;;
  esac
done
[[ -n $OUT_DIR ]] || usage
case $OUT_DIR in
  /*) ;;
  *) printf 'nx-repro-build: --out must be absolute: %s\n' "$OUT_DIR" >&2; exit 2 ;;
esac
mkdir -p -- "$OUT_DIR"

say() { printf '[nx-repro-build] %s\n' "$*"; }
fail() { printf 'nx-repro-build: FAIL %s\n' "$*" >&2; exit 1; }

git -C "$REPO_ROOT" rev-parse --git-dir >/dev/null 2>&1 \
  || fail "not a git repository: $REPO_ROOT"
HEAD_COMMIT=$(git -C "$REPO_ROOT" rev-parse HEAD)

DIRTY_COUNT=$(git -C "$REPO_ROOT" status --porcelain --untracked-files=no | wc -l)
if (( DIRTY_COUNT > 0 )); then
  say "notice: working tree is dirty; $DIRTY_COUNT modified file(s) are" \
      "EXCLUDED from this proof — the proof is over HEAD $HEAD_COMMIT"
else
  say "working tree clean; proof is over HEAD $HEAD_COMMIT"
fi

# --- temp area + the two detached worktrees (different path depth) ----------
WORK_PARENT=${TMPDIR:-/tmp}
WORK_PARENT=$(cd -- "$WORK_PARENT" && pwd -P)
WORK=$(mktemp -d "$WORK_PARENT/nx-repro-build.XXXXXX")
WT1="$WORK/a/wt1"
WT2="$WORK/b/deeper/wt2"
OUT_A="$WORK/out-a"
OUT_B="$WORK/out-b"
FAKE_HOME="$WORK/home"
mkdir -p -- "$WORK/a" "$WORK/b/deeper" "$OUT_A" "$OUT_B" "$FAKE_HOME"

cleanup() {
  local status=$?
  trap - EXIT
  # Never touch the user's real worktrees/branches: remove ONLY the two
  # detached worktrees this run created, then the owned mktemp tree.
  local wt
  for wt in "$WT1" "$WT2"; do
    if [[ -d $wt ]]; then
      git -C "$REPO_ROOT" worktree remove --force -- "$wt" >/dev/null 2>&1 \
        || rm -rf -- "$wt"
    fi
  done
  git -C "$REPO_ROOT" worktree prune >/dev/null 2>&1 || true
  case $WORK in
    "$WORK_PARENT"/nx-repro-build.??????) rm -rf -- "$WORK" ;;
    *) printf 'nx-repro-build: refused cleanup outside owned mktemp: %s\n' \
         "$WORK" >&2; status=1 ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# Create the worktrees under a permissive umask. git applies `0777 & ~umask`
# to the executable bit on checkout, so under the battery's umask 077 a fresh
# `worktree add` yields the nxsplash ELF as 0700 and the generator rightly
# refuses (it requires exactly 0755). Checking out under umask 022 makes git
# write the recorded 0755 directly. A sweep afterwards is the deterministic
# safety net: every path git records as 100755 is forced to exactly 0755,
# unconditionally (0700 is still "executable", so a -x guard would wrongly
# skip it).
say "creating detached worktree 1 (depth 3): $WT1"
(umask 022; git -C "$REPO_ROOT" worktree add --detach --quiet -- "$WT1" "$HEAD_COMMIT")
say "creating detached worktree 2 (depth 4): $WT2"
(umask 022; git -C "$REPO_ROOT" worktree add --detach --quiet -- "$WT2" "$HEAD_COMMIT")

for repro_wt in "$WT1" "$WT2"; do
  git -C "$repro_wt" ls-files -s | while read -r repro_bits _rest; do
    [ "$repro_bits" = "100755" ] || continue
    repro_rel=${_rest#*$'\t'}
    [ -f "$repro_wt/$repro_rel" ] && chmod 0755 "$repro_wt/$repro_rel"
  done
done

# --- generation under the controlled environment ----------------------------
run_generator() {
  # $1 worktree root  $2 output root  $3 example rel path  $4 name
  local wt=$1 out=$2 example=$3 name=$4
  (
    umask "$REPRO_UMASK"
    exec env -i \
      PATH="$REPRO_PATH" \
      HOME="$FAKE_HOME" \
      LC_ALL=C LANG=C TZ=UTC \
      SOURCE_DATE_EPOCH="$REPRO_SOURCE_DATE_EPOCH" \
      PYTHONHASHSEED=0 \
      PYTHONDONTWRITEBYTECODE=1 \
      python3 "$wt/framework/nxgenerator/nxgenerator.py" \
        "$wt/$example" \
        --output "$out/$name" \
        --source-root "$wt"
  ) >/dev/null
}

for i in "${!EXAMPLES[@]}"; do
  say "worktree 1: generating ${EXAMPLE_NAMES[$i]} from ${EXAMPLES[$i]}"
  run_generator "$WT1" "$OUT_A" "${EXAMPLES[$i]}" "${EXAMPLE_NAMES[$i]}"
  say "worktree 2: generating ${EXAMPLE_NAMES[$i]} from ${EXAMPLES[$i]}"
  run_generator "$WT2" "$OUT_B" "${EXAMPLES[$i]}" "${EXAMPLE_NAMES[$i]}"
done

# --- optional test hook ------------------------------------------------------
if [[ ${NXREPRO_TEST_MUTATE:-0} == 1 ]]; then
  MUTATE_TARGET=$(cd -- "$OUT_B" && find . -type f | LC_ALL=C sort | head -n1)
  MUTATE_TARGET=${MUTATE_TARGET#./}
  [[ -n $MUTATE_TARGET ]] || fail "mutate hook found no generated file"
  say "TEST HOOK: NXREPRO_TEST_MUTATE=1 appending one byte to tree B file:" \
      "$MUTATE_TARGET"
  printf 'X' >> "$OUT_B/$MUTATE_TARGET"
fi

# --- byte + mode comparison --------------------------------------------------
# Manifest line format: "<octal mode> <sha256> <relative path>" for files,
# plus "<octal mode> dir <relative path>" for directories.
build_manifest() {
  local root=$1 dest=$2
  (
    cd -- "$root"
    {
      find . -type f | LC_ALL=C sort | while IFS= read -r f; do
        printf '%s %s %s\n' "$(stat -c %a -- "$f")" \
          "$(sha256sum -- "$f" | awk '{print $1}')" "${f#./}"
      done
      find . -mindepth 1 -type d | LC_ALL=C sort | while IFS= read -r d; do
        printf '%s dir %s\n' "$(stat -c %a -- "$d")" "${d#./}"
      done
    } > "$dest"
  )
}

MANIFEST_A="$WORK/manifest-a.txt"
MANIFEST_B="$WORK/manifest-b.txt"
build_manifest "$OUT_A" "$MANIFEST_A"
build_manifest "$OUT_B" "$MANIFEST_B"

# Load exceptions (relative paths; '#' comments and blanks ignored).
declare -A EXCEPTED=()
if [[ -f $EXCEPTIONS_FILE ]]; then
  while IFS= read -r line; do
    line=${line%%#*}
    line=${line#"${line%%[![:space:]]*}"}
    line=${line%"${line##*[![:space:]]}"}
    [[ -n $line ]] && EXCEPTED[$line]=1
  done < "$EXCEPTIONS_FILE"
else
  fail "missing exceptions file: $EXCEPTIONS_FILE"
fi

DIVERGENT=()
while IFS= read -r path; do
  [[ -n $path ]] || continue
  if [[ -n ${EXCEPTED[$path]:-} ]]; then
    say "excepted divergence (justified in repro-exceptions.txt): $path"
  else
    DIVERGENT+=("$path")
  fi
done < <(
  { diff -- "$MANIFEST_A" "$MANIFEST_B" || true; } \
    | sed -n 's/^[<>] [0-7]* [a-f0-9dir]* //p' | LC_ALL=C sort -u
)

# Belt and braces: full recursive byte diff must agree with the manifests.
DIFF_R_OUT="$WORK/diff-r.txt"
if ! diff -r -q -- "$OUT_A" "$OUT_B" > "$DIFF_R_OUT" 2>&1; then
  if (( ${#DIVERGENT[@]} == 0 )) && (( ${#EXCEPTED[@]} == 0 )); then
    cat -- "$DIFF_R_OUT" >&2
    fail "diff -r found divergence the manifest comparison missed"
  fi
fi

if (( ${#DIVERGENT[@]} > 0 )); then
  printf 'nx-repro-build: DIVERGENT paths (not excepted):\n' >&2
  printf '  %s\n' "${DIVERGENT[@]}" >&2
  fail "outputs are NOT byte-identical across the two clean worktrees" \
       "(${#DIVERGENT[@]} divergent path(s))"
fi
VERDICT=identical
say "outputs byte-identical across both worktrees (files, modes, dirs)"

# --- ELF leg -----------------------------------------------------------------
if (( WITH_ELF )); then
  say "running ELF double cross-build leg: framework/nxabi/tools/nx-abi-gate.sh"
  "$ABI_GATE" || fail "nx-abi-gate.sh failed"
else
  say "ELF double-build leg skipped here: it is delegated to the nxabi gate" \
      "(framework/nxabi/tools/nx-abi-gate.sh) in the battery; pass --with-elf" \
      "to run it now"
fi

# --- provenance --------------------------------------------------------------
say "writing BUILD-PROVENANCE.json to $OUT_DIR"
# Provenance identity is taken from worktree 1 (clean HEAD content), not from
# the possibly-dirty user checkout.
python3 "$PROVENANCE_TOOL" \
  --repo-root "$WT1" \
  --out "$OUT_DIR/BUILD-PROVENANCE.json" \
  --head-commit "$HEAD_COMMIT" \
  --verdict "$VERDICT" \
  --source-date-epoch "$REPRO_SOURCE_DATE_EPOCH" \
  --umask "$REPRO_UMASK" \
  --lc-all C --lang C --tz UTC \
  --manifest-a "$MANIFEST_A" \
  --manifest-b "$MANIFEST_B" \
  --command "env -i PATH=$REPRO_PATH LC_ALL=C LANG=C TZ=UTC SOURCE_DATE_EPOCH=$REPRO_SOURCE_DATE_EPOCH PYTHONHASHSEED=0 python3 <wt1>/framework/nxgenerator/nxgenerator.py <wt1>/${EXAMPLES[0]} --output <out1>/${EXAMPLE_NAMES[0]} --source-root <wt1>" \
  --command "env -i PATH=$REPRO_PATH LC_ALL=C LANG=C TZ=UTC SOURCE_DATE_EPOCH=$REPRO_SOURCE_DATE_EPOCH PYTHONHASHSEED=0 python3 <wt1>/framework/nxgenerator/nxgenerator.py <wt1>/${EXAMPLES[1]} --output <out1>/${EXAMPLE_NAMES[1]} --source-root <wt1>" \
  --command "env -i PATH=$REPRO_PATH LC_ALL=C LANG=C TZ=UTC SOURCE_DATE_EPOCH=$REPRO_SOURCE_DATE_EPOCH PYTHONHASHSEED=0 python3 <wt2>/framework/nxgenerator/nxgenerator.py <wt2>/${EXAMPLES[0]} --output <out2>/${EXAMPLE_NAMES[0]} --source-root <wt2>" \
  --command "env -i PATH=$REPRO_PATH LC_ALL=C LANG=C TZ=UTC SOURCE_DATE_EPOCH=$REPRO_SOURCE_DATE_EPOCH PYTHONHASHSEED=0 python3 <wt2>/framework/nxgenerator/nxgenerator.py <wt2>/${EXAMPLES[1]} --output <out2>/${EXAMPLE_NAMES[1]} --source-root <wt2>" \
  --command "diff -r <out1> <out2> && compare mode+sha256 manifests" \
  || fail "nx-build-provenance.py failed"

say "OK: reproducible build proven; provenance at $OUT_DIR/BUILD-PROVENANCE.json"
