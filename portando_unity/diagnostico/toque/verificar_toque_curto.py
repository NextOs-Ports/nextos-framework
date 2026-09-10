#!/usr/bin/env python3
"""Analyse an explicit, finite input trace. Never injects events or approves a port."""
import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import sys

STAGES = ("origin", "delivery", "consumer")
EDGES = ("DOWN", "MOVE", "UP", "CANCEL")
LIMIT = 4 * 1024 * 1024
TOKEN = re.compile(r"[A-Za-z0-9_.-]{1,80}\Z")


class InvalidTrace(ValueError):
    pass


def integer(value):
    return type(value) is int and value >= 0


def token(value):
    if not isinstance(value, str) or TOKEN.fullmatch(value) is None:
        return False
    for candidate in re.findall(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])", value):
        try:
            ipaddress.ip_address(candidate)
        except ValueError:
            continue
        return False
    return True


def analyse(data):
    if not isinstance(data, dict) or data.get("schema") != "unity-short-press/1":
        raise InvalidTrace("schema deve ser unity-short-press/1")
    if data.get("contract") != "stateful-press":
        raise InvalidTrace("o contrato deve observar press e release, não apenas Submit")
    if not token(data.get("run_id")) or not token(data.get("action")):
        raise InvalidTrace("run_id/action devem ser identificadores simples")
    kind = data.get("evidence_kind")
    if kind not in ("synthetic", "transcription", "runtime"):
        raise InvalidTrace("evidence_kind inválido")
    sha = data.get("executable_sha256")
    if sha is not None and (not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha)):
        raise InvalidTrace("executable_sha256 inválido")
    if kind != "synthetic" and (sha is None or sha == "0" * 64):
        raise InvalidTrace("registro não sintético exige hash do executável observado")
    frames = data.get("native_render_enter_us")
    if (not isinstance(frames, list) or len(frames) < 2
            or not all(integer(n) for n in frames)
            or any(a >= b for a, b in zip(frames, frames[1:]))):
        raise InvalidTrace("registrar ao menos dois nativeRender_enter crescentes")
    coverage = data.get("coverage")
    if not isinstance(coverage, dict) or set(coverage) - set(STAGES):
        raise InvalidTrace("coverage inválido")
    for stage, window in coverage.items():
        if window is None:
            continue
        if (not isinstance(window, dict)
                or not integer(window.get("start_us"))
                or not integer(window.get("end_us"))
                or window["end_us"] <= window["start_us"]
                or not integer(window.get("dropped"))
                or type(window.get("complete")) is not bool
                or not token(window.get("boundary"))):
            raise InvalidTrace("janela de observação inválida")
        if stage == "consumer" and window["boundary"] not in (
                "game_action_state", "game_press_release_callback"):
            raise InvalidTrace("consumer precisa observar estado/ação do jogo")
    events = data.get("events")
    if not isinstance(events, list) or len(events) > 20000:
        raise InvalidTrace("events inválido ou excede 20000 registros")
    groups = defaultdict(lambda: defaultdict(list))
    last_time = -1
    for event in events:
        if (not isinstance(event, dict) or event.get("stage") not in STAGES
                or event.get("edge") not in EDGES
                or not token(event.get("gesture"))
                or not token(event.get("route"))
                or not token(event.get("owner_context"))
                or not integer(event.get("t_us"))):
            raise InvalidTrace("evento inválido")
        time = event["t_us"]
        if time < last_time:
            raise InvalidTrace("events deve estar ordenado pelo instante da observação")
        last_time = time
        window = coverage.get(event["stage"])
        if window is not None and not window["start_us"] <= time <= window["end_us"]:
            raise InvalidTrace("evento fora da janela declarada")
        groups[event["gesture"]][event["stage"]].append(event)
    if not any(stages.get("origin") for stages in groups.values()):
        raise InvalidTrace("sem gesto observado na origem")

    findings = []
    short = []
    latencies = []
    def add(code, gesture=None, stage=None):
        item = {"code": code}
        if gesture is not None:
            item["gesture"] = gesture
        if stage is not None:
            item["stage"] = stage
        if item not in findings:
            findings.append(item)

    # A bounded window must extend to a later native frame after the input.
    # A log ending at UP is not evidence that the consumer lost UP.
    def sufficient(stage, start, deadline):
        w = coverage.get(stage)
        return (w is not None and w["complete"] is True and w["dropped"] == 0
                and w["start_us"] <= start and w["end_us"] >= deadline)

    def moves_outside_press(stream):
        held = False
        for event in stream:
            if event["edge"] == "DOWN":
                held = True
            elif event["edge"] in ("UP", "CANCEL"):
                held = False
            elif not held:
                return True
        return False

    for gesture, stages in sorted(groups.items()):
        origin = stages.get("origin", [])
        expected = [e["edge"] for e in origin if e["edge"] != "MOVE"]
        if expected not in (["DOWN", "UP"], ["DOWN", "CANCEL"]) or moves_outside_press(origin):
            add("ORIGIN_SEQUENCE_INVALID", gesture, "origin")
            continue
        owner = origin[0]["owner_context"]
        if len({(e["route"], e["owner_context"]) for e in origin}) > 1:
            add("ROUTE_OR_OWNER_CHANGED", gesture, "origin")
        start, end = origin[0]["t_us"], origin[-1]["t_us"]
        next_index = bisect_right(frames, end)
        if next_index >= len(frames):
            add("OBSERVATION_WINDOW_INCOMPLETE", gesture)
            continue
        deadline = frames[next_index]
        if not sufficient("origin", start, deadline):
            add("ORIGIN_COVERAGE_INCOMPLETE", gesture, "origin")
            continue
        left = bisect_right(frames, start) - 1
        right = bisect_right(frames, end) - 1
        if expected[-1] == "UP" and left == right and 0 <= left < len(frames) - 1:
            short.append({"gesture": gesture, "down_us": start, "up_us": end,
                          "between_native_render_enter_us": [frames[left], frames[left + 1]]})
        for stage in ("delivery", "consumer"):
            stream = stages.get(stage, [])
            actual = [e["edge"] for e in stream if e["edge"] != "MOVE"]
            upstream = origin if stage == "delivery" else stages.get("delivery", [])
            upstream_end = max([end] + [e["t_us"] for e in upstream])
            stage_next_index = bisect_right(frames, upstream_end)
            complete = (stage_next_index < len(frames)
                        and sufficient(stage, start, frames[stage_next_index]))
            if not complete:
                add("STAGE_NOT_FULLY_OBSERVED", gesture, stage)
                continue
            if actual != expected:
                code = "EDGE_SEQUENCE_MISMATCH"
                if not actual:
                    code = "GESTURE_MISSING"
                elif Counter(actual)["DOWN"] > 1:
                    code = "DUPLICATE_DOWN"
                elif Counter(actual)["UP"] > 1:
                    code = "DUPLICATE_UP"
                elif "DOWN" in actual and not any(e in actual for e in ("UP", "CANCEL")):
                    code = "RELEASE_MISSING"
                add(code, gesture, stage)
            if moves_outside_press(stream):
                add("MOVE_OUTSIDE_PRESS", gesture, stage)
            if stream and (len({e["route"] for e in stream}) > 1
                           or any(e["owner_context"] != owner for e in stream)):
                add("ROUTE_OR_OWNER_CHANGED", gesture, stage)
            upstream_edges = [e for e in upstream if e["edge"] != "MOVE"]
            stage_edges = [e for e in stream if e["edge"] != "MOVE"]
            if actual == [e["edge"] for e in upstream_edges]:
                delays = [b["t_us"] - a["t_us"] for a, b in zip(upstream_edges, stage_edges)]
                if any(delay < 0 for delay in delays):
                    add("CAUSAL_ORDER_INVALID", gesture, stage)
                elif delays:
                    latencies.append({"gesture": gesture, "stage": stage,
                                      "from_previous_stage_us": delays})
        if not stages.get("delivery") and stages.get("consumer"):
            add("CONSUMER_WITHOUT_DELIVERY_RECORD", gesture)
    # Gestures invented downstream must not disappear from the report.
    for gesture, stages in groups.items():
        if not stages.get("origin") and any(stages.values()):
            add("ORIGIN_SEQUENCE_INVALID", gesture, "origin")

    inconclusive = {"ORIGIN_SEQUENCE_INVALID", "ORIGIN_COVERAGE_INCOMPLETE",
                    "OBSERVATION_WINDOW_INCOMPLETE", "STAGE_NOT_FULLY_OBSERVED",
                    "CONSUMER_WITHOUT_DELIVERY_RECORD", "CAUSAL_ORDER_INVALID"}
    review = {"ROUTE_OR_OWNER_CHANGED"}
    defects = [f for f in findings if f["code"] not in inconclusive | review]
    if defects:
        status = "TRACE_DEFECT"
    elif any(f["code"] in inconclusive for f in findings):
        status = "INCONCLUSIVE"
    elif findings:
        status = "REVIEW_REQUIRED"
    elif not short:
        status = "NO_SHORT_PRESS_OBSERVED"
    else:
        status = "TRACE_CONSISTENT"
    return {
        "schema": "unity-short-press-report/1", "status": status,
        "evidence_kind": kind, "run_id": data["run_id"], "action": data["action"],
        "executable_sha256": sha, "short_presses": short, "latencies": latencies,
        "findings": findings,
        "scope": "consistência do registro fornecido; não injeta input nem aprova o port",
        "physical_validation": "NOT_ESTABLISHED_BY_THIS_TOOL",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="arquivo JSON explícito, no host")
    args = parser.parse_args()
    try:
        if not args.trace.is_file() or args.trace.stat().st_size > LIMIT:
            raise InvalidTrace("esperado arquivo regular de até 4 MiB")
        with args.trace.open("rb") as source:
            raw = source.read(LIMIT + 1)
        if len(raw) > LIMIT:
            raise InvalidTrace("arquivo excede 4 MiB")
        result = analyse(json.loads(raw))
        result["trace_sha256"] = hashlib.sha256(raw).hexdigest()
    except (OSError, ValueError, RecursionError, TypeError, KeyError):
        # Do not echo raw paths, log lines or exception contents.
        print(json.dumps({"status": "INVALID_INPUT",
                          "message": "registro inválido; conferir schema e contrato no README"},
                         ensure_ascii=False))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False))
    return {"TRACE_CONSISTENT": 0, "TRACE_DEFECT": 1}.get(result["status"], 3)


if __name__ == "__main__":
    sys.exit(main())
