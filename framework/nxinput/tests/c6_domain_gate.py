# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C6 -- the ordinal domains, against the pinned sources.

CLASS: SOURCE_AUDIT. Nothing here runs an SDL. It reads the enumeration loops
out of the pinned upstream `SDL_sysjoystick.c` files, hashes those files
against the pins, and holds the result against what nxinput_sdl claims.

The question it answers is the one the whole SDL2 -> SDL3 conversion turns on:
do the majors number the same pad the same way? An assumption in either
direction would be a bug -- assume they are equal and a divergent pin
silently corrupts every mapping; assume they differ and a conversion fires
where none is needed. So it is measured, per pin, every run.
"""

import argparse, hashlib, json, re, subprocess, sys

BTN_JOYSTICK, KEY_MAX, BTN_MISC = 0x120, 0x2FF, 0x100
ABS_MAX = 0x40

CONST = {"BTN_JOYSTICK": BTN_JOYSTICK, "KEY_MAX": KEY_MAX,
         "BTN_MISC": BTN_MISC, "ABS_MAX": ABS_MAX, "0": 0}

BUTTON_LOOP = re.compile(
    r"for\s*\(\s*i\s*=\s*(\w+)\s*;\s*i\s*<\s*(\w+)\s*;\s*\+\+i\s*\)")
AXIS_LOOP = re.compile(
    r"for\s*\(\s*i\s*=\s*(\w+)\s*;\s*i\s*<\s*(\w+)\s*;\s*\+\+i\s*\)")


def sha256(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def extract(path):
    """The button scan ranges and the axis scan range, from the source."""
    text = open(path, encoding="utf-8", errors="replace").read()
    # ConfigJoystick() is the function that assigns the ordinals: it walks
    # the capability bitmaps and hands out joystick->nbuttons / ->naxes in
    # scan order. That IS the domain, so it is the only region read.
    # The definition, not the call. 2.0.10 wraps the return type onto its
    # own line, so the anchor is the name followed by its parameter list.
    m = re.search(r"^(?:static\s+void\s+)?ConfigJoystick\s*\(", text, re.M)
    if m is None:
        raise SystemExit("%s: ConfigJoystick() not found; the pin cannot be "
                         "audited by inspection and must not be assumed"
                         % path)
    start = m.start()
    body = text[start:start + 7000]
    buttons = []
    for first, limit in BUTTON_LOOP.findall(body):
        if first in CONST and limit in CONST:
            buttons.append((CONST[first], CONST[limit]))
        if len(buttons) == 2:
            break
    axis = None
    if re.search(r"for\s*\(\s*i\s*=\s*0\s*;\s*i\s*<\s*ABS_MAX\s*;\s*\+\+i",
                 body):
        axis = (0, ABS_MAX)
    # Three distinct backend behaviours, read out of the source rather than
    # graded: skip nothing, skip the detected pairs, or skip the whole hat
    # range unconditionally.
    if re.search(r"i >= ABS_HAT0X && i <= ABS_HAT3Y &&\s*"
                 r"joystick->hwdata->has_hat\[\(i - ABS_HAT0X\) / 2\]", body):
        skips_hats = 1
    elif re.search(r"if\s*\(\s*i == ABS_HAT0X\s*\)\s*\{\s*"
                   r"i = ABS_HAT3Y;\s*continue;", body):
        skips_hats = 2
    else:
        skips_hats = 0
    return buttons, axis, skips_hats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True)
    ap.add_argument("--source", action="append", required=True,
                    help="name=path of a pinned SDL_sysjoystick.c")
    ap.add_argument("--pins", required=True)
    ap.add_argument("--init-source", action="append", default=[],
                    help="name=path of the file that decides WHEN each major "
                         "loads its built-in gamepad mapping database")
    args = ap.parse_args()

    pins = json.load(open(args.pins))
    out = subprocess.run([args.probe], capture_output=True, text=True,
                         check=True).stdout
    claimed = {}
    for line in out.splitlines():
        if line.startswith("domain "):
            name = line.split()[1]
            btn = re.findall(r"\[(0x[0-9a-f]+),(0x[0-9a-f]+)\)",
                             line.split("buttons=")[1].split(" axes=")[0])
            ax = re.findall(r"\[(0x[0-9a-f]+),(0x[0-9a-f]+)\)",
                            line.split("axes=")[1])
            claimed[name] = {
                "buttons": [(int(a, 16), int(b, 16)) for a, b in btn],
                "axis": (int(ax[0][0], 16), int(ax[0][1], 16)),
                "skip_hats": int(line.split("skip_hats=")[1]),
            }

    fails = []
    checks = 0

    def check(ok, label):
        nonlocal checks
        checks += 1
        print(("ok   " if ok else "FAIL ") + label)
        if not ok:
            fails.append(label)

    measured = {}
    for spec in args.source:
        name, path = spec.split("=", 1)
        pin = pins["sdl"][name]
        check(sha256(path) == pin["sha256"],
              "%s: the source file is the pinned one" % name)
        buttons, axis, skips = extract(path)
        measured[name] = (buttons, axis, skips)
        domain = pin["domain"]
        want = claimed[domain]
        want["skip_hats"] = pin["skip_hats"]
        check(buttons == want["buttons"],
              "%s: button scan %s matches the %s domain" %
              (name, [(hex(a), hex(b)) for a, b in buttons], domain))
        check(axis == want["axis"],
              "%s: axis scan %s matches the %s domain" %
              (name, (hex(axis[0]), hex(axis[1])) if axis else None, domain))
        check(skips == want["skip_hats"],
              "%s: the axis scan treats the hat range as skip_hats=%d, which "
              "is what the %s domain says" % (name, skips, domain))

    # THE finding, stated as a measurement rather than a premise.
    #
    # The three EXECUTED pins are the ones the conversion question is about.
    # 2.0.10 is a source audit only and is deliberately excluded here: it is
    # the pin that shows the domain DID move once, and folding it into an
    # equality claim would hide exactly that.
    executed = {k: v for k, v in measured.items()
                if pins["sdl"][k]["role"].endswith("EXECUTED")}
    values = list(executed.values())
    equal = all(v == values[0] for v in values)
    check(equal, "the three EXECUTED pins enumerate a pad identically, so a "
                 "mapping crosses SDL2 -> SDL3 byte-intact and no conversion "
                 "is owed: %s" % sorted(executed))
    check("equal sdl2_sdl3=%d" % (1 if equal else 0) in out,
          "nxinput_sdl claims that same equality and no more")
    check("equal joydev_sdl2=0" in out,
          "the legacy joydev dialect is NOT the SDL evdev domain -- that is "
          "the one conversion that is really owed")
    check("equal legacy_sdl2=0" in out,
          "SDL2 2.0.10 is NOT the modern SDL2 domain either: its axis scan "
          "drops the whole hat range, so a pad with an unpaired ABS_HAT axis "
          "numbers differently. Recorded as a distinct domain, not smoothed "
          "over")

    # ------------------------------------------------ authority 4's window
    #
    # Whether the runtime's OWN database can answer at the announce boundary
    # is not a preference, it is an ordering fact of each major, and the two
    # majors differ. Auditing it from the pinned sources is what turns "we
    # saw SDL3 refuse" into "SDL3 cannot answer there, by construction".
    order = pins.get("mapping_init_order", {})
    for spec in args.init_source:
        name, path = spec.split("=", 1)
        entry = order[name]
        check(sha256(path) == entry["sha256"],
              "%s: the initialisation-order source is the pinned one" % name)
        text = open(path, encoding="utf-8", errors="replace").read()
        positions = []
        for needle in entry["must_contain_in_order"]:
            idx = text.find(needle)
            check(idx >= 0, "%s: the source still contains %r" % (name, needle))
            positions.append(idx)
        check(all(a >= 0 for a in positions) and
              positions == sorted(positions),
              "%s: %s -- %s" % (name,
                                " then ".join(entry["must_contain_in_order"]),
                                entry["consequence"]))

    print("\nc6_domain_gate: %d checks, %d passed, %d failed"
          % (checks, checks - len(fails), len(fails)))
    if fails:
        print("c6_domain_gate: FAIL")
        return 1
    print("c6_domain_gate: PASS (SOURCE_AUDIT; no SDL was executed here)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
