#!/usr/bin/env python3
"""Build GLES2 variants for Prizefighters 2's Unity 2022 shader blobs.

Prizefighters 2 was built with OpenGLES3 as its only graphics API.  Its main
data.unity3d therefore contains platform 9 programs only, while the Mali-450
player requests platform 5 (OpenGLES2).

This tool is deliberately version-specific.  It accepts the ShaderProgram
layout used by Unity 2022.3.62f2 / program version 202012090, converts only
programs that have a GLES2 representation, adds a platform-5 blob alongside the
original platform-9 blob, and updates the parsed variant table.  Unsupported
programs stay GLES3-only instead of being silently emitted as broken GLES2.

The source asset is never modified.  Use --output to create a BYO-data copy.
"""

from __future__ import annotations

import argparse
import collections
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import UnityPy
import lz4.block


PROGRAM_VERSION = 202012090
GLES2_PLATFORM = 5
GLES3_PLATFORM = 9
GLES2_SENTINEL = struct.pack("<II", PROGRAM_VERSION, 1) + bytes(20)
ENTRY = struct.Struct("<III")

PROGRAM_KEYS = (
    "progVertex",
    "progFragment",
    "progGeometry",
    "progHull",
    "progDomain",
    "progRayTracing",
)

SUPPORTED_EXTENSIONS = {
    "GL_EXT_shader_texture_lod",
    "GL_OES_EGL_image_external",
    "GL_OES_standard_derivatives",
}


class ShaderFormatError(RuntimeError):
    pass


class UnsupportedProgram(RuntimeError):
    pass


@dataclass
class BlobResult:
    raw: bytes
    index_map: dict[int, int]
    converted: int
    skipped: collections.Counter[str]


@dataclass
class RunStats:
    shaders_seen: int = 0
    shaders_patched: int = 0
    programs_converted: int = 0
    variants_added: int = 0
    passes_enabled: int = 0
    skipped: collections.Counter[str] | None = None

    def __post_init__(self) -> None:
        if self.skipped is None:
            self.skipped = collections.Counter()


def scalar(value: object) -> int:
    if isinstance(value, list):
        if len(value) != 1:
            raise ShaderFormatError(f"expected one scalar value, got {value!r}")
        value = value[0]
    return int(value)


def shader_name(data: dict) -> str:
    return data.get("m_Name") or data.get("m_ParsedForm", {}).get("m_Name") or "<unnamed>"


def unpack_platform_blob(data: dict, platform_index: int) -> bytes:
    compressed = bytes(data["compressedBlob"])
    offset = scalar(data["offsets"][platform_index])
    compressed_len = scalar(data["compressedLengths"][platform_index])
    decompressed_len = scalar(data["decompressedLengths"][platform_index])
    end = offset + compressed_len
    if offset < 0 or end > len(compressed):
        raise ShaderFormatError("compressed shader blob range is outside compressedBlob")
    return lz4.block.decompress(
        compressed[offset:end],
        uncompressed_size=decompressed_len,
    )


def parse_entries(raw: bytes) -> list[tuple[int, int, int]]:
    if len(raw) < 4:
        raise ShaderFormatError("ShaderProgram is shorter than its count field")
    count = struct.unpack_from("<I", raw)[0]
    header_end = 4 + count * ENTRY.size
    if count > 1_000_000 or header_end > len(raw):
        raise ShaderFormatError(f"invalid ShaderProgram count {count}")
    entries = [ENTRY.unpack_from(raw, 4 + i * ENTRY.size) for i in range(count)]
    previous_end = header_end
    for index, (offset, length, reserved) in enumerate(entries):
        if reserved != 0:
            raise ShaderFormatError(f"entry {index} has non-zero reserved field {reserved}")
        if offset < header_end or length < 8 or offset + length > len(raw):
            raise ShaderFormatError(
                f"entry {index} has invalid range {offset:#x}+{length:#x}"
            )
        if offset < previous_end:
            raise ShaderFormatError(f"entry {index} overlaps the previous entry")
        previous_end = offset + length
    return entries


def find_program_source(program: bytes) -> tuple[int, int]:
    hits: list[int] = []
    for marker in (b"#ifdef VERTEX", b"#version"):
        start = 0
        while True:
            hit = program.find(marker, start)
            if hit < 0:
                break
            hits.append(hit)
            start = hit + 1
    for hit in sorted(set(hits)):
        if hit < 4:
            continue
        source_len = struct.unpack_from("<I", program, hit - 4)[0]
        if 0 < source_len <= len(program) - hit:
            return hit, source_len
    raise UnsupportedProgram("no GLSL source")


