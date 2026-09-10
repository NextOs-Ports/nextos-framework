#!/usr/bin/env python3
"""Fail closed if Party Hard regains duplicate or unsafe input authority."""
from pathlib import Path

port = Path(__file__).resolve().parents[1]
source = (port / "src/input.c").read_text()
helper = (port / "src/input_guard.c").read_text()
build = (port / "build_universal.sh").read_text()

required_source = (
    'optional_sdl("SDL_JoystickPathForIndex")',
    'static const char device_root[] = "/dev/input/";',
    "O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW",
    "fstat(fd, &info)",
    "S_ISCHR(info.st_mode)",
    "EVIOCGBIT(EV_KEY",
    "EVIOCGKEY(sizeof state)",
    "EVIOCGABS(code)",
    "EVIOCGABS(xcode)",
    "EVIOCGABS(ycode)",
    "SDL_CONTROLLER_BINDTYPE_HAT",
    "ph_input_guard_button_value(",
    "ph_input_guard_axis_value(",
    "ph_input_resolve_dpad(",
    "float hx = 0.0f;",
    "float hy = 0.0f;",
    'deliver_neutral_motion("controller-removed")',
    'deliver_neutral_motion("focus-lost")',
    'deliver_neutral_motion("controller-unavailable")',
    'deliver_neutral_motion("exit-chord")',
    'deliver_neutral_motion("input-fatal")',
    'deliver_neutral_motion("shutdown")',
)
for token in required_source:
    assert token in source, f"missing input release contract: {token}"

# The node comes only from SDL's admitted device identity. Never rediscover,
# synthesize or scan Linux input nodes inside this port.
for forbidden in ("/dev/input/event", "opendir(", "scandir(", "readdir("):
    assert forbidden not in source, f"unsafe input-node discovery returned: {forbidden}"

# Party Hard's measured native route is Android DPAD KeyEvent. Reintroducing a
# HAT level duplicates authority and resurrects menu skipping/stale movement.
for forbidden in ("(float)(rg - lf)", "(float)(dn - up)"):
    assert forbidden not in source, f"duplicate D-pad HAT route returned: {forbidden}"

for token in (
    "if (raw && snapshot_valid && physically_neutral && binding_supported)",
    "if (raw && snapshot_valid && physically_centered)",
    "ph_input_hat_axis_is_centered",
):
    assert token in helper, f"neutral-only helper contract missing: {token}"

# Bench fault injection must remain absent from a public ELF.
for token in (
    "ST_INPUT_ENGINE_PROBE",
    "ST_INPUT_STALE_BUTTON",
    "ST_INPUT_STALE_AXIS",
):
    assert token in build, f"public bench-string gate missing: {token}"

print("PASS: exact-device neutral guard, KeyEvent-only D-pad and lifecycle releases locked")
