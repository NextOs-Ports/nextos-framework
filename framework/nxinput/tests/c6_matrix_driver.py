# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C6 -- run ONE scenario against ONE real SDL.

The driver owns the pads and the clock; it owns NO answers. It creates the
kernel devices, discovers the GUID by ASKING SDL for it, writes the C3
sources, launches the real consumer against the seam-carrying SDL and injects
a scripted sequence of real evdev events. Everything the gate later judges
comes out of the consumer's transcript and out of the seam's own receipts.

Nothing here parses a mapping to decide what SDL owes: that is
c6_expectations, which derives its expectations independently.
"""

import argparse, hashlib, json, os, shutil, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c6_uinput as U


MUOS_FIXTURE_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests",
    "fixtures", "muos-2601.1"))
MUOS_ROM_FIXTURE = os.path.join(
    MUOS_FIXTURE_DIR, "gamecontrollerdb-rg40xx-h-modern.txt")
MUOS_ROM_FIXTURE_SHA256 = (
    "bcb4c8297d3fbdff96ee68006fcd21a8576a7ef99f604f974b29fc8dc17261a8")
# 0.10.0: the OTHER official half of the same user preference. Both files
# come byte-intact from the muOS 2601.1 RG40XX-H ROM; the boot symlinks
# /usr/lib/gamecontrollerdb.txt at one of them (default: retro).
MUOS_ROM_FIXTURE_RETRO = os.path.join(
    MUOS_FIXTURE_DIR, "gamecontrollerdb-rg40xx-h-retro.txt")
MUOS_ROM_FIXTURE_RETRO_SHA256 = (
    "c7732e14f1c78ba1e0c0f24601c15886f9b213b7e9439ee32439afb791cc4016")
MUOS_ROM_GUID = "19000000010000000100000000010000"


def load_muos_fixture(path, sha256, name=b"Deeplay-keys"):
    """Load, without retargeting, one official line copied from the ROM."""
    with open(path, "rb") as fh:
        fixture = fh.read()
    digest = hashlib.sha256(fixture).hexdigest()
    if digest != sha256:
        raise RuntimeError("muOS 2601.1 controller fixture hash changed: %s" %
                           digest)
    prefix = MUOS_ROM_GUID.encode("ascii") + b"," + name + b","
    matches = [line for line in fixture.splitlines() if line.startswith(prefix)]
    if len(matches) != 1:
        raise RuntimeError("muOS fixture must contain exactly one %s line" %
                           name.decode("ascii"))
    return matches[0].decode("ascii")


def load_muos_rom_mapping():
    mapping = load_muos_fixture(MUOS_ROM_FIXTURE, MUOS_ROM_FIXTURE_SHA256)
    if len(mapping) != 315:
        raise RuntimeError("muOS Deeplay line length changed: %d" % len(mapping))
    return mapping


def run_consumer(binary, scenario, out, seconds, env, log):
    with open(log, "wb") as fh:
        return subprocess.run(
            [binary, "--out", out, "--scenario", scenario,
             "--seconds", str(seconds)],
            env=env, stdout=fh, stderr=subprocess.STDOUT, timeout=seconds + 60)


def discover_guid(binary, work, base_env, name="NXC6 Test Pad",
                  vendor=0x0912, product=0xc5a1, tag="discover",
                  pad_kwargs=None):
    """Ask the REAL SDL what GUID it assigns this pad.

    The GUID is never computed here. Reimplementing SDL's GUID composition
    would make the whole selection test circular: we would be checking SDL
    against our own idea of SDL.
    """
    # The SAME name as the scenario pads. SDL folds a CRC of the device name
    # into the GUID, so a discovery pad called anything else would hand back
    # a GUID no scenario pad ever has -- and every mapping would then miss.
    pad = U.Pad(name, vendor=vendor, product=product,
                **(pad_kwargs or {}))
    try:
        env = dict(base_env)
        env.pop("NXC6_SEAM", None)            # native SDL: pure discovery
        env.pop("SDL_GAMECONTROLLERCONFIG", None)
        out = os.path.join(work, "%s.json" % tag)
        run_consumer(binary, "discover", out, 3, env,
                     os.path.join(work, "%s.log" % tag))
        data = load(out)
        for dev in data.get("devices", []):
            if dev.get("guid"):
                return dev["guid"]
        return None
    finally:
        pad.close()


def load(path):
    """The consumer streams JSON-ish lines; close the trailing comma."""
    with open(path) as fh:
        text = fh.read()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"malformed": True, "raw": text}


FULL = ("{guid},NXC6 Test Pad,"
        "a:b0,b:b1,x:b2,y:b3,"
        "leftshoulder:b4,rightshoulder:b5,"
        "back:b8,start:b9,guide:b10,leftstick:b11,rightstick:b12,"
        "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
        "leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,"
        "righttrigger:a5,platform:Linux,")

# A/B deliberately unbound and the triggers kept: the case the mission calls
# "A/B=null, L2/R2=actions". A group with no binding must be SILENT on both
# the event path and the polling path.
NULLAB = ("{guid},NXC6 Test Pad,"
          "x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
          "back:b8,start:b9,guide:b10,leftstick:b11,rightstick:b12,"
          "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
          "leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,"
          "righttrigger:a5,platform:Linux,")

# a and b swapped against the physical order. The sovereign mapping owns the
# answer; a framework that "corrects" this is the exact defect C3 removed.
OWNERSWAP = FULL.replace("a:b0,b:b1", "a:b1,b:b0")

# A third, distinct binding, so the priority run can tell all three sources
# apart by the RESULT rather than by a label: x and y are swapped here.
BUNDLE_VARIANT = FULL.replace("x:b2,y:b3", "x:b3,y:b2")

# Literal field mapping shipped by muOS 2601.1 for RG40XX-H. Its bN values
# are joydev ordinals: KEY_ESC/volume keys precede the gamepad codes. Current
# SDL2/SDL3 use evdev ordinals (gamepad range first), so this case is the
# functional regression for the domain projection rather than another
# synthetic permutation of FULL.
MUOS_JOYDEV = ("{guid},Deeplay-keys,"
               "a:b4,b:b3,x:b5,y:b6,leftshoulder:b7,rightshoulder:b8,"
               "lefttrigger:b13,righttrigger:b14,guide:b11,start:b10,"
               "back:b9,dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,"
               "volumedown:b1,volumeup:b2,leftx:a0,lefty:a1,leftstick:b12,"
               "rightx:a2,righty:a3,rightstick:b15,platform:Linux,")
MUOS_ROM_EXACT = load_muos_rom_mapping()
MUOS_ROM_RETRO = load_muos_fixture(MUOS_ROM_FIXTURE_RETRO,
                                   MUOS_ROM_FIXTURE_RETRO_SHA256)
MUOS_ROM_SCENARIOS = ("muos_rom_exact", "muos_rom_bundle")
# 0.10.0 layout-authority scenarios (mission 7.1): the SAME uinput pad and
# GUID under both official layouts, the live authorities always outranking
# the frozen bundle, and FACE_LAYOUT selecting only which bundle VARIANT may
# serve as authority 3 when no live source resolves.
MUOS_LAYOUT_SCENARIOS = (
    "muos_symlink_modern",    # declared db file -> symlink -> modern.txt
    "muos_symlink_retro",     # same pad, symlink -> retro.txt
    "muos_env_wins",          # live env mapping beats db AND bundle
    "muos_base_wins",         # live db beats an opposite bundle variant
    "muos_variant_modern",    # FACE_LAYOUT=modern: variant is authority 3
    "muos_variant_retro",     # FACE_LAYOUT=retro: variant is authority 3
    "muos_auto_empty",        # auto + no live source + invariant base only:
                              # fail-closed, receipt, pad never announced
)
MUOS_SCENARIOS = (("muos_joydev",) + MUOS_ROM_SCENARIOS +
                  MUOS_LAYOUT_SCENARIOS)
MUOS_KEYS = [0x01, 0x72, 0x73] + list(range(0x130, 0x13d))
MUOS_AXES = [U.ABS_X, U.ABS_Y, U.ABS_RX, U.ABS_RY,
             U.ABS_HAT0X, U.ABS_HAT0Y]
MUOS_RANGES = {
    U.ABS_X: (-32768, 32767), U.ABS_Y: (-32768, 32767),
    U.ABS_RX: (-32768, 32767), U.ABS_RY: (-32768, 32767),
    U.ABS_HAT0X: (-1, 1), U.ABS_HAT0Y: (-1, 1),
}

# ---------------------------------------------------------------- discovery
# nxinput 0.10.1: the RG40XX-H field-failure CLASS. A node that carries game
# buttons but no ABS_X+ABS_Y pair was invisible to BOTH majors' non-udev
# fallback (SDL_EVDEV_GuessDeviceClass grants JOYSTICK only from that axis
# pair): SDL_GetJoysticks() stayed empty, the seam never ran, video was
# perfect and input was zero -- the exit chord included. These scenarios are
# the red of that defect: without the capability sweep in the seam patch,
# discover_guid() itself finds no device and the run fails. The plain
# keyboard node proves the sweep does not over-admit.
DISCOVERY_SCENARIOS = ("stickless_combined", "stickless_plain",
                       "keyboard_only")
STICKLESS_COMBINED_KEYS = ([0x01, 0x72, 0x73] + list(range(0x130, 0x13d)) +
                           [0x220, 0x221, 0x222, 0x223])
STICKLESS_PLAIN_KEYS = list(range(0x130, 0x13d))
KEYBOARD_ONLY_KEYS = list(range(0x01, 0x20))
# SDL numbers buttons in evdev traversal order: BTN_JOYSTICK..KEY_MAX first,
# then 0..BTN_JOYSTICK. So 0x130..0x13c are b0..b12, the BTN_DPAD block is
# b13..b16 and the keyboard keys (ESC, volume) trail after them.
STICKLESS_COMBINED_MAP = (
    "{guid},NXC6 Stickless Pad,"
    "a:b0,b:b1,x:b3,y:b4,back:b10,start:b11,guide:b12,"
    "leftshoulder:b6,rightshoulder:b7,lefttrigger:b8,righttrigger:b9,"
    "dpup:b13,dpdown:b14,dpleft:b15,dpright:b16,platform:Linux,")
STICKLESS_PLAIN_MAP = (
    "{guid},NXC6 Stickless Pad,"
    "a:b0,b:b1,x:b3,y:b4,back:b10,start:b11,"
    "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,")

MAPPINGS = {
    "matrix": FULL, "null": NULLAB, "ownerswap": OWNERSWAP,
    "crc_alias": FULL,
    "twopads": FULL, "hotplug": FULL, "chord": FULL,
    "guid_same_mapping": FULL, "guid_divergent": FULL,
    "muos_joydev": MUOS_JOYDEV,
    "muos_rom_exact": MUOS_ROM_EXACT,
    # The same exact ROM bytes are supplied through authority 3 below. Keep
    # this None so they can never leak into SDL_GAMECONTROLLERCONFIG.
    "muos_rom_bundle": None,
    # Layout-authority runs build their sources as FILES below; only
    # muos_env_wins puts a line in the environment (authority 1).
    "muos_symlink_modern": None,
    "muos_symlink_retro": None,
    "muos_env_wins": MUOS_ROM_RETRO,
    "muos_base_wins": None,
    "muos_variant_modern": None,
    "muos_variant_retro": None,
    "muos_auto_empty": None,
    "stickless_combined": STICKLESS_COMBINED_MAP,
    "stickless_plain": STICKLESS_PLAIN_MAP,
    "keyboard_only": None,
    "negative_syntax": "{guid},NXC6 Test Pad,a:bZZ,platform:Linux,",
    "negative_empty": "{guid},NXC6 Test Pad,platform:Linux,",
    "negative_other_guid": ("ffffffffffffffffffffffffffffffff,Other Pad,"
                            "a:b0,platform:Linux,"),
    # No entry this device can use, anywhere. Authority 5 (raw passthrough)
    # is then the only step left -- and it is legal ONLY because the port
    # declared its consumer understands a raw pad. The same list without the
    # declaration is `negative_other_guid`, which must fail.
    "raw_declared": ("ffffffffffffffffffffffffffffffff,Other Pad,"
                     "a:b0,platform:Linux,"),
    # Real entries out of the sealed C1 corpus, run against the real SDL.
    # This is the end-to-end half of the corpus claim: the syntactic pass
    # reads all 946 artifacts, and these runs take actual official bytes and
    # put them through a real GameController/Gamepad consumer.
    "corpus1": None, "corpus2": None, "corpus3": None,
    "native": None,
    # Authority 4. Nothing above it is present and the pad is one SDL really
    # ships an entry for, so the runtime's own database is what answers.
    "builtin": None,
    # Authorities 2 and 3, reached because the ones above them are absent.
    "cfw_db": None,
    "bundle": None,
    # All three present, all three DIFFERENT. The order is not a preference:
    # authority 1 must win, and the proof is which binding the pad ends up
    # with, not which file the framework says it read.
    "priority": None,
}


def corpus_entries(corpus_path, git_dir, want):
    """`want` official gamecontrollerdb entries that FIT the harness pad.

    Fitting matters: an entry that names b19 on a pad with 13 buttons is
    unreachable, and C3 correctly refuses it. Running one would test the
    refusal, not the mapping. So the selection keeps only entries whose every
    ordinal exists on this pad, and takes one per distinct class so the three
    runs are not the same case three times.

    Only the GUID FIELD is retargeted, to this pad's GUID. Every binding is
    used byte-intact -- retargeting the identity is what lets an entry written
    for another physical device be exercised at all; rewriting a binding would
    be inventing evidence.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import c6_expectations as X
    import c6_corpus_gate as G

    corpus = json.load(open(corpus_path))
    seen, out = set(), []
    for sha, art in sorted(corpus["artifacts"].items()):
        if art["kind"] != "sdl-gamecontrollerdb":
            continue
        content = art.get("content")
        if not art.get("content_embedded") or content is None:
            if git_dir is None:
                continue
            blob = subprocess.run(
                ["git", "--git-dir", git_dir, "cat-file", "blob",
                 art["git_blob"]], stdout=subprocess.PIPE, check=True).stdout
            if hashlib.sha256(blob).hexdigest() != art["sha256"]:
                continue
            content = blob.decode("utf-8", "replace")
        for line in content.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split(",")
            if len(fields) < 3 or len(fields[0]) != 32:
                continue
            bindings = X.parse(line)
            if not bindings:
                continue
            fits = True
            for value in bindings.values():
                plain = value.lstrip("+-").rstrip("~")
                if plain[:1] == "b" and plain[1:].isdigit():
                    fits = fits and int(plain[1:]) < 13
                elif plain[:1] == "a" and plain[1:].isdigit():
                    fits = fits and int(plain[1:]) < 6
                elif plain[:1] == "h" and "." in plain:
                    fits = fits and int(plain[1:].split(".")[0]) < 1
                else:
                    fits = False
            if not fits:
                continue
            groups, kinds, _, _ = G.classify_db(line)
            key = (",".join(sorted(groups)), ",".join(sorted(kinds)))
            if key in seen:
                continue
            seen.add(key)
            out.append((sha, line))
            if len(out) == want:
                return out
    return out


