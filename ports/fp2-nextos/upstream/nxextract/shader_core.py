#!/usr/bin/env python3
"""Pinned Unity shader parsing shared by FP2's portable NXExtract hook.

This is the data-only portion of the exact translator audit that produced the
physically approved Freedom Planet 2 payload.  It intentionally contains no
compiler, validator, cache outside the NXExtract stage, or host-tool lookup.
"""

import os
import re
import struct

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
