#!/usr/bin/env python3
"""Replace FP2's provisional GLES2 blobs with eligible exact translations."""

import argparse
import copy
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

import lz4.block
import UnityPy

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
from audit_exact_gles2 import (  # noqa: E402
    GPU_SPIRV,
    PLATFORM_VULKAN,
    Translator,
    bases_under,
    blob_contexts,
    flatten_struct_uniforms,
    metadata_text,
    parse_entries,
    platform_blob,
)

from serialized_patch import SerializedFileLayout  # noqa: E402


SUBPROGRAM_VERSION = 201802150
GPU_GLES2 = 5
PLATFORM_GLES20 = 5
STAGES = ("progVertex", "progFragment", "progGeometry", "progHull",
          "progDomain")
PARAMETER_FIELDS = (
    "m_VectorParams", "m_MatrixParams", "m_TextureParams", "m_BufferParams",
    "m_ConstantBuffers", "m_ConstantBufferBindings", "m_UAVParams",
    "m_Samplers",
)

# These two passes share one Vulkan program.  Its SPIR-V samples an Android
# texture as ARGB and rotates it with ``yzwx`` before writing the render target.
# That backend swizzle is correct for Unity's Vulkan image view, but not after
# the texture has become ordinary RGBA on GLES2: red becomes alpha and the
# transparent padding of sprites leaks as a coloured rectangle.  The title
# number 2 and the running character in the tutorial are the two physical
# witnesses in FP2 1.2.8.
#
# Keep the exception deliberately narrow and pin the translated input bytes.
# A different upstream shader must fail instead of receiving this correction
# by name alone.
STENCIL_SPRITE_SHADERS = {
    "Sprites/StencilDraw",
    "Sprites/StencilInvert",
}
STENCIL_SPRITE_FRAGMENT_SHA256 = (
    "a76108b0bac21cef2367d14b365c639631c99ae10ea8ca9e61dd5ee094184e52"
)
STENCIL_SPRITE_FRAGMENT_GLES2 = """#version 100
precision mediump float;
precision highp int;

#ifndef SPIRV_CROSS_CONSTANT_ID_1
#define SPIRV_CROSS_CONSTANT_ID_1 false
#endif
const bool _UseExternalAlpha = SPIRV_CROSS_CONSTANT_ID_1;

uniform mediump sampler2D _MainTex;
uniform mediump sampler2D _AlphaTex;

varying highp vec2 vs_TEXCOORD1;
varying highp vec4 vs_TEXCOORD0;

void main()
{
    highp vec4 sampleColor = texture2D(_MainTex, vs_TEXCOORD1);
    if (_UseExternalAlpha)
        sampleColor.w = texture2D(_AlphaTex, vs_TEXCOORD1).x;
    highp vec4 color = sampleColor * vs_TEXCOORD0;
    color.xyz *= color.www;
    gl_FragData[0] = color;
}
"""
STENCIL_SPRITE_CORRECTION_SHA256 = (
    "0d20ce6291da8cfb5e18f31d3cff7b37f6b4ff0a0a874ec613e23ffeb5a46e74"
)

