#!/usr/bin/env python3
"""Small source gate for the Godot 4 source-ABI glue."""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
header = (root / "engine-glue/nxinput_gptk_godot4_glue.h").read_text()
source = (root / "engine-glue/nxinput_gptk_godot4_glue.cpp").read_text()

for token in (
    "nxinput_godot_action_preview",
    "nxinput_godot_action_commit",
    "input->parse_input_event",
    "nxinput_godot_split_vector",
    "nxinput_godot4_vector_release",
    "nxinput-gptk-godot4-glue/1",
):
    if token not in source:
        raise SystemExit(f"Godot runtime template: FAIL: missing {token}")
for forbidden in (
    "tearscape", "start_coop", "attack2", "res://", "SDL_",
    "0.55f", "0.30f", "real engine consumer accepted",
):
    if forbidden.lower() in (header + source).lower():
        raise SystemExit(f"Godot runtime template: FAIL: port policy leaked: {forbidden}")
action_callback = source[
    source.index("int nxinput_godot4_action_callback"):
    source.index("int nxinput_godot4_vector_callback")
]
if action_callback.index("godot4_enqueue(") > action_callback.index(
        "nxinput_godot_action_commit"):
    raise SystemExit("Godot runtime template: FAIL: action commits before enqueue")
print("Godot runtime template: PASS source-ABI=1 port-policy=absent")
