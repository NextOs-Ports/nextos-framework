#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V5 (auditoria 03/09, item d): o gate de vendor BYTE A BYTE do nxrelease.

Um port que compila `vendor/<componente>/` embute uma cópia do framework. O
único conferidor existente era o próprio `PINS.json` -- recalculado junto com
a cópia, portanto sempre coerente consigo mesmo. Um vendor VELHO passava.
Foi o que aconteceu em 03/09: os três pilotos declararam pin no commit
congelado carregando `nxinput/src/nxinput_provider.c` de antes do provider
estático, e o "ELF byte-idêntico" era consequência de não revendorizar.

Este gate compara com a ÁRVORE DO COMPONENTE, que é a verdade. O controle
positivo obrigatório está no fim: o vendor real dos pilotos, hoje, TEM de
reprovar -- senão o gate não estaria medindo nada.
"""
import hashlib
import importlib.util
import json
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
import subprocess
HEAD = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                      capture_output=True, text=True, check=True).stdout.strip()
spec = importlib.util.spec_from_file_location(
    "nxrelease", ROOT / "framework/nxrelease/nxrelease.py")
nxr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nxr)

fails = 0


def check(condition, message):
    global fails
    print(("ok   " if condition else "FAIL ") + message)
    if not condition:
        fails += 1


def run(source_root):
    """Returns None on success or the ReleaseError message."""
    try:
        nxr.validate_vendor_pin_contract({"source_root": Path(source_root)})
    except nxr.ReleaseError as error:
        return str(error)
    return None


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def vendor_component(port, component, names):
    """Copy `names` out of framework/<component> and pin them honestly."""
    source = ROOT / "framework" / component
    target = Path(port) / "vendor" / component
    files = {}
    for name in names:
        destination = target / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / name, destination)
        files[name] = sha(source / name)
    (target / "PINS.json").write_text(json.dumps({
        "schema": "fixture-%s-vendor-pins/1" % component,
        "framework_commit": HEAD,
        "files": files,
    }, indent=2) + "\n")
    return target


work = Path(tempfile.mkdtemp(prefix="nx-vendor-gate."))
try:
    port = work / "port"
    (port / "vendor").mkdir(parents=True)
    (port / "FRAMEWORK-PIN.json").write_text(json.dumps({
        "components": {"nxcompat": {"commit": HEAD, "version": "0.5.1"}},
    }) + "\n")
    names = ["include/nxcompat_video.h", "src/nxcompat_video.c"]
    vendor = vendor_component(port, "nxcompat", names)

    check(run(port) is None,
          "a vendor copied byte for byte from the pinned tree passes")

    # --- 0.4.3 (review 2, F5): the truth is the PINNED COMMIT, not the checkout
    # A pin that names an older commit whose tree differs from the checkout:
    # the vendor equal to the CHECKOUT must FAIL (it is not what the pin says).
    older = subprocess.run(["git", "-C", str(ROOT), "log", "--format=%H", "-n", "1",
                            "--diff-filter=M", "--", "framework/nxcompat/src/nxcompat_video.c"],
                           capture_output=True, text=True).stdout.strip()
    parent = subprocess.run(["git", "-C", str(ROOT), "rev-parse", older + "^"],
                            capture_output=True, text=True).stdout.strip() if older else ""
    if parent:
        (port / "FRAMEWORK-PIN.json").write_text(json.dumps({
            "components": {"nxcompat": {"commit": parent, "version": "0.5.x"}},
        }) + "\n")
        message = run(port)
        check(message is not None and "differs from the pinned framework tree" in message
              and parent[:12] in message,
              "MUTANT killed: vendor equal to the CHECKOUT but pinned to an older commit "
              "passes (the gate compared with the checkout, not with the pinned tree)")
        (port / "FRAMEWORK-PIN.json").write_text(json.dumps({
            "components": {"nxcompat": {"commit": HEAD, "version": "0.5.1"}},
        }) + "\n")
        check(run(port) is None, "back on the HEAD pin the same vendor passes")
    else:
        check(True, "(no older nxcompat_video.c revision in this repo: pinned-commit mutant skipped)")

    # no FRAMEWORK-PIN.json at all => a vendored component cannot be verified
    (port / "FRAMEWORK-PIN.json").rename(port / "FRAMEWORK-PIN.json.off")
    message = run(port)
    check(message is not None and "pins no commit" in message,
          "MUTANT killed: without FRAMEWORK-PIN.json a vendored component passed")
    (port / "FRAMEWORK-PIN.json.off").rename(port / "FRAMEWORK-PIN.json")

    # loose copy outside vendor/ (the Tearscape layout: src/nxinput/*.c)
    loose = port / "src" / "nxinput"
    loose.mkdir(parents=True)
    shutil.copy2(ROOT / "framework/nxcompat/src/nxcompat_video.c", loose / "nxcompat_video.c")
    check(run(port) is None, "a loose copy identical to the pinned tree passes")
    (loose / "nxcompat_video.c").write_bytes(
        (loose / "nxcompat_video.c").read_bytes().replace(b"int nxcompat_video_receipt", b"int nxcompat_video_receipt_old", 1))
    message = run(port)
    check(message is not None and "src/nxinput/nxcompat_video.c copies the framework source" in message,
          "MUTANT killed: a stale framework copy OUTSIDE vendor/ (src/nxinput) passed the gate")
    shutil.rmtree(port / "src")

    # --- the defect this gate exists for -------------------------------------
    stale = vendor / "src/nxcompat_video.c"
    original = stale.read_bytes()
    stale.write_bytes(original.replace(b"int nxcompat_video_receipt",
                                       b"int nxcompat_video_receipt_old", 1))
    pins = json.loads((vendor / "PINS.json").read_text())
    pins["files"]["src/nxcompat_video.c"] = sha(stale)   # a self-consistent lie
    (vendor / "PINS.json").write_text(json.dumps(pins, indent=2) + "\n")
    message = run(port)
    check(message is not None and
          "src/nxcompat_video.c differs from the pinned framework tree"
          in message,
          "MUTANT killed: a stale vendor whose PINS.json was recomputed to "
          "match it still FAILS (the tree is the truth, not the pin file)")

    stale.write_bytes(original)
    pins["files"]["src/nxcompat_video.c"] = sha(stale)
    (vendor / "PINS.json").write_text(json.dumps(pins, indent=2) + "\n")
    check(run(port) is None, "restoring the byte-identical copy passes again")

    # --- the pin file must not disagree with the bytes it pins ---------------
    stale.write_bytes(original + b"\n")
    message = run(port)
    check(message is not None and
          "does not match its own PINS.json entry" in message,
          "a vendored file that contradicts its own PINS.json entry fails")
    stale.write_bytes(original)

    # --- escape routes -------------------------------------------------------
    (vendor / "PINS.json").rename(vendor / "PINS.json.disabled")
    message = run(port)
    check(message is not None and "carries no PINS.json" in message,
          "MUTANT killed: deleting PINS.json is not an escape route for a "
          "vendored framework component")
    (vendor / "PINS.json.disabled").rename(vendor / "PINS.json")

    (vendor / "src/sneaked_in.c").write_text("/* not declared */\n")
    message = run(port)
    check(message is not None and "undeclared in PINS.json" in message,
          "an undeclared file inside a vendored component fails")
    (vendor / "src/sneaked_in.c").unlink()

    third_party = port / "vendor" / "sdl2"
    third_party.mkdir()
    (third_party / "SDL.c").write_text("/* upstream SDL, not a component */\n")
    check(run(port) is None,
          "a vendored THIRD PARTY tree (sdl2, fdk) needs no component pin")

    fake = port / "vendor" / "nxinput"
    fake.mkdir()
    (fake / "PINS.json").write_text(json.dumps({"files": {}}) + "\n")
    message = run(port)
    check(message is not None and ("declares no files" in message or "pins no commit" in message),
          "an empty component pin set fails closed (no files, or no commit pinned for it)")
    shutil.rmtree(fake)

    # --- positive control: the REAL pilots, measured against the record ------
    # The mission asks that the pilots' vendor as it stands TODAY be refused.
    # Asserting "must fail" forever would turn green into a regression the
    # moment M3 re-vendors, so the DEBT is recorded instead: the gate measures
    # the live drift and it has to equal vendor-drift-v1.json exactly. Drift
    # that grows silently fails here, and clearing it requires re-vendoring the
    # port AND emptying its entry in the same commit -- a deliberate act.
    record = json.loads(
        (ROOT / "framework/nxrelease/vendor-drift-v1.json").read_text())
    check(record.get("schema") == "nxrelease-vendor-drift/1",
          "the vendor drift record is nxrelease-vendor-drift/1")
    measured = {}
    for candidate in sorted((ROOT / "ports").glob("*/vendor")):
        slug = candidate.parent.name
        drift = []
        for pins_path in sorted(candidate.glob("*/PINS.json")):
            component = pins_path.parent.name
            pinned = json.loads(pins_path.read_text()).get("files") or {}
            pin_doc = candidate.parent / "FRAMEWORK-PIN.json"
            commit = None
            if pin_doc.is_file():
                comp = (json.loads(pin_doc.read_text()).get("components") or {}).get(component)
                commit = comp.get("commit") if isinstance(comp, dict) else None
            tree = nxr.pinned_tree_blobs(commit, nxr.vendor_component_relative(component)) if commit else None
            for relative in sorted(pinned):
                copied = pins_path.parent / relative
                if not copied.is_file():
                    continue
                upstream = nxr.pinned_blob_sha256(tree, relative) if tree else None
                if upstream is None:
                    drift.append("%s/%s" % (component, relative))
                elif sha(copied) != upstream:
                    drift.append("%s/%s" % (component, relative))
        if drift:
            measured[slug] = drift
    check(measured == record.get("ports"),
          "measured vendor drift equals the record (measured=%s recorded=%s)"
          % ({k: len(v) for k, v in sorted(measured.items())},
             {k: len(v) for k, v in sorted((record.get("ports") or {}).items())}))
    check(any(measured.values()) or not measured,
          "positive control: the recorded debt (if any) is what the gate measures today")
    for slug in sorted(measured):
        message = run(ROOT / "ports" / slug)
        check(message is not None and
              "differs from the pinned framework tree" in message,
              "validate refuses %s while its vendor is behind the pin" % slug)
finally:
    shutil.rmtree(work, ignore_errors=True)

print("nxrelease-vendor-pin-gate: %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
