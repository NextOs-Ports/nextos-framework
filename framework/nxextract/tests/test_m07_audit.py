#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Static completeness gate for the NXExtract 1.2.7 / M07 evidence."""

import hashlib
import json
import re
import subprocess
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
AUDIT_PATH = REPO_ROOT / "framework/nxextract/m07-audit-v1.json"
NX_ROOT = REPO_ROOT / "suportando_outros_devices/extrator-universal"
PIN_GATE = REPO_ROOT / "suportando_outros_devices/tools/check-nxextract-pin.sh"
# The attested UI is the sealed 1.2.16 release binary -- the same object
# nxbootstrap, nxrelease and every generated port pin. Earlier attestations
# named the 1.2.7 and 1.2.9 builds and drifted from the canonical release
# without anyone noticing, because ui/build/ happened to still hold an old
# binary on the host that ran the gate.
EXPECTED_UI_SHA256 = (
    "8e4a68ae0a611096d23b04628b4f2e8b5cf34755fe9b71d134bc0d7ea6ccf987"
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_json(path):
    def no_duplicates(pairs):
        value = {}
        for key, item in pairs:
            require(key not in value, "duplicate JSON key %s in %s" % (key, path))
            value[key] = item
        return value

    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=no_duplicates)


def main():
    audit = load_json(AUDIT_PATH)
    require(audit.get("schema_version") == 1 and audit.get("milestone") == "M07",
            "M07 audit header changed")
    require(audit.get("scope") ==
            "local-filesystem-runtime-and-attested-ui-fallback",
            "M07 audit scope changed")
    require(audit.get("release_device_evidence") is False,
            "M07 audit incorrectly claims a full physical 1.2.7 release")
    require(set(audit) == {"schema_version", "milestone", "scope",
                           "release_device_evidence", "requirements"},
            "M07 audit has an unknown top-level field")

    requirements = audit.get("requirements")
    expected_ids = ["M07-%03d" % number for number in range(1, 21)]
    require(isinstance(requirements, list) and
            [item.get("id") for item in requirements] == expected_ids,
            "M07 audit IDs are incomplete or reordered")
    for item in requirements:
        require(set(item) == {"id", "implementation", "tests"},
                "%s has an unknown field" % item.get("id"))
        for group in ("implementation", "tests"):
            references = item.get(group)
            require(isinstance(references, list) and references,
                    "%s lacks %s evidence" % (item["id"], group))
            for reference in references:
                require(set(reference) == {"path", "token"},
                        "%s has malformed evidence" % item["id"])
                relative = Path(reference.get("path", ""))
                require(not relative.is_absolute() and ".." not in relative.parts,
                        "%s evidence escapes the repository" % item["id"])
                evidence = REPO_ROOT / relative
                require(evidence.is_file() and not evidence.is_symlink(),
                        "%s evidence is missing/linked: %s" %
                        (item["id"], relative))
                token = reference.get("token")
                require(isinstance(token, str) and token and
                        token in evidence.read_text(encoding="utf-8"),
                        "%s token is absent from %s: %r" %
                        (item["id"], relative, token))

    audit_text = AUDIT_PATH.read_text(encoding="utf-8")
    require(re.search(r"(?:^|[^0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?:[^0-9]|$)",
                      audit_text) is None,
            "M07 audit contains a device/test IP")
    version = (NX_ROOT / "VERSION").read_text(encoding="utf-8").strip()
    # M07 closed on the 1.2 line. The allowlist exists so an UNREVIEWED engine
    # cannot slip in, not to freeze the minor forever: a successor is added
    # here at the moment of its bump, exactly like the engine identity registry.
    require(version in ("1.2.7", "1.2.9", "1.2.10", "1.2.11", "1.2.12",
                        "1.2.13", "1.2.15", "1.2.16", "1.2.17",
                        "1.2.18", "1.2.19", "1.2.20", "1.2.21", "1.3.0"),
            "NXExtract VERSION is not valid: %s" % version)
    engine = (NX_ROOT / "nxextract.py").read_text(encoding="utf-8")
    require('NXEXTRACT_VERSION = "' in engine,
            "NXExtract engine version header is missing")
    require(PIN_GATE.stat().st_mode & 0o111,
            "whole-bundle pin gate is not executable")
    # O gate tem que RODAR verde, nao apenas existir: um pin cravado apodreceu
    # (1.2.9 vs engine 1.2.17) e este teste continuou verde por so' olhar o
    # bit +x. Executar e' a unica prova de que a rede de seguranca esta' viva.
    pin_run = subprocess.run(
        ["bash", str(PIN_GATE), "--inventory"],
        capture_output=True, text=True, timeout=600,
        env={"LC_ALL": "C", "LANG": "C", "PATH": "/usr/bin:/bin"})
    require(pin_run.returncode == 0,
            "whole-bundle pin gate failed: %s" %
            (pin_run.stderr.strip().splitlines()[-1:] or ["no stderr"])[0])
    require("NXEXTRACT PIN OK" in pin_run.stdout,
            "whole-bundle pin gate did not report PIN OK")

    suite = unittest.defaultTestLoader.discover(
        str(NX_ROOT / "tests"), pattern="test_nxextract.py"
    )
    # This was a whitelist of every count the suite had ever had, which grew by
    # one entry each time a test was added -- so the pin fired on the one
    # direction that is always welcome and had to be edited to say yes. What it
    # is for is catching regressions being REMOVED. 106 is the highest count
    # the audit closed on; the list of historical values carries no meaning.
    require(suite.countTestCases() >= 106,
            "M07 Python regression count fell below the audited 106")
    # A identidade canonica da UI e' o artefato RASTREADO em ui/release/ (com
    # manifesto), nunca um build local: exigir ui/build/ tornava o gate
    # dependente de estado do host -- verde onde sobrou um binario velho,
    # vermelho em toda worktree limpa (o proprio aviso no topo deste arquivo).
    manifest = load_json(NX_ROOT / "ui/release/manifest-v1.json")
    artifacts = manifest.get("artifacts")
    require(isinstance(artifacts, dict) and len(artifacts) >= 4,
            "UI release manifest lost architectures")
    for architecture, record in artifacts.items():
        released = NX_ROOT / "ui/release" / architecture / "nxextract-ui"
        require(released.is_file() and not released.is_symlink(),
                "released UI ELF is missing/linked: %s" % architecture)
        released_hash = hashlib.sha256(released.read_bytes()).hexdigest()
        require(released_hash == record.get("sha256"),
                "released UI no longer matches its manifest: %s" % architecture)
    require(artifacts.get("aarch64", {}).get("sha256") == EXPECTED_UI_SHA256,
            "canonical aarch64 UI drifted from the attested identity")
    ui = NX_ROOT / "ui/build/nxextract-ui"
    if ui.exists():
        require(ui.is_file() and not ui.is_symlink(),
                "local UI build is not a regular file")
        ui_hash = hashlib.sha256(ui.read_bytes()).hexdigest()
        accepted = {record.get("sha256") for record in artifacts.values()}
        # The 1.2.10 and 1.2.9 builds stay accepted for hosts that have not
        # rebuilt yet; the canonical release is the manifest set above.
        accepted.update((
            "7ca901d8515ab9a084be81e05888e1fd03cec80fb03896df6331c1c95698ef56",
            "046afb583f5a211c946495e639409f81d9cfec706788eeccb7924b0e8e5a50b6"))
        require(ui_hash in accepted,
                "local UI build no longer matches any attested binary: %s"
                % ui_hash)

    print("M07 audit gate passed: 20 requirements, %d Python cases "
          "(floor 106), release_device_evidence=0"
          % suite.countTestCases())


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, json.JSONDecodeError) as error:
        print("M07 audit gate failed: %s" % error)
        raise SystemExit(1)
