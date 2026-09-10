#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxobs 0.4.4 gate: the support bundle preserves the sanitized NXC6-SEAM /
NXC6-DOMAIN admission receipts (nxinput 0.10.0, contract 5.9) as OBSERVED
events -- which authority won, why each step yielded, the domain
classification counts, the live-database acquisition evidence and the
FACE_LAYOUT -- while pids, timestamps, prose and unlisted fields are
dropped and nothing is ever promoted."""

import importlib.util
import pathlib
import sys

COMPONENT = pathlib.Path(__file__).resolve().parent.parent


def require(value, message):
    if not value:
        print("nxobs V4 c6-receipt gate FAILED: %s" % message)
        sys.exit(1)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ANNOUNCE = (
    "NXC6-SEAM seq=3 t_ns=123456789 pid=4242 tid=4243 sdl=sdl3 instance=0 "
    "guid=19004ca6010000000100000000010000 stage=announce result=admit "
    "source=cfw-db-guid step_env=source-empty step_cfw=ok step_bundle=ok "
    "step_builtin=ok step_raw=ok readback_checked=1 source_crc_aliases=1 "
    "dup_lastwins=0 domain_lines=1 domain_bindings=15 "
    "source_domain=joydev-legacy target_domain=sdl3-evdev name=muOS-Keys "
    "db_class=canonical db_target=retro db_retries=3 db_elapsed_ms=75 "
    "face_layout=auto effective_guid=19004ca6010000000100000000010000 "
    "map_fnv1a64=00d1e2f3a4b5c6d7 map_bytes=315"
)
BLOCK = (
    "NXC6-SEAM seq=4 t_ns=2 pid=4242 tid=4243 sdl=sdl2 instance=1 "
    "guid=19004ca6010000000100000000010000 stage=slots result=block "
    "reason=no-free-slot (refusing rather than evicting a live pad)"
)
DOMAIN = (
    "NXC6-DOMAIN guid=19004ca6010000000100000000010000 matching=1 "
    "rewritten=1 rewritten_bindings=15 native=0 identical=0 ambiguous=0 "
    "invalid=0 volume_markers=2 result=rewritten"
)


def main():
    support = load("nxobs_support_v4c6", COMPONENT / "nx-support-bundle.py")

    events = []
    support.parse_runtime([ANNOUNCE, BLOCK, DOMAIN], events, "r1")
    require(len(events) == 3, "the three receipt lines were not all kept")
    require([item["phase"] for item in events] ==
            ["c6-seam", "c6-seam", "c6-domain"],
            "the receipt kinds lost their phases")
    require(all(item["source"] == "input" and item["status"] == "observed"
                for item in events),
            "ingestion must stay OBSERVED and never promote any state")

    announce = events[0]["details"]
    require(announce.get("source") == "cfw-db-guid" and
            announce.get("stage") == "announce" and
            announce.get("result") == "admit",
            "the announce receipt lost the winning authority")
    for field in ("step_env", "step_cfw", "step_bundle", "step_builtin",
                  "step_raw"):
        require(field in announce, "the per-step reason %s was lost" % field)
    require(announce.get("db_class") == "canonical" and
            announce.get("db_target") == "retro" and
            announce.get("db_retries") == "3" and
            announce.get("db_elapsed_ms") == "75",
            "the live-database acquisition evidence was lost")
    require(announce.get("face_layout") == "auto" and
            announce.get("name") == "muOS-Keys" and
            announce.get("map_fnv1a64") == "00d1e2f3a4b5c6d7",
            "the layout/name/hash evidence was lost")
    require("pid" not in announce and "tid" not in announce and
            "t_ns" not in announce,
            "process-local noise must be dropped from the shared bundle")

    block = events[1]["details"]
    require(block.get("result") == "block" and
            block.get("reason") == "no-free-slot",
            "the block receipt lost its reason")
    require(all("refusing" not in value for value in block.values()),
            "free-form prose must never become a detail field")

    domain = events[2]["details"]
    require(domain.get("rewritten_bindings") == "15" and
            domain.get("ambiguous") == "0" and
            domain.get("result") == "rewritten" and
            domain.get("volume_markers") == "2",
            "the domain classification counts were lost")

    events = []
    support.parse_runtime(
        ["NXC6-SEAM pid=1 t_ns=2 /home/user/personal secret=1"], events,
        "r1")
    require(events == [] or all(
        "personal" not in str(item) and "secret" not in str(item)
        for item in events),
        "an unlisted or hostile token must never enter the bundle")

    print("nxobs 0.4.4 c6-receipt gate passed: announce=1 block=1 domain=1 "
          "noise_dropped=1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
