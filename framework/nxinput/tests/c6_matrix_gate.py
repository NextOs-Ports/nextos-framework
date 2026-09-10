# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C6 -- judge what the REAL SDL libraries answered.

Every CHECK below is about a transcript produced by a real SDL library in its
own process, against real kernel pads, with the seam linked into that library.
The gate contributes no answers: it compares the transcript with
c6_expectations, which derives what SDL owes independently of nxinput.

CLASS: REAL_API_HOST for everything the two majors answered. It is NOT
physical proof -- the pads are uinput devices on this host's kernel -- and no
line here may say otherwise.
"""

import argparse, hashlib, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c6_expectations as E
import c6_uinput as U

CHECKS = 0
FAILS = []


def check(ok, label):
    global CHECKS
    CHECKS += 1
    if ok:
        print("ok   %s" % label)
    else:
        FAILS.append(label)
        print("FAIL %s" % label)


def events(consumer, path=None):
    out = [e for e in consumer.get("trace", []) if e.get("kind") == "event"]
    return [e for e in out if path is None or e.get("path") == path]


def polls(consumer):
    return [p for p in consumer.get("trace", []) if p.get("kind") == "poll"]


def seam_stage(result, stage):
    return [l for l in result.get("seam_receipt", []) if "stage=%s" % stage in l]


def admit_lines(result):
    return [l for l in seam_stage(result, "announce") if "result=admit" in l]


def source_of(line):
    for token in line.split():
        if token.startswith("source="):
            return token.split("=", 1)[1]
    return None


# The physical order sequence_full() injects, as (kind, code, mask).
FULL_BUTTONS = [U.BTN_A, U.BTN_B, U.BTN_X, U.BTN_Y, U.BTN_TL, U.BTN_TR,
                U.BTN_SELECT, U.BTN_START, U.BTN_MODE, U.BTN_THUMBL,
                U.BTN_THUMBR]
FULL_HATS = [(U.ABS_HAT0Y, 1), (U.ABS_HAT0Y, 4), (U.ABS_HAT0X, 8),
             (U.ABS_HAT0X, 2)]
MUOS_KEYS = [0x01, 0x72, 0x73] + list(range(0x130, 0x13d))
MUOS_AXES = [U.ABS_X, U.ABS_Y, U.ABS_RX, U.ABS_RY,
             U.ABS_HAT0X, U.ABS_HAT0Y]
MUOS_BUTTONS = list(range(0x130, 0x13d))
MUOS_ROM_SCENARIOS = ("muos_rom_exact", "muos_rom_bundle")
MUOS_SCENARIOS = ("muos_joydev",) + MUOS_ROM_SCENARIOS
MUOS_ROM_GUID = "19000000010000000100000000010000"
MUOS_LIVE_GUID = "19004ca6010000000100000000010000"
MUOS_ROM_FIXTURE_SHA256 = (
    "bcb4c8297d3fbdff96ee68006fcd21a8576a7ef99f604f974b29fc8dc17261a8")
MUOS_ROM_FIXTURE = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests",
    "fixtures", "muos-2601.1", "gamecontrollerdb-rg40xx-h-modern.txt"))


def exact_muos_rom_mapping():
    with open(MUOS_ROM_FIXTURE, "rb") as fh:
        fixture = fh.read()
    prefix = (MUOS_ROM_GUID + ",Deeplay-keys,").encode("ascii")
    matches = [line for line in fixture.splitlines() if line.startswith(prefix)]
    return fixture, [line.decode("ascii") for line in matches]


def judge(result):
    api = result["which"]
    scenario = result["scenario"]
    tag = "%s/%s" % (api, scenario)
    consumer = result.get("consumer") or {}
    mapping = result.get("mapping")
    guid = result.get("guid")

    if consumer.get("malformed"):
        check(False, "%s: the consumer transcript is not valid JSON" % tag)
        return
    check(consumer.get("keyboard_events") == 0,
          "%s: zero synthetic keyboard events" % tag)

    blocked = scenario in ("negative_syntax", "negative_empty",
                           "negative_other_guid")
    if blocked:
        check(consumer.get("enumerated") == 0,
              "%s: the refused pad was never announced" % tag)
        check(consumer.get("joysticks_visible") == 0,
              "%s: the refused pad is invisible to SDL_Joystick too" % tag)
        check(admit_lines(result) == [],
              "%s: nothing was admitted" % tag)
        check(len(seam_stage(result, "authority")) >= 1 or
              len(seam_stage(result, "collision")) >= 1,
              "%s: the seam recorded a determinate refusal" % tag)
        check(events(consumer) == [],
              "%s: no control ever reached the consumer" % tag)
        return

    if scenario == "keyboard_only":
        # nxinput 0.10.1 discovery sweep must NOT over-admit: a node with
        # plain keyboard keys and no game-button range stays invisible. The
        # seam records nothing because SDL never saw a joystick at all.
        check(consumer.get("enumerated") == 0,
              "%s: a plain keyboard node is never enumerated" % tag)
        check(consumer.get("joysticks_visible") == 0,
              "%s: the keyboard is invisible to SDL_Joystick" % tag)
        check(admit_lines(result) == [],
              "%s: nothing was admitted" % tag)
        check(events(consumer) == [],
              "%s: no control ever reached the consumer" % tag)
        return

    if scenario == "native":
        check(len(seam_stage(result, "declaration")) >= 1,
              "%s: an unadopted port is answered no-declaration" % tag)
        check(admit_lines(result) == [],
              "%s: no-declaration admits nothing and blocks nothing" % tag)
        check(consumer.get("enumerated", 0) >= 1,
              "%s: SDL's native behaviour is preserved" % tag)
        check(len(events(consumer)) > 0,
              "%s: the native pad still delivers controls" % tag)
        return

    # The zeroed name-CRC identity is SDL's own rule on BOTH executed majors:
    # SDL3 and SDL2 >= 2.26 write the name CRC into the live GUID and fall
    # back to the zero-CRC database entry. 0.7.2 reproduces it on the SDL2
    # route too (the dArkOS GO-Super physical case), so the scenario is
    # admitted everywhere and judged by the same bindings as `matrix`.

    # ---------------------------------------------------------- admitted
    admits = admit_lines(result)
    expected_source = {
        "matrix": "env-get-controls", "null": "env-get-controls",
        "crc_alias": "env-get-controls",
        "ownerswap": "env-get-controls", "chord": "env-get-controls",
        "hotplug": "env-get-controls", "twopads": "env-get-controls",
        "guid_same_mapping": "env-get-controls",
        "guid_divergent": "env-get-controls",
        "muos_joydev": "env-get-controls",
        "muos_rom_exact": "env-get-controls",
        "muos_rom_bundle": "port-bundle",
        "priority": "env-get-controls",
        "cfw_db": "cfw-db-guid", "bundle": "port-bundle",
        "corpus1": "env-get-controls", "corpus2": "env-get-controls",
        "corpus3": "env-get-controls",
        "stickless_combined": "env-get-controls",
        "stickless_plain": "env-get-controls",
        "builtin": "runtime-builtin", "raw_declared": "raw-passthrough",
    }[scenario]

    #
    # AUTHORITY 4 IS NOT ANSWERABLE ON SDL3 AT THIS BOUNDARY, and that is a
    # property of SDL, verified in the pinned sources rather than inferred
    # from a red run:
    #
    #   SDL2  SDL_JoystickInit() calls SDL_GameControllerInitMappings()
    #         BEFORE the joystick drivers' Init(), so by the time
    #         MaybeAddDevice() runs the built-in database is loaded.
    #   SDL3  SDL_Init(SDL_INIT_GAMEPAD) fully initialises SDL_INIT_JOYSTICK
    #         first -- which is where MaybeAddDevice() and therefore the seam
    #         run -- and only afterwards calls SDL_InitGamepads(). The mapping
    #         database is still empty when the seam asks.
    #
    # So on SDL3 the order legitimately reaches step 6 and refuses the pad
    # before gameplay. That is fail-closed and correct; it is NOT something to
    # paper over, and it is NOT something to "fix" by making the seam load
    # SDL3's mappings early, which would replace the engine's native flow
    # instead of intercepting it. The consequence for a port is real and is
    # written down: on SDL3, a port must declare a source (authority 1, 2 or
    # 3); it cannot fall back on SDL's own database at this boundary.
    #
    if scenario == "builtin" and api.startswith("sdl3"):
        check(consumer.get("enumerated") == 0,
              "%s: SDL3's built-in database is not loaded yet at the announce "
              "boundary, so authority 4 yields and the pad is refused BEFORE "
              "gameplay rather than admitted unproved" % tag)
        check(any("step_builtin=not-available" in l
                  for l in seam_stage(result, "authority")),
              "%s: the receipt says exactly why -- step_builtin=not-available"
              % tag)
        check(admit_lines(result) == [],
              "%s: nothing was admitted on an unanswerable authority" % tag)
        return
    check(len(admits) >= 1, "%s: the seam admitted the pad" % tag)
    if admits:
        check(all(source_of(l) == expected_source for l in admits),
              "%s: authority that won is %s" % (tag, expected_source))
        if scenario != "raw_declared":
            check(all("readback_checked=1" in l for l in admits),
                  "%s: a live readback stands behind the decision" % tag)
        else:
            # Raw passthrough installs no mapping, so there is nothing to
            # read back. What guards it is the declaration, and only that.
            check(all("readback_checked=0" in l for l in admits),
                  "%s: raw passthrough claims no readback it does not have"
                  % tag)
        if scenario == "crc_alias":
            check(all("source_crc_aliases=1" in line for line in admits),
                  "%s: the receipt exposes exactly one CRC identity "
                  "projection on this SDL major" % tag)
        if scenario in MUOS_SCENARIOS:
            target = "sdl3-evdev" if api.startswith("sdl3") else "sdl2-evdev"
            check(all("domain_lines=1" in line and
                      "domain_bindings=15" in line and
                      "source_domain=joydev-legacy" in line and
                      ("target_domain=%s" % target) in line
                      for line in admits),
                  "%s: the receipt proves one capability-gated joydev -> %s "
                  "projection with all 15 button bindings" % (tag, target))
        if scenario in MUOS_ROM_SCENARIOS:
            check(all("source_crc_aliases=1" in line and
                      "domain_lines=1" in line and
                      "domain_bindings=15" in line for line in admits),
                  "%s: CRC alias and all 15 joydev bindings were composed in "
                  "the SAME admission" % tag)
    check(consumer.get("deliveries", 0) > 0 or scenario == "raw_declared",
          "%s: the consumer really received controls" % tag)
    check(len(result.get("consumer_receipt", [])) ==
          consumer.get("deliveries", -1),
          "%s: one consumer receipt per delivery, from the callback" % tag)

    if scenario == "priority":
        # Three sources, three different bodies. Only the winner's binding can
        # be observable.
        check(source_of(admits[0]) == "env-get-controls" if admits else False,
              "%s: authority 1 outranks the file and the bundle" % tag)

    if scenario == "raw_declared":
        return

    if scenario in ("stickless_combined", "stickless_plain"):
        # THE discovery claim (nxinput 0.10.1): a game-button node without an
        # ABS_X+ABS_Y pair is enumerated by the real SDL. Before the
        # capability sweep this exact run died earlier -- discover_guid()
        # found no device -- which is the RG40XX-H field failure reproduced.
        check(consumer.get("joysticks_visible", 0) >= 1,
              "%s: a game-button node without ABS_X/ABS_Y is enumerated"
              % tag)
        check(len(events(consumer, "button")) >= 8,
              "%s: face buttons arrive as mapped gamepad controls" % tag)
        return

    # ------------------------------------------- the per-group expectations
    if scenario.startswith("corpus"):
        check(bool(result.get("corpus_artifact_sha256")),
              "%s: the mapping came from a named artifact of the sealed C1 "
              "corpus" % tag)

    if scenario in ("matrix", "null", "ownerswap", "crc_alias", "cfw_db",
                    "bundle", "builtin", "priority") + MUOS_SCENARIOS or \
            scenario.startswith("corpus"):
        # The text of whatever source actually won. For `builtin` that is
        # SDL's own database, which the driver never wrote and must not
        # pretend to know.
        effective = result.get("effective_mapping", mapping)
        button_events = events(consumer, "button")

        if effective is not None:
            source_keys = MUOS_KEYS if scenario in MUOS_SCENARIOS else U.KEYS
            source_axes = MUOS_AXES if scenario in MUOS_SCENARIOS else U.AXES
            source_buttons = (MUOS_BUTTONS if scenario in MUOS_SCENARIOS
                              else FULL_BUTTONS)
            source_domain = ("joydev" if scenario in MUOS_SCENARIOS
                             else "evdev")
            seq = [E.group_for_physical(
                       effective, source_keys, source_axes, "button", c,
                       button_domain=source_domain)
                   for c in source_buttons]
            # Only the groups SDL reports on the BUTTON path belong in the
            # button-order claim. A trigger bound to a button still arrives as
            # an axis -- see c6_expectations.AXIS_OUTPUT_GROUPS -- and is
            # checked on the axis path below instead.
            expected = [g for g in seq
                        if g is not None and E.output_kind(g) == "button"]
            axis_via_button = [g for g in seq
                               if g is not None and E.output_kind(g) == "axis"]
            #
            # PRESS ORDER is the claim, not global interleaving.
            #
            # The physical taps are strictly serial, so the order in which
            # SDL reports the PRESSES is a real property of the mapping and
            # is checked exactly. The order in which a release is interleaved
            # with the next press is not: SDL is entitled to hold a release
            # back -- it demonstrably does so for GUIDE -- and demanding a
            # rigid press/release/press/release braid would fail the run for
            # something no mapping decides. What must hold for every group,
            # and is checked below, is that it is pressed exactly once,
            # released exactly once, and released after it was pressed.
            #
            got_press = [e["group"] for e in button_events if e["pressed"]]
            check(got_press[:len(expected)] == expected,
                  "%s: every button reached the group the mapping names, in "
                  "the order they were physically pressed" % tag)
            for group in axis_via_button:
                # Bound to a physical BUTTON, reported by SDL as an AXIS.
                vals = [e["value"] for e in events(consumer, "axis")
                        if e["group"] == group]
                check(bool(vals) and max(vals) > 16384 and 0 in vals,
                      "%s: %s is bound to a button and still arrives on the "
                      "AXIS path, pressed and released" % (tag, group))
            for group in expected:
                idx = [i for i, e in enumerate(button_events)
                       if e["group"] == group]
                downs = [i for i in idx if button_events[i]["pressed"]]
                ups = [i for i in idx if not button_events[i]["pressed"]]
                check(len(downs) == 1 and len(ups) == 1 and ups[0] > downs[0],
                      "%s: %s was pressed once and released once, in that "
                      "order" % (tag, group))
            silent = set(E.GROUPS) | {E.GUIDE}
            silent -= E.bound_groups(effective)
            seen = set(e["group"] for e in events(consumer))
            check(not (silent & seen),
                  "%s: groups the mapping leaves unbound stay silent on the "
                  "event path (%s)" % (tag, sorted(silent) or "none"))
            for group in silent:
                values = set()
                for p in polls(consumer):
                    v = p["state"].get(group)
                    values.update(v if isinstance(v, list) else [v])
                check(values <= {0},
                      "%s: unbound group %s stays silent on the POLLING path "
                      "too" % (tag, group))

        # All 18 groups exist in every transcript, bound or not.
        for p in polls(consumer)[:1]:
            check(set(p["state"]) == set(E.GROUPS) | {E.GUIDE},
                  "%s: all 18 V2 groups plus GUIDE are reported" % tag)

        if (effective is not None and scenario != "null"
                and not scenario.startswith("corpus")):
            seen = set(e["group"] for e in events(consumer))
            check(set(E.GROUPS) <= seen,
                  "%s: all 18 V2 control groups were exercised (missing %s)"
                  % (tag, sorted(set(E.GROUPS) - seen)))
            hats = set(e["group"] for e in button_events
                       if e["group"].startswith("DPAD_"))
            check(len(hats) == 4, "%s: all four hat directions" % tag)
            axis_events = events(consumer, "axis")
            for group in ("LEFT_STICK", "RIGHT_STICK"):
                vals = [e["value"] for e in axis_events
                        if e["group"] == group]
                check(vals and min(vals) <= -32000 and max(vals) >= 32000
                      and 0 in vals,
                      "%s: %s reached both extremes and returned to centre"
                      % (tag, group))
            # The muOS profile binds triggers to physical buttons. SDL still
            # reports them on the axis path, but the only valid values are
            # released/pressed. Every other full profile uses real axes and
            # must walk a multi-step analogue range.
            if scenario not in MUOS_SCENARIOS:
                for group in ("L2", "R2"):
                    vals = sorted(set(e["value"] for e in axis_events
                                      if e["group"] == group))
                    check(len(vals) >= 3 and min(vals) == 0,
                          "%s: %s walked a real analogue range %s"
                          % (tag, group, vals[:5]))

        if scenario in MUOS_ROM_SCENARIOS:
            fixture, fixture_lines = exact_muos_rom_mapping()
            check(hashlib.sha256(fixture).hexdigest() ==
                  MUOS_ROM_FIXTURE_SHA256,
                  "%s: exact ROM fixture has the pinned SHA-256" % tag)
            check(fixture_lines == [mapping],
                  "%s: mapping is the one intact Deeplay line from the ROM "
                  "fixture" % tag)
            check(result.get("muos_fixture_sha256") ==
                  MUOS_ROM_FIXTURE_SHA256,
                  "%s: driver records the same pinned ROM fixture" % tag)
            check(result.get("pad_bustype") == U.BUS_HOST and
                  result.get("pad_name") == "muOS-Keys" and
                  guid == MUOS_LIVE_GUID,
                  "%s: the kernel pad is muOS-Keys and SDL derives the exact "
                  "BUS_HOST live GUID %s" % (tag, MUOS_LIVE_GUID))
            check(mapping[:4] == guid[:4] and mapping[8:32] == guid[8:32]
                  and mapping[4:8] == "0000" and guid[4:8] != "0000",
                  "%s: the intact zero-CRC ROM identity aliases the live SDL "
                  "name-CRC identity" % tag)
            presses = [e["group"] for e in events(consumer, "button")
                       if e["pressed"]]
            check(presses[:8] ==
                  ["B", "A", "X", "Y", "L1", "R1", "SELECT", "START"],
                  "%s: physical B/A and Start reach their exact mapped groups "
                  "before gameplay" % tag)
            if scenario == "muos_rom_bundle":
                declarations = result.get("source_declarations") or {}
                check(declarations.get("env_mapping") is False and
                      declarations.get("cfw_db") is False and
                      declarations.get("port_bundle") is True,
                      "%s: authority 3 is declared with authority 1 and 2 "
                      "absent" % tag)
                check(result.get("muos_bundle_payload") ==
                      "NXCONTROLLER_PROFILES/1\n" + mapping + "\n",
                      "%s: authority-3 bundle is header plus the byte-intact "
                      "ROM line" % tag)

    if scenario == "chord":
        trace = consumer.get("trace", [])
        chord_at = [i for i, e in enumerate(trace) if e.get("kind") == "chord"]
        trig_at = [i for i, e in enumerate(trace)
                   if e.get("kind") == "event" and e.get("group") in ("L2", "R2")]
        check(len(chord_at) == 1, "%s: the chord fired exactly once" % tag)
        check(bool(trig_at) and bool(chord_at) and min(chord_at) > max(trig_at),
              "%s: L2+R2 did NOT fire the chord; only SELECT+START did" % tag)

    if scenario == "twopads":
        check(len(consumer.get("devices", [])) == 2,
              "%s: both pads were admitted and opened" % tag)
        check(len(admits) == 2, "%s: two independent admissions" % tag)
        srcs = set(l.split("guid=")[1].split()[0] for l in admits)
        check(len(srcs) == 2,
              "%s: the two pads were selected by their OWN GUID out of a "
              "heterogeneous list" % tag)
        check(consumer.get("chord_fired") == 0,
              "%s: SELECT on one pad and START on another is NOT the chord"
              % tag)
        by_pad = {}
        for e in events(consumer, "button"):
            by_pad.setdefault(e["instance"], []).append(e["group"])
        check(len(by_pad) == 2, "%s: both pads delivered controls" % tag)

    if scenario == "guid_same_mapping":
        check(len(admits) == 2,
              "%s: two instances of one GUID with the SAME mapping both "
              "admitted" % tag)

    if scenario == "guid_divergent":
        # Two divergent entries for one GUID in one list. SDL's store
        # replaces on load, so both pads resolve to the LAST entry -- the
        # same line -- and both are admitted; the receipt counts the
        # divergent duplicate the last-wins tolerance resolved. (This
        # scenario used to fail closed; the muOS 2026-08-31 regression proved
        # real CFW databases legally carry exactly this shape.)
        check(len(admits) == 2,
              "%s: both instances resolve to the last-wins mapping and are "
              "admitted" % tag)
        check(all("dup_lastwins=1" in l for l in admits),
              "%s: the tolerated divergent duplicate is counted in the "
              "receipt" % tag)

    if scenario == "hotplug":
        forgets = seam_stage(result, "forget")
        check(len(forgets) == 1, "%s: exactly one instance was forgotten" % tag)
        check(len(admits) == 2,
              "%s: the reconnected pad was resolved again, not inherited"
              % tag)
        instances = [l.split("instance=")[1].split()[0] for l in admits]
        check(instances[0] != instances[1],
              "%s: the reconnection is a NEW instance" % tag)
        after = [e for e in events(consumer, "button")
                 if str(e["instance"]) == instances[1]]
        check(bool(after),
              "%s: the reconnected pad delivers controls again" % tag)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--result", action="append", required=True)
    args = ap.parse_args()

    seen = set()
    for path in args.result:
        with open(path) as fh:
            result = json.load(fh)
        if "error" in result:
            check(False, "%s/%s: %s" % (result.get("which"),
                                        result.get("scenario"),
                                        result["error"]))
            continue
        seen.add((result["which"], result["scenario"]))
        judge(result)

    apis = set(a for a, _ in seen)
    check(len(apis) == 3,
          "three real SDL libraries answered: %s" % sorted(apis))
    for api in apis:
        check(len([s for a, s in seen if a == api]) >= 14,
              "%s ran the full scenario set" % api)

    print("\nc6_matrix_gate: %d checks, %d passed, %d failed"
          % (CHECKS, CHECKS - len(FAILS), len(FAILS)))
    if FAILS:
        for f in FAILS:
            print("  failed: %s" % f)
        print("c6_matrix_gate: FAIL")
        return 1
    print("c6_matrix_gate: PASS (REAL_API_HOST for the SDL answers; "
          "physical PENDING_PHYSICAL)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