# Exact measured policy for the accepted FP2 1.2.8 payload.  These omissions
# are Vulkan/deferred/MRT/depth/terrain/VR variants which cannot be represented
# by GLES2.  Every forward 2D variant remains exact.  Pinning both counts makes
# a newly failing game shader fatal instead of silently installing a generic
# program or broadening this exception list.
SKIPPED_VARIANT_POLICY = {
    "Hidden/BlitCopyWithDepth": (0, 1),
    "Hidden/BlitToDepth": (0, 1),
    "Hidden/BlitToDepth_MSAA": (0, 1),
    "Hidden/ConvertTexture": (0, 1),
    "Hidden/Internal-DeferredShading": (29, 24),
    "Hidden/Internal-MotionVectors": (2, 1),
    "Hidden/Internal-PrePassLighting": (14, 12),
    "Hidden/Internal-ScreenSpaceShadows": (0, 16),
    "Hidden/TerrainEngine/Details/BillboardWavingDoublePass": (16, 12),
    "Hidden/TerrainEngine/Details/Vertexlit": (18, 8),
    "Hidden/TerrainEngine/Details/WavingDoublePass": (20, 12),
    "Hidden/TerrainEngine/Splatmap/Diffuse-AddPass": (31, 34),
    "Hidden/TerrainEngine/Splatmap/Diffuse-Base": (16, 16),
    "Hidden/TerrainEngine/Splatmap/Standard-AddPass": (36, 56),
    "Hidden/TerrainEngine/Splatmap/Standard-Base": (20, 28),
    "Hidden/VR/BlitTexArraySlice": (0, 1),
    "Legacy Shaders/Diffuse": (14, 4),
    "Nature/Terrain/Diffuse": (35, 34),
    "Nature/Terrain/Standard": (40, 56),
    "Standard": (96, 82),
    "Super Text Mesh/Unlit/Default": (4, 1),
    "Super Text Mesh/Unlit/Outline": (28, 9),
    "Super Text Mesh/Unlit/Pixel Snap ": (4, 1),
}


def align_buffer(buffer):
    while len(buffer) % 4:
        buffer.append(0)


def build_entry(source, source_map, keywords=()):
    entry = bytearray()
    entry += struct.pack("<II", SUBPROGRAM_VERSION, GPU_GLES2)
    entry += struct.pack("<IIII", 0, 0, 0, 0)
    entry += struct.pack("<I", len(keywords))
    for keyword in keywords:
        raw = keyword.encode("utf-8")
        entry += struct.pack("<I", len(raw)) + raw
        align_buffer(entry)
    raw_source = source.encode("utf-8")
    entry += struct.pack("<I", len(raw_source)) + raw_source
    align_buffer(entry)
    entry += struct.pack("<8I", source_map, 0, 1, 0, 0, 0, 0, 0)
    return bytes(entry)


def build_blob(entries):
    header = bytearray(struct.pack("<I", len(entries)))
    header += b"\0" * (8 * len(entries))
    body = bytearray()
    for index, payload in enumerate(entries):
        struct.pack_into("<II", header, 4 + 8 * index,
                         len(header) + len(body), len(payload))
        body += payload
    return bytes(header) + bytes(body)


def combine_stages(vertex, fragment):
    if not vertex.startswith("#version 100"):
        raise ValueError("vertex output is not ESSL100")
    if not fragment.startswith("#version 100"):
        raise ValueError("fragment output is not ESSL100")
    return ("#ifdef VERTEX\n" + vertex.rstrip() + "\n#endif\n"
            "#ifdef FRAGMENT\n" + fragment.rstrip() + "\n#endif\n")


def correct_backend_fragment(shader_name, fragment):
    """Remove one proven Vulkan-only channel swizzle from GLES2 output."""
    if shader_name not in STENCIL_SPRITE_SHADERS:
        return fragment
    digest = hashlib.sha256(fragment.encode("utf-8")).hexdigest()
    if digest != STENCIL_SPRITE_FRAGMENT_SHA256:
        raise ValueError(
            "unexpected translated fragment for %s: got %s, expected %s" %
            (shader_name, digest, STENCIL_SPRITE_FRAGMENT_SHA256))
    correction_digest = hashlib.sha256(
        STENCIL_SPRITE_FRAGMENT_GLES2.encode("utf-8")).hexdigest()
    if correction_digest != STENCIL_SPRITE_CORRECTION_SHA256:
        raise ValueError(
            "Stencil sprite correction changed: got %s, expected %s" %
            (correction_digest, STENCIL_SPRITE_CORRECTION_SHA256))
    return STENCIL_SPRITE_FRAGMENT_GLES2


