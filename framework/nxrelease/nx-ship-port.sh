#!/usr/bin/env bash
# Take a port from source to a proven package in one command.
#
#   refresh pins -> build and verify the package -> prove it on real devices
#
# Each of those steps existed already; what did not exist was the guarantee
# that they run in that order with nothing derived by hand in between. Closing
# a package used to mean regenerating the launcher, then chasing a chain of
# hash edits until the release gate stopped complaining, then copying the zip
# to each device and reading logs. Every manual step there is a place where a
# stale pin or an unproven artifact reaches a release.
#
# Usage:
#   nx-ship-port.sh --port-dir DIR [--framework-root DIR]
#                   [--prove IP:USER:LAUNCHER]...
#                   [--skip-build]
#
# Password auth: export SSHPASS before calling (sshpass -e). A password inside
# --prove is REFUSED: argv leaks to every process via /proc/*/cmdline and to
# the shell history -- exactly the file class the audits scrub for.
#
# --prove may be repeated, once per device. A device that fails to draw fails
# the whole run.
set -euo pipefail

export LC_ALL=C

PORT_DIR="" FRAMEWORK_ROOT="" SKIP_BUILD=0
PROVE_TARGETS=()

fail() { printf 'nx-ship-port: %s\n' "$*" >&2; exit 1; }
step() { printf '\n=== nx-ship-port: %s ===\n' "$*"; }

while [ $# -gt 0 ]; do
  case $1 in
    --port-dir) PORT_DIR=${2:-}; shift 2 ;;
    --framework-root) FRAMEWORK_ROOT=${2:-}; shift 2 ;;
    --prove) PROVE_TARGETS+=("${2:-}"); shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    *) fail "unknown argument: $1" ;;
  esac
done

[ -n "$PORT_DIR" ] || fail 'missing --port-dir'
PORT_DIR=$(CDPATH= cd -- "$PORT_DIR" && pwd -P) || fail 'bad --port-dir'

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
[ -n "$FRAMEWORK_ROOT" ] || FRAMEWORK_ROOT=$(dirname -- "$SCRIPT_DIR")
FRAMEWORK_ROOT=$(CDPATH= cd -- "$FRAMEWORK_ROOT" && pwd -P)

NXPORT_SCHEMA=$(python3 -B - "$PORT_DIR" <<'PY'
import json
import pathlib
import sys

port = pathlib.Path(sys.argv[1])
project = port / "nxproject.json"
manifest = port / "nxport.json"
try:
    if project.is_file():
        value = json.loads(project.read_text(encoding="utf-8"))["nxport"]
    else:
        value = json.loads(manifest.read_text(encoding="utf-8"))
    schema = value.get("schema_version", 1)
    if type(schema) is not int or schema not in (1, 2, 3):
        raise ValueError("unsupported nxport schema")
except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
    raise SystemExit("cannot determine nxport schema: %s" % error)
print(schema)
PY
) || fail 'cannot determine nxport schema'

if [ "$NXPORT_SCHEMA" -lt 3 ] && [ ! -f "$PORT_DIR/nxrelease.json" ]; then
  fail "no nxrelease.json under legacy port $PORT_DIR"
fi

# Schema 1/2 ports keep their additive legacy contract: pins are refreshed
# before their existing helper runs. Schema 3 explicitly opts into the runtime
# closure and seals only after the project ELF exists.
if [ "$SKIP_BUILD" = 0 ] && [ "$NXPORT_SCHEMA" -lt 3 ]; then
  step "refreshing legacy pins for $(basename "$PORT_DIR")"
  python3 -B "$SCRIPT_DIR/nx-refresh-pins.py" \
    --port-dir "$PORT_DIR" --framework-root "$FRAMEWORK_ROOT"
fi

if [ "$SKIP_BUILD" = 0 ]; then
  [ -x "$PORT_DIR/package/build-package.sh" ] ||
    fail 'the port has no package/build-package.sh'

  # Release outputs are never overwritten, so a rebuild needs the previous
  # attempt cleared rather than a second directory nobody will look at. Ports
  # publish under different names, so clear each one they may use.
  rm -rf -- "$PORT_DIR/.build" "$PORT_DIR/dist"

  step 'building and verifying the package'
  # package/build-package.sh builds the ELF first and only then seals the
  # immutable runtime generation.  Refreshing before the build would bind a
  # stale prebuilt ELF (or fail on a clean checkout where it is intentionally
  # absent), so this orchestrator must not invert that order.
  # Ports name the framework root differently; export both spellings rather
  # than making each port's build script the odd one out.
  if ! NX_FRAMEWORK_ROOT="$FRAMEWORK_ROOT" \
       NEXTOS_FRAMEWORK_ROOT="$FRAMEWORK_ROOT" TMPDIR="$PORT_DIR/tmpbuild" \
       "$PORT_DIR/package/build-package.sh"; then
    if [ "$NXPORT_SCHEMA" -lt 3 ]; then
      step 'legacy build failed; refreshing pins once more before retry'
      python3 -B "$SCRIPT_DIR/nx-refresh-pins.py" \
        --port-dir "$PORT_DIR" --framework-root "$FRAMEWORK_ROOT"
    else
      step 'build failed; retrying the complete build-and-seal flow once'
    fi
    rm -rf -- "$PORT_DIR/.build" "$PORT_DIR/dist"
    NX_FRAMEWORK_ROOT="$FRAMEWORK_ROOT" \
      NEXTOS_FRAMEWORK_ROOT="$FRAMEWORK_ROOT" TMPDIR="$PORT_DIR/tmpbuild" \
      "$PORT_DIR/package/build-package.sh" ||
      fail 'the package does not build'
  fi
