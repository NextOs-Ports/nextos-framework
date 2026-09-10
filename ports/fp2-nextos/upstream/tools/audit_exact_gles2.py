#!/usr/bin/env python3
"""Audit exact Unity Vulkan/SMOL-V -> GLES2 translation for every shader entry.

This tool is deliberately read-only with respect to the game data.  It builds
and validates ESSL100 in a cache outside the port, then emits one report with
all incompatibilities instead of discovering them one device launch at a time.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import threading

import lz4.block
import UnityPy


GPU_SPIRV = 25
PLATFORM_VULKAN = 18

CHANNEL_NAMES = {
    0: "in_POSITION0",
    1: "in_NORMAL0",
    2: "in_TANGENT0",
    3: "in_COLOR0",
    4: "in_TEXCOORD0",
    5: "in_TEXCOORD1",
    6: "in_TEXCOORD2",
    7: "in_TEXCOORD3",
    8: "in_TEXCOORD4",
    9: "in_TEXCOORD5",
    10: "in_TEXCOORD6",
    11: "in_TEXCOORD7",
    12: "in_BLENDWEIGHT0",
    13: "in_BLENDINDICES0",
}

STRUCT_UNIFORM = re.compile(
    r"struct\s+(?P<type>[A-Za-z_]\w*)\s*\{(?P<body>.*?)\};\s*"
    r"uniform\s+(?P=type)\s+(?P<instance>[A-Za-z_]\w*)\s*;",
    re.DOTALL,
)


def u32(blob, offset):
    return struct.unpack_from("<I", blob, offset)[0]


def align4(value):
    return (value + 3) & ~3


def parse_entries(blob):
    count = u32(blob, 0)
    if 4 + 8 * count > len(blob):
        raise ValueError("invalid shader entry table")
    result = []
    for index in range(count):
        offset, size = struct.unpack_from("<II", blob, 4 + 8 * index)
        entry = blob[offset:offset + size]
        if len(entry) != size or len(entry) < 32:
            raise ValueError("truncated shader entry %d" % index)
        cursor = 24
        keyword_count = u32(entry, cursor)
        cursor += 4
        keywords = []
        for _ in range(keyword_count):
            length = u32(entry, cursor)
            cursor += 4
            keywords.append(entry[cursor:cursor + length].decode("utf-8",
                                                                  "replace"))
            cursor = align4(cursor + length)
        source_length = u32(entry, cursor)
        cursor += 4
        source = entry[cursor:cursor + source_length]
        if len(source) != source_length:
            raise ValueError("truncated source in shader entry %d" % index)
        result.append({"index": index, "keywords": keywords, "source": source})
    return result


def bases_under(data_dir):
    bases = set()
    for root, _dirs, files in os.walk(data_dir):
        for name in files:
            if name.endswith((".resource", ".config", ".dll", ".xml")):
                continue
            bases.add(re.sub(r"\.split\d+$", "", os.path.join(root, name)))
    return sorted(bases)


def platform_blob(tree, platform):
    platforms = list(tree["platforms"])
    index = platforms.index(platform)
    offset = tree["offsets"][index]
    packed_length = tree["compressedLengths"][index]
    raw_length = tree["decompressedLengths"][index]
    packed = bytes(tree["compressedBlob"])[offset:offset + packed_length]
    raw = lz4.block.decompress(packed, uncompressed_size=raw_length)
    if len(raw) != raw_length:
        raise ValueError("wrong decompressed Vulkan blob size")
    return raw


def context_score(subprogram):
    return (100 * len(subprogram.get("m_ConstantBuffers", [])) +
            50 * len(subprogram.get("m_TextureParams", [])) +
            20 * len(subprogram.get("m_VectorParams", [])) +
            20 * len(subprogram.get("m_MatrixParams", [])) +
            10 * len(subprogram.get("m_Channels", {}).get("m_Channels", [])) +
            len(subprogram.get("m_ConstantBufferBindings", [])))


def blob_contexts(tree):
    contexts = {}
    for subshader in tree["m_ParsedForm"]["m_SubShaders"]:
        for shader_pass in subshader["m_Passes"]:
            for stage in ("progVertex", "progFragment", "progGeometry",
                          "progHull", "progDomain"):
                for subprogram in shader_pass.get(stage, {}).get(
                        "m_SubPrograms", []):
                    if subprogram["m_GpuProgramType"] != GPU_SPIRV:
                        continue
                    blob_index = subprogram["m_BlobIndex"]
                    candidate = (shader_pass, subprogram, stage)
                    previous = contexts.get(blob_index)
                    if previous is None or context_score(subprogram) > context_score(
                            previous[1]):
                        contexts[blob_index] = candidate
    return contexts


def names_by_index(shader_pass):
    raw = shader_pass.get("m_NameIndices", [])
    if isinstance(raw, dict):
        return {int(index): name for name, index in raw.items()}
    return {int(index): name for name, index in raw}


def descriptor_binding(encoded):
    return (int(encoded) >> 16) & 0xff, int(encoded) & 0xffff


def matrix_name(name, row_count):
    if name.startswith("hlslcc_mtx"):
        return name
    return "hlslcc_mtx%dx%d%s" % (row_count, row_count, name)


def metadata_text(shader_pass, subprogram):
    names = names_by_index(shader_pass)
    lines = []

    for channel in subprogram["m_Channels"]["m_Channels"]:
        source = int(channel["source"])
        target = int(channel["target"])
        if source not in CHANNEL_NAMES:
            raise ValueError("unknown Unity vertex channel %d" % source)
        location = target - 13
        if location < 0:
            raise ValueError("invalid Vulkan vertex target %d" % target)
        lines.append(("input", location, CHANNEL_NAMES[source]))

    buffers = {int(buffer["m_NameIndex"]): buffer
               for buffer in subprogram["m_ConstantBuffers"]}
    for binding in subprogram["m_ConstantBufferBindings"]:
        name_index = int(binding["m_NameIndex"])
        if name_index not in names or name_index not in buffers:
            raise ValueError("constant buffer has no resolved name/body")
        descriptor_set, bind_point = descriptor_binding(binding["m_Index"])
        buffer = buffers[name_index]
        lines.append(("cb", descriptor_set, bind_point, names[name_index]))
        for vector in buffer["m_VectorParams"]:
            member_index = int(vector["m_NameIndex"])
            if member_index not in names:
                raise ValueError("unresolved constant-buffer vector name")
            lines.append(("member", descriptor_set, bind_point,
                          int(vector["m_Index"]), "vector", names[member_index]))
        for matrix in buffer["m_MatrixParams"]:
            member_index = int(matrix["m_NameIndex"])
            if member_index not in names:
                raise ValueError("unresolved constant-buffer matrix name")
            lines.append(("member", descriptor_set, bind_point,
                          int(matrix["m_Index"]), "matrix",
                          matrix_name(names[member_index],
                                      int(matrix["m_RowCount"]))))
        if buffer.get("m_StructParams"):
            raise ValueError("constant-buffer structs are not mapped yet")

    direct = []
    for vector in subprogram["m_VectorParams"]:
        name_index = int(vector["m_NameIndex"])
        if name_index not in names:
            raise ValueError("unresolved direct vector name")
        direct.append((int(vector["m_Index"]), "vector", names[name_index]))
    for matrix in subprogram["m_MatrixParams"]:
        name_index = int(matrix["m_NameIndex"])
        if name_index not in names:
            raise ValueError("unresolved direct matrix name")
        direct.append((int(matrix["m_Index"]), "matrix",
                       matrix_name(names[name_index], int(matrix["m_RowCount"]))))
    for order, (_encoded, kind, name) in enumerate(sorted(direct)):
        lines.append(("direct", order, kind, name))

    for texture in subprogram["m_TextureParams"]:
        name_index = int(texture["m_NameIndex"])
        if name_index not in names:
            raise ValueError("unresolved texture name")
        descriptor_set, bind_point = descriptor_binding(texture["m_Index"])
        lines.append(("texture", descriptor_set, bind_point, names[name_index]))

    if subprogram["m_BufferParams"] or subprogram["m_UAVParams"]:
        raise ValueError("storage buffer/UAV metadata is not GLES2-compatible")

    # Stable ordering gives stable cache keys while retaining one line per
    # original field for easy auditing.
    return "".join("\t".join(map(str, line)) + "\n"
                   for line in sorted(set(lines), key=lambda value: tuple(map(str, value))))


def flatten_struct_uniforms(source):
    while True:
        match = STRUCT_UNIFORM.search(source)
        if match is None:
            break
        declarations = []
        for raw in match.group("body").splitlines():
            declaration = raw.strip()
            if not declaration:
                continue
            if not declaration.endswith(";"):
                raise ValueError("unexpected UBO member: %s" % declaration)
            declarations.append("uniform %s" % declaration)
        source = (source[:match.start()] + "\n".join(declarations) +
                  source[match.end():])
        source = source.replace(match.group("instance") + ".", "")
    if "UnityCB_" in source or re.search(r"\bstruct\s+UnityType_", source):
        raise ValueError("an emitted uniform buffer was not flattened")
    return source


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as src:
        for block in iter(lambda: src.read(1024 * 1024), b""):
            digest.update(block)
    return digest.digest()


class Translator:
    def __init__(self, executable, cache_dir, glslang):
        resolved_translator = (shutil.which(executable) if os.sep not in executable
                               else executable)
        resolved_glslang = (shutil.which(glslang) if os.sep not in glslang
                            else glslang)
        if not resolved_translator or not os.path.isfile(resolved_translator):
            raise ValueError("translator executable was not found: %s" % executable)
        if not resolved_glslang or not os.path.isfile(resolved_glslang):
            raise ValueError("glslangValidator was not found: %s" % glslang)
        self.executable = os.path.realpath(resolved_translator)
        self.cache_dir = os.path.realpath(cache_dir)
        self.glslang = os.path.realpath(resolved_glslang)
        self.tool_hash = sha256_file(self.executable)
        self.validator_hash = sha256_file(self.glslang)
        os.makedirs(self.cache_dir, exist_ok=True)

    def key(self, source, metadata):
        digest = hashlib.sha256()
        digest.update(b"fp2-exact-gles2-audit-v3\0")
        digest.update(self.tool_hash)
        digest.update(self.validator_hash)
        digest.update(source)
        digest.update(b"\0")
        digest.update(metadata.encode())
        return digest.hexdigest()

    def translate(self, task):
        key = task["key"]
        directory = os.path.join(self.cache_dir, key[:2], key)
        final_vertex = os.path.join(directory, "vertex.final.vert")
        final_fragment = os.path.join(directory, "fragment.final.frag")
        validated = os.path.join(directory, "validated.ok")
        if (os.path.exists(final_vertex) and os.path.exists(final_fragment) and
                os.path.exists(validated)):
            with open(validated, encoding="ascii") as marker:
                if marker.read() == key + "\n":
                    return {"key": key, "cached": True,
                            "vertex": final_vertex,
                            "fragment": final_fragment}

        os.makedirs(directory, exist_ok=True)
        if os.path.exists(validated):
            os.unlink(validated)
        source_path = os.path.join(directory, "source.bin")
        meta_path = os.path.join(directory, "metadata.tsv")
        vertex_path = os.path.join(directory, "vertex.glsl")
        fragment_path = os.path.join(directory, "fragment.glsl")
        with open(source_path, "wb") as out:
            out.write(task["source"])
        with open(meta_path, "w", encoding="utf-8", newline="\n") as out:
            out.write(task["metadata"])

        process = subprocess.run(
            [self.executable, source_path, meta_path, vertex_path, fragment_path],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if process.returncode:
            raise RuntimeError(process.stderr.strip() or "translator failed")

        for stage, source_file, final_file in (
                ("vert", vertex_path, final_vertex),
                ("frag", fragment_path, final_fragment)):
            with open(source_file, encoding="utf-8") as src:
                glsl = flatten_struct_uniforms(src.read())
            with open(final_file, "w", encoding="utf-8", newline="\n") as out:
                out.write(glsl)
            validation = subprocess.run(
                [self.glslang, "-S", stage, final_file], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if validation.returncode:
                raise RuntimeError("%s validation: %s" %
                                   (stage, validation.stdout.strip()))
        link_validation = subprocess.run(
            [self.glslang, "-l", final_vertex, final_fragment], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if link_validation.returncode:
            raise RuntimeError("program link validation: %s" %
                               link_validation.stdout.strip())
        temporary_marker = "%s.tmp.%d.%d" % (validated, os.getpid(),
                                               threading.get_ident())
        with open(temporary_marker, "w", encoding="ascii", newline="\n") as marker:
            marker.write(key + "\n")
        os.replace(temporary_marker, validated)
        return {"key": key, "cached": False,
                "vertex": final_vertex, "fragment": final_fragment}


def collect_tasks(data_dir, translator):
    tasks = {}
    uses = {}
    shaders = 0
    native_gles = 0
    source_entries = 0
    collection_errors = []
    for base in bases_under(data_dir):
        load_path = base if os.path.exists(base) else base + ".split0"
        try:
            env = UnityPy.load(load_path)
        except Exception as exc:  # noqa: BLE001
            collection_errors.append({"asset": os.path.relpath(base, data_dir),
                                      "error": "load: %s" % exc})
            continue
        for obj in env.objects:
            if obj.type.name != "Shader":
                continue
            shaders += 1
            try:
                tree = obj.read_typetree()
                platforms = list(tree["platforms"])
                if PLATFORM_VULKAN not in platforms:
                    continue
                # The five shaders carrying Unity's own GLES20+GLES3x+Vulkan
                # programs are positive templates, not translation targets.
                if 5 in platforms and 9 in platforms:
                    native_gles += 1
                    continue
                entries = parse_entries(platform_blob(tree, PLATFORM_VULKAN))
                contexts = blob_contexts(tree)
                shader_name = tree["m_ParsedForm"]["m_Name"]
                for entry in entries:
                    if not entry["source"]:
                        continue
                    source_entries += 1
                    context = contexts.get(entry["index"])
                    if context is None:
                        raise ValueError("entry %d has no serialized subprogram"
                                         % entry["index"])
                    metadata = metadata_text(context[0], context[1])
                    key = translator.key(entry["source"], metadata)
                    tasks.setdefault(key, {"key": key,
                                           "source": entry["source"],
                                           "metadata": metadata})
                    uses.setdefault(key, []).append({
                        "asset": os.path.relpath(base, data_dir),
                        "path_id": obj.path_id,
                        "shader": shader_name,
                        "entry": entry["index"],
                        "keywords": entry["keywords"],
                    })
            except Exception as exc:  # noqa: BLE001
                collection_errors.append({
                    "asset": os.path.relpath(base, data_dir),
                    "path_id": obj.path_id,
                    "error": str(exc),
                })
    return tasks, uses, {
        "shader_objects": shaders,
        "native_gles_shader_objects": native_gles,
        "source_entries": source_entries,
        "unique_tasks": len(tasks),
        "collection_errors": collection_errors,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir")
    ap.add_argument("report")
    ap.add_argument("--translator", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--glslang", default="glslangValidator")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    translator = Translator(args.translator, args.cache, args.glslang)
    tasks, uses, summary = collect_tasks(args.data_dir, translator)
    print("preflight: shaders=%d native-gles=%d source-entries=%d unique=%d" %
          (summary["shader_objects"], summary["native_gles_shader_objects"],
           summary["source_entries"], summary["unique_tasks"]), flush=True)

    successes = {}
    failures = []
    cached = 0
    completed = 0
    lock = threading.Lock()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        future_to_key = {pool.submit(translator.translate, task): key
                         for key, task in tasks.items()}
        for future in concurrent.futures.as_completed(future_to_key):
            key = future_to_key[future]
            try:
                result = future.result()
                successes[key] = result
                cached += int(result["cached"])
            except Exception as exc:  # noqa: BLE001
                failures.append({"key": key, "error": str(exc),
                                 "uses": uses[key]})
            with lock:
                completed += 1
                if completed % 25 == 0 or completed == len(tasks):
                    print("translate: %d/%d ok=%d fail=%d" %
                          (completed, len(tasks), len(successes), len(failures)),
                          flush=True)

    report = {
        "summary": summary,
        "translator_sha256": translator.tool_hash.hex(),
        "glslang_sha256": translator.validator_hash.hex(),
        "translated_ok": len(successes),
        "translated_cached": cached,
        "translated_failed": len(failures),
        "failures": failures,
    }
    coverage = {}
    failed_keys = {failure["key"] for failure in failures}
    for key, key_uses in uses.items():
        for use in key_uses:
            identity = "%s#%s:%s" % (use["asset"], use["path_id"],
                                      use["shader"])
            item = coverage.setdefault(identity, {
                "asset": use["asset"], "path_id": use["path_id"],
                "shader": use["shader"], "exact_entries": 0,
                "failed_entries": 0})
            field = "failed_entries" if key in failed_keys else "exact_entries"
            item[field] += 1
    report["shader_coverage"] = sorted(
        coverage.values(), key=lambda item: (item["shader"], item["asset"]))
    os.makedirs(os.path.dirname(os.path.realpath(args.report)), exist_ok=True)
    with open(args.report, "w") as out:
        json.dump(report, out, indent=2, sort_keys=True)
        out.write("\n")
    print("result: ok=%d failed=%d collection-errors=%d report=%s" %
          (len(successes), len(failures), len(summary["collection_errors"]),
           os.path.realpath(args.report)), flush=True)
    raise SystemExit(1 if failures or summary["collection_errors"] else 0)


if __name__ == "__main__":
    main()
