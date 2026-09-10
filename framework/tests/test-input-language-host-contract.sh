#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V3 audit (blocker 2, renamed for ponto 10): HOST CONTRACT test of the chain
# file -> parser -> snapshot/context -> sink. Generates a real v3 port, installs
# the nxinput/nxcompat libraries, EDITS the generated NEXTOSCONTROLLERS.gptk
# (swap A/B) and writes NEXTOSSETTINGS.txt language=pt-BR, then a consumer that
# links ONLY the installed libs reads those files and proves the swapped action
# reaches the final sink and pt-BR is resolved. Also asserts the generated
# adapter-contract declares language_access (contract required, blocker 5/2).
#
# This is deliberately NOT an end-to-end (E2E) test: it materializes with the
# generator and delivers to an instrumented HOST sink, on the build host, with
# no launcher and no running game. A real E2E approval is launcher -> adapter
# -> visible game on the device and is tracked separately (device-side, ponto
# 1/3/12); this gate only proves the host-side library contract holds.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
GEN="$ROOT/framework/nxgenerator/nxgenerator.py"
EX="$ROOT/framework/nxgenerator/examples/nxproject-aarch64.example.json"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nx-host-contract.XXXXXX")
cleanup() { case "$WORK" in "${TMPDIR:-/tmp}"/nx-host-contract.*) rm -rf -- "$WORK";; esac; }
trap cleanup EXIT INT TERM
fail() { printf 'input-language-host-contract: %s\n' "$*" >&2; exit 1; }
CC=${CC:-cc}

# 1. install the two installed libraries into a prefix
PREFIX="$WORK/prefix"
for comp in nxcompat nxinput; do
  cmake -S "$ROOT/framework/$comp" -B "$WORK/build-$comp" -DCMAKE_BUILD_TYPE=Release \
        >/dev/null
  cmake --build "$WORK/build-$comp" --parallel >/dev/null
  cmake --install "$WORK/build-$comp" --prefix "$PREFIX" >/dev/null
done

# 2. generate a v3 port with a language_access contract
python3 - "$EX" "$WORK/project.json" <<'PY'
import json, sys
d = json.loads(open(sys.argv[1], encoding="utf-8").read())
d["schema_version"] = 3
d["language_access"] = {"mode": "adapter", "supported": ["en", "pt-BR"],
                        "fallback": "en", "sinks": ["unity-i2loc"]}
# nxgenerator 0.2.14: schema v3 declara o contrato de controles por ação/sink
d["controls"] = {
    "actions": [
        {"id": "example.accept", "kind": "button",
         "sinks": ["adapter.menu.accept"]},
        {"id": "example.back", "kind": "button",
         "sinks": ["adapter.menu.back"]},
        {"id": "example.move", "kind": "vector",
         "sinks": ["adapter.game.move"]},
        {"id": "example.pause", "kind": "button",
         "sinks": ["adapter.game.pause"]},
    ],
    "contexts": {
        "menu": {"A": "example.accept", "B": "example.back"},
        "gameplay": {"A": "example.accept", "B": "example.back",
                     "LEFT_STICK": "example.move", "START": "example.pause"},
    },
}
open(sys.argv[2], "w", encoding="utf-8").write(json.dumps(d))
PY
python3 -B "$GEN" "$WORK/project.json" --output "$WORK/out" >/dev/null
PORT="$WORK/out/nxexample-aarch64"
[ -f "$PORT/defaults/NEXTOSCONTROLLERS.gptk" ] || fail "no generated default gptk"
[ -f "$PORT/defaults/NEXTOSSETTINGS.txt" ] || fail "no generated default settings"

# adapter-contract must DECLARE language_access (contract required)
python3 - "$PORT/adapter/adapter-contract.json" <<'PY'
import json, sys
c = json.loads(open(sys.argv[1], encoding="utf-8").read())
acc = c.get("language_access")
assert isinstance(acc, dict) and acc.get("mode") == "adapter", "language_access not declared"
assert acc.get("fallback") in acc.get("supported", []), "fallback not in supported"
assert acc.get("sinks"), "no language sinks declared"
PY

# 3. materialize the owner's editable copies from defaults, then EDIT them:
#    swap A and B in gameplay, and set language=pt-BR.
python3 - "$PORT" <<'PY'
import re, sys, pathlib
port = pathlib.Path(sys.argv[1])
gptk = (port / "defaults/NEXTOSCONTROLLERS.gptk").read_text(encoding="utf-8")
# swap the gameplay A and B action assignments
lines = gptk.splitlines()
ctx = None
a_line = b_line = None
for i, ln in enumerate(lines):
    s = ln.strip()
    if s.startswith("[") and s.endswith("]"):
        ctx = s[1:-1]
    if ctx == "gameplay":
        if re.match(r"\s*A\s*=", ln): a_line = i
        if re.match(r"\s*B\s*=", ln): b_line = i
assert a_line is not None and b_line is not None, "no gameplay A/B lines"
av = lines[a_line].split("=", 1)[1].strip()
bv = lines[b_line].split("=", 1)[1].strip()
lines[a_line] = "A = %s" % bv
lines[b_line] = "B = %s" % av
(port / "NEXTOSCONTROLLERS.gptk").write_text("\n".join(lines) + "\n", encoding="utf-8")
(port / "NEXTOSSETTINGS.txt").write_text("# NEXTOS_SETTINGS/1\nlanguage=pt-BR\n", encoding="utf-8")
print("swapped_gameplay_A_to=%s" % bv)
# expose the expected action to the shell via a file
(port / ".expected_action").write_text(bv, encoding="utf-8")
PY
EXPECT_A=$(cat "$PORT/.expected_action")

# 4. compile the host-contract consumer linking ONLY the installed libs and run it
"$CC" -std=c99 -Wall -Wextra -Werror -O1 \
  -I "$PREFIX/include" \
  "$ROOT/framework/tests/host-contract/host_contract_consumer.c" \
  "$PREFIX/lib/libnxinput-gptk.a" "$PREFIX/lib/libnxcompat.a" -lm \
  -o "$WORK/consumer"
"$WORK/consumer" "$PORT/NEXTOSCONTROLLERS.gptk" "$PORT/NEXTOSSETTINGS.txt" \
  "$EXPECT_A" "pt-BR"

echo "input-language-host-contract: PASS (edited FILE -> installed parser -> sink '$EXPECT_A'; NEXTOSSETTINGS -> pt-BR; adapter-contract declares language_access)"
