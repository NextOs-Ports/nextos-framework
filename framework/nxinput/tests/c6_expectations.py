# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C6 -- what the real SDL OWES, derived independently.

This file is the reference the gate judges against, and it must not share a
line of reasoning with the code under test. It therefore:

  * takes the pad's capabilities from the KERNEL side of the harness (the
    exact EV_KEY/EV_ABS codes uinput was told to create), never from nxinput;
  * re-derives the ordinal numbering from the SDL backend's documented scan
    order, written out here in plain Python;
  * parses the mapping text with its own small parser;
  * predicts, for one physical event, which of the 18 V2 groups SDL must
    report -- or that SDL must stay SILENT.

If nxinput_sdl and this file ever disagree, the gate fails. That is the point:
a mutation in the module cannot also mutate its own expectation.
"""

BTN_JOYSTICK = 0x120
KEY_MAX = 0x2FF
ABS_HAT0X, ABS_HAT3Y = 0x10, 0x17
ABS_MAX = 0x40

# The SDL control name that carries each V2 group. Two names for the sticks,
# because a stick is a pair.
GROUPS = {
    "A": ["a"], "B": ["b"], "X": ["x"], "Y": ["y"],
    "L1": ["leftshoulder"], "R1": ["rightshoulder"],
    "L2": ["lefttrigger"], "R2": ["righttrigger"],
    "L3": ["leftstick"], "R3": ["rightstick"],
    "START": ["start"], "SELECT": ["back"],
    "DPAD_UP": ["dpup"], "DPAD_DOWN": ["dpdown"],
    "DPAD_LEFT": ["dpleft"], "DPAD_RIGHT": ["dpright"],
    "LEFT_STICK": ["leftx", "lefty"],
    "RIGHT_STICK": ["rightx", "righty"],
}
GUIDE = "GUIDE"
METADATA = ("platform", "hint", "crc", "type", "sdk", "hidapi")

# WHICH PATH A GROUP ARRIVES ON, and why it is not the physical kind.
#
# SDL decides the output kind from the CONTROL, never from what the mapping
# bound it to. `lefttrigger` is SDL_CONTROLLER_AXIS_TRIGGERLEFT even when the
# mapping says `lefttrigger:b10`: a button-bound trigger is reported as an
# AXIS going 0 -> 32767 -> 0, not as a button press. Real official corpus
# entries do exactly this -- the OpenSimHardware OSH PB Controller entry binds
# both triggers to buttons -- so an expectation that looked for L2 on the
# button path would be wrong about SDL, not about the mapping.
AXIS_OUTPUT_GROUPS = {"L2", "R2", "LEFT_STICK", "RIGHT_STICK"}


def output_kind(group):
    """"axis" or "button": the path SDL reports this group on."""
    return "axis" if group in AXIS_OUTPUT_GROUPS else "button"


def button_ordinals(key_codes):
    """evdev code -> SDL button ordinal.

    The Linux backend of both majors scans [BTN_JOYSTICK, KEY_MAX) first and
    [0, BTN_JOYSTICK) second. Written here from the upstream loops, not read
    from nxinput_sdl.
    """
    high = sorted(c for c in key_codes if BTN_JOYSTICK <= c < KEY_MAX)
    low = sorted(c for c in key_codes if c < BTN_JOYSTICK)
    return {code: i for i, code in enumerate(high + low)}


def joydev_button_ordinals(key_codes):
    """evdev code -> legacy PortMaster/joydev ordinal.

    The muOS field mapping includes lower media keys in the same ascending
    sweep as its gamepad keys. The volume bindings positively expose that
    dialect; this independent expectation must model the SOURCE domain, not
    read the projection performed by nxinput.
    """
    return {code: i for i, code in enumerate(sorted(c for c in key_codes
                                                    if c < KEY_MAX))}


def axis_ordinals(abs_codes):
    """evdev ABS code -> SDL axis ordinal, hat pairs removed."""
    hats = {c for c in abs_codes if ABS_HAT0X <= c <= ABS_HAT3Y}
    paired = set()
    for c in sorted(hats):
        base = ABS_HAT0X + ((c - ABS_HAT0X) // 2) * 2
        if base in abs_codes and base + 1 in abs_codes:
            paired.add(base)
            paired.add(base + 1)
    order = sorted(c for c in abs_codes if c < ABS_MAX and c not in paired)
    return {code: i for i, code in enumerate(order)}


def hat_ordinals(abs_codes):
    """hat index -> (X code, Y code) for every complete pair."""
    out = {}
    for base in range(ABS_HAT0X, ABS_HAT3Y, 2):
        if base in abs_codes and base + 1 in abs_codes:
            out[(base - ABS_HAT0X) // 2] = (base, base + 1)
    return out


def parse(mapping):
    """{control name: raw binding} for one mapping line's real bindings."""
    out = {}
    if not mapping:
        return out
    for field in mapping.split(",")[2:]:
        if ":" not in field or not field:
            continue
        key, value = field.split(":", 1)
        if key in METADATA:
            continue
        out[key] = value
    return out


def select_entry(config, guid):
    """The entry for `guid` in a heterogeneous list -- exact GUID only.

    Comments and blank lines are skipped. A list with the GUID twice and two
    different bodies has no answer: order must never decide it.
    """
    hits = []
    for line in (config or "").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.split(",", 1)[0].lower() == guid.lower():
            hits.append(line)
    if not hits:
        return None
    if any(h != hits[0] for h in hits):
        return "AMBIGUOUS"
    return hits[0]


def group_for_physical(mapping, key_codes, abs_codes, kind, code, mask=None,
                       button_domain="evdev"):
    """Which V2 group must SDL report for this physical input, or None.

    None means SILENCE is the correct answer -- which is what the A/B=null
    case is entirely about.
    """
    bindings = parse(mapping)
    buttons = (joydev_button_ordinals(key_codes)
               if button_domain == "joydev" else button_ordinals(key_codes))
    axes = axis_ordinals(abs_codes)
    hats = hat_ordinals(abs_codes)

    if kind == "button":
        want = "b%d" % buttons[code]
    elif kind == "axis":
        want = "a%d" % axes[code]
    elif kind == "hat":
        index = next(i for i, (x, y) in hats.items() if code in (x, y))
        want = "h%d.%d" % (index, mask)
    else:
        raise ValueError(kind)

    for group, controls in list(GROUPS.items()) + [(GUIDE, ["guide"])]:
        for control in controls:
            value = bindings.get(control)
            if value is None:
                continue
            # A half-axis or inverted binding still names the same axis.
            plain = value.lstrip("+-").rstrip("~")
            if plain == want or value == want:
                return group
    return None


def bound_groups(mapping):
    """The V2 groups this mapping actually binds. Everything else must be
    silent on BOTH the event path and the polling path."""
    bindings = parse(mapping)
    out = set()
    for group, controls in list(GROUPS.items()) + [(GUIDE, ["guide"])]:
        if any(c in bindings for c in controls):
            out.add(group)
    return out
