#!/usr/bin/env bash
# Resolve the framework repository used by standalone development tools.
# The resolved path is host-only input and is never embedded in release bytes.

sc_resolve_framework_repository() {
  local port_dir=$1
  local candidate=
  local repository_top=

  if [[ -n ${SC_FRAMEWORK_REPOSITORY:-} ]]; then
    candidate=$SC_FRAMEWORK_REPOSITORY
  elif [[ -n ${SC_FRAMEWORK_ROOT_HOST:-} ]]; then
    candidate=$(CDPATH= cd -- "$SC_FRAMEWORK_ROOT_HOST/.." 2>/dev/null && pwd -P) || {
      printf 'invalid SC_FRAMEWORK_ROOT_HOST: %s\n' "$SC_FRAMEWORK_ROOT_HOST" >&2
      return 1
    }
  else
    repository_top=$(git -C "$port_dir" rev-parse --show-toplevel 2>/dev/null) || return 1
    for candidate in "$repository_top" "$port_dir/../.." "$port_dir/.."; do
      if [[ -f $candidate/framework/nxgenerator/nxgenerator.py ]]; then
        break
      fi
      candidate=
    done
  fi

  [[ -n $candidate ]] || {
    printf '%s\n' \
      'framework repository not found; set SC_FRAMEWORK_REPOSITORY to its checkout root' >&2
    return 1
  }
  candidate=$(CDPATH= cd -- "$candidate" 2>/dev/null && pwd -P) || {
    printf 'framework repository is not a directory: %s\n' "$candidate" >&2
    return 1
  }
  [[ -f $candidate/framework/nxgenerator/framework_pin.py &&
     -f $candidate/framework/nxrelease/nxrelease.py &&
     -f $candidate/framework/nxbootstrap/tools/generate-port.py ]] || {
    printf 'incomplete framework repository: %s\n' "$candidate" >&2
    return 1
  }
  git -C "$candidate" rev-parse --git-dir >/dev/null 2>&1 || {
    printf 'framework repository is not a Git checkout: %s\n' "$candidate" >&2
    return 1
  }
  printf '%s\n' "$candidate"
}
