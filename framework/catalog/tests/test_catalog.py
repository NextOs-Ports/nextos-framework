#!/usr/bin/env python3
"""Fail closed when the evidence catalog overstates support or loses an entry."""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
CATALOG_PATH = ROOT / "framework/catalog/ports-v1.json"
CHECKS_PATH = ROOT / "framework/catalog/port-check-definitions-v1.json"
EXPANSION_PATH = ROOT / "framework/catalog/port-checks-v1.tsv"
LOCAL_PORTS_PATH = ROOT / "framework/catalog/local-port-directories-v1.txt"
ABI_DEFINITIONS_PATH = ROOT / "framework/catalog/abi-check-definitions-v1.json"
ABI_VARIANTS_PATH = ROOT / "framework/catalog/abi-variants-v1.json"
ABI_EXPANSION_PATH = ROOT / "framework/catalog/abi-checks-v1.tsv"

PUBLIC_IDS = {
    "castleofillusion",
    "geometrydash",
    "geometrydash_subzero",
    "hitmango",
    "horizonchase",
    "kotor",
    "oceanhorn",
    "partyboard",
    "pikmin",
    "prizefighters2",
    "sonic4ep2",
    "sor4",
    "stardewvalley",
    "summertimesaga",
    "tasm2_127",
    "terraria",
    "tightrope",
}

REQUIRED_MISSION_IDS = PUBLIC_IDS | {
    "bully2",
    "dysmantle",
    "limbo_local",
    "chrono_pilot",
    "gta3",
    "gtasa",
    "gtasaf",
    "gtavc",
    "gtactw",
    "lcs_legacy",
    "gtalcs2",
    "gta3de",
}

NON_UNIVERSAL_IDS = {
    "partyboard",
    "pikmin",
    "bully2",
    "dysmantle",
    "limbo_local",
    "chrono_pilot",
    "gta3",
    "gtasa",
    "gtasaf",
    "gtavc",
    "gtactw",
    "lcs_legacy",
    "gtalcs2",
    "gta3de",
}

ALLOWED_LEVELS = {
    "physical-full",
    "physical-subsystem",
    "community-confirmed",
    "release-fix",
    "designed-only",
    "negative-wip",
}


def fail(message: str) -> None:
    raise AssertionError(message)


def lookup(value, dotted_path):
    for component in dotted_path.split("."):
        if not isinstance(value, dict) or component not in value:
            fail(f"missing ABI field: {dotted_path}")
        value = value[component]
    return value


def compact(value):
    if isinstance(value, str):
        return value.replace("\t", " ").replace("\r", " ").replace("\n", " ")
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"))