def sequence_full(pad):
    """Every one of the 18 groups, press AND release, then the analogues."""
    for code in (U.BTN_A, U.BTN_B, U.BTN_X, U.BTN_Y, U.BTN_TL, U.BTN_TR,
                 U.BTN_SELECT, U.BTN_START, U.BTN_MODE, U.BTN_THUMBL,
                 U.BTN_THUMBR):
        pad.tap(code)
    # hats: every direction, each returned to centre
    for code, value in ((U.ABS_HAT0Y, -1), (U.ABS_HAT0Y, 1),
                        (U.ABS_HAT0X, -1), (U.ABS_HAT0X, 1)):
        pad.abs(code, value); time.sleep(0.12)
        pad.abs(code, 0); time.sleep(0.08)
    # sticks: both extremes and back to centre, so the centre is observed and
    # not merely assumed
    for code in (U.ABS_X, U.ABS_Y, U.ABS_RX, U.ABS_RY):
        pad.abs(code, -32768); time.sleep(0.10)
        pad.abs(code, 32767); time.sleep(0.10)
        pad.abs(code, 0); time.sleep(0.08)
    # triggers: a real half range, 0..255, walked in steps
    for code in (U.ABS_Z, U.ABS_RZ):
        for v in (0, 64, 128, 255, 0):
            pad.abs(code, v); time.sleep(0.08)


