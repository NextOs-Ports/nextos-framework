#!/usr/bin/env python3
"""Planejamento offline de payloads 2D; não converte, não mede RSS nem aprova GL."""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

SCHEMA = "unity-texture-plan/1"
MAX_INPUT = 8 * 1024 * 1024
FORMATS = {"RGBA8888": 4, "RGB888": 3, "RGB565": 2, "RGBA4444": 2,
           "A8": 1, "RGBA16F": 8, "ETC1": None, "ETC1_DUAL": None}
ROLES = {"color", "normal", "data", "depth", "render_target", "font_sdf",
         "video", "unknown"}
ALPHAS = {"opaque", "blend", "cutout", "premultiplied", "unknown"}


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError("inteiro fora do contrato")
    return value


def dimensions(width, height, levels):
    width, height = integer(width, 1, 65536), integer(height, 1, 65536)
    maximum = max(width, height).bit_length()
    count = maximum if levels == "full" else integer(levels, 1, maximum)
    return [(max(1, width >> n), max(1, height >> n)) for n in range(count)]


def payload(fmt, dims):
    if fmt == "ETC1":
        return sum(((w + 3) // 4) * ((h + 3) // 4) * 8 for w, h in dims)
    if fmt == "ETC1_DUAL":
        return 2 * payload("ETC1", dims)
    return sum(w * h * FORMATS[fmt] for w, h in dims)


def texture(item):
    required = {"id", "width", "height", "levels", "source_format", "role",
                "alpha", "dynamic"}
    if not isinstance(item, dict) or set(item) != required:
        raise ValueError("campos da textura divergentes")
    if not isinstance(item["id"], str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,64}", item["id"]):
        raise ValueError("identificador deve ser um alias simples")
    for key, allowed in (("source_format", FORMATS), ("role", ROLES), ("alpha", ALPHAS)):
        if not isinstance(item[key], str) or item[key] not in allowed:
            raise ValueError("enum de textura inválido")
    if type(item["dynamic"]) is not bool:
        raise ValueError("dynamic deve ser booleano")
    dims = dimensions(item["width"], item["height"], item["levels"])
    sizes = {fmt: payload(fmt, dims) for fmt in FORMATS}
    source = item["source_format"]
    candidate = None
    if source in {"ETC1", "ETC1_DUAL"}:
        decision = "KEEP_EXISTING_COMPRESSED_CONTRACT"
    elif item["role"] != "color":
        decision = "REQUIRES_ROLE_SPECIFIC_ANALYSIS"
    elif source == "RGBA16F":
        decision = "REQUIRES_HDR_RANGE_ANALYSIS"
    elif source == "A8":
        decision = "REQUIRES_CHANNEL_CONTRACT_ANALYSIS"
    elif item["dynamic"]:
        decision = "REQUIRES_UPDATE_PATH_ANALYSIS"
    elif item["alpha"] == "unknown":
        decision = "REQUIRES_ALPHA_ANALYSIS"
    elif source in {"RGB888", "RGB565"} and item["alpha"] != "opaque":
        decision = "REQUIRES_EXTERNAL_ALPHA_SOURCE_ANALYSIS"
    else:
        candidate = "ETC1" if item["alpha"] == "opaque" else "ETC1_DUAL"
        if sizes[candidate] >= sizes[source]:
            decision = "NO_PAYLOAD_SAVING"
            candidate = None
        else:
            decision = "CANDIDATE_REQUIRES_SHADER_AND_VISUAL_PROOF"
    selected = candidate or source
    before, after = sizes[source], sizes[selected]
    return {
        "id": item["id"], "decision": decision, "candidate_format": candidate,
        "source_format": source, "levels": len(dims),
        "mip_dimensions": dims, "payload_comparison_bytes": sizes,
        "source_payload_bytes": before, "planned_payload_bytes": after,
        "potential_saving_bytes": before - after,
        "potential_saving_percent": round(100 * (before - after) / before, 3),
        "fragment_samplers_for_one_sampled_image": 2 if selected == "ETC1_DUAL" else 1,
        "full_chain_rgba8888_staging_if_materialized_bytes": sizes["RGBA8888"],
        "runtime_validated": False,
    }


def plan(data):
    if not isinstance(data, dict) or set(data) != {"schema", "kind", "textures"}:
        raise ValueError("documento divergente")
    if data["schema"] != SCHEMA or data["kind"] not in {"synthetic", "inventory"}:
        raise ValueError("schema/kind divergente")
    items = data["textures"]
    if not isinstance(items, list) or not 1 <= len(items) <= 10000:
        raise ValueError("lista de texturas inválida")
    rows = [texture(item) for item in items]
    if len({row["id"] for row in rows}) != len(rows):
        raise ValueError("identificador de textura duplicado")
    before = sum(row["source_payload_bytes"] for row in rows)
    after = sum(row["planned_payload_bytes"] for row in rows)
    return {
        "schema": SCHEMA, "kind": data["kind"], "textures": rows,
        "totals": {"source_payload_bytes": before, "planned_payload_bytes": after,
                   "potential_saving_bytes": before - after,
                   "potential_saving_percent": round(100 * (before - after) / before, 3)},
        "assumption": "ALL_LISTED_IMAGES_SIMULTANEOUSLY_RESIDENT_ONCE",
        "gpu_driver_allocation_bytes": "NOT_MEASURED",
        "rss_bytes": "NOT_MEASURED", "conversion_peak_bytes": "NOT_MEASURED",
        "fps_change": "NOT_MEASURED", "physical_validation": "NOT_ESTABLISHED",
        "action": "PLAN_ONLY_NO_CONVERSION",
    }


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("chave JSON duplicada")
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inventory", type=Path, help="JSON explícito; saída em stdout")
    args = parser.parse_args()
    try:
        if not args.inventory.is_file():
            raise ValueError("entrada deve ser arquivo regular")
        with args.inventory.open("rb") as stream:
            raw = stream.read(MAX_INPUT + 1)
        if len(raw) > MAX_INPUT:
            raise ValueError("entrada maior que 8 MiB")
        report = plan(json.loads(raw, object_pairs_hook=unique_keys))
        report["input_sha256"] = hashlib.sha256(raw).hexdigest()
    except (OSError, ValueError, TypeError, RecursionError):
        print("Entrada inválida: confira schema, campos, limites e acesso ao JSON.", file=sys.stderr)
        return 2
    print(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
