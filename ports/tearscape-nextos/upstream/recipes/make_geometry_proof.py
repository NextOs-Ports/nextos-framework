#!/usr/bin/env python3
"""WAYLAND_GEOMETRY_PROOF gate consumer for the nx-geometry-proof/1 receipt.

The Tearscape display server appends one JSON line per event to the file
named by NXGEOMETRY_RECEIPT (see nx_sdl2_geometry.cpp): an "init" line after
the first authoritative configure (or, for the raw fbdev provider, at
provider acceptance) and one "resize" line per live size change.

`read_geometry_proof` turns that receipt into a verdict a release gate can
fail on. It refuses, for the SDL2 provider:

* a configured size equal to the raw framebuffer while the display bounds
  differ from it (the portrait/cropped window the fix exists for);
* an empty app id (the compositor could not match the window);
* a timed-out configure wait;
* a drawable different from the configured window size after init;
* a run_id/generation different from the video proof tuple when given.

The raw fbdev provider passes when every size equals the raw panel size.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

SCHEMA = "nx-geometry-proof/1"
INIT_FIELDS = (
    "schema", "kind", "provider", "video_driver", "app_id", "app_id_source",
    "raw_fb_w", "raw_fb_h", "display_w", "display_h", "requested_w",
    "requested_h", "configured_w", "configured_h", "drawable_w", "drawable_h",
    "fullscreen", "configure_events", "wait_ms", "timed_out", "run_id",
    "generation", "port_id",
)
RESIZE_FIELDS = ("schema", "kind", "w", "h", "drawable_w", "drawable_h", "notified")
ASPECT_SCHEMA = "nx-aspect-policy/1"
ASPECT_FIELDS = (
    "schema", "kind", "policy_schema", "provider", "requested", "project_value",
    "effective", "source", "changed", "drawable_w", "drawable_h",
    "content_x", "content_y", "content_w", "content_h",
)
ASPECT_SOURCES = (
    "invalid", "engine", "owner-cfg", "override-cfg", "auto-wide", "auto-fill",
    "auto-square", "auto-unknown",
)
INT_INIT_FIELDS = (
    "raw_fb_w", "raw_fb_h", "display_w", "display_h", "requested_w",
    "requested_h", "configured_w", "configured_h", "drawable_w", "drawable_h",
    "configure_events", "wait_ms",
)


class GeometryProofError(RuntimeError):
    """The geometry receipt is not acceptable release evidence."""


def _fail(message: str) -> None:
    raise GeometryProofError(f"geometry proof: {message}")


def _parse_lines(path: Path) -> list[dict]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        _fail(f"cannot read receipt: {error}")
    records = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            _fail(f"line {number} is not JSON: {error}")
        if not isinstance(record, dict):
            _fail(f"line {number} is not a JSON object")
        if record.get("schema") != SCHEMA:
            _fail(f"line {number} schema is not {SCHEMA}")
        records.append(record)
    if not records:
        _fail("receipt is empty")
    return records


def _check_fields(record: dict, fields: tuple[str, ...], kind: str) -> None:
    missing = [field for field in fields if field not in record]
    if missing:
        _fail(f"{kind} line lacks {', '.join(missing)}")


def read_geometry_proof(
        path: Path, run_id: str | None = None, generation: str | None = None,
) -> dict:
    """Validate the receipt at `path`; return a summary dict or raise."""
    records = _parse_lines(Path(path))
    inits = [record for record in records if record.get("kind") == "init"]
    resizes = [record for record in records if record.get("kind") == "resize"]
    aspects = [record for record in records if record.get("kind") == "aspect"]
    if len(inits) != 1:
        _fail(f"expected exactly one init line, found {len(inits)}")
    if len(inits) + len(resizes) + len(aspects) != len(records):
        _fail("receipt carries an unknown kind")
    if len(aspects) > 1:
        _fail(f"expected at most one aspect line, found {len(aspects)}")
    aspect = aspects[0] if aspects else None
    if aspect is not None:
        _check_fields(aspect, ASPECT_FIELDS, "aspect")
        if aspect["policy_schema"] != ASPECT_SCHEMA:
            _fail(f"aspect policy schema {aspect['policy_schema']!r} is not {ASPECT_SCHEMA}")
        for field in ("drawable_w", "drawable_h", "content_x", "content_y", "content_w", "content_h"):
            if type(aspect[field]) is not int or aspect[field] < 0:
                _fail(f"aspect field {field} is not a non-negative integer")
        if type(aspect["changed"]) is not bool:
            _fail("aspect field changed is not a boolean")
        if aspect["effective"] not in ("ignore", "keep"):
            _fail(f"aspect effective {aspect['effective']!r} is neither ignore nor keep")
        if aspect["source"] not in ASPECT_SOURCES:
            _fail(f"aspect source {aspect['source']!r} is unknown")
        if aspect["content_w"] > aspect["drawable_w"] or aspect["content_h"] > aspect["drawable_h"]:
            _fail("aspect content rect exceeds the drawable")
        if aspect["effective"] == "ignore" and (
                aspect["content_w"] != aspect["drawable_w"] or aspect["content_h"] != aspect["drawable_h"]):
            _fail("aspect ignore must fill the drawable")
        if aspect["effective"] == "keep" and aspect["drawable_w"] > 0 and aspect["drawable_h"] > 0:
            if abs(aspect["content_w"] * 9 - aspect["content_h"] * 16) > 16:
                _fail("aspect keep content rect is not 16:9")
    init = inits[0]
    _check_fields(init, INIT_FIELDS, "init")
    for field in INT_INIT_FIELDS:
        if type(init[field]) is not int or init[field] < 0:
            _fail(f"init field {field} is not a non-negative integer")
    for field in ("fullscreen", "timed_out"):
        if type(init[field]) is not bool:
            _fail(f"init field {field} is not a boolean")
    for field in ("provider", "video_driver", "app_id", "app_id_source",
                  "run_id", "generation", "port_id"):
        if not isinstance(init[field], str):
            _fail(f"init field {field} is not a string")
    for index, resize in enumerate(resizes, 1):
        _check_fields(resize, RESIZE_FIELDS, f"resize #{index}")
        for field in ("w", "h", "drawable_w", "drawable_h"):
            if type(resize[field]) is not int or resize[field] <= 0:
                _fail(f"resize #{index} field {field} is not a positive integer")
        if type(resize["notified"]) is not bool:
            _fail(f"resize #{index} notified is not a boolean")
        if (resize["w"], resize["h"]) != (resize["drawable_w"], resize["drawable_h"]):
            _fail(f"resize #{index} logical size differs from its drawable")

    provider = init["provider"]
    if provider not in ("sdl2", "fbdev"):
        _fail(f"unknown provider {provider!r}")
    if run_id is not None and init["run_id"] != run_id:
        _fail("run_id differs from the video proof tuple")
    if generation is not None and init["generation"] != generation:
        _fail("generation differs from the video proof tuple")

    raw = (init["raw_fb_w"], init["raw_fb_h"])
    display = (init["display_w"], init["display_h"])
    configured = (init["configured_w"], init["configured_h"])
    drawable = (init["drawable_w"], init["drawable_h"])
    if configured[0] <= 0 or configured[1] <= 0:
        _fail("configured size is empty")
    if init["timed_out"]:
        _fail(f"configure wait timed_out after {init['wait_ms']} ms")
    if drawable != configured:
        _fail(f"drawable {drawable} differs from configured {configured} after init")

    if provider == "sdl2":
        if not init["app_id"]:
            _fail("app_id is empty (compositor cannot match the window)")
        if display[0] > 0 and display[1] > 0 and display != raw and configured == raw:
            _fail(f"configured {configured} equals the raw framebuffer while the "
                  f"display is {display} (portrait/cropped window)")
        if display[0] > 0 and display[1] > 0 and configured != display and not resizes:
            _fail(f"configured {configured} differs from display {display} "
                  "without a later resize")
        if not init["fullscreen"]:
            _fail("window is not fullscreen after init")
    else:
        if raw[0] <= 0 or raw[1] <= 0:
            _fail("fbdev provider without a raw framebuffer geometry")
        if configured != raw or display != raw or drawable != raw:
            _fail("fbdev provider geometry must equal the raw framebuffer")
        if resizes:
            _fail("fbdev provider cannot resize")

    final = (resizes[-1]["w"], resizes[-1]["h"]) if resizes else configured
    return {
        "aspect": aspect,
        "provider": provider,
        "video_driver": init["video_driver"],
        "app_id": init["app_id"],
        "app_id_source": init["app_id_source"],
        "raw_fb_w": raw[0], "raw_fb_h": raw[1],
        "display_w": display[0], "display_h": display[1],
        "configured_w": configured[0], "configured_h": configured[1],
        "final_w": final[0], "final_h": final[1],
        "configure_events": init["configure_events"],
        "wait_ms": init["wait_ms"],
        "fullscreen": init["fullscreen"],
        "resizes": len(resizes),
        "resize_notified": all(resize["notified"] for resize in resizes) if resizes else None,
        "run_id": init["run_id"],
        "generation": init["generation"],
        "port_id": init["port_id"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--run-id")
    parser.add_argument("--generation")
    args = parser.parse_args()
    try:
        summary = read_geometry_proof(args.receipt, args.run_id, args.generation)
    except GeometryProofError as error:
        print(f"WAYLAND_GEOMETRY_PROOF: FAIL: {error}", file=sys.stderr)
        return 1
    print(json.dumps(summary, sort_keys=True))
    print("WAYLAND_GEOMETRY_PROOF: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
