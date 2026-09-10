#!/usr/bin/env python3
"""Synthetic contract tests. They do not run or certify any Unity game."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verificar_toque_curto import analyse, InvalidTrace

ROOT = Path(__file__).resolve().parent


def specimen():
    coverage = {}
    for stage, boundary in (("origin", "sdl_event"),
                            ("delivery", "native_injection"),
                            ("consumer", "game_press_release_callback")):
        coverage[stage] = dict(start_us=0, end_us=150000, dropped=0,
                               complete=True, boundary=boundary)
    events = []
    for stage, down, up, route in (
            ("origin", 10000, 30000, "pad-r3"),
            ("delivery", 68000, 69000, "android-touch"),
            ("consumer", 70000, 71000, "game-touch")):
        for edge, time in (("DOWN", down), ("UP", up)):
            events.append(dict(stage=stage, gesture="tap-1", edge=edge,
                               t_us=time, route=route, owner_context="menu"))
    return dict(schema="unity-short-press/1", contract="stateful-press",
                evidence_kind="synthetic", executable_sha256=None,
                run_id="fixture-short-tap", action="menu-touch",
                native_render_enter_us=[0, 66667, 133334],
                coverage=coverage, events=sorted(events, key=lambda e: e["t_us"]))


class TraceTests(unittest.TestCase):
    def test_complete_tap_between_frames(self):
        result = analyse(specimen())
        self.assertEqual(result["status"], "TRACE_CONSISTENT")
        self.assertEqual(result["short_presses"][0]["between_native_render_enter_us"], [0, 66667])
        self.assertEqual(result["physical_validation"], "NOT_ESTABLISHED_BY_THIS_TOOL")

    def test_final_state_sampling_loses_both_edges(self):
        trace = specimen()
        trace["events"] = [e for e in trace["events"] if e["stage"] == "origin"]
        result = analyse(trace)
        self.assertEqual(result["status"], "TRACE_DEFECT")
        self.assertEqual({f["stage"] for f in result["findings"] if f["code"] == "GESTURE_MISSING"},
                         {"delivery", "consumer"})

    def test_enqueue_does_not_prove_consumption(self):
        trace = specimen()
        trace["events"] = [e for e in trace["events"] if e["stage"] != "consumer"]
        trace["coverage"]["consumer"] = None
        self.assertEqual(analyse(trace)["status"], "INCONCLUSIVE")

    def test_full_consumer_probe_reveals_loss_after_delivery(self):
        trace = specimen()
        trace["events"] = [e for e in trace["events"] if e["stage"] != "consumer"]
        result = analyse(trace)
        self.assertEqual(result["status"], "TRACE_DEFECT")
        self.assertIn({"code": "GESTURE_MISSING", "gesture": "tap-1", "stage": "consumer"},
                      result["findings"])

    def test_duplicate_down_is_not_a_valid_fix(self):
        trace = specimen()
        duplicate = copy.deepcopy(trace["events"][-2])
        duplicate["t_us"] += 1
        trace["events"].append(duplicate)
        trace["events"].sort(key=lambda e: e["t_us"])
        result = analyse(trace)
        self.assertTrue(any(f["code"] == "DUPLICATE_DOWN" for f in result["findings"]))

    def test_missing_release_detected(self):
        trace = specimen()
        trace["events"] = [e for e in trace["events"]
                           if not (e["stage"] == "consumer" and e["edge"] == "UP")]
        self.assertTrue(any(f["code"] == "RELEASE_MISSING" for f in analyse(trace)["findings"]))

    def test_dropped_records_invalidate_negative_evidence(self):
        trace = specimen()
        trace["events"] = [e for e in trace["events"] if e["stage"] != "consumer"]
        trace["coverage"]["consumer"]["dropped"] = 1
        self.assertEqual(analyse(trace)["status"], "INCONCLUSIVE")

    def test_probe_must_extend_to_later_native_frame(self):
        trace = specimen()
        trace["native_render_enter_us"] = [0, 20000]
        self.assertEqual(analyse(trace)["status"], "INCONCLUSIVE")

    def test_long_press_does_not_cover_short_press_case(self):
        trace = specimen()
        for e in trace["events"]:
            if e["stage"] == "origin" and e["edge"] == "UP":
                e["t_us"] = 67000
        trace["events"].sort(key=lambda e: e["t_us"])
        self.assertEqual(analyse(trace)["status"], "NO_SHORT_PRESS_OBSERVED")

    def test_route_change_is_not_auto_approved(self):
        trace = specimen()
        trace["events"][-1]["owner_context"] = "gameplay"
        self.assertEqual(analyse(trace)["status"], "REVIEW_REQUIRED")

    def test_low_level_queue_cannot_be_called_game_consumer(self):
        trace = specimen()
        trace["coverage"]["consumer"]["boundary"] = "input_queue"
        with self.assertRaises(InvalidTrace):
            analyse(trace)

    def test_runtime_requires_executable_identity(self):
        trace = specimen()
        trace["evidence_kind"] = "runtime"
        with self.assertRaises(InvalidTrace):
            analyse(trace)

    def test_runtime_hash_must_be_a_string(self):
        trace = specimen()
        trace["evidence_kind"] = "runtime"
        trace["executable_sha256"] = int("1" * 64)
        with self.assertRaises(InvalidTrace):
            analyse(trace)

    def test_causality_for_both_edges(self):
        for stage, edge, time in (("consumer", "DOWN", 40000),
                                  ("consumer", "UP", 41000),
                                  ("delivery", "UP", 20000)):
            trace = specimen()
            for event in trace["events"]:
                if event["stage"] == stage and event["edge"] == edge:
                    event["t_us"] = time
            trace["events"].sort(key=lambda e: e["t_us"])
            with self.subTest(stage=stage, edge=edge):
                self.assertNotEqual(analyse(trace)["status"], "TRACE_CONSISTENT")

    def test_consumer_window_must_cover_delivery_and_next_frame(self):
        trace = specimen()
        trace["events"] = [e for e in trace["events"] if e["stage"] != "consumer"]
        trace["coverage"]["consumer"]["end_us"] = 66667
        result = analyse(trace)
        self.assertEqual(result["status"], "INCONCLUSIVE")
        self.assertFalse(any(f["code"] == "GESTURE_MISSING" for f in result["findings"]))

    def test_owner_changed_in_both_downstream_stages(self):
        trace = specimen()
        for event in trace["events"]:
            if event["stage"] != "origin":
                event["owner_context"] = "gameplay"
        self.assertEqual(analyse(trace)["status"], "REVIEW_REQUIRED")

    def test_origin_route_change_requires_review(self):
        trace = specimen()
        trace["events"][1]["route"] = "pad-a"
        self.assertEqual(analyse(trace)["status"], "REVIEW_REQUIRED")

    def test_move_after_up_is_not_ignored(self):
        trace = specimen()
        event = copy.deepcopy(trace["events"][-1])
        event.update(edge="MOVE", t_us=72000)
        trace["events"].append(event)
        self.assertEqual(analyse(trace)["status"], "TRACE_DEFECT")

    def test_trace_order_and_private_identifiers_rejected(self):
        for mutation in ("order", "ip", "embedded_ip", "path"):
            trace = specimen()
            if mutation == "order":
                trace["events"].reverse()
            else:
                trace["run_id"] = {"ip": "192.0.2.9", "embedded_ip": "run-192.0.2.9",
                                   "path": "private/path"}[mutation]
            with self.subTest(mutation=mutation), self.assertRaises(InvalidTrace):
                analyse(trace)

    def test_cli_exit_codes_and_no_raw_error_echo(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "input.json"
            trace.write_text(json.dumps(specimen()))
            command = [sys.executable, str(ROOT / "verificar_toque_curto.py"), str(trace)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0)
            trace.write_text("private malformed content")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn("private", result.stdout)
            self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
