#!/usr/bin/env python3
"""Regenerate FP2's semantic NXExtract policy from legal local fixtures.

The output contains transformed paths, counts, and first-seen task decisions.
It never copies shader text, game data, container names, fixture paths, or
owner-content digests.
"""

import argparse
import hashlib
import json
import os
import re
import sys


OLD_DOMAIN = b"fp2-exact-gles2-audit-v3\0"
CHUNK = 1024 * 1024


def logical_bases(root):
    result = set()
    for directory, _directories, files in os.walk(root):
        for name in files:
            relative = os.path.relpath(os.path.join(directory, name), root)
            result.add(re.sub(r"\.split\d+$", "", relative).replace(os.sep, "/"))
    return sorted(result)


def logical_hash(root, relative):
    base = os.path.join(root, *relative.split("/"))
    if os.path.isfile(base):
        parts = [base]
    else:
        parts = []
        index = 0
        while os.path.isfile("%s.split%d" % (base, index)):
            parts.append("%s.split%d" % (base, index))
            index += 1
    if not parts:
        raise RuntimeError("missing logical asset " + relative)
    digest = hashlib.sha256()
    size = 0
    for path in parts:
        with open(path, "rb") as stream:
            for block in iter(lambda: stream.read(CHUNK), b""):
                digest.update(block)
                size += len(block)
    return size, digest.hexdigest()


def old_key(source, metadata, translator_hash, validator_hash):
    digest = hashlib.sha256()
    digest.update(OLD_DOMAIN)
    digest.update(bytes.fromhex(translator_hash))
    digest.update(bytes.fromhex(validator_hash))
    digest.update(source)
    digest.update(b"\0")
    digest.update(metadata.encode("utf-8"))
    return digest.hexdigest()


def collect_tasks(data_dir, relatives, shader_core, failed,
                  translator_hash, validator_hash):
    decisions = []
    decisions_by_identity = {}
    source_entries = 0
    shader_objects = 0
    native = 0
    old_seen = set()
    for relative in relatives:
        base = os.path.join(data_dir, *relative.split("/"))
        load_path = base if os.path.exists(base) else base + ".split0"
        try:
            environment = shader_core.UnityPy.load(load_path)
        except Exception:
            continue
        for obj in environment.objects:
            if obj.type.name != "Shader":
                continue
            shader_objects += 1
            tree = obj.read_typetree()
            platforms = list(tree["platforms"])
            if shader_core.PLATFORM_VULKAN not in platforms:
                continue
            if 5 in platforms and 9 in platforms:
                native += 1
                continue
            entries = shader_core.parse_entries(
                shader_core.platform_blob(tree, shader_core.PLATFORM_VULKAN))
            contexts = shader_core.blob_contexts(tree)
            for entry in entries:
                if not entry["source"]:
                    continue
                source_entries += 1
                context = contexts.get(entry["index"])
                if context is None:
                    raise RuntimeError("shader entry has no serialized context")
                metadata = shader_core.metadata_text(context[0], context[1])
                previous = old_key(entry["source"], metadata,
                                   translator_hash, validator_hash)
                old_seen.add(previous)
                decision = "O" if previous in failed else "T"
                identity = (entry["source"], metadata)
                known = decisions_by_identity.get(identity)
                if known is None:
                    decisions_by_identity[identity] = decision
                    decisions.append(decision)
                elif known != decision:
                    raise RuntimeError("one intrinsic task has conflicting decisions")
    if failed - old_seen:
        raise RuntimeError("translation report contains tasks absent from raw data")
    return "".join(decisions), {
        "shader_objects": shader_objects,
        "native_gles_shader_objects": native,
        "source_entries": source_entries,
        "unique_tasks": len(decisions),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw-assets", required=True)
    parser.add_argument("--approved-assets", required=True)
    parser.add_argument("--translation-report", required=True)
    parser.add_argument("--vendor-python", required=True)
    parser.add_argument("--lz4", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    os.environ["PF2_LZ4_LIBRARY"] = os.path.realpath(args.lz4)
    sys.path.insert(0, os.path.realpath(args.vendor_python))
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import shader_core

    with open(args.translation_report, "r", encoding="utf-8") as stream:
        report = json.load(stream)
    raw_data = os.path.join(args.raw_assets, "bin", "Data")
    approved_data = os.path.join(args.approved_assets, "bin", "Data")
    raw_bases = logical_bases(raw_data)
    if raw_bases != logical_bases(approved_data):
        raise RuntimeError("raw and approved logical asset names differ")
    outputs = []
    for relative in raw_bases:
        before = logical_hash(raw_data, relative)
        after = logical_hash(approved_data, relative)
        if before != after:
            outputs.append({"path": "assets/bin/Data/" + relative})
    if len(outputs) != 31:
        raise RuntimeError("expected 31 changed logical assets, found %d" % len(outputs))
    if "assets/bin/Data/globalgamemanagers" not in {item["path"] for item in outputs}:
        raise RuntimeError("BuildSettings output is absent")

    failed = set(report["translation_failures"])
    runtime_relatives = [
        item["path"].split("assets/bin/Data/", 1)[1]
        for item in sorted(outputs, key=lambda item: item["path"])
        if item["path"] != "assets/bin/Data/globalgamemanagers"
    ]
    sequence, task_stats = collect_tasks(
        raw_data, runtime_relatives, shader_core, failed,
        report["translator_sha256"], report["glslang_sha256"])
    if task_stats != {
            "shader_objects": 83, "native_gles_shader_objects": 0,
            "source_entries": 959, "unique_tasks": 702}:
        raise RuntimeError("unexpected transformed shader corpus %r" % task_stats)
    if sequence.count("T") != 411 or sequence.count("O") != 291:
        raise RuntimeError("unexpected task decision counts")

    policy = {
        "format": 2,
        "package": "com.GalaxyTrail.FP2",
        "preparation": "fp2-vulkan-to-exact-gles2-v1",
        "translator": {"path": "nxextract/bin/fp2-smolv-cross-nextos"},
        "lz4": {"path": "nxextract/lib/aarch64/liblz4.so.1"},
        "task_decisions": {
            "contract": "first-seen-unique-source-metadata-v1",
            "omit_token": "O",
            "repeat": "reuse-first-decision-and-cache-key",
            "sequence": sequence,
            "translate_token": "T",
        },
        "logical_outputs": sorted(outputs, key=lambda item: item["path"]),
        "expected_stats": {
            "exact_entries": 548,
            "files": 30,
            "installed_records": 2736,
            "omitted_records": 1986,
            "shader_objects": 80,
            "skipped_entries": 411,
            "tasks_accepted": 411,
            "tasks_seen": 702,
            "tasks_skipped": 291,
        },
    }
    with open(args.output, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(policy, stream, ensure_ascii=True, indent=2, sort_keys=True)
        stream.write("\n")
    print("wrote semantic policy: 31 outputs, 411 translate, 291 omit")


if __name__ == "__main__":
    main()