else
  step "refreshing pins for existing build of $(basename "$PORT_DIR")"
  python3 -B "$SCRIPT_DIR/nx-refresh-pins.py" \
    --port-dir "$PORT_DIR" --framework-root "$FRAMEWORK_ROOT"
fi

# Ports publish under different output directories, and a repository may carry
# an older archive in dist/ beside the one this run just produced in .build/.
# The archive's mtime is deterministic (SOURCE_DATE_EPOCH) and says nothing;
# the parent directory's mtime is polluted by this very tool writing
# proof-images/ next to the zip. The inode change time is neither: it records
# when the file was actually written to disk and no build option rewinds it.
ARCHIVE=""
newest=0
for candidate_dir in "$PORT_DIR/.build" "$PORT_DIR/dist"; do
  [ -d "$candidate_dir" ] || continue
  while IFS= read -r zip; do
    written=$(stat -c %Z "$zip" 2>/dev/null || echo 0)
    if [ "$written" -gt "$newest" ]; then
      newest=$written
      ARCHIVE=$zip
    fi
  done < <(find "$candidate_dir" -name '*.zip' -type f 2>/dev/null)
done
[ -n "$ARCHIVE" ] || fail 'no archive was produced'

# A schema-3 canonical package has already reopened the final ZIP exactly once
# inside nxrelease bundle.  Reopening it again here used to add latency without
# adding a new authority.  Legacy helpers and --skip-build outputs still need
# this boundary because nx-ship-port cannot know whether they verified bytes.
if [ "$NXPORT_SCHEMA" -lt 3 ] || [ "$SKIP_BUILD" = 1 ]; then
  step 'authenticating legacy/existing archive'
  if ! python3 -B "$FRAMEWORK_ROOT/nxrelease/nxrelease.py" \
      verify --archive "$ARCHIVE"; then
    fail 'nxrelease archive verification failed'
  fi
else
  step 'using canonical package verification (one ZIP reopen total)'
fi

ARCHIVE_SHA=$(sha256sum "$ARCHIVE" | cut -d' ' -f1)
printf '\nnx-ship-port: archive %s\nnx-ship-port: sha256 %s\n' \
  "$(basename "$ARCHIVE")" "$ARCHIVE_SHA"

[ ${#PROVE_TARGETS[@]} -gt 0 ] || {
  printf '\nnx-ship-port: built and verified; no device was asked for\n'
  exit 0
}

HARNESS="$FRAMEWORK_ROOT/nxobs/nx-device-launch.sh"
[ -f "$HARNESS" ] || fail "device harness not found: $HARNESS"

FAILED=0
for target in "${PROVE_TARGETS[@]}"; do
  IFS=: read -r ip user launcher extra <<EOF
$target
EOF
  # Onda v2 (AUD-10): senha em argv e' proibida -- vaza em /proc/*/cmdline e
  # no historico. Quatro campos = formato antigo com senha embutida: recusar
  # com a instrucao certa em vez de aceitar em silencio.
  if [ -n "${extra:-}" ]; then
    fail "password inside --prove is refused; export SSHPASS instead (target: $ip:$user:...)"
  fi
  password=${SSHPASS:-}
  [ -n "${ip:-}" ] && [ -n "${user:-}" ] && [ -n "${launcher:-}" ] ||
    fail "malformed --prove target: $target"

  device_dir=$(dirname -- "$launcher")

  step "installing on $ip"
  if [ -n "$password" ]; then
    sshpass -e scp -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
      "$ARCHIVE" "$user@$ip:/tmp/nx-ship.zip"
    sshpass -e ssh -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -n "$user@$ip" \
      "cd '$device_dir' && unzip -o -q /tmp/nx-ship.zip"
    : # senha flui por SSHPASS no ambiente, nunca por argv
  else
    scp -o BatchMode=yes -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
      "$ARCHIVE" "$user@$ip:/tmp/nx-ship.zip"
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -n "$user@$ip" \
      "cd '$device_dir' && unzip -o -q /tmp/nx-ship.zip"
  fi

  step "proving on $ip"
  # Proof images land next to the archive, one per device, measured by the
  # harness: an empty capture fails the run even when the verdict says OK.
  if NX_PROOF_OUT="$(dirname -- "$ARCHIVE")/proof-images" \
     bash "$HARNESS" --host "$ip" --user "$user" \
       --launcher "$launcher" --seconds "${NX_PROVE_SECONDS:-42}"; then
    printf 'nx-ship-port: %s proved the image\n' "$ip"
  else
    printf 'nx-ship-port: %s did not prove the image\n' "$ip"
    FAILED=1
  fi
done

if [ "$FAILED" = 0 ]; then
  printf '\nnx-ship-port: PASS — %s built, verified and proven on %d device(s)\n' \
    "$(basename "$ARCHIVE")" "${#PROVE_TARGETS[@]}"
  exit 0
fi
printf '\nnx-ship-port: FAIL — the package is not proven on every device\n'
exit 1