def replace_platform_blob(tree, platform, raw_segment):
    packed_segment = lz4.block.compress(raw_segment, mode="high_compression",
                                        store_size=False)
    platforms = list(tree["platforms"])
    old_blob = bytes(tree["compressedBlob"])
    segments = []
    raw_lengths = []
    found = False
    for index, existing_platform in enumerate(platforms):
        offset = tree["offsets"][index]
        length = tree["compressedLengths"][index]
        if existing_platform == platform:
            segments.append(packed_segment)
            raw_lengths.append(len(raw_segment))
            found = True
        else:
            segments.append(old_blob[offset:offset + length])
            raw_lengths.append(tree["decompressedLengths"][index])
    if not found:
        platforms.insert(0, platform)
        segments.insert(0, packed_segment)
        raw_lengths.insert(0, len(raw_segment))

    offsets = []
    cursor = 0
    for segment in segments:
        offsets.append(cursor)
        cursor += len(segment)
    tree["platforms"] = platforms
    tree["offsets"] = offsets
    tree["compressedLengths"] = [len(segment) for segment in segments]
    tree["decompressedLengths"] = raw_lengths
    tree["compressedBlob"] = b"".join(segments)


def remove_platform_blob(tree, platform):
    platforms = list(tree["platforms"])
    old_blob = bytes(tree["compressedBlob"])
    kept_platforms = []
    segments = []
    raw_lengths = []
    for index, existing_platform in enumerate(platforms):
        if existing_platform == platform:
            continue
        offset = tree["offsets"][index]
        length = tree["compressedLengths"][index]
        kept_platforms.append(existing_platform)
        segments.append(old_blob[offset:offset + length])
        raw_lengths.append(tree["decompressedLengths"][index])
    offsets = []
    cursor = 0
    for segment in segments:
        offsets.append(cursor)
        cursor += len(segment)
    tree["platforms"] = kept_platforms
    tree["offsets"] = offsets
    tree["compressedLengths"] = [len(segment) for segment in segments]
    tree["decompressedLengths"] = raw_lengths
    tree["compressedBlob"] = b"".join(segments)


def clone_for_gles2(subprogram):
    clone = copy.deepcopy(subprogram)
    clone["m_GpuProgramType"] = GPU_GLES2
    for field in PARAMETER_FIELDS:
        if field in clone:
            clone[field] = []
    return clone


def variant_key(subprogram):
    return (tuple(subprogram.get("m_KeywordIndices", [])),
            int(subprogram.get("m_ShaderHardwareTier", 0)))


def install_records(tree, usable_entries):
    installed = 0
    omitted = 0
    for subshader in tree["m_ParsedForm"]["m_SubShaders"]:
        for shader_pass in subshader["m_Passes"]:
            vertex_vulkan = [subprogram for subprogram in
                             shader_pass.get("progVertex", {}).get(
                                 "m_SubPrograms", [])
                             if subprogram["m_GpuProgramType"] == GPU_SPIRV]
            usable_variants = {variant_key(subprogram)
                               for subprogram in vertex_vulkan
                               if subprogram["m_BlobIndex"] in usable_entries}
            for stage in STAGES:
                program = shader_pass.get(stage)
                if not program:
                    continue
                existing = program.get("m_SubPrograms", [])
                vulkan = [subprogram for subprogram in existing
                          if subprogram["m_GpuProgramType"] == GPU_SPIRV]
                clones = []
                for subprogram in vulkan:
                    eligible = False
                    if stage == "progVertex":
                        eligible = subprogram["m_BlobIndex"] in usable_entries
                    elif stage == "progFragment":
                        eligible = variant_key(subprogram) in usable_variants
                    # Geometry/tessellation stages cannot be represented by
                    # GLES2 and must not advertise a fake platform-5 program.
                    if eligible:
                        clones.append(clone_for_gles2(subprogram))
                        installed += 1
                    else:
                        omitted += 1
                program["m_SubPrograms"] = clones + [
                    subprogram for subprogram in existing
                    if subprogram["m_GpuProgramType"] != GPU_GLES2]
    return installed, omitted


def split_parts(path):
    parts = []
    index = 0
    while True:
        candidate = "%s.split%d" % (path, index)
        if not os.path.exists(candidate):
            break
        parts.append(candidate)
        index += 1
    return parts


def read_serialized_blob(path):
    parts = split_parts(path)
    if parts:
        return b"".join(open(part, "rb").read() for part in parts)
    return open(path, "rb").read()