def split_stages(source: str) -> tuple[str, str]:
    vertex_at = source.find("#ifdef VERTEX")
    fragment_at = source.find("#ifdef FRAGMENT")
    if vertex_at < 0 or fragment_at <= vertex_at:
        raise UnsupportedProgram("not a combined vertex/fragment program")
    for marker in ("#ifdef GEOMETRY", "#ifdef HULL", "#ifdef DOMAIN"):
        if marker in source:
            raise UnsupportedProgram("contains a non-GLES2 shader stage")
    vertex = source[vertex_at:fragment_at]
    fragment = source[fragment_at:]
    return vertex, fragment


def extension_names(section: str) -> set[str]:
    return set(
        re.findall(
            r"^#extension\s+([A-Za-z0-9_]+)\s*:\s*(?:enable|require)\s*$",
            section,
            flags=re.MULTILINE,
        )
    )


def sampler_types(section: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for kind, name in re.findall(
        r"\b(sampler2D|samplerCube|samplerExternalOES)\s+([A-Za-z_]\w*)",
        section,
    ):
        result[name] = kind
    return result


def replace_texture_calls(section: str, stage: str) -> str:
    samplers = sampler_types(section)

    def replace(match: re.Match[str]) -> str:
        operation = match.group(1)
        sampler = match.group(2)
        kind = samplers.get(sampler)
        if kind is None:
            raise UnsupportedProgram(f"cannot type texture sampler {sampler}")
        if operation == "texture":
            function = "textureCube" if kind == "samplerCube" else "texture2D"
        elif stage == "vertex":
            function = "textureCubeLod" if kind == "samplerCube" else "texture2DLod"
        else:
            function = "textureCubeLodEXT" if kind == "samplerCube" else "texture2DLodEXT"
        return f"{function}({sampler},"

    return re.sub(
        r"\b(textureLod|texture)\s*\(\s*([A-Za-z_]\w*)\s*,",
        replace,
        section,
    )


def add_helpers(section: str) -> str:
    helpers: list[str] = []
    if re.search(r"\broundEven\s*\(", section):
        helpers.append(
            """
float roundEven(float value) { return floor(value + 0.5); }
vec2 roundEven(vec2 value) { return floor(value + vec2(0.5)); }
vec3 roundEven(vec3 value) { return floor(value + vec3(0.5)); }
vec4 roundEven(vec4 value) { return floor(value + vec4(0.5)); }
""".strip()
        )
    if re.search(r"\btranspose\s*\(", section):
        section = re.sub(r"\btranspose\s*\(", "pf2_transpose(", section)
        helpers.append(
            """
mat3 pf2_transpose(mat3 m) {
    return mat3(
        m[0][0], m[1][0], m[2][0],
        m[0][1], m[1][1], m[2][1],
        m[0][2], m[1][2], m[2][2]);
}
mat4 pf2_transpose(mat4 m) {
    return mat4(
        m[0][0], m[1][0], m[2][0], m[3][0],
        m[0][1], m[1][1], m[2][1], m[3][1],
        m[0][2], m[1][2], m[2][2], m[3][2],
        m[0][3], m[1][3], m[2][3], m[3][3]);
}
""".strip()
        )
    if helpers:
        main_at = section.find("void main")
        if main_at < 0:
            raise UnsupportedProgram("program has no main function")
        section = section[:main_at] + "\n".join(helpers) + "\n" + section[main_at:]
    return section


def check_unsupported(section: str) -> None:
    blockers = (
        (r"\bsampler2DArray\b", "sampler2DArray"),
        (r"\bsampler2DShadow\b", "sampler2DShadow"),
        (r"\bsamplerCubeShadow\b", "samplerCubeShadow"),
        (r"\b(?:uvec[234]|uint)\b", "unsigned integers"),
        (r"\bflat\b", "flat interpolation"),
        (r"\bgl_(?:Layer|ViewID\w*|InstanceID|VertexID)\b", "ES3 built-in"),
        (r"\bgl_FragDepth\b", "fragment depth output"),
        (
            r"\b(?:(?:[iu]?image)(?:1D|2D|3D|Cube|Buffer|2DArray)"
            r"|atomic\w*|bitfield\w*)\b",
            "image/atomic/bitfield operation",
        ),
        (r"(?<!&)&(?!&)|(?<!\|)\|(?!\|)|\^|<<|>>", "bitwise operation"),
        (r"\bswitch\s*\(", "switch statement"),
        (r"\b(?:EmitVertex|EndPrimitive)\s*\(", "geometry operation"),
        (r"\bSV_Target[1-9]\d*\b", "multiple render targets"),
        (r"layout\s*\(\s*location\s*=\s*[1-9]\d*\s*\)\s*out", "multiple render targets"),
    )
    for pattern, reason in blockers:
        if re.search(pattern, section):
            raise UnsupportedProgram(reason)
    unsupported_extensions = extension_names(section) - SUPPORTED_EXTENSIONS
    if unsupported_extensions:
        raise UnsupportedProgram(
            "unsupported extension " + ",".join(sorted(unsupported_extensions))
        )


def translate_section(section: str, stage: str) -> str:
    check_unsupported(section)
    section = re.sub(r"#version\s+(?:300|310)\s+es", "#version 100", section)
    section = re.sub(
        r"^#define\s+HLSLCC_ENABLE_UNIFORM_BUFFERS\s+1\s*$",
        "#define HLSLCC_ENABLE_UNIFORM_BUFFERS 0",
        section,
        flags=re.MULTILINE,
    )
    section = re.sub(
        r"^#define\s+UNITY_SUPPORTS_UNIFORM_LOCATION\s+1\s*$",
        "#define UNITY_SUPPORTS_UNIFORM_LOCATION 0",
        section,
        flags=re.MULTILINE,
    )

    # These layouts are outside HLSLcc's disabled macro blocks.
    # Never consume a newline here: joining the following #define/#else/#endif
    # onto a macro line changes the preprocessor structure.
    section = re.sub(r"layout\s*\([^)\n]*\)[ \t]*", "", section)

    if stage == "vertex":
        section = re.sub(r"^[ \t]*in[ \t]+", "attribute ", section, flags=re.MULTILINE)
        section = re.sub(r"^[ \t]*out[ \t]+", "varying ", section, flags=re.MULTILINE)
    else:
        section = re.sub(r"^[ \t]*in[ \t]+", "varying ", section, flags=re.MULTILINE)

        def fragment_output(match: re.Match[str]) -> str:
            return f"#define {match.group(1)} gl_FragData[0]"

        section = re.sub(
            r"^[ \t]*out[ \t]+(?:(?:lowp|mediump|highp)[ \t]+)?"
            r"vec4[ \t]+([A-Za-z_]\w*)[ \t]*;[ \t]*$",
            fragment_output,
            section,
            flags=re.MULTILINE,
        )
        if re.search(r"^[ \t]*out[ \t]+", section, flags=re.MULTILINE):
            raise UnsupportedProgram("unsupported fragment output")
        section = re.sub(
            r"^[ \t]*precision[ \t]+highp[ \t]+float[ \t]*;[ \t]*$",
            "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
            "precision highp float;\n"
            "#else\n"
            "precision mediump float;\n"
            "#endif",
            section,
            count=1,
            flags=re.MULTILINE,
        )
        if ("dFdx(" in section or "dFdy(" in section) and (
            "GL_OES_standard_derivatives" not in section
        ):
            section = section.replace(
                "#version 100",
                "#version 100\n#extension GL_OES_standard_derivatives : enable",
                1,
            )

    section = replace_texture_calls(section, stage)
    section = add_helpers(section)

    if "#version 100" not in section:
        raise UnsupportedProgram("unexpected GLSL version")
    if re.search(r"\b(?:texture|textureLod)\s*\(", section):
        raise UnsupportedProgram("untranslated texture operation")
    if re.search(r"layout\s*\(", section):
        raise UnsupportedProgram("untranslated layout qualifier")
    return section


def translate_source(source: bytes) -> bytes:
    text = source.decode("utf-8")
    vertex, fragment = split_stages(text)
    vertex = translate_section(vertex, "vertex")
    fragment = translate_section(fragment, "fragment")
    return (vertex.rstrip() + "\n" + fragment.lstrip()).encode("utf-8")


def convert_program(program: bytes) -> bytes:
    if len(program) < 8:
        raise ShaderFormatError("shader subprogram is shorter than its fixed header")
    version, program_type = struct.unpack_from("<Ii", program)
    if version != PROGRAM_VERSION:
        raise ShaderFormatError(
            f"unexpected shader program version {version}, expected {PROGRAM_VERSION}"
        )
    if program_type not in (3, 4):
        raise UnsupportedProgram(f"program type {program_type}")
    source_at, source_len = find_program_source(program)
    source_end = source_at + source_len
    trailing_at = (source_end + 3) & ~3
    if trailing_at > len(program):
        raise ShaderFormatError("GLSL source alignment exceeds its program entry")

    translated = translate_source(program[source_at:source_end])
    output = bytearray(program[: source_at - 4])
    struct.pack_into("<i", output, 4, 5)
    output += struct.pack("<I", len(translated))
    output += translated
    output += bytes((-len(output)) & 3)
    output += program[trailing_at:]
    output += bytes((-len(output)) & 3)
    return bytes(output)


def convert_blob(raw: bytes) -> BlobResult:
    entries = parse_entries(raw)
    programs: list[bytes] = []
    index_map: dict[int, int] = {}
    skipped: collections.Counter[str] = collections.Counter()

    for old_index, (offset, length, _reserved) in enumerate(entries):
        try:
            program = convert_program(raw[offset : offset + length])
        except UnsupportedProgram as exc:
            skipped[str(exc)] += 1
            continue
        index_map[old_index] = len(programs) + 1
        programs.append(program)

    count = len(programs) + 1
    header_len = 4 + count * ENTRY.size
    offset = header_len
    output_entries = [(offset, len(GLES2_SENTINEL), 0)]
    body = bytearray(GLES2_SENTINEL)
    offset += len(GLES2_SENTINEL)
    for program in programs:
        output_entries.append((offset, len(program), 0))
        body += program
        offset += len(program)

    output = bytearray(struct.pack("<I", count))
    for entry in output_entries:
        output += ENTRY.pack(*entry)
    output += body
    parse_entries(output)
    return BlobResult(
        raw=bytes(output),
        index_map=index_map,
        converted=len(programs),
        skipped=skipped,
    )


def patch_program_variants(program: object, index_map: dict[int, int]) -> int:
    if not isinstance(program, dict):
        return 0
    added = 0
    player_lists = program.get("m_PlayerSubPrograms") or []
    parameter_lists = program.get("m_ParameterBlobIndices") or []
    if len(parameter_lists) != len(player_lists):
        raise ShaderFormatError(
            "m_ParameterBlobIndices does not match m_PlayerSubPrograms"
        )
    for tier, variants in enumerate(player_lists):
        if not isinstance(variants, list):
            continue
        parameters = parameter_lists[tier]
        if not isinstance(parameters, list) or len(parameters) != len(variants):
            raise ShaderFormatError(
                "parameter-blob count does not match player-subprogram count"
            )
        duplicates: list[dict] = []
        duplicate_parameters: list[int] = []
        seen: set[tuple[int, int, tuple[int, ...]]] = set()
        for variant_index, variant in enumerate(variants):
            if not isinstance(variant, dict):
                continue
            old_index = variant.get("m_BlobIndex")
            new_index = index_map.get(old_index)
            if new_index is None:
                continue
            duplicate = dict(variant)
            duplicate["m_BlobIndex"] = new_index
            duplicate["m_GpuProgramType"] = 5
            key = (
                new_index,
                5,
                tuple(duplicate.get("m_KeywordIndices") or []),
            )
            if key not in seen:
                duplicates.append(duplicate)
                # Unity 2022's own adjacent GLES2/GLES3 blobs use parameter
                # index zero for every type-5 combined program, even when the
                # corresponding type-4 variants use distinct indices.
                duplicate_parameters.append(0)
                seen.add(key)
        if duplicates:
            variants[:0] = duplicates
            parameters[:0] = duplicate_parameters
            added += len(duplicates)
    if added:
        # A GLES3-only Unity 2022 shader promotes bindings shared by all of its
        # variants into m_CommonParameters.  They are not common anymore after
        # a GLES2 platform is added: GLES3 describes UBO bindings there while
        # GLES2 uses ordinary uniforms from its own program metadata.
        #
        # Genuine Unity 2022 assets built for adjacent platforms [5, 9] leave
        # this structure empty (including custom shaders with several textures)
        # and keep the original GLES3 parameter blobs beside the zero-indexed
        # GLES2 variants.  Retaining the GLES3 common table makes Unity reject
        # the otherwise valid GLES2 program before it ever reaches glCompile.
        common = program.get("m_CommonParameters")
        if isinstance(common, dict):
            for key, value in common.items():
                if isinstance(value, list):
                    common[key] = []
    return added


def patch_parsed_form(data: dict, index_map: dict[int, int]) -> tuple[int, int]:
    passes_enabled = 0
    variants_added = 0
    parsed = data.get("m_ParsedForm")
    if not isinstance(parsed, dict):
        return passes_enabled, variants_added
    for subshader in parsed.get("m_SubShaders") or []:
        for shader_pass in subshader.get("m_Passes") or []:
            pass_added = 0
            for key in PROGRAM_KEYS:
                pass_added += patch_program_variants(shader_pass.get(key), index_map)
            if pass_added:
                platforms = shader_pass.get("m_Platforms")
                if isinstance(platforms, list) and GLES2_PLATFORM not in platforms:
                    insert_at = platforms.index(GLES3_PLATFORM) if GLES3_PLATFORM in platforms else 0
                    platforms.insert(insert_at, GLES2_PLATFORM)
                    passes_enabled += 1
                variants_added += pass_added
    return passes_enabled, variants_added


def patch_shader(data: dict) -> tuple[BlobResult, int, int]:
    platforms = data.get("platforms")
    if not isinstance(platforms, list) or GLES3_PLATFORM not in platforms:
        raise UnsupportedProgram("no GLES3 platform")
    if GLES2_PLATFORM in platforms:
        raise UnsupportedProgram("already has GLES2 platform")

    source_index = platforms.index(GLES3_PLATFORM)
    result = convert_blob(unpack_platform_blob(data, source_index))
    if not result.index_map:
        raise UnsupportedProgram("no convertible programs")

    old_blob = bytes(data["compressedBlob"])
    new_raw = result.raw
    new_compressed = lz4.block.compress(new_raw, store_size=False)

    # Prefix platform 5 and shift every original compressed-blob offset.
    data["compressedBlob"] = list(new_compressed + old_blob)
    data["offsets"] = [[0]] + [
        [scalar(value) + len(new_compressed)] for value in data["offsets"]
    ]
    data["compressedLengths"] = [[len(new_compressed)]] + [
        [scalar(value)] for value in data["compressedLengths"]
    ]
    data["decompressedLengths"] = [[len(new_raw)]] + [
        [scalar(value)] for value in data["decompressedLengths"]
    ]
    data["platforms"] = [GLES2_PLATFORM] + list(platforms)
    if isinstance(data.get("stageCounts"), list):
        source_stage_count = data["stageCounts"][source_index]
        data["stageCounts"] = [source_stage_count] + list(data["stageCounts"])

    passes_enabled, variants_added = patch_parsed_form(data, result.index_map)
    if variants_added == 0:
        raise ShaderFormatError("converted programs are not referenced by the parsed shader")
    return result, passes_enabled, variants_added


def process(
    source: Path,
    output: Path | None,
    only_names: set[str] | None = None,
) -> RunStats:
    environment = UnityPy.load(str(source))
    stats = RunStats()
    for obj in environment.objects:
        if obj.type.name != "Shader":
            continue
        stats.shaders_seen += 1
        data = obj.read_typetree()
        name = shader_name(data)
        if only_names and name not in only_names:
            continue
        try:
            result, passes_enabled, variants_added = patch_shader(data)
        except UnsupportedProgram as exc:
            stats.skipped[f"shader: {exc}"] += 1
            print(f"SKIP {name}: {exc}")
            continue
        obj.save_typetree(data)
        stats.shaders_patched += 1
        stats.programs_converted += result.converted
        stats.variants_added += variants_added
        stats.passes_enabled += passes_enabled
        stats.skipped.update(result.skipped)
        print(
            f"OK   {name}: {result.converted} programs, "
            f"{variants_added} parsed variants, {passes_enabled} passes"
        )

    if output is not None:
        if source.resolve() == output.resolve():
            raise ShaderFormatError("refusing to overwrite the source asset")
        output.parent.mkdir(parents=True, exist_ok=True)
        # UnityPy defaults to an uncompressed UnityFS rewrite, which inflates
        # PF2's 3.6 MiB asset to roughly 51 MiB.  Keep the BYO asset compact.
        output.write_bytes(environment.file.save(packer="lz4"))
    return stats


def print_stats(stats: RunStats) -> None:
    print(
        "\nSummary: "
        f"{stats.shaders_patched}/{stats.shaders_seen} shaders patched, "
        f"{stats.programs_converted} programs converted, "
        f"{stats.variants_added} variants added across "
        f"{stats.passes_enabled} passes"
    )
    if stats.skipped:
        print("Skipped programs/shaders:")
        for reason, count in stats.skipped.most_common():
            print(f"  {count:4}  {reason}")


def verify_output(path: Path) -> None:
    environment = UnityPy.load(str(path))
    shaders = 0
    platform5 = 0
    program5 = 0
    for obj in environment.objects:
        if obj.type.name != "Shader":
            continue
        shaders += 1
        data = obj.read_typetree()
        platforms = data.get("platforms") or []
        if GLES2_PLATFORM not in platforms:
            continue
        platform5 += 1
        raw = unpack_platform_blob(data, platforms.index(GLES2_PLATFORM))
        entries = parse_entries(raw)
        types = [
            struct.unpack_from("<i", raw, offset + 4)[0]
            for offset, _length, _reserved in entries
        ]
        if not types or types[0] != 1 or any(value != 5 for value in types[1:]):
            raise ShaderFormatError(
                f"{shader_name(data)} has malformed GLES2 program types {types}"
            )
        program5 += len(types) - 1
    print(
        f"Verified {path}: {platform5}/{shaders} shaders have platform 5, "
        f"{program5} GLES2 programs"
    )


def validate_with_glslang(path: Path) -> None:
    validator = shutil.which("glslangValidator")
    if validator is None:
        raise RuntimeError("glslangValidator was not found")
    environment = UnityPy.load(str(path))
    failures: list[str] = []
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pf2-glsl-") as tmp:
        tmpdir = Path(tmp)
        for obj in environment.objects:
            if obj.type.name != "Shader":
                continue
            data = obj.read_typetree()
            platforms = data.get("platforms") or []
            if GLES2_PLATFORM not in platforms:
                continue
            raw = unpack_platform_blob(data, platforms.index(GLES2_PLATFORM))
            for index, (offset, length, _reserved) in enumerate(parse_entries(raw)):
                program = raw[offset : offset + length]
                try:
                    source_at, source_len = find_program_source(program)
                except UnsupportedProgram:
                    continue
                source = program[source_at : source_at + source_len].decode("utf-8")
                try:
                    vertex, fragment = split_stages(source)
                except UnsupportedProgram as exc:
                    failures.append(f"{shader_name(data)}[{index}]: {exc}")
                    continue
                for stage, text, suffix in (
                    ("vert", vertex, ".vert"),
                    ("frag", fragment, ".frag"),
                ):
                    # Resolve the outer Unity stage guard for a standalone validator.
                    standalone = re.sub(
                        r"^#ifdef\s+(?:VERTEX|FRAGMENT)\s*\n",
                        "",
                        text,
                        count=1,
                    ).rstrip()
                    if not standalone.endswith("#endif"):
                        failures.append(
                            f"{shader_name(data)}[{index}] {stage}: "
                            "outer stage guard is not balanced"
                        )
                        continue
                    standalone = standalone[: -len("#endif")].rstrip() + "\n"
                    shader_path = tmpdir / f"shader-{obj.path_id}-{index}{suffix}"
                    shader_path.write_text(standalone)
                    run = subprocess.run(
                        [validator, "-S", stage, str(shader_path)],
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        check=False,
                    )
                    checked += 1
                    if run.returncode:
                        failures.append(
                            f"{shader_name(data)}[{index}] {stage}:\n{run.stdout.strip()}"
                        )
    if failures:
        print("\n\n".join(failures[:30]), file=sys.stderr)
        raise RuntimeError(
            f"glslang rejected {len(failures)} of {checked} translated stages"
        )
    print(f"glslangValidator accepted all {checked} translated stages")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="pristine PF2 data.unity3d")
    parser.add_argument(
        "--output",
        type=Path,
        help="write the patched BYO-data asset here; never overwrites source",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="run structural verification and glslangValidator on the output",
    )
    parser.add_argument(
        "--only",
        action="append",
        default=[],
        metavar="SHADER",
        help="patch only this exact shader name; may be repeated",
    )
    args = parser.parse_args()

    if not args.source.is_file():
        parser.error(f"source does not exist: {args.source}")
    if args.validate and args.output is None:
        parser.error("--validate requires --output")

    stats = process(args.source, args.output, set(args.only))
    print_stats(stats)
    if args.output is not None:
        verify_output(args.output)
        if args.validate:
            validate_with_glslang(args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ShaderFormatError, RuntimeError, UnicodeDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
