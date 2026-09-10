# SPDX-License-Identifier: GPL-3.0-only
"""nxinput 0.10.0 -- the muOS layout-authority gate (REAL_API_HOST).

The dedicated runner for the new boundary, registered in the official test
matrix. It exercises the PRODUCTION code -- the same seam the three pinned
SDL libraries have linked in -- against the two OFFICIAL layout databases of
the muOS 2601.1 RG40XX-H ROM, and judges by what the real consumer received
and by the seam's own receipts. Expectations are derived HERE, from the
fixtures, independently of the production code.

Cases covered (mission numbering):
  1/2   both official fixtures, hash-pinned;
  3     same uinput pad + GUID, one run per symlink target: the effective
        mapping follows the symlink both times;
  4     Deeplay-keys and muOS-Keys are data identity only (the pad is
        muOS-Keys, the database line is Deeplay-keys; admission still works
        and no rule selects by name);
  5     a live env mapping beats database and bundle;
  6     the live database beats an OPPOSITE bundle variant;
  7     auto (invariant base bundle) never resolves a modern/retro line;
  8     an explicit variant resolves ONLY when authorities 1/2 are absent;
  38    the empty ladder is a receipted fail-closed block, not a mute pad;
  41    the receipt carries the 0.10.0 evidence fields.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# The joydev rank of the RG40XX-H event node (KEY_ESC, the volume keys, then
# the 13 gamepad codes) -- derived from the driver's own pad definition.
PAD_KEYS = [0x01, 0x72, 0x73] + list(range(0x130, 0x13d))

# The two official lines, interpreted HERE: semantic -> joydev ordinal ->
# kernel code. The first four kernel taps of sequence_muos() are
# 0x130..0x133, so each layout predicts a distinct semantic order.
EXPECT_FIRST_FOUR = {
    "modern": ["B", "A", "X", "Y"],  # a:b4->0x131 b:b3->0x130 x:b5 y:b6
    "retro": ["A", "B", "Y", "X"],   # a:b3->0x130 b:b4->0x131 y:b5 x:b6
}

SCENARIO_EXPECT = {
    "muos_symlink_modern": ("admit", "modern"),
    "muos_symlink_retro": ("admit", "retro"),
    "muos_env_wins": ("admit", "retro"),      # env retro vs files modern
    "muos_base_wins": ("admit", "modern"),    # db modern vs bundle retro
    "muos_variant_modern": ("admit", "modern"),
    "muos_variant_retro": ("admit", "retro"),
    "muos_auto_empty": ("block", None),
}

RECEIPT_FIELDS_41 = (
    "seq=", "sdl=", "guid=", "source=", "step_env=", "step_cfw=",
    "step_bundle=", "step_builtin=", "step_raw=", "dup_lastwins=",
    "source_crc_aliases=", "domain_lines=", "source_domain=",
    "target_domain=", "name=", "db_class=", "db_target=", "db_retries=",
    "db_elapsed_ms=", "face_layout=", "readback_checked=",
)


def run_driver(consumer, which, scenario, work, out):
    cmd = [sys.executable, "-B", os.path.join(HERE, "c6_matrix_driver.py"),
           "--consumer", consumer, "--which", which,
           "--scenario", scenario, "--work", work, "--out", out]
    return subprocess.run(cmd).returncode


def pressed_groups(result):
    trace = result.get("consumer", {}).get("trace", [])
    return [entry.get("group") for entry in trace
            if entry.get("kind") == "event" and entry.get("path") == "button"
            and entry.get("pressed") == 1]


def judge(scenario, result, failures):
    verdict, layout = SCENARIO_EXPECT[scenario]
    consumer = result.get("consumer", {})
    receipts = result.get("seam_receipt", [])
    announce = [line for line in receipts if " stage=announce " in line]
    blocks = [line for line in receipts if " stage=authority " in line and
              "result=block" in line]

    def check(cond, label):
        if not cond:
            failures.append("%s: %s" % (scenario, label))

    if verdict == "block":
        check(consumer.get("joysticks_visible") == 0,
              "an empty ladder must never announce the pad "
              "(joysticks_visible=%r)" % consumer.get("joysticks_visible"))
        check(len(blocks) >= 1,
              "the fail-closed block must leave a receipt (case 38)")
        for line in blocks[:1]:
            check("reason=" in line and "step_bundle=" in line,
                  "the block receipt names the terminal reason per step")
        return

    check(consumer.get("joysticks_visible") == 1,
          "the pad must be announced exactly once")
    devices = consumer.get("devices", [])
    check(len(devices) == 1 and devices[0].get("is_gamepad"),
          "the pad must be classified as a gamepad")
    first_four = pressed_groups(result)[:4]
    check(first_four == EXPECT_FIRST_FOUR[layout],
          "layout %s expects %s, consumer saw %s" %
          (layout, EXPECT_FIRST_FOUR[layout], first_four))
    check(len(announce) == 1, "exactly one announce receipt")
    for line in announce:
        for field in RECEIPT_FIELDS_41:
            check(field in line,
                  "announce receipt is missing the %r field (case 41)" %
                  field)
        check("name=muOS-Keys" in line,
              "the device name enters the receipt as evidence only")
        # Case 4: the winning line's identity is Deeplay-keys DATA while the
        # live pad is muOS-Keys -- proof that the name never selected.
        check("effective_guid=1900" in line,
              "the effective mapping is the muOS GUID entry")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer", action="append", required=True,
                        metavar="WHICH=PATH")
    parser.add_argument("--work", required=True)
    parser.add_argument("--scenarios", default=",".join(SCENARIO_EXPECT))
    args = parser.parse_args()

    consumers = dict(item.split("=", 1) for item in args.consumer)
    scenarios = [s for s in args.scenarios.split(",") if s]
    unknown = [s for s in scenarios if s not in SCENARIO_EXPECT]
    if unknown:
        print("unknown scenarios: %s" % unknown)
        return 2

    failures = []
    for which, consumer in sorted(consumers.items()):
        for scenario in scenarios:
            out = os.path.join(args.work,
                               "layout-%s-%s.json" % (which, scenario))
            rc = run_driver(consumer, which, scenario, args.work, out)
            if rc != 0 or not os.path.exists(out):
                failures.append("%s/%s: driver rc=%d" % (which, scenario, rc))
                continue
            result = json.load(open(out))
            if "error" in result:
                failures.append("%s/%s: %s" % (which, scenario,
                                               result["error"]))
                continue
            before = len(failures)
            judge(scenario, result, failures)
            print("-- %s/%s: %s" % (which, scenario,
                                    "ok" if len(failures) == before
                                    else "FAIL"))

    if failures:
        print("muos_layout_gate: %d failure(s)" % len(failures))
        for failure in failures:
            print("  " + failure)
        return 1
    print("muos_layout_gate: ALL PASS (%d consumers x %d scenarios)" %
          (len(consumers), len(scenarios)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