def write_serialized_blob(path, blob):
    parts = split_parts(path)
    if not parts:
        with open(path, "wb") as out:
            out.write(blob)
        return "single"
    chunk_size = os.path.getsize(parts[0])
    cursor = 0
    index = 0
    while cursor < len(blob):
        piece = blob[cursor:cursor + chunk_size]
        with open("%s.split%d" % (path, index), "wb") as out:
            out.write(piece)
        cursor += len(piece)
        index += 1
    for stale in parts[index:]:
        os.remove(stale)
    return "split(%d)" % index


class ExactInjector:
    def __init__(self, translator):
        self.translator = translator
        self.results = {}
        self.failures = {}
        self.validated_corrections = set()

    def validate_correction(self, shader_name, vertex, fragment):
        digest = hashlib.sha256(
            vertex.encode("utf-8") + b"\0" + fragment.encode("utf-8")
        ).hexdigest()
        if digest in self.validated_corrections:
            return
        with tempfile.TemporaryDirectory(
                prefix="fp2-backend-correction-",
                dir=self.translator.cache_dir) as directory:
            vertex_path = os.path.join(directory, "program.vert")
            fragment_path = os.path.join(directory, "program.frag")
            for path, source in ((vertex_path, vertex),
                                 (fragment_path, fragment)):
                with open(path, "w", encoding="utf-8", newline="\n") as out:
                    out.write(source)
            for command, label in (
                    ([self.translator.glslang, "-S", "frag", fragment_path],
                     "fragment validation"),
                    ([self.translator.glslang, "-l", vertex_path,
                      fragment_path], "program link validation")):
                result = subprocess.run(
                    command, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT)
                if result.returncode:
                    raise ValueError("%s for %s: %s" %
                                     (label, shader_name,
                                      result.stdout.strip()))
        self.validated_corrections.add(digest)

    def translate(self, source, metadata):
        key = self.translator.key(source, metadata)
        if key not in self.results and key not in self.failures:
            task = {"key": key, "source": source, "metadata": metadata}
            try:
                self.results[key] = self.translator.translate(task)
            except Exception as exc:  # noqa: BLE001
                self.failures[key] = str(exc)
        return key, self.results.get(key)

    def convert_shader(self, tree, shader_name):
        platforms = list(tree["platforms"])
        if PLATFORM_VULKAN not in platforms:
            return None
        if PLATFORM_GLES20 in platforms and 9 in platforms:
            return {"native": True}

        entries = parse_entries(platform_blob(tree, PLATFORM_VULKAN))
        if not any(entry["source"] for entry in entries):
            return {"empty": True}
        contexts = blob_contexts(tree)
        output_entries = []
        usable = set()
        exact = skipped = 0
        failure_details = []
        for entry in entries:
            if not entry["source"]:
                output_entries.append(build_entry("", 0, entry["keywords"]))
                continue
            context = contexts.get(entry["index"])
            if context is None:
                raise ValueError("entry %d has no Vulkan context" % entry["index"])
            metadata = metadata_text(context[0], context[1])
            key, result = self.translate(entry["source"], metadata)
            source_map = int(context[1]["m_Channels"]["m_SourceMap"])
            if result:
                with open(result["vertex"]) as src:
                    vertex = flatten_struct_uniforms(src.read())
                with open(result["fragment"]) as src:
                    fragment = flatten_struct_uniforms(src.read())
                corrected_fragment = correct_backend_fragment(
                    shader_name, fragment)
                if corrected_fragment != fragment:
                    self.validate_correction(
                        shader_name, vertex, corrected_fragment)
                fragment = corrected_fragment
                output_entries.append(build_entry(
                    combine_stages(vertex, fragment), source_map,
                    entry["keywords"]))
                usable.add(entry["index"])
                exact += 1
            else:
                output_entries.append(build_entry("", 0, entry["keywords"]))
                skipped += 1
                failure_details.append({"entry": entry["index"],
                                        "keywords": entry["keywords"],
                                        "error": self.failures[key]})

        installed, omitted = install_records(tree, usable)
        if usable:
            replace_platform_blob(tree, PLATFORM_GLES20,
                                  build_blob(output_entries))
        else:
            # A platform entry with no usable record still advertises a route
            # to Unity.  Remove it completely for depth/VR-only shaders.
            remove_platform_blob(tree, PLATFORM_GLES20)
        return {"native": False, "exact_entries": exact,
                "skipped_entries": skipped, "installed_records": installed,
                "omitted_records": omitted, "failures": failure_details}


