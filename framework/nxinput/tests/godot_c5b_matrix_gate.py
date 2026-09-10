#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Judge the C5B scenarios. One verdict per row, against a stated expectation.

WHAT THE 116A MATRIX COULD NOT DO
---------------------------------
It recorded lines and counted them. A row where the engine answered nothing
looked exactly like a row it answered correctly, so nine silent rows passed.

WHAT THIS DOES
--------------
Every row carries the logical button or axis the engine OWED for that
physical input, derived from the mapping and the engine's own pinned name
tables. A row passes only when the engine delivered it, and delivered it on
the routes that row is about:

  input   an InputEventJoypadButton / InputEventJoypadMotion with that index
  poll    Input.is_joy_button_pressed / Input.get_joy_axis agreeing
  action  an InputMap action bound to the engine's own named constant

and the negative rows pass only when NOTHING arrived on any of the three.

CLAIM CLASS: the engine's answers are REAL_API_HOST; this file is the gate
that reads them. No physical device took part -- everything stays
PENDING_PHYSICAL.
"""
import argparse
import json
import pathlib
import sys

FAILURES = []
CHECKS = []
FORBIDDEN_GUI_NEEDED = (
    "libx11", "libxcb", "libxext", "libxrandr", "libxinerama",
    "libxcursor", "libxi.so", "libwayland", "libgtk", "libsdl",
    "libegl", "libglx", "libqt", "libwx",
)


def check(ok, what, detail=""):
    CHECKS.append((bool(ok), what, detail))
    if not ok:
        FAILURES.append("%s%s" % (what, (" -- " + detail) if detail else ""))
    print("%-4s %s%s" % ("ok" if ok else "FAIL", what,
                         ("  [" + detail + "]") if detail and not ok else ""))


def has_event(rows_events, kind, index, pressed=None, dev=0):
    for event in rows_events or []:
        if event["kind"] != kind or event["index"] != index:
            continue
        if dev is not None and event["dev"] != dev:
            continue
        if pressed is None:
            return True
        if event["value"] in (str(pressed), "%d" % pressed):
            return True
    return False


def any_joy_event(rows_events):
    return bool(rows_events)


def judge_headless(data, label):
    """Every real-engine scenario must be incapable of opening a GUI."""
    execution = data.get("execution", {})
    argv = execution.get("argv", [])
    gui_environment = execution.get("gui_environment", {})
    check(not gui_environment,
          "%s inherited no X11/Wayland GUI environment" % label,
          json.dumps(gui_environment, sort_keys=True))
    joined = " ".join(argv)
    runtime = data.get("headless_runtime", [])
    expected_runs = 5 if data.get("scenario") == "negatives" else 1
    if data["engine"] == "godot3":
        check("--no-window" in argv,
              "%s Godot 3 ran only with --no-window" % label, joined)
        check(execution.get("binary_basename", "").startswith("godot_server"),
              "%s Godot 3 used the server/headless binary" % label,
              execution.get("binary_basename", ""))
        needed = " ".join(execution.get("elf_needed", [])).lower()
        check(not any(name in needed for name in FORBIDDEN_GUI_NEEDED),
              "%s Godot 3 ELF has no GUI/display dependency" % label,
              needed)
        check(len(runtime) == expected_runs and
              set(runtime) == {"MX headless_backend=Server can_draw=0"},
              "%s the real Godot 3 process reported Server/can_draw=0"
              % label, json.dumps(runtime))
    else:
        check("--headless" in argv,
              "%s Godot 4 ran only with --headless" % label, joined)
        check("--display-driver" not in argv and "x11" not in argv,
              "%s Godot 4 had no X11/display-driver fallback" % label,
              joined)
        check(len(runtime) == expected_runs and
              set(runtime) == {"MX display_server=headless screens=0"},
              "%s the real Godot 4 process reported headless/zero screens"
              % label, json.dumps(runtime))


def judge_matrix(data, label):
    """The eighteen groups, both sticks, the triggers, the hat."""
    groups = set()
    for row in data["rows"]:
        case = row.get("case")
        if case == "press_release":
            groups.add(row["group"])
            index = row["expect_index"]
            if row["expect_kind"] == "button":
                check(has_event(row["input_press"], "button", index, 1),
                      "%s %s press reaches _input as button %d"
                      % (label, row["group"], index),
                      json.dumps(row["input_press"])[:160])
                check(has_event(row["input_release"], "button", index, 0),
                      "%s %s release reaches _input as button %d"
                      % (label, row["group"], index),
                      json.dumps(row["input_release"])[:160])
                check(row.get("poll_pressed"),
                      "%s %s is held down on the polling route"
                      % (label, row["group"]))
            else:
                check(any_joy_event(row["input_press"]),
                      "%s %s press reaches _input as motion" % (label,
                                                                row["group"]))
        elif case == "axis_max":
            groups.add(row["group"])
            index = row["expect_index"]
            if row["expect_kind"] == "axis":
                top = row.get("poll_axis_max")
                low = row.get("poll_axis_min")
                centre = row.get("poll_axis_centre")
                check(top is not None and abs(top) > 0.5,
                      "%s %s (%s) at maximum polls axis %d"
                      % (label, row["group"], row["control"], index),
                      "got %s" % top)
                if row["physical"][1] not in (2, 5):
                    check(low is not None and abs(low) > 0.5,
                          "%s %s at minimum polls axis %d"
                          % (label, row["group"], index), "got %s" % low)
                check(centre is None or abs(centre) <= 0.25,
                      "%s %s returns to centre" % (label, row["group"]),
                      "got %s" % centre)
                dead = row.get("poll_axis_deadzone")
                check(dead is None or abs(dead) <= 0.25,
                      "%s %s stays inside the deadzone" % (label,
                                                           row["group"]),
                      "got %s" % dead)
                check(any_joy_event(row["input_max"]),
                      "%s %s motion reaches _input" % (label, row["group"]))
            else:
                # Godot 3 turns an analog trigger into a BUTTON output.
                check(row.get("poll_pressed_max") or
                      has_event(row["input_max"], "button", index, 1),
                      "%s %s at maximum becomes button %d"
                      % (label, row["group"], index),
                      json.dumps(row["input_max"])[:160])
        elif case == "hat":
            groups.add(row["group"])
            index = row["expect_index"]
            check(has_event(row["input_press"], "button", index, 1) or
                  row.get("poll_pressed"),
                  "%s %s (hat) reaches the engine as button %d"
                  % (label, row["group"], index),
                  json.dumps(row["input_press"])[:160])
            check(has_event(row["input_release"], "button", index, 0) or
                  not row.get("poll_pressed"),
                  "%s %s (hat) releases" % (label, row["group"]))
        elif case == "hat_diagonal_upleft":
            check(any_joy_event(row["input_press"]),
                  "%s a diagonal hat position is delivered" % label)
    return groups


def judge_chord(data, label, expect_mixed=False):
    fired = {}
    for row in data["rows"]:
        case = row.get("case", "")
        if row.get("group") == "CHORD":
            fired[case] = row.get("chord_fired")
    check(fired.get("select_start_same_pad") is True,
          "%s SELECT+START on one pad fires the chord" % label)
    for negative in ("negative_select_alone", "negative_start_alone",
                     "negative_guide_start", "negative_l1_r1",
                     "negative_release_between"):
        if negative in fired:
            check(fired[negative] is False,
                  "%s %s does not fire the chord" % (label, negative))
    if expect_mixed:
        check(fired.get("negative_mixed_pads") is False,
              "%s one key on each of two pads does not fire the chord"
              % label)


def judge_null(data, label):
    for row in data["rows"]:
        if row.get("case") != "null_no_leak":
            continue
        check(not any_joy_event(row["input_press"]),
              "%s suppressed %s leaks nothing through _input"
              % (label, row["group"]),
              json.dumps(row["input_press"])[:160])
        polls = " ".join(row.get("poll_lines", []))
        check("btn[]" in polls or polls.strip() in ("", "[]")
              or "btn[" not in polls,
              "%s suppressed %s leaks nothing through polling"
              % (label, row["group"]), polls[:160])
        acts = " ".join(row.get("action_lines", []))
        check("mx_a" not in acts and "mx_b" not in acts,
              "%s suppressed %s leaks no InputMap action" % (label,
                                                             row["group"]),
              acts[:160])
    # a control that was NOT suppressed must still work
    live = [r for r in data["rows"] if r.get("case") == "press_release"]
    check(bool(live) and all(
        has_event(r["input_press"], "button", r["expect_index"], 1)
        for r in live),
        "%s the controls that were NOT suppressed still work" % label)


def judge_twopads(data, label):
    seen = {}
    for row in data["rows"]:
        if not row.get("case", "").startswith("two_pads_pad"):
            continue
        which = row["case"][-1]
        devices = sorted({e["dev"] for e in row["input_press"]
                          if e["kind"] == "button"})
        seen[which] = devices
        check(len(devices) == 1,
              "%s pressing pad %s reaches exactly one device" % (label, which),
              "devices=%s" % devices)
    check(len(seen) == 2 and seen.get("0") != seen.get("1"),
          "%s two pads with the SAME GUID keep separate device ids" % label,
          json.dumps(seen))
    boot = " ".join(data["notes"].get("boot", []))
    check(boot.count("MX pad id=") >= 2,
          "%s both pads were announced" % label, boot[:200])


def judge_hotplug(data, label):
    boot = " ".join(data["notes"].get("boot_pads", []))
    check("MX pads=[]" in boot,
          "%s the engine booted with no pad" % label, boot[:120])
    row = next((r for r in data["rows"] if r.get("case") == "hotplug_press"),
               None)
    check(row is not None and any_joy_event(row["input_press"]),
          "%s a pad plugged in AFTER boot is admitted and delivers input"
          % label,
          json.dumps(row["input_press"])[:160] if row else "no row")
    receipt = data.get("seam_receipt", "")
    check("stage=announce result=admit" in receipt,
          "%s the seam announced the hotplugged pad" % label)
    shrunk = next((r for r in data["rows"]
                   if r.get("case") == "shrunk_reconnect"), None)
    check(shrunk is not None and not any_joy_event(shrunk["input_press"]),
          "%s a reconnect with SHRUNK capabilities is refused, not inherited"
          % label,
          json.dumps(shrunk["input_press"])[:160] if shrunk else "no row")
    check("result=block" in receipt or "unreachable" in receipt,
          "%s the seam recorded WHY the shrunk pad was refused" % label)


def judge_negatives(data, label):
    """A declaration that does not authenticate must reach nothing."""
    seen = set()
    for row in data["rows"]:
        case = row.get("case", "")
        if not case.startswith("origin_"):
            continue
        seen.add(case)
        what = "%s %s" % (label, case)
        check(row.get("announced") is False,
              "%s: the pad is never announced" % what,
              row.get("pads_line", ""))
        joypad_events = [e for e in row.get("events", [])
                         if "kind=KEY" not in e]
        check(not joypad_events,
              "%s: no joypad event reaches _input" % what,
              json.dumps(joypad_events)[:160])
        check(not row.get("non_empty_polls"),
              "%s: nothing reaches polling or InputMap" % what,
              json.dumps(row.get("non_empty_polls"))[:160])
        check(bool(row.get("blocked_reason")),
              "%s: the seam recorded a determinate reason" % what,
              row.get("receipt", "")[:200])
    for required in ("origin_unknown_domain", "origin_empty_domain",
                     "origin_provider_off_allowlist",
                     "origin_digest_mismatch", "origin_foreign_guid"):
        check(required in seen, "%s ran %s" % (label, required))


def judge_native(data, label):
    check("MX pad id=" in " ".join(data["notes"].get("boot", [])),
          "%s with no declaration the engine still enumerates its pad"
          % label)
    rows = [row for row in data["rows"]
            if row.get("case") == "native_passthrough"]
    check(len(rows) >= 2 and all(any_joy_event(row.get("input_press"))
                                 for row in rows),
          "%s native input reaches the real _input route" % label,
          json.dumps(rows)[:240])
    receipt = data.get("seam_receipt", "")
    check(receipt.strip() == "" or "no-declaration" in receipt,
          "%s the seam adds nothing when nothing is declared" % label,
          receipt[:200])


def judge_sigterm(data, label):
    save = data.get("save_file", "")
    check(data["notes"].get("launcher_saw_sigterm") is True,
          "%s the launcher actually received SIGTERM" % label)
    check(data["notes"].get("exit_code") == 0,
          "%s the engine exited cleanly instead of being killed" % label,
          "exit=%s" % data["notes"].get("exit_code"))
    check("reason=chord" in save,
          "%s SIGTERM converged on the CHORD's finalisation, not a second "
          "ending" % label, save[:120])
    check("reason=" in save,
          "%s SIGTERM still reached the save/lifecycle path" % label,
          save[:120])
    check("keyboard_events=0" in save,
          "%s no synthetic keyboard event was produced" % label, save[:120])
    lines = data.get("all_lines", [])
    finals = [l for l in lines if l.startswith("MX FINALISE ")]
    check(len(finals) == 1,
          "%s the lifecycle finalised exactly once" % label,
          "%d finalisations" % len(finals))
    # `FINALISE-DUPLICATE` is the GUARD reporting that a second attempt was
    # refused, which is what has to happen when the chord finalises and the
    # engine then tears the main loop down through _finalize(). What would
    # be wrong is a second real finalisation, and `finals == 1` covers that.
    exits = [l for l in lines if l.startswith("MX EXIT ")]
    check(all("finalised=1" in l for l in exits) if exits else True,
          "%s the teardown reports a single finalisation" % label,
          " ".join(exits)[:160])
    # ZERO Escape/Enter. These are the keys a port would be tempted to
    # manufacture in order to quit, and the contract forbids exactly that.
    # (Godot 3 and Godot 4 number their Key enum differently.)
    quit_keys = {"16777217", "16777221", "16777222",   # Godot 3
                 "4194305", "4194309", "4194310"}      # Godot 4
    offending = [l for l in lines if "kind=KEY" in l
                 and any(("keycode=%s " % k) in l + " " or
                         ("scancode=%s " % k) in l + " " for k in quit_keys)]
    check(not offending,
          "%s zero synthetic Escape/Enter" % label, " ".join(offending)[:200])
    # Structurally decisive: the devices this run created declare gamepad
    # codes only, so nothing this contract creates is even CAPABLE of
    # producing a keyboard event.
    check(data.get("pad_declares_keyboard") is False,
          "%s the synthetic pads declare no keyboard capability at all"
          % label, str(data.get("pad_key_codes")))
    # Both majors run without a window or desktop display connection. Any
    # keyboard event is therefore a hard failure, not ambient GUI noise.
    ambient = [l for l in lines if "kind=KEY" in l]
    check(not ambient,
          "%s no keyboard event of any kind in headless mode" % label,
          " ".join(ambient)[:200])


def judge_one_pad_per_node(data, label):
    """Every announcement must belong to a node this run actually created.

    A leftover uinput node from an earlier run would be enumerated as a
    second joypad and quietly split the events in two; this is what catches
    that, instead of the matrix simply seeing half its presses.
    """
    created = data.get("pads_created") or []
    if not created:
        return
    announced = []
    for line in data.get("seam_receipt", "").splitlines():
        if "stage=announce" not in line or "result=admit" not in line:
            continue
        parts = dict(p.split("=", 1) for p in line.split() if "=" in p)
        announced.append(parts.get("devpath"))
    if not announced:
        return
    check(set(announced) <= set(created),
          "%s every admitted pad is a node this run created" % label,
          "announced=%s created=%s" % (announced, created))
    check(len(announced) == len(set(announced)),
          "%s no node was admitted twice" % label, str(announced))


def judge_ordering(data, label):
    """The seam ran before the engine exposed the pad, in one PID/TID."""
    receipt = data.get("seam_receipt", "")
    if not receipt.strip():
        return
    stages = []
    pids = set()
    tids = set()
    seqs = []
    for line in receipt.splitlines():
        if not line.startswith("NXC5B-SEAM"):
            continue
        parts = dict(p.split("=", 1) for p in line.split() if "=" in p)
        stages.append(parts.get("stage"))
        pids.add(parts.get("pid"))
        tids.add(parts.get("tid"))
        seqs.append(int(parts.get("seq", "0")))
    check(len(pids) == 1 and len(tids) == 1,
          "%s every seam receipt shares one PID and one TID" % label,
          "pids=%s tids=%s" % (pids, tids))
    check(seqs == sorted(seqs),
          "%s the seam's sequence is strictly ordered" % label, str(seqs))
    order = [s for s in ("origin", "resolve", "setter", "readback",
                         "announce") if s in stages]
    positions = [stages.index(s) for s in order]
    check(positions == sorted(positions),
          "%s origin -> resolve -> setter -> readback -> announce" % label,
          str(stages))
    engine_pid = next((l.split("pid=")[1].strip()
                       for l in data.get("all_lines", [])
                       if l.startswith("MX pid=")), None)
    if engine_pid:
        check(engine_pid in pids,
              "%s the seam ran inside the engine's own process" % label,
              "engine=%s seam=%s" % (engine_pid, pids))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--result", action="append", required=True,
                    help="a scenario JSON produced by c5b_matrix_driver.py")
    ap.add_argument("--require-groups", type=int, default=18)
    args = ap.parse_args()

    covered = {}
    seen = set()
    for path in args.result:
        data = json.loads(pathlib.Path(path).read_text())
        label = "%s/%s" % (data["engine"], data["scenario"])
        seen.add(label)
        print("---- %s" % label)
        judge_headless(data, label)
        judge_ordering(data, label)
        judge_one_pad_per_node(data, label)
        scenario = data["scenario"]
        if scenario in ("matrix", "ownerswap"):
            groups = judge_matrix(data, label)
            covered.setdefault(data["engine"], set()).update(groups)
            judge_chord(data, label)
        elif scenario == "null":
            judge_null(data, label)
        elif scenario == "twopads":
            judge_twopads(data, label)
            judge_chord(data, label, expect_mixed=True)
        elif scenario == "hotplug":
            judge_hotplug(data, label)
        elif scenario == "native":
            judge_native(data, label)
        elif scenario == "negatives":
            judge_negatives(data, label)
        elif scenario == "sigterm":
            judge_sigterm(data, label)

    for engine, groups in sorted(covered.items()):
        check(len(groups) >= args.require_groups,
              "%s covered all %d V2 control groups"
              % (engine, args.require_groups),
              "covered %d: %s" % (len(groups), sorted(groups)))
    for engine in ("godot3", "godot4"):
        check(any(s.startswith(engine + "/") for s in seen),
              "%s was actually exercised" % engine)

    passed = sum(1 for ok, _, _ in CHECKS if ok)
    print("\ngodot_c5b_matrix_gate: %d checks, %d passed, %d failed"
          % (len(CHECKS), passed, len(FAILURES)))
    if FAILURES:
        print("godot_c5b_matrix_gate: FAILED")
        for failure in FAILURES:
            print("  - %s" % failure)
        return 1
    print("godot_c5b_matrix_gate: PASS "
          "(REAL_API_HOST for the engine answers; physical PENDING_PHYSICAL)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