def main() -> int:
    catalog_text = CATALOG_PATH.read_text(encoding="utf-8")
    catalog = json.loads(catalog_text)
    definitions = json.loads(CHECKS_PATH.read_text(encoding="utf-8"))

    entries = catalog["entries"]
    by_id = {entry["canonical_id"]: entry for entry in entries}
    if len(entries) != 29 or len(by_id) != 29:
        fail("catalog must contain exactly 29 unique entries")
    if set(by_id) != REQUIRED_MISSION_IDS:
        fail(f"catalog ID drift: {sorted(set(by_id) ^ REQUIRED_MISSION_IDS)}")

    current_public = {
        entry_id
        for entry_id, entry in by_id.items()
        if entry["classification"]["publication"] == "current-public"
    }
    if current_public != PUBLIC_IDS:
        fail(f"current-public set drift: {sorted(current_public ^ PUBLIC_IDS)}")

    listed_local_ports = {
        line
        for line in LOCAL_PORTS_PATH.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    }
    # Inventário de presença = diretórios de port RASTREADOS pelo git.
    # `iterdir()` misturava scratch dirs locais não rastreados e derrubava a
    # bateria em qualquer checkout de trabalho; contagem cravada quebrava a
    # cada port novo. O invariante real é: lista == conjunto rastreado.
    tracked = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "--", "ports/"],
        check=True, capture_output=True, text=True).stdout
    actual_local_ports = {
        line.split("/", 2)[1]
        for line in tracked.splitlines()
        if line.startswith("ports/") and line.count("/") >= 2
    }
    if not listed_local_ports or not actual_local_ports:
        fail("local port inventory is empty (listing or git tracking)")
    if listed_local_ports != actual_local_ports:
        fail(f"local port inventory drift: {sorted(listed_local_ports ^ actual_local_ports)}")

    # Taxonomia de ports (onda pos-v2, pedido do dono): TODO port rastreado
    # tem entrada com status/engine validos, e ports/INDEX.md e' o retrato
    # exato da taxonomia -- e' o gate que mantem o indice SEMPRE atualizado.
    taxonomy_path = ROOT / "framework/catalog/port-taxonomy-v1.json"
    taxonomy = json.loads(taxonomy_path.read_text(encoding="utf-8"))
    tax_ports = taxonomy.get("ports", {})
    allowed_status = {"publicado", "completo", "jogavel", "em-andamento",
                      "parado", "nogo", "nao-triado"}
    allowed_integration = {"so-loader", "standalone", "runtime-web",
                           "recomp", "desconhecido"}
    if set(tax_ports) != actual_local_ports:
        fail("port taxonomy drift: %s"
             % sorted(set(tax_ports) ^ actual_local_ports))
    for slug, entry in tax_ports.items():
        if entry.get("status") not in allowed_status:
            fail("taxonomy status invalido em %s: %r"
                 % (slug, entry.get("status")))
        if entry.get("integration") not in allowed_integration:
            fail("taxonomy integration invalida em %s: %r"
                 % (slug, entry.get("integration")))
        if not isinstance(entry.get("engine"), str) or not entry["engine"]:
            fail("taxonomy engine ausente em %s" % slug)
    index_check = subprocess.run(
        [sys.executable, "-B",
         str(ROOT / "framework/nxrelease/nx-port-status.py"),
         "--repo-root", str(ROOT), "--check-index"],
        capture_output=True, text=True)
    if index_check.returncode != 0:
        fail("ports/INDEX.md desatualizado: %s"
             % index_check.stderr.strip()[:200])

    devices = json.loads(
        (ROOT / "framework" / "catalog" / "devices-v1.json").read_text(
            encoding="utf-8"))
    if devices.get("schema_version") != 1:
        fail("devices-v1.json schema_version changed")
    if "NEVER a sufficient condition" not in devices.get("meaning", ""):
        fail("devices-v1.json lost its no-name-selection rule")
    device_levels = set(devices.get("evidence_levels", []))
    families = devices.get("families", [])
    if len(families) != 6:
        fail("devices-v1.json family count drifted")
    family_fields = {"id", "firmware", "arch", "gpu", "display",
                     "gles_ceiling", "resolution_facts", "audio",
                     "evidence", "evidence_ports"}
    for family in families:
        if set(family) != family_fields:
            fail(f"devices-v1.json family fields drifted: {family.get('id')}")
        if family["evidence"] not in device_levels:
            fail(f"devices-v1.json evidence level unknown: {family.get('id')}")

    for entry_id in PUBLIC_IDS:
        digest = by_id[entry_id]["release"]["sha256"]
        if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            fail(f"invalid public ZIP SHA-256 for {entry_id}: {digest}")
        if not by_id[entry_id]["source"]["remote_url"].startswith(
            "https://github.com/NextOs-Ports/"
        ):
            fail(f"unexpected public source URL for {entry_id}")

    for entry_id in NON_UNIVERSAL_IDS:
        scope = by_id[entry_id]["classification"]["support_scope"]
        if scope.startswith("multi-device"):
            fail(f"non-universal entry was promoted: {entry_id} -> {scope}")

    for entry_id, entry in by_id.items():
        classification = entry["classification"]
        if classification["support_scope"] == "negative-wip" and classification["positive_reference"]:
            fail(f"negative WIP marked positive: {entry_id}")
        generalization = entry["generalization"]
        if not generalization.get("allowed") or not generalization.get("forbidden"):
            fail(f"missing generalization boundary: {entry_id}")
        for record in entry["evidence"]["physical_stacks"]:
            if record["level"] not in ALLOWED_LEVELS:
                fail(f"unknown evidence level in {entry_id}: {record['level']}")
        local_locator = entry["source"]["local_path"]
        if local_locator.startswith("external:"):
            if (not entry["source"]["remote_url"].startswith(
                    "https://github.com/NextOs-Ports/") or
                    not re.fullmatch(r"[0-9a-f]{40}",
                                     entry["source"]["snapshot"])):
                fail(f"unverifiable external source for {entry_id}")
            continue
        local_path = Path(local_locator)
        resolved_path = local_path if local_path.is_absolute() else ROOT / local_path
        if not resolved_path.exists():
            fail(f"missing local source path for {entry_id}: {resolved_path}")

    if by_id["limbo_local"]["classification"]["positive_reference"]:
        fail("Limbo WIP must stay negative-only")
    if "ports/tasm2" in json.dumps(by_id["tasm2_127"]["source"], sort_keys=True):
        fail("abandoned ports/tasm2 must not be the positive source")
    private_ip = re.compile(
        r"\b(?:10|127)\.(?:\d{1,3}\.){2}\d{1,3}\b"
        r"|\b192\.168\.(?:\d{1,3}\.)\d{1,3}\b"
        r"|\b172\.(?:1[6-9]|2\d|3[01])\.(?:\d{1,3}\.)\d{1,3}\b"
    )
    if private_ip.search(catalog_text):
        fail("catalog must not contain private/test IP addresses")

    checks = definitions["checks"]
    if definitions["checks_per_entry"] != 42 or len(checks) != 42:
        fail("the PORT matrix must stay at exactly 42 checks")
    if len({item["id"] for item in checks}) != 42:
        fail("duplicate PORT check ID")

    lines = EXPANSION_PATH.read_text(encoding="utf-8").splitlines()
    if len(lines) != 1 + 29 * 42:
        fail(f"expected 1219 TSV lines, got {len(lines)}")
    if lines[0] != "id\tentry\tcheck\tcategory\tstatus\tvalue":
        fail("unexpected TSV header")
    if len({line.split("\t", 1)[0] for line in lines[1:]}) != 29 * 42:
        fail("duplicate expanded PORT ID")

    abi_definitions = json.loads(
        ABI_DEFINITIONS_PATH.read_text(encoding="utf-8"))
    abi_catalog_text = ABI_VARIANTS_PATH.read_text(encoding="utf-8")
    abi_variants = json.loads(abi_catalog_text)
    abi_checks = abi_definitions.get("checks", [])
    if (abi_definitions.get("checks_per_variant") != 28 or
            len(abi_checks) != 28 or
            len({item.get("id") for item in abi_checks}) != 28):
        fail("the ABI matrix must stay at exactly 28 unique checks")
    variants = abi_variants.get("variants", [])
    by_variant = {item.get("variant_id"): item for item in variants}
    expected_variants = {
        "bully2-aarch64",
        "horizonchase-aarch64",
        "kotor-armv7",
        "sonic4ep2-aarch64",
        "tasm2_127-armv7",
    }
    if set(by_variant) != expected_variants:
        fail(f"M09/M10 ABI variant drift: {sorted(set(by_variant))}")
    for variant in variants:
        port = variant["canonical_port"]
        if (variant.get("abi") not in {"armv7", "aarch64"} or
                variant.get("positive_reference") is not True or
                port not in by_id or
                by_id[port]["classification"]["positive_reference"] is not True):
            fail(f"invalid positive ABI variant: {variant.get('variant_id')}")
        if variant["abi"] == "aarch64":
            required = (
                ("guest.elf_class", "ELF64"),
                ("guest.elf_class", "LP64"),
                ("guest.machine", "EM_AARCH64"),
                ("guest.machine", "e_flags=0"),
                ("guest.eabi", "e_flags=0"),
                ("guest.float_abi", "AAPCS64"),
                ("guest.pcs_boundary", "16 bytes"),
                ("guest.interpreter_scope", "no PT_INTERP"),
                ("relocations.format", "ELF64 RELA"),
                ("host.pt_interp", "/lib/ld-linux-aarch64.so.1"),
                ("host.glibc_ceiling", "GLIBC_2.30"),
                ("gates.initializers", "guest_initializers_executed=0"),
                ("gates.initializers", "guest_jni_onload_executed=0"),
            )
            for path, token in required:
                if token not in str(lookup(variant, path)):
                    fail(f"AArch64 invariant missing in {variant['variant_id']}: {path} -> {token}")
    if "ports/tasm2/" in abi_catalog_text or private_ip.search(abi_catalog_text):
        fail("ABI variants contain a negative source path or private address")

    abi_lines = ABI_EXPANSION_PATH.read_text(encoding="utf-8").splitlines()
    if len(abi_lines) != 1 + 5 * 28:
        fail(f"expected 141 ABI TSV lines, got {len(abi_lines)}")
    header = "id\tport\tabi\tcheck\tcategory\tstatus\tvalue"
    if abi_lines[0] != header:
        fail("unexpected ABI TSV header")
    expected_abi_lines = []
    for variant in sorted(variants, key=lambda item: item["variant_id"]):
        for check in abi_checks:
            row_id = "ABI-%s-%s-%s" % (
                variant["canonical_port"], variant["abi"], check["id"])
            expected_abi_lines.append("\t".join((
                row_id, variant["canonical_port"], variant["abi"],
                check["id"], check["category"], "recorded",
                compact(lookup(variant, check["path"])),
            )))
    if abi_lines[1:] != expected_abi_lines:
        fail("ABI TSV is stale or not deterministic")
    if len({line.split("\t", 1)[0] for line in abi_lines[1:]}) != 5 * 28:
        fail("duplicate expanded ABI ID")

    for path in (CATALOG_PATH, CHECKS_PATH, EXPANSION_PATH, LOCAL_PORTS_PATH,
                 ABI_DEFINITIONS_PATH, ABI_VARIANTS_PATH, ABI_EXPANSION_PATH):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if line.endswith((" ", "\t")):
                fail(f"trailing whitespace: {path}:{number}")

    print("catalog_entries=29 public_packages=17 port_checks=42 "
          "port_rows=1218 armv7_variants=2 aarch64_variants=3 "
          "abi_checks=28 abi_rows=140 status=PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, KeyError, json.JSONDecodeError) as error:
        print(f"catalog audit failed: {error}", file=sys.stderr)
        raise SystemExit(1)