def process_file(path, injector, dry_run=False):
    load_path = path if os.path.exists(path) else path + ".split0"
    env = UnityPy.load(load_path)
    replacements = {}
    shader_results = []
    assets_file = None
    for obj in env.objects:
        if obj.type.name != "Shader":
            continue
        tree = obj.read_typetree()
        shader_name = tree["m_ParsedForm"]["m_Name"]
        result = injector.convert_shader(tree, shader_name)
        if not result or result.get("native") or result.get("empty"):
            continue
        if result["skipped_entries"]:
            measured = (result["exact_entries"], result["skipped_entries"])
            expected = SKIPPED_VARIANT_POLICY.get(shader_name)
            if measured != expected:
                raise ValueError(
                    "unapproved skipped variants for %s: got %s, expected %s" %
                    (shader_name, measured, expected))
        if assets_file is None:
            assets_file = obj.assets_file
        elif assets_file is not obj.assets_file:
            raise ValueError("one base unexpectedly contains multiple serialized files")
        replacements[obj.path_id] = bytes(obj.save_typetree(tree))
        shader_results.append({"name": shader_name,
                               **result})
    if not replacements:
        return None

    original = read_serialized_blob(path)
    layout = SerializedFileLayout(
        original, [(obj.path_id, obj.byte_start, obj.byte_size)
                   for obj in assets_file.objects.values()])
    if layout.rebuild({}) != layout.blob:
        raise ValueError("serialized no-op rebuild is not byte-identical")
    rebuilt = layout.rebuild(replacements)
    how = "dry-run" if dry_run else write_serialized_blob(path, rebuilt)
    return {"path": path, "before": len(original), "after": len(rebuilt),
            "write": how, "shaders": shader_results}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir")
    ap.add_argument("--translator", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--report", required=True)
    ap.add_argument("--glslang", default="glslangValidator")
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    translator = Translator(args.translator, args.cache, args.glslang)
    injector = ExactInjector(translator)
    results = []
    for path in bases_under(args.data_dir):
        if args.only and os.path.basename(path) not in set(args.only):
            continue
        try:
            result = process_file(path, injector, args.dry_run)
        except Exception as exc:  # noqa: BLE001
            results.append({"path": path, "fatal": str(exc)})
            print("FATAL %s: %s" % (os.path.relpath(path, args.data_dir), exc),
                  flush=True)
            continue
        if not result:
            continue
        results.append(result)
        exact = sum(shader["exact_entries"] for shader in result["shaders"])
        skipped = sum(shader["skipped_entries"] for shader in result["shaders"])
        print("%-48s shader=%d exact=%d skipped=%d %d->%d %s" %
              (os.path.relpath(path, args.data_dir), len(result["shaders"]),
               exact, skipped, result["before"], result["after"],
               result["write"]), flush=True)

    report = {"data_dir": os.path.realpath(args.data_dir),
              "dry_run": args.dry_run, "files": results,
              "translator_sha256": translator.tool_hash.hex(),
              "glslang_sha256": translator.validator_hash.hex(),
              "translation_failures": injector.failures}
    os.makedirs(os.path.dirname(os.path.realpath(args.report)), exist_ok=True)
    with open(args.report, "w") as out:
        json.dump(report, out, indent=2, sort_keys=True)
        out.write("\n")
    fatals = sum("fatal" in result for result in results)
    print("DONE files=%d fatals=%d report=%s" %
          (len(results), fatals, os.path.realpath(args.report)), flush=True)
    raise SystemExit(1 if fatals else 0)


if __name__ == "__main__":
    main()