def sequence_stickless(pad):
    """Only what the stickless shapes physically have: face buttons."""
    for code in (U.BTN_A, U.BTN_B, U.BTN_X, U.BTN_Y):
        pad.tap(code)


def sequence_muos(pad):
    """Every physical RG40XX-H button in DTS order, then hats/sticks."""
    for code in range(0x130, 0x13d):
        pad.tap(code)
    for code, value in ((U.ABS_HAT0Y, -1), (U.ABS_HAT0Y, 1),
                        (U.ABS_HAT0X, -1), (U.ABS_HAT0X, 1)):
        pad.abs(code, value); time.sleep(0.12)
        pad.abs(code, 0); time.sleep(0.08)
    for code in (U.ABS_X, U.ABS_Y, U.ABS_RX, U.ABS_RY):
        pad.abs(code, -32768); time.sleep(0.10)
        pad.abs(code, 32767); time.sleep(0.10)
        pad.abs(code, 0); time.sleep(0.08)


def sequence_chord(pad):
    """SELECT+START must fire. L2+R2 must not."""
    pad.key(U.BTN_TL2, True); pad.abs(U.ABS_Z, 255)
    pad.key(U.BTN_TR2, True); pad.abs(U.ABS_RZ, 255)
    time.sleep(0.5)
    pad.key(U.BTN_TL2, False); pad.abs(U.ABS_Z, 0)
    pad.key(U.BTN_TR2, False); pad.abs(U.ABS_RZ, 0)
    time.sleep(0.4)
    pad.key(U.BTN_SELECT, True); time.sleep(0.15)
    pad.key(U.BTN_START, True); time.sleep(0.5)
    pad.key(U.BTN_START, False); pad.key(U.BTN_SELECT, False)
    time.sleep(0.3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--consumer", required=True)
    ap.add_argument("--which", required=True)
    ap.add_argument("--scenario", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=14)
    ap.add_argument("--corpus", default=None)
    ap.add_argument("--corpus-git-dir", default=None)
    args = ap.parse_args()

    work = os.path.join(args.work, "%s-%s" % (args.which, args.scenario))
    os.makedirs(work, exist_ok=True)
    seam_receipt = os.path.join(work, "seam-receipt.txt")
    consumer_receipt = os.path.join(work, "consumer-receipt.txt")
    # Both receipt sinks APPEND, on purpose: inside one run nothing may
    # overwrite an earlier line. That makes a leftover file from a previous
    # attempt indistinguishable from this attempt's own evidence, so the
    # scenario starts by removing them. A re-run then produces its own
    # receipts and only its own.
    for stale in (seam_receipt, consumer_receipt):
        if os.path.exists(stale):
            os.unlink(stale)

    base = dict(os.environ)
    for key in ("SDL_GAMECONTROLLERCONFIG", "SDL_GAMECONTROLLERCONFIG_FILE",
                "NXCONTROLLER_PROFILES", "NXC6_STAGED_MAPPING", "NXC6_SEAM"):
        base.pop(key, None)
    base["SDL_VIDEODRIVER"] = "dummy"
    base["SDL_AUDIODRIVER"] = "dummy"

    # The `builtin` case needs a pad SDL's own database knows. Everything
    # else uses the neutral test pad, which SDL has no entry for -- that is
    # precisely why those runs must reach a declared source or fail.
    pad_kwargs = {}
    if args.scenario == "builtin":
        PAD_NAME, PAD_VENDOR, PAD_PRODUCT = (
            "Microsoft X-Box 360 pad", 0x045e, 0x028e)
    elif args.scenario in MUOS_SCENARIOS:
        PAD_NAME = ("muOS-Keys"
                    if args.scenario in MUOS_ROM_SCENARIOS +
                    MUOS_LAYOUT_SCENARIOS
                    else "Deeplay-keys")
        PAD_VENDOR, PAD_PRODUCT = 0x0001, 0x0001
        pad_kwargs = {"keys": MUOS_KEYS, "axes": MUOS_AXES,
                      "ranges": MUOS_RANGES}
        if args.scenario in MUOS_ROM_SCENARIOS + MUOS_LAYOUT_SCENARIOS:
            # Exact input_id encoded by the ROM's 1900...00010000 GUID.
            # The kernel pad is muOS-Keys while control.txt selects the
            # Deeplay-keys database line. SDL supplies the non-zero muOS-Keys
            # name CRC at runtime; the database line retains its zero CRC.
            pad_kwargs.update({"bustype": U.BUS_HOST, "version": 0x0100})
    elif args.scenario in DISCOVERY_SCENARIOS:
        PAD_NAME, PAD_VENDOR, PAD_PRODUCT = "NXC6 Stickless Pad", 0x0912, 0xc5b7
        if args.scenario == "stickless_combined":
            pad_kwargs = {"keys": STICKLESS_COMBINED_KEYS, "axes": [],
                          "ranges": {}}
        elif args.scenario == "stickless_plain":
            pad_kwargs = {"keys": STICKLESS_PLAIN_KEYS,
                          "axes": [U.ABS_HAT0X, U.ABS_HAT0Y],
                          "ranges": {U.ABS_HAT0X: (-1, 1),
                                     U.ABS_HAT0Y: (-1, 1)}}
        else:
            PAD_NAME = "NXC6 Keyboard"
            pad_kwargs = {"keys": KEYBOARD_ONLY_KEYS, "axes": [],
                          "ranges": {}}
    else:
        PAD_NAME, PAD_VENDOR, PAD_PRODUCT = "NXC6 Test Pad", 0x0912, 0xc5a1
    globals()["PAD"] = (PAD_NAME, PAD_VENDOR, PAD_PRODUCT)
    if args.scenario == "keyboard_only":
        # This node must never be discovered; there is no GUID to learn.
        guid = "00000000000000000000000000000000"
    else:
        guid = discover_guid(args.consumer, work, base, PAD_NAME, PAD_VENDOR,
                             PAD_PRODUCT, pad_kwargs=pad_kwargs)
    if guid is None:
        json.dump({"scenario": args.scenario, "which": args.which,
                   "error": "sdl assigned no guid to the discovery pad"},
                  open(args.out, "w"), indent=1)
        return 1

    guid2 = None
    if args.scenario == "twopads":
        guid2 = discover_guid(args.consumer, work, base, "NXC6 Second Pad",
                              0x0913, 0xc5a2, "discover2")
        if guid2 is None:
            json.dump({"scenario": args.scenario, "which": args.which,
                       "error": "sdl assigned no guid to the second pad"},
                      open(args.out, "w"), indent=1)
            return 1

    env = dict(base)
    env["NXC6_SEAM"] = "1"
    env["NXC6_RECEIPT"] = seam_receipt
    env["NXC6_CONSUMER_RECEIPT"] = consumer_receipt
    # Authority 2 (official CFW database) and authority 3 (the bundle pinned
    # in the port) are files, not variables. They are written here as the CFW
    # and the ZIP would ship them.
    muos_bundle_payload = None
    if args.scenario == "builtin":
        # Nothing declared above authority 4 -- but the port IS adopting, so
        # the seam must not read this as "no declaration". An empty bundle
        # path is a declaration that resolves to nothing.
        env["NXCONTROLLER_PROFILES"] = os.path.join(work, "absent.nxb")
    if args.scenario == "muos_rom_bundle":
        # Exactly what a port ZIP retains: the authority-3 header followed by
        # the byte-intact, zero-CRC line from the official ROM fixture. There
        # is deliberately no authority-1 mapping and no authority-2 CFW file.
        bundle_path = os.path.join(work, "controllers.nxb")
        muos_bundle_payload = ("NXCONTROLLER_PROFILES/1\n" +
                               MUOS_ROM_EXACT + "\n")
        with open(bundle_path, "wb") as fh:
            fh.write(muos_bundle_payload.encode("ascii"))
        env["NXCONTROLLER_PROFILES"] = bundle_path
    if args.scenario in MUOS_LAYOUT_SCENARIOS:
        # The muOS mechanism, faithfully: two official database FILES (both
        # halves of the user preference, byte-intact from the ROM) and ONE
        # symlink the boot points at one of them. The bundles are the three
        # 0.10.0 port files: the invariant base (never a mutable modern/retro
        # line) and the two authenticated variants.
        modern_path = os.path.join(work, "modern.txt")
        retro_path = os.path.join(work, "retro.txt")
        shutil.copyfile(MUOS_ROM_FIXTURE, modern_path)
        shutil.copyfile(MUOS_ROM_FIXTURE_RETRO, retro_path)
        link_path = os.path.join(work, "gamecontrollerdb.txt")
        base_bundle = os.path.join(work, "controllers.nxb")
        modern_bundle = os.path.join(work, "controllers-modern.nxb")
        retro_bundle = os.path.join(work, "controllers-retro.nxb")
        with open(base_bundle, "w") as fh:
            # Invariant identities only. The muOS GUID is deliberately ABSENT:
            # freezing either face layout here is the 0.9.0 defect.
            fh.write("NXCONTROLLER_PROFILES/1\n")
            fh.write("190000004b4800000011000000010000,GO-Super Gamepad,"
                     "a:b1,b:b0,back:b12,dpdown:b9,dpleft:b10,dpright:b11,"
                     "dpup:b8,guide:b16,leftshoulder:b4,leftstick:b14,"
                     "lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,"
                     "rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,"
                     "start:b13,x:b2,y:b3,platform:Linux,\n")
        with open(modern_bundle, "w") as fh:
            fh.write("NXCONTROLLER_PROFILES/1\n" + MUOS_ROM_EXACT + "\n")
        with open(retro_bundle, "w") as fh:
            fh.write("NXCONTROLLER_PROFILES/1\n" + MUOS_ROM_RETRO + "\n")
        if args.scenario == "muos_symlink_modern":
            os.symlink("modern.txt", link_path)
            env["SDL_GAMECONTROLLERCONFIG_FILE"] = link_path
        elif args.scenario == "muos_symlink_retro":
            os.symlink("retro.txt", link_path)
            env["SDL_GAMECONTROLLERCONFIG_FILE"] = link_path
        elif args.scenario == "muos_env_wins":
            # Authority 1 says retro while BOTH files below say modern.
            os.symlink("modern.txt", link_path)
            env["SDL_GAMECONTROLLERCONFIG_FILE"] = link_path
            env["NXCONTROLLER_PROFILES"] = modern_bundle
        elif args.scenario == "muos_base_wins":
            # The live database says modern; the frozen variant says retro.
            # The live authority must win even against an explicit variant.
            os.symlink("modern.txt", link_path)
            env["SDL_GAMECONTROLLERCONFIG_FILE"] = link_path
            env["NXCONTROLLER_PROFILES"] = retro_bundle
        elif args.scenario == "muos_variant_modern":
            env["NXCONTROLLER_PROFILES"] = modern_bundle
        elif args.scenario == "muos_variant_retro":
            env["NXCONTROLLER_PROFILES"] = retro_bundle
        elif args.scenario == "muos_auto_empty":
            # A DEAD declared symlink (the boot has not recreated it) plus
            # only the invariant base bundle: the ladder is empty and the
            # pad must fail closed with a receipt, never gameplay-mute.
            os.symlink("absent-target.txt", link_path)
            env["SDL_GAMECONTROLLERCONFIG_FILE"] = link_path
            env["NXCONTROLLER_PROFILES"] = base_bundle
    if args.scenario in ("cfw_db", "bundle", "priority"):
        db_path = os.path.join(work, "gamecontrollerdb.txt")
        with open(db_path, "w") as fh:
            fh.write("# NXC6 CFW database\n")
            fh.write("030000000000000000000000000000ab,Decoy One,a:b3,"
                     "platform:Linux,\n")
            fh.write((OWNERSWAP if args.scenario == "priority"
                      else FULL).format(guid=guid) + "\n")
        bundle_path = os.path.join(work, "controllers.nxb")
        with open(bundle_path, "w") as fh:
            fh.write("NXCONTROLLER_PROFILES/1\n")
            fh.write("# pinned inside the port ZIP\n")
            fh.write((BUNDLE_VARIANT if args.scenario == "priority"
                      else FULL).format(guid=guid) + "\n")
        if args.scenario == "cfw_db":
            env["SDL_GAMECONTROLLERCONFIG_FILE"] = db_path
        elif args.scenario == "bundle":
            env["NXCONTROLLER_PROFILES"] = bundle_path
        else:
            env["SDL_GAMECONTROLLERCONFIG_FILE"] = db_path
            env["NXCONTROLLER_PROFILES"] = bundle_path

    if args.scenario == "raw_declared":
        # Declared, never inferred: the seam reads this and nothing else.
        env["NXINPUT_RAW_CONSUMER_DECLARED"] = "1"
    corpus_source = None
    if args.scenario.startswith("corpus"):
        index = int(args.scenario[len("corpus"):]) - 1
        picked = corpus_entries(args.corpus, args.corpus_git_dir, index + 1)
        if len(picked) <= index:
            json.dump({"scenario": args.scenario, "which": args.which,
                       "error": "the sealed corpus has no %dth entry that "
                                "fits this pad" % (index + 1)},
                      open(args.out, "w"), indent=1)
            return 1
        corpus_source, entry = picked[index]
        fields = entry.split(",")
        fields[0] = guid          # retarget the IDENTITY only
        env["SDL_GAMECONTROLLERCONFIG"] = ",".join(fields)

    template = MAPPINGS[args.scenario]
    mapping = template.format(guid=guid) if template else None
    if args.scenario == "muos_rom_bundle":
        # Record the actual source line for independent gate comparison, but
        # do not export it: authority 3 must be the only declared source.
        mapping = MUOS_ROM_EXACT
    if args.scenario == "crc_alias":
        # SDL3 adds a CRC16 of the device name to bytes 2-3 of the live GUID,
        # while PortMaster's SDL2-format database leaves that word zero. Do
        # not compute the CRC here: SDL supplied the live GUID above. Change
        # only that word, exactly as a real PortMaster database does.
        database_guid = guid[:4] + "0000" + guid[8:]
        mapping = FULL.format(guid=database_guid)
    if args.scenario.startswith("corpus"):
        mapping = env["SDL_GAMECONTROLLERCONFIG"]
    if args.scenario == "priority":
        # Authority 1, distinct from both files below it.
        mapping = FULL.format(guid=guid)
    if args.scenario == "twopads":
        # Deliberately heterogeneous, and deliberately NOT in arrival order:
        # decoys first, then the second pad, then the first. If selection
        # took the first line, or the first matching prefix, both pads would
        # get the wrong entry and the run would still "work" -- which is why
        # the gate checks the resulting bindings, not just admission.
        mapping = "\n".join([
            "# NXC6 heterogeneous list: several devices, one file",
            "030000000000000000000000000000ab,Decoy One,a:b3,b:b2,"
            "platform:Linux,",
            "",
            "030000000000000000000000000000cd,Decoy Two,a:b5,start:b1,"
            "platform:Linux,",
            OWNERSWAP.format(guid=guid2),
            FULL.format(guid=guid),
        ])
    if mapping is not None and args.scenario != "muos_rom_bundle":
        env["SDL_GAMECONTROLLERCONFIG"] = mapping

    pads = []
    # `effective_mapping` is the text of the source the C3 order is expected
    # to pick, so the gate can predict what SDL owes. It is None for
    # `builtin`, where the winner is SDL's own database and the driver has no
    # business claiming to know it.
    effective = mapping
    if args.scenario == "cfw_db":
        effective = FULL.format(guid=guid)
    elif args.scenario == "bundle":
        effective = FULL.format(guid=guid)
    elif args.scenario in ("builtin", "raw_declared", "native", "twopads"):
        effective = None
    elif args.scenario in MUOS_LAYOUT_SCENARIOS:
        # The layout gate (muos_layout_gate.py) derives its own expectations
        # from the fixtures; the driver claims nothing here.
        effective = None
    elif args.scenario == "guid_divergent":
        # The env carries FULL then OWNERSWAP for the same GUID; SDL's
        # replace-on-load store makes the LAST one effective.
        effective = OWNERSWAP.format(guid=guid)
    elif args.scenario.startswith("corpus"):
        effective = mapping
    result = {"scenario": args.scenario, "which": args.which, "guid": guid,
              "guid2": guid2, "mapping": mapping,
              "pad_name": PAD_NAME,
              "effective_mapping": effective,
              "muos_bundle_payload": muos_bundle_payload,
              "source_declarations": {
                  "env_mapping": "SDL_GAMECONTROLLERCONFIG" in env,
                  "cfw_db": "SDL_GAMECONTROLLERCONFIG_FILE" in env,
                  "port_bundle": "NXCONTROLLER_PROFILES" in env,
              },
              "corpus_artifact_sha256": corpus_source,
              "muos_fixture_sha256": (MUOS_ROM_FIXTURE_SHA256
                                       if args.scenario in MUOS_ROM_SCENARIOS
                                       else None),
              "pad_bustype": (U.BUS_HOST
                               if args.scenario in MUOS_ROM_SCENARIOS
                               else U.BUS_USB)}
    try:
        if args.scenario in ("twopads", "guid_same_mapping",
                             "guid_divergent"):
            # twopads: two DIFFERENT devices. guid_*: two IDENTICAL ones, so
            # the kernel gives them the same GUID -- the case SDL's
            # GUID-keyed mapping store cannot separate.
            pads.append(U.Pad(PAD_NAME, vendor=PAD_VENDOR,
                              product=PAD_PRODUCT, **pad_kwargs))
            if args.scenario == "twopads":
                pads.append(U.Pad("NXC6 Second Pad", vendor=0x0913,
                                  product=0xc5a2))
            else:
                pads.append(U.Pad(PAD_NAME, vendor=PAD_VENDOR,
                                  product=PAD_PRODUCT))
            if args.scenario == "guid_divergent":
                # Same GUID, two divergent entries in one list: SDL's store
                # replaces on load, so the LAST entry is what the runtime
                # executes. Both pads must resolve to it, identically.
                env["SDL_GAMECONTROLLERCONFIG"] = (
                    mapping + "\n" + OWNERSWAP.format(guid=guid))
        elif args.scenario != "hotplug":
            pads.append(U.Pad(PAD_NAME, vendor=PAD_VENDOR,
                              product=PAD_PRODUCT, **pad_kwargs))

        runner = threading.Thread(
            target=run_consumer,
            args=(args.consumer, args.scenario,
                  os.path.join(work, "consumer.json"), args.seconds, env,
                  os.path.join(work, "consumer.log")))
        runner.start()
        time.sleep(2.2)   # let SDL settle before anything is injected

        if args.scenario == "hotplug":
            first = U.Pad(PAD_NAME, vendor=PAD_VENDOR,
                          product=PAD_PRODUCT); pads.append(first)
            time.sleep(1.6)
            first.tap(U.BTN_A)
            time.sleep(0.6)
            first.close(); pads.remove(first)
            time.sleep(1.4)
            again = U.Pad(PAD_NAME, vendor=PAD_VENDOR,
                          product=PAD_PRODUCT); pads.append(again)
            time.sleep(1.6)
            again.tap(U.BTN_B)
        elif args.scenario == "chord":
            sequence_chord(pads[0])
        elif args.scenario == "twopads":
            # SELECT on one pad and START on the other must NOT be a chord.
            pads[0].key(U.BTN_SELECT, True)
            pads[1].key(U.BTN_START, True)
            time.sleep(0.6)
            pads[1].key(U.BTN_START, False)
            pads[0].key(U.BTN_SELECT, False)
            time.sleep(0.3)
            pads[0].tap(U.BTN_A)
            pads[1].tap(U.BTN_B)
        else:
            for pad in pads:
                if args.scenario in DISCOVERY_SCENARIOS:
                    sequence_stickless(pad)
                elif args.scenario in MUOS_SCENARIOS:
                    sequence_muos(pad)
                else:
                    sequence_full(pad)

        runner.join(timeout=args.seconds + 60)
        result["consumer"] = load(os.path.join(work, "consumer.json"))
    finally:
        for pad in pads:
            pad.close()

    for name, path in (("seam_receipt", seam_receipt),
                       ("consumer_receipt", consumer_receipt)):
        result[name] = (open(path).read().splitlines()
                        if os.path.exists(path) else [])
    result["work"] = work
    with open(args.out, "w") as fh:
        json.dump(result, fh, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
