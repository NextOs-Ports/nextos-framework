#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Preserve an append-only source checkpoint outside the repository.
# The command creates a new directory and never replaces or deletes a previous
# checkpoint.
set -euo pipefail

usage() {
  printf 'usage: %s --repo ABSOLUTE_REPO --checkpoint-root ABSOLUTE_DIRECTORY -- PATH [PATH ...]\n' \
    "${0##*/}" >&2
  exit 2
}

[[ ${1:-} == --repo && -n ${2:-} ]] || usage
repo=$2
shift 2
[[ ${1:-} == --checkpoint-root && -n ${2:-} ]] || usage
checkpoint_root=$2
shift 2
[[ ${1:-} == -- ]] || usage
shift
(( $# > 0 )) || usage

case $repo in /*) ;; *) usage ;; esac
case $checkpoint_root in /*) ;; *) usage ;; esac
# .git is a directory in a primary checkout and a regular file in a linked
# git worktree; both are valid repositories for checkpoint capture.
[[ -e $repo/.git && ! -L $repo/.git && ! -L $repo ]] || {
  printf 'capture-checkpoint: invalid repository: %s\n' "$repo" >&2
  exit 2
}
case $checkpoint_root in
  /|/home|"$repo"|"$repo"/*)
    printf 'capture-checkpoint: checkpoint root must be outside the repository and non-broad\n' >&2
    exit 2
    ;;
esac

paths=()
for requested in "$@"; do
  case $requested in
    ''|/*|../*|*/../*|*/..|.)
      printf 'capture-checkpoint: unsafe relative path: %s\n' "$requested" >&2
      exit 2
      ;;
  esac
  [[ -e $repo/$requested || -L $repo/$requested ]] || {
    printf 'capture-checkpoint: path does not exist: %s\n' "$requested" >&2
    exit 2
  }
  paths+=("$requested")
done

umask 077
mkdir -p -- "$checkpoint_root"
[[ -d $checkpoint_root && ! -L $checkpoint_root &&
   -w $checkpoint_root && -x $checkpoint_root ]] || {
  printf 'capture-checkpoint: unsafe checkpoint root: %s\n' "$checkpoint_root" >&2
  exit 1
}

stamp=$(date -u +%Y%m%dT%H%M%S)
checkpoint_id=${stamp}Z-pid$$-${RANDOM}${RANDOM}
checkpoint_dir=$checkpoint_root/$checkpoint_id
mkdir -- "$checkpoint_dir"

cd -- "$repo"
{
  printf 'format=nxframework-checkpoint-v1\n'
  printf 'checkpoint_id=%s\n' "$checkpoint_id"
  printf 'created_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)"
  printf 'repo=%s\n' "$repo"
  printf 'git_head=%s\n' "$(git rev-parse HEAD 2>/dev/null || printf unborn)"
  printf 'git_branch=%s\n' \
    "$(git symbolic-ref --quiet --short HEAD 2>/dev/null || printf detached)"
  printf 'included_path_count=%s\n' "${#paths[@]}"
  for path in "${paths[@]}"; do
    printf 'included_path=%s\n' "$path"
  done
} > "$checkpoint_dir/metadata.txt"

git status --short -- "${paths[@]}" > "$checkpoint_dir/git-status.txt"
git diff --binary -- "${paths[@]}" > "$checkpoint_dir/tracked-worktree.patch"
git diff --cached --binary -- "${paths[@]}" > "$checkpoint_dir/tracked-index.patch"

find "${paths[@]}" -type f -print0 |
  LC_ALL=C sort -z |
  xargs -0 -r sha256sum -- > "$checkpoint_dir/source-files.sha256"

tar --format=posix --numeric-owner -czf "$checkpoint_dir/source-snapshot.tar.gz" \
  -- "${paths[@]}"

(
  cd -- "$checkpoint_dir"
  sha256sum metadata.txt git-status.txt tracked-worktree.patch \
    tracked-index.patch source-files.sha256 source-snapshot.tar.gz \
    > MANIFEST.sha256
)

printf '%s\n' "$checkpoint_dir"
