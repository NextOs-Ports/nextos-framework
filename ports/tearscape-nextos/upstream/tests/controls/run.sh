#!/bin/bash
# Directed host gate for the NEXTOSCONTROLLERS live runtime (no device, no ZIP).
set -euo pipefail
HERE=$(cd -- "$(dirname -- "$0")" && pwd -P)
SRC="$HERE/../../src/nxinput"
OUT=$(mktemp -d /tmp/claude-1000/nxgptk-gate-XXXX 2>/dev/null || mktemp -d)
trap 'rm -rf "$OUT"' EXIT
if (( $# > 1 )); then
	printf 'usage: %s [generated-NEXTOSCONTROLLERS.gptk]\n' "$0" >&2
	exit 2
fi
# 0.2.17 (V5): the schema-4 closure is proven by the FRAMEWORK's universal
# host gate, compiled here against THIS PORT's vendored bytes. Until now the
# port carried its own harness, and when the owner moved to schema 4 that
# harness stayed on the V1-V3 loader: it rejected every schema-4 owner with
# NXI1006 and the failure surfaced three cascades later as "generated vector
# reaches the declared action sink" (M1c diagnosis; audit item E11). A port
# does not own a controls harness -- it owns the contract it declares.
# The gate source is VENDORED like every other nxinput byte this port
# compiles, and pinned by recipes/refresh_release_inputs.py. Compiling it from
# the live framework tree would prove the port against whatever the framework
# happens to be right now, not against the commit the port declares.
cc -O1 -Wall -Wextra -I"$SRC" -o "$OUT/host-gate" \
  "$SRC/nx-gptk4-host-gate.c" \
  "$SRC/nxinput_gptk4_preinit.c" "$SRC/nxinput_gptk4_bridge.c" \
  "$SRC/nxinput_gptk4.c" "$SRC/nxinput_gptk.c" "$SRC/nxinput_gptk_motion.c" \
  "$SRC/nxinput_gptk_live.c" "$SRC/nxinput_sha256.c" \
  "$SRC/nxinput_gptk_loader.c" "$SRC/nxinput_gptk_preinit.c" -lm
if (( $# == 1 )); then
	# The generated owner lives next to the generated port; the gate wants the
	# game directory that holds it (owner and defaults/).
	OWNER_DIR=$(cd -- "$(dirname -- "$1")" && pwd -P)
	if [[ $(basename -- "$OWNER_DIR") == defaults ]]; then
		OWNER_DIR=$(dirname -- "$OWNER_DIR")
	fi
	CLOSURE_ARGS=()
	# M1b decision: CONTROLS-CLOSURE.json is the ONE closure artefact. The TSV
	# is only this binary's command-line encoding, rendered on the spot --
	# never a second file checked into the tree.
	CLOSURE_JSON="$OWNER_DIR/CONTROLS-CLOSURE.json"
	if [[ -f $CLOSURE_JSON ]]; then
		python3 - "$CLOSURE_JSON" > "$OUT/closure.tsv" <<'PYCLOSURE'
import json, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
if doc.get("schema") != "nx-controls-closure/1":
    raise SystemExit("CONTROLS-CLOSURE.json is not nx-controls-closure/1")
for case in doc["cases"]:
    print("\t".join((case["context"], case["control"], case["action"],
                      case["sink"], case["event"], str(case["delivery_count"]))))
PYCLOSURE
		CLOSURE_ARGS=(--closure "$OUT/closure.tsv")
		printf 'TEARSCAPE CLOSURE: rendered %s cases from CONTROLS-CLOSURE.json\n' \
			"$(wc -l < "$OUT/closure.tsv")"
	fi
	"$OUT/host-gate" --contract "$HERE/../../adapter/adapter-contract.json" \
		--owner-dir "$OWNER_DIR" --contexts menu,gameplay "${CLOSURE_ARGS[@]}"
fi
# The port's remaining V1-V3 cases are its own (inline maps, swap, fail-closed).
cc -O1 -Wall -Wextra -I"$SRC" -o "$OUT/gate" \
  "$HERE/test_gptk_runtime_host.c" \
  "$SRC/nxinput_gptk.c" "$SRC/nxinput_gptk_live.c" \
  "$SRC/nxinput_gptk_loader.c" "$SRC/nxinput_gptk_motion.c" -lm
"$OUT/gate"
cc -O1 -Wall -Wextra \
	-I"$SRC" \
	-I"$HERE/../../src/godot_engine/v4-universal/drivers/sdl" \
  -o "$OUT/context-gate" "$HERE/test_tearscape_gptk_context.c"
"$OUT/context-gate"
c++ -std=c++17 -Wall -Wextra -Werror \
	-I"$HERE/../../src/godot_engine/v4-universal/drivers/sdl" \
	-c "$HERE/../../src/godot_engine/v4-universal/drivers/sdl/nxinput_gptk_godot.cpp" \
	-o "$OUT/nxinput-gptk-nonfbdev.o"
printf 'TEARSCAPE GPTK NON-FBDEV TU: PASS\n'
# 0.2.17: the logical-player pad set (union, max-magnitude axes, same-pad
# chord, cross-pad denial logged once, compaction, cap) is a pure unit with
# no Godot/SDL header; prove it on the host.
c++ -std=c++17 -Wall -Wextra -Werror \
	-I"$HERE/../../src/godot_engine/v4-universal/drivers/sdl" \
	-o "$OUT/padset-gate" "$HERE/test_tearscape_padset.cpp" \
	"$HERE/../../src/godot_engine/v4-universal/drivers/sdl/tearscape_padset.cpp"
"$OUT/padset-gate"
# 0.2.17: the GPTK evidence receipt (JSON lines read back by the framework's
# automated on-device proof and the release lock) is a pure unit too: exact
# line shapes, adapter sink-id table, append/line-buffered file, release
# attribution and vector EDGE semantics are proven on the host.
c++ -std=c++17 -Wall -Wextra -Werror \
	-I"$HERE/../../src/godot_engine/v4-universal/drivers/sdl" \
	-o "$OUT/receipt-gate" "$HERE/test_tearscape_gptk_receipt.cpp" \
	"$HERE/../../src/godot_engine/v4-universal/drivers/sdl/tearscape_gptk_receipt.cpp"
TMPDIR="$OUT" "$OUT/receipt-gate"

bash "$HERE/test_layout_authority.sh"
