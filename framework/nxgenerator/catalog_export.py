#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate and export one deterministic NextOS website catalog entry.

The website entry is an editorial contract.  It deliberately remains separate
from nxport.json (runtime identity) and port.json (PortMaster metadata).  This
tool cross-binds the three identities without adding website fields to either
runtime contract.
"""

import argparse
import ctypes
import errno
import hashlib
import importlib.util
import ipaddress
import json
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import stat
from urllib.parse import urlsplit
from xml.etree import ElementTree
import zlib


ROOT = Path(__file__).resolve().parent
CORE_PATH = ROOT / "nxgenerator.py"
CATALOG_SCHEMA = ROOT / "schema" / "nextos-port-catalog-v1.schema.json"
MAX_INPUT_BYTES = 64 * 1024
MAX_IMAGE_BYTES = 16 * 1024 * 1024
MAX_DECODED_IMAGE_BYTES = 128 * 1024 * 1024
CATALOG_KEYS = (
    "id", "name", "description", "build", "links", "images",
    "installation",
)
BUILD_TYPES = ("alpha", "beta", "rc", "stable")
SLUG_PATTERN = r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
SLUG_RE = re.compile(SLUG_PATTERN)
SEMVER_IDENTIFIER = (
    r"(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
)
SEMVER_PATTERN = (
    r"(?:0|[1-9][0-9]*)[.](?:0|[1-9][0-9]*)[.]"
    r"(?:0|[1-9][0-9]*)"
    r"(?:-" + SEMVER_IDENTIFIER + r"(?:[.]" + SEMVER_IDENTIFIER + r")*)?"
    r"(?:[+][0-9A-Za-z-]+(?:[.][0-9A-Za-z-]+)*)?"
)
SEMVER_RE = re.compile(SEMVER_PATTERN)
URL_PATH_RE = re.compile(r"/[A-Za-z0-9._~/-]+")
IMAGE_EXTENSIONS = (".svg", ".png", ".webp", ".jpg", ".jpeg")
FORBIDDEN_ORIGIN_TOKENS = (
    "apkpure", "apkmirror", "apkvision", "5play", "apkcombo",
)
SCHEMA_ID = "urn:nextos:schema:port-catalog:1"
SCHEMA_CANONICAL_SHA256 = (
    "353f6d057629f34916e19b644bce9733708bcaac021826fa8a53a0f467010cd1"
)
PRIVATE_TEXT_PATH_RE = re.compile(
    r"(?<![A-Za-z0-9])(?:"
    r"/(?:home/[^/\s]+|Users/[^/\s]+|root|mnt|tmp|var/tmp)(?=/|\s|$)"
    r"|[A-Za-z]:[\\/])",
    re.IGNORECASE,
)
IPV6_TEXT_RE = re.compile(
    r"(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{0,4}:){2,}"
    r"[0-9A-Fa-f]{0,4}(?![0-9A-Fa-f:])"
)
SAFE_SVG_ELEMENTS = frozenset((
    "svg", "g", "defs", "path", "rect", "circle", "ellipse", "line",
    "polyline", "polygon", "text", "tspan", "title", "desc", "symbol",
    "linearGradient", "radialGradient", "stop", "clipPath", "mask",
    "pattern", "filter", "feGaussianBlur", "feOffset", "feBlend",
    "feColorMatrix", "feComponentTransfer", "feFuncR", "feFuncG", "feFuncB",
    "feFuncA", "feMerge", "feMergeNode",
))
SEMANTIC_RULES = (
    "plain-text-no-controls-markup-private-literals-or-package-origins",
    "https-public-dns-no-credentials-port-query-fragment-or-ip",
    "catalog-id-equals-canonical-nxport-id-plus-nextos",
    "source-is-github-nextos-ports-and-repository-basename-equals-catalog-id",
    "latest-release-is-source-releases-latest-download-nxport-id-zip",
    "images-live-below-port-json-catalog-id-with-safe-posix-paths",
    "nxextract-installation-names-nxport-id-gamedata",
)


class CatalogError(Exception):
    """The editorial entry or its publication boundary is invalid."""


def _load_core():
    specification = importlib.util.spec_from_file_location(
        "nxgenerator_catalog_core", CORE_PATH
    )
    if specification is None or specification.loader is None:
        raise CatalogError("cannot load the nxgenerator core")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


CORE = _load_core()


def _real_path_without_symlinks(value, context):
    path = Path(os.path.abspath(os.fspath(value)))
    cursor = Path(path.anchor)
    for part in path.parts[1:]:
        cursor = cursor / part
        if cursor.is_symlink():
            raise CatalogError("%s traverses a symlink" % context)
    return path


def _open_readonly_nofollow(value, context):
    """Open a file through a directory-fd chain without following symlinks."""
    path = Path(os.path.abspath(os.fspath(value)))
    if path == Path(path.anchor):
        raise CatalogError("%s must name a file" % context)
    directory_descriptor = -1
    try:
        directory_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        directory_flags |= getattr(os, "O_DIRECTORY", 0)
        directory_flags |= getattr(os, "O_NOFOLLOW", 0)
        directory_descriptor = os.open(path.anchor, directory_flags)
        for part in path.parts[1:-1]:
            next_descriptor = os.open(
                part, directory_flags, dir_fd=directory_descriptor
            )
            os.close(directory_descriptor)
            directory_descriptor = next_descriptor
        file_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        file_flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(
            path.name, file_flags, dir_fd=directory_descriptor
        )
    except OSError as error:
        raise CatalogError("%s is not safely readable: %s" % (context, error))
    finally:
        if directory_descriptor >= 0:
            try:
                os.close(directory_descriptor)
            except OSError:
                pass
    return path, descriptor


def load_strict_json(path, context):
    """Read one bounded, stable, single-link JSON file through O_NOFOLLOW."""
    path, descriptor = _open_readonly_nofollow(path, context)
    try:
        try:
            before = os.fstat(descriptor)
            if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                    before.st_size > MAX_INPUT_BYTES):
                raise CatalogError(
                    "%s must be a single-link regular file of at most 64 KiB" %
                    context
                )
            with os.fdopen(descriptor, "rb", closefd=False) as stream:
                payload = stream.read(MAX_INPUT_BYTES + 1)
            after = os.fstat(descriptor)
        except OSError as error:
            raise CatalogError(
                "%s could not be read completely: %s" % (context, error)
            ) from error
    finally:
        try:
            os.close(descriptor)
        except OSError:
            pass
    if (len(payload) != before.st_size or len(payload) > MAX_INPUT_BYTES or
            (before.st_dev, before.st_ino, before.st_size, before.st_mode,
             before.st_mtime_ns, before.st_ctime_ns) !=
            (after.st_dev, after.st_ino, after.st_size, after.st_mode,
             after.st_mtime_ns, after.st_ctime_ns) or
            after.st_nlink != 1):
        raise CatalogError("%s changed while it was being read" % context)
    if payload.startswith(b"\xef\xbb\xbf"):
        raise CatalogError("%s cannot contain a UTF-8 BOM" % context)

    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise CatalogError(
                    "%s contains duplicate key %r" % (context, key)
                )
            result[key] = value
        return result

    def reject_constant(value):
        raise CatalogError(
            "%s contains non-JSON constant %s" % (context, value)
        )

    try:
        return json.loads(
            payload.decode("utf-8"), object_pairs_hook=unique_pairs,
            parse_constant=reject_constant,
        )
    except CatalogError:
        raise
    except (UnicodeDecodeError, ValueError) as error:
        raise CatalogError("%s is not valid UTF-8 JSON: %s" % (context, error))


def _exact_object(value, context, keys):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise CatalogError(
            "%s must contain exactly: %s" %
            (context, ", ".join(keys))
        )
    return value


def _schema_type_matches(value, expected):
    return {
        "object": isinstance(value, dict),
        "array": isinstance(value, list),
        "string": isinstance(value, str),
        "boolean": isinstance(value, bool),
    }.get(expected, False)


def validate_schema_instance(value, schema, context="catalog"):
    """Validate the JSON-Schema subset used by the checked-in v1 contract."""
    expected_type = schema.get("type")
    if expected_type is not None and not _schema_type_matches(
            value, expected_type):
        raise CatalogError("%s has the wrong JSON type" % context)
    if "enum" in schema and value not in schema["enum"]:
        raise CatalogError("%s is outside the schema enum" % context)
    if isinstance(value, str):
        if len(value) < schema.get("minLength", 0):
            raise CatalogError("%s is shorter than the schema minimum" % context)
        if len(value) > schema.get("maxLength", len(value)):
            raise CatalogError("%s exceeds the schema maximum" % context)
        if "pattern" in schema and re.search(schema["pattern"], value) is None:
            raise CatalogError("%s does not match the schema pattern" % context)
    elif isinstance(value, dict):
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        missing = set(required) - set(value)
        if missing:
            raise CatalogError(
                "%s lacks schema field(s): %s" %
                (context, ", ".join(sorted(missing)))
            )
        if schema.get("additionalProperties") is False:
            extra = set(value) - set(properties)
            if extra:
                raise CatalogError(
                    "%s has unknown schema field(s): %s" %
                    (context, ", ".join(sorted(extra)))
                )
        for key, item in value.items():
            if key in properties:
                validate_schema_instance(
                    item, properties[key], "%s.%s" % (context, key)
                )
    elif isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            raise CatalogError("%s has too few schema items" % context)
        if len(value) > schema.get("maxItems", len(value)):
            raise CatalogError("%s has too many schema items" % context)
        if schema.get("uniqueItems"):
            identities = [
                json.dumps(item, ensure_ascii=False, sort_keys=True,
                           separators=(",", ":"))
                for item in value
            ]
            if len(identities) != len(set(identities)):
                raise CatalogError("%s has duplicate schema items" % context)
        item_schema = schema.get("items")
        if item_schema is not None:
            for index, item in enumerate(value):
                validate_schema_instance(
                    item, item_schema, "%s[%d]" % (context, index)
                )


def audit_catalog_schema(schema):
    try:
        canonical = json.dumps(
            schema, ensure_ascii=False, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
    except (TypeError, ValueError):
        canonical = b""
    if hashlib.sha256(canonical).hexdigest() != SCHEMA_CANONICAL_SHA256:
        raise CatalogError("catalog schema contract drifted from the exporter")
    return schema


def load_catalog_schema():
    schema = load_strict_json(CATALOG_SCHEMA, "catalog schema")
    audit_catalog_schema(schema)
    return schema


def _plain_text(value, context, maximum):
    if (not isinstance(value, str) or not value or len(value) > maximum or
            value != value.strip()):
        raise CatalogError(
            "%s must be bounded, non-empty text without edge whitespace" %
            context
        )
    if any(ord(character) < 0x20 or 0x7f <= ord(character) <= 0x9f
           for character in value):
        raise CatalogError("%s contains a control character" % context)
    if any(0xd800 <= ord(character) <= 0xdfff for character in value):
        raise CatalogError("%s contains an isolated Unicode surrogate" % context)
    if "<" in value or ">" in value:
        raise CatalogError("%s must be plain text, not markup" % context)
    folded = value.casefold()
    if any(token in folded for token in FORBIDDEN_ORIGIN_TOKENS):
        raise CatalogError("%s reveals a forbidden package origin" % context)
    try:
        CORE.reject_public_literal(value, context)
    except CORE.ProjectError as error:
        raise CatalogError(str(error)) from error
    if PRIVATE_TEXT_PATH_RE.search(value) or IPV6_TEXT_RE.search(value):
        raise CatalogError("%s contains a private host literal" % context)
    return value


def _https_url(value, context):
    value = _plain_text(value, context, 512)
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError as error:
        raise CatalogError("%s is not a valid URL: %s" % (context, error))
    hostname = parsed.hostname
    if (parsed.scheme != "https" or not parsed.netloc or not hostname or
            parsed.username is not None or parsed.password is not None or
            port is not None or parsed.query or parsed.fragment or
            not URL_PATH_RE.fullmatch(parsed.path)):
        raise CatalogError(
            "%s must be an absolute HTTPS URL without credentials, port, "
            "query or fragment" % context
        )
    hostname = hostname.casefold()
    dns_labels = hostname.split(".")
    dns_label_re = re.compile(
        r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    )
    if (hostname == "localhost" or hostname.endswith((".local", ".localhost"))
            or len(hostname) > 253 or len(dns_labels) < 2 or
            any(dns_label_re.fullmatch(label) is None
                for label in dns_labels)):
        raise CatalogError("%s uses a private or invalid hostname" % context)
    try:
        ipaddress.ip_address(hostname)
    except ValueError:
        pass
    else:
        raise CatalogError("%s must not use an IP literal" % context)
    if any(part in ("", ".", "..") for part in parsed.path.split("/")[1:]):
        raise CatalogError("%s contains an unsafe URL path" % context)
    return value, parsed


def _image_path(value, context, catalog_id):
    value = _plain_text(value, context, 256)
    if "\\" in value:
        raise CatalogError("%s must use POSIX separators" % context)
    logical = PurePosixPath(value)
    if (logical.is_absolute() or logical.as_posix() != value or
            len(logical.parts) < 3 or
            any(part in ("", ".", "..") or part.startswith(".")
                for part in logical.parts) or
            logical.parts[:2] != ("port-json", catalog_id) or
            logical.suffix.casefold() not in IMAGE_EXTENSIONS):
        raise CatalogError(
            "%s must be a safe image below port-json/%s/" %
            (context, catalog_id)
        )
    return value


def _read_stable_asset(path, context):
    path, descriptor = _open_readonly_nofollow(path, context)
    try:
        try:
            before = os.fstat(descriptor)
            if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                    not 0 < before.st_size <= MAX_IMAGE_BYTES):
                raise CatalogError(
                    "%s must be a non-empty single-link regular file up to "
                    "16 MiB" % context
                )
            with os.fdopen(descriptor, "rb", closefd=False) as stream:
                payload = stream.read(MAX_IMAGE_BYTES + 1)
            after = os.fstat(descriptor)
        except OSError as error:
            raise CatalogError(
                "%s could not be read completely: %s" % (context, error)
            ) from error
    finally:
        try:
            os.close(descriptor)
        except OSError:
            pass
    if (len(payload) != before.st_size or len(payload) > MAX_IMAGE_BYTES or
            (before.st_dev, before.st_ino, before.st_size, before.st_mode,
             before.st_mtime_ns, before.st_ctime_ns) !=
            (after.st_dev, after.st_ino, after.st_size, after.st_mode,
             after.st_mtime_ns, after.st_ctime_ns) or after.st_nlink != 1):
        raise CatalogError("%s changed while it was being read" % context)
    return payload


def _valid_png(payload):
    if not payload.startswith(b"\x89PNG\r\n\x1a\n"):
        return False
    position = 8
    seen_ihdr = False
    seen_idat = False
    seen_iend = False
    compressed = []
    expected_scanline_bytes = None
    row_bytes = None
    while position < len(payload):
        if len(payload) - position < 12:
            return False
        length = int.from_bytes(payload[position:position + 4], "big")
        chunk_type = payload[position + 4:position + 8]
        data_start = position + 8
        data_end = data_start + length
        crc_end = data_end + 4
        if (length > MAX_IMAGE_BYTES or crc_end > len(payload) or
                re.fullmatch(rb"[A-Za-z]{4}", chunk_type) is None):
            return False
        chunk = payload[data_start:data_end]
        expected_crc = int.from_bytes(payload[data_end:crc_end], "big")
        if (zlib.crc32(chunk_type + chunk) & 0xffffffff) != expected_crc:
            return False
        if not seen_ihdr and chunk_type != b"IHDR":
            return False
        if chunk_type == b"IHDR":
            if seen_ihdr or length != 13:
                return False
            width = int.from_bytes(chunk[0:4], "big")
            height = int.from_bytes(chunk[4:8], "big")
            bit_depth = chunk[8]
            color_type = chunk[9]
            allowed_depths = {
                0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8),
                4: (8, 16), 6: (8, 16),
            }
            if (not 0 < width <= 16384 or not 0 < height <= 16384 or
                    bit_depth not in allowed_depths.get(color_type, ()) or
                    chunk[10] != 0 or chunk[11] != 0 or chunk[12] not in (0, 1)):
                return False
            if chunk[12] == 0:
                channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
                row_bytes = (width * channels * bit_depth + 7) // 8
                expected_scanline_bytes = height * (row_bytes + 1)
                if expected_scanline_bytes > MAX_DECODED_IMAGE_BYTES:
                    return False
            seen_ihdr = True
        elif chunk_type == b"IDAT":
            if not seen_ihdr or seen_iend or not chunk:
                return False
            seen_idat = True
            compressed.append(chunk)
        elif chunk_type == b"IEND":
            if length != 0 or not seen_idat or seen_iend or crc_end != len(payload):
                return False
            seen_iend = True
        position = crc_end
    if not (seen_ihdr and seen_idat and seen_iend):
        return False
    try:
        decoder = zlib.decompressobj()
        decoded = decoder.decompress(
            b"".join(compressed), MAX_DECODED_IMAGE_BYTES + 1
        )
    except zlib.error:
        return False
    if (not 0 < len(decoded) <= MAX_DECODED_IMAGE_BYTES or not decoder.eof or
            decoder.unused_data or decoder.unconsumed_tail):
        return False
    if expected_scanline_bytes is not None:
        if len(decoded) != expected_scanline_bytes:
            return False
        stride = row_bytes + 1
        if any(decoded[offset] > 4
               for offset in range(0, len(decoded), stride)):
            return False
    return True


def _valid_webp(payload):
    if (len(payload) < 20 or payload[:4] != b"RIFF" or
            payload[8:12] != b"WEBP" or
            int.from_bytes(payload[4:8], "little") != len(payload) - 8):
        return False
    position = 12
    seen_image = False
    while position < len(payload):
        if len(payload) - position < 8:
            return False
        fourcc = payload[position:position + 4]
        length = int.from_bytes(payload[position + 4:position + 8], "little")
        data_start = position + 8
        data_end = data_start + length
        padded_end = data_end + (length & 1)
        if data_end > len(payload) or padded_end > len(payload):
            return False
        chunk = payload[data_start:data_end]
        if fourcc == b"VP8X":
            if length != 10:
                return False
            width = 1 + int.from_bytes(chunk[4:7], "little")
            height = 1 + int.from_bytes(chunk[7:10], "little")
            if not 0 < width <= 16384 or not 0 < height <= 16384:
                return False
        elif fourcc == b"VP8L":
            if length <= 5 or chunk[0] != 0x2f:
                return False
            bits = int.from_bytes(chunk[1:5], "little")
            width = 1 + (bits & 0x3fff)
            height = 1 + ((bits >> 14) & 0x3fff)
            seen_image = seen_image or (width <= 16384 and height <= 16384)
        elif fourcc == b"VP8 ":
            if (length <= 10 or chunk[3:6] != b"\x9d\x01\x2a"):
                return False
            width = int.from_bytes(chunk[6:8], "little") & 0x3fff
            height = int.from_bytes(chunk[8:10], "little") & 0x3fff
            seen_image = seen_image or (0 < width <= 16384 and
                                        0 < height <= 16384)
        position = padded_end
    return position == len(payload) and seen_image


def _valid_jpeg(payload):
    if (len(payload) < 12 or payload[:2] != b"\xff\xd8" or
            payload[-2:] != b"\xff\xd9"):
        return False
    position = 2
    frame_components = None
    while position < len(payload) - 2:
        if payload[position] != 0xff:
            return False
        while position < len(payload) and payload[position] == 0xff:
            position += 1
        if position >= len(payload):
            return False
        marker = payload[position]
        position += 1
        if marker in (0x01,) or 0xd0 <= marker <= 0xd7:
            continue
        if marker in (0xd8, 0xd9) or position + 2 > len(payload):
            return False
        length = int.from_bytes(payload[position:position + 2], "big")
        if length < 2 or position + length > len(payload):
            return False
        segment = payload[position + 2:position + length]
        if marker in (0xc0, 0xc1, 0xc2, 0xc3, 0xc5, 0xc6, 0xc7,
                      0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf):
            if len(segment) < 6:
                return False
            height = int.from_bytes(segment[1:3], "big")
            width = int.from_bytes(segment[3:5], "big")
            components = segment[5]
            if not 0 < width <= 16384 or not 0 < height <= 16384:
                return False
            if not 1 <= components <= 4 or len(segment) != 6 + 3 * components:
                return False
            frame_components = components
        if marker == 0xda:
            if frame_components is None or len(segment) < 1:
                return False
            scan_components = segment[0]
            entropy = payload[position + length:-2]
            return (1 <= scan_components <= frame_components and
                    len(segment) == 4 + 2 * scan_components and
                    bool(entropy))
        position += length
    return False


def _xml_name(value):
    if value.startswith("{") and "}" in value:
        namespace, local = value[1:].split("}", 1)
        return namespace, local
    return "", value


def _valid_svg(payload):
    try:
        source = payload.decode("utf-8")
    except UnicodeDecodeError:
        return False
    folded = source.casefold()
    if any(token in folded for token in (
            "<!doctype", "<!entity", "<?xml-stylesheet", "javascript:",
            "vbscript:", "data:", "file:", "@import", "expression(")):
        return False
    try:
        root = ElementTree.fromstring(source)
    except ElementTree.ParseError:
        return False
    root_namespace, root_name = _xml_name(root.tag)
    if (root_name != "svg" or root_namespace not in
            ("", "http://www.w3.org/2000/svg")):
        return False
    fragment = re.compile(r"#[A-Za-z_][A-Za-z0-9_.:-]*")
    for element in root.iter():
        namespace, name = _xml_name(element.tag)
        if (namespace not in ("", "http://www.w3.org/2000/svg") or
                name not in SAFE_SVG_ELEMENTS):
            return False
        for raw_name, raw_value in element.attrib.items():
            attribute_namespace, attribute = _xml_name(raw_name)
            if attribute_namespace not in (
                    "", "http://www.w3.org/2000/svg",
                    "http://www.w3.org/1999/xlink"):
                return False
            attribute_folded = attribute.casefold()
            value = raw_value.strip()
            value_folded = value.casefold()
            if (attribute_folded.startswith("on") or
                    attribute_folded == "style" or
                    any(token in value_folded for token in (
                        "javascript:", "vbscript:", "data:", "file:",
                        "http:", "https:", "//", "@import", "expression(",
                    ))):
                return False
            if attribute_folded in ("href", "src") and not fragment.fullmatch(value):
                return False
            for reference in re.findall(
                    r"url\(\s*['\"]?([^)'\"\s]+)['\"]?\s*\)", value,
                    flags=re.IGNORECASE):
                if fragment.fullmatch(reference) is None:
                    return False
    return True


def _validate_image_payload(path, payload, context):
    suffix = PurePosixPath(path).suffix.casefold()
    if suffix == ".png":
        valid = _valid_png(payload)
    elif suffix in (".jpg", ".jpeg"):
        valid = _valid_jpeg(payload)
    elif suffix == ".webp":
        valid = _valid_webp(payload)
    elif suffix == ".svg":
        valid = _valid_svg(payload)
    else:
        valid = False
    if not valid:
        raise CatalogError("%s content failed bounded %s format checks" %
                           (context, suffix))


def validate_catalog_assets(entry, assets_root=None):
    """Validate real website images when placeholder=false or root is given."""
    if assets_root is None:
        if entry["images"]["placeholder"] is False:
            raise CatalogError(
                "catalog images with placeholder=false require --assets-root"
            )
        return {}
    root = _real_path_without_symlinks(assets_root, "catalog assets root")
    if not root.is_dir():
        raise CatalogError("catalog assets root must be a real directory")
    records = {}
    paths = [entry["images"]["cover"]] + entry["images"]["screenshots"]
    for index, logical in enumerate(paths):
        context = "catalog asset %d" % index
        path = root.joinpath(*PurePosixPath(logical).parts)
        payload = _read_stable_asset(path, context)
        _validate_image_payload(logical, payload, context)
        records[logical] = hashlib.sha256(payload).hexdigest()
    return records


def validate_catalog(document, project_config):
    """Return the exact Ronax v1 shape in its stable presentation order."""
    validate_schema_instance(document, load_catalog_schema())
    entry = _exact_object(document, "catalog", CATALOG_KEYS)
    catalog_id = _plain_text(entry["id"], "catalog.id", 63)
    if not SLUG_RE.fullmatch(catalog_id):
        raise CatalogError("catalog.id must be a lowercase hyphenated slug")

    nxport = project_config["nxport"]
    canonical_catalog_id = (
        nxport["id"] if nxport["id"].endswith("-nextos")
        else nxport["id"] + "-nextos"
    )
    if catalog_id != canonical_catalog_id:
        raise CatalogError(
            "catalog.id must be the canonical nxport id with -nextos suffix: "
            "%s" % canonical_catalog_id
        )
    name = _plain_text(entry["name"], "catalog.name", 128)
    if name.casefold() != nxport["title"].casefold():
        raise CatalogError(
            "catalog.name differs from nxport.title (case changes are allowed)"
        )
    description = _plain_text(
        entry["description"], "catalog.description", 2048
    )

    build = _exact_object(
        entry["build"], "catalog.build", ("version", "type")
    )
    version = _plain_text(build["version"], "catalog.build.version", 64)
    if not SEMVER_RE.fullmatch(version):
        raise CatalogError("catalog.build.version must be SemVer")
    build_type = _plain_text(build["type"], "catalog.build.type", 16)
    if build_type not in BUILD_TYPES:
        raise CatalogError(
            "catalog.build.type must be one of %s" % ", ".join(BUILD_TYPES)
        )
    if build_type == "stable" and "-" in version.split("+", 1)[0]:
        raise CatalogError(
            "catalog.build.type stable cannot use a prerelease version"
        )

    links = _exact_object(
        entry["links"], "catalog.links", ("source", "latest_release")
    )
    source, _ = _https_url(
        links["source"], "catalog.links.source"
    )
    latest_release, _ = _https_url(
        links["latest_release"], "catalog.links.latest_release"
    )
    expected_source = "https://github.com/NextOs-Ports/%s" % catalog_id
    if source != expected_source:
        raise CatalogError(
            "catalog source must be https://github.com/NextOs-Ports/<id>"
        )
    expected_release = (
        expected_source + "/releases/latest/download/" +
        nxport["id"] + ".zip"
    )
    if latest_release != expected_release:
        raise CatalogError(
            "catalog latest_release must be the source repository's exact "
            "releases/latest/download/<nxport.id>.zip URL"
        )

    images = _exact_object(
        entry["images"], "catalog.images",
        ("cover", "screenshots", "placeholder"),
    )
    cover = _image_path(images["cover"], "catalog.images.cover", catalog_id)
    raw_screenshots = images["screenshots"]
    if (not isinstance(raw_screenshots, list) or
            not 1 <= len(raw_screenshots) <= 12):
        raise CatalogError(
            "catalog.images.screenshots must contain 1 to 12 images"
        )
    screenshots = [
        _image_path(item, "catalog.images.screenshots[%d]" % index,
                    catalog_id)
        for index, item in enumerate(raw_screenshots)
    ]
    if len(screenshots) != len(set(screenshots)) or cover in screenshots:
        raise CatalogError("catalog images must be unique")
    if not isinstance(images["placeholder"], bool):
        raise CatalogError("catalog.images.placeholder must be a boolean")

    installation = _plain_text(
        entry["installation"], "catalog.installation", 2048
    )
    if project_config["recipe_source"] is not None:
        owner_path = "%s/%s/" % (
            nxport["id"], project_config["owner_data"]["directory"]
        )
        if owner_path.casefold() not in installation.casefold():
            raise CatalogError(
                "catalog.installation must name the canonical owner-data "
                "path %s" % owner_path
            )

    return {
        "id": catalog_id,
        "name": name,
        "description": description,
        "build": {"version": version, "type": build_type},
        "links": {"source": source, "latest_release": latest_release},
        "images": {
            "cover": cover,
            "screenshots": screenshots,
            "placeholder": images["placeholder"],
        },
        "installation": installation,
    }


def render_catalog(entry):
    return (json.dumps(entry, ensure_ascii=False, indent=2) + "\n").encode(
        "utf-8"
    )


def _rename_noreplace_at(directory_descriptor, source_name, destination_name):
    try:
        renameat2 = ctypes.CDLL(None, use_errno=True).renameat2
    except AttributeError as error:
        raise CatalogError(
            "host lacks renameat2; no-overwrite publication is unavailable"
        ) from error
    renameat2.argtypes = (
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
        ctypes.c_uint,
    )
    renameat2.restype = ctypes.c_int
    result = renameat2(
        directory_descriptor, os.fsencode(source_name),
        directory_descriptor, os.fsencode(destination_name), 1,
    )
    if result == 0:
        return
    selected_errno = ctypes.get_errno()
    if selected_errno in (errno.EEXIST, errno.ENOTEMPTY):
        raise CatalogError(
            "refusing to overwrite catalog output: %s" % destination_name
        )
    raise CatalogError(
        "cannot publish catalog output without replacement: %s" %
        os.strerror(selected_errno)
    )


def _directory_path_matches(path, descriptor_state):
    try:
        current = os.lstat(path)
    except OSError:
        return False
    return (stat.S_ISDIR(current.st_mode) and
            (current.st_dev, current.st_ino) ==
            (descriptor_state.st_dev, descriptor_state.st_ino))


def publish_no_replace(payload, output):
    output = _real_path_without_symlinks(output, "catalog output")
    if output.suffix != ".json" or output.name.startswith("."):
        raise CatalogError("catalog output must be a visible .json file")
    parent = output.parent
    if not parent.is_dir():
        raise CatalogError("catalog output parent must already exist")
    if output.exists() or output.is_symlink():
        raise CatalogError("refusing to overwrite catalog output: %s" % output)

    try:
        listed_parent = os.lstat(parent)
    except OSError as error:
        raise CatalogError("catalog output parent is not readable: %s" % error)
    if not stat.S_ISDIR(listed_parent.st_mode):
        raise CatalogError("catalog output parent must be a real directory")

    descriptor = -1
    directory_descriptor = -1
    temporary_name = None
    temporary_identity = None
    published = False
    staged_state = None
    try:
        directory_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        directory_flags |= getattr(os, "O_DIRECTORY", 0)
        directory_flags |= getattr(os, "O_NOFOLLOW", 0)
        directory_descriptor = os.open(parent, directory_flags)
        parent_before = os.fstat(directory_descriptor)
        if ((listed_parent.st_dev, listed_parent.st_ino) !=
                (parent_before.st_dev, parent_before.st_ino) or
                not _directory_path_matches(parent, parent_before)):
            raise CatalogError("catalog output parent changed before publication")
        try:
            os.stat(output.name, dir_fd=directory_descriptor,
                    follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            raise CatalogError(
                "refusing to overwrite catalog output: %s" % output
            )
        temporary_flags = (
            os.O_WRONLY | os.O_CREAT | os.O_EXCL |
            getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        )
        for _ in range(128):
            candidate = ".nextos-catalog.%s" % secrets.token_hex(16)
            try:
                descriptor = os.open(
                    candidate, temporary_flags, 0o600,
                    dir_fd=directory_descriptor,
                )
            except FileExistsError:
                continue
            temporary_name = candidate
            temporary_state = os.fstat(descriptor)
            temporary_identity = (temporary_state.st_dev, temporary_state.st_ino)
            break
        if descriptor < 0:
            raise CatalogError("cannot reserve a catalog staging file")
        os.fchmod(descriptor, 0o644)
        stream = os.fdopen(descriptor, "wb", closefd=True)
        descriptor = -1
        with stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
            staged_state = os.fstat(stream.fileno())
        if not _directory_path_matches(parent, parent_before):
            raise CatalogError("catalog output parent changed during staging")
        os.fsync(directory_descriptor)
        _rename_noreplace_at(
            directory_descriptor, temporary_name, output.name
        )
        published = True
        temporary_name = None
        published_state = os.stat(
            output.name, dir_fd=directory_descriptor,
            follow_symlinks=False,
        )
        if (staged_state is None or
                (published_state.st_dev, published_state.st_ino,
                 published_state.st_size, stat.S_IMODE(published_state.st_mode),
                 published_state.st_nlink) !=
                (staged_state.st_dev, staged_state.st_ino,
                 staged_state.st_size, 0o644, 1)):
            raise CatalogError("published catalog output changed at commit")
        if not _directory_path_matches(parent, parent_before):
            raise CatalogError("catalog output parent changed during publication")
        os.fsync(directory_descriptor)
    except (CatalogError, OSError) as error:
        rollback_error = None
        if published and directory_descriptor >= 0:
            try:
                current = os.stat(
                    output.name, dir_fd=directory_descriptor,
                    follow_symlinks=False,
                )
                if (staged_state is None or
                        (current.st_dev, current.st_ino) !=
                        (staged_state.st_dev, staged_state.st_ino)):
                    raise OSError(
                        errno.ESTALE,
                        "published output name no longer belongs to staging",
                    )
                os.unlink(output.name, dir_fd=directory_descriptor)
                os.fsync(directory_descriptor)
                published = False
            except FileNotFoundError:
                try:
                    os.fsync(directory_descriptor)
                    published = False
                except OSError as selected:
                    rollback_error = selected
            except OSError as selected:
                rollback_error = selected
        if rollback_error is not None:
            raise CatalogError(
                "catalog output was fully written but durability failed and "
                "rollback also failed: %s" % rollback_error
            ) from error
        if isinstance(error, CatalogError):
            raise
        raise CatalogError("cannot publish catalog output: %s" % error) from error
    finally:
        if descriptor >= 0:
            try:
                os.close(descriptor)
            except OSError:
                pass
        if temporary_name is not None and directory_descriptor >= 0:
            try:
                current = os.stat(
                    temporary_name, dir_fd=directory_descriptor,
                    follow_symlinks=False,
                )
                if (temporary_identity is not None and
                        (current.st_dev, current.st_ino) == temporary_identity):
                    os.unlink(temporary_name, dir_fd=directory_descriptor)
            except OSError:
                pass
        if directory_descriptor >= 0:
            try:
                os.close(directory_descriptor)
            except OSError:
                pass
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Export a deterministic NextOS website catalog entry"
    )
    parser.add_argument("catalog_manifest", type=Path)
    parser.add_argument("--nxproject", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--assets-root", type=Path,
        help=("website checkout root containing port-json/ assets; required "
              "when placeholder is false"),
    )
    parser.add_argument(
        "--source-root", type=Path, default=CORE.REPOSITORY,
        help="root for nxproject inputs (defaults to the framework repository)",
    )
    options = parser.parse_args(argv)
    try:
        project = load_strict_json(options.nxproject, "nxproject manifest")
        try:
            config = CORE.validate_project(project, options.source_root)
        except CORE.ProjectError as error:
            raise CatalogError("invalid nxproject: %s" % error) from error
        catalog = load_strict_json(
            options.catalog_manifest, "catalog manifest"
        )
        entry = validate_catalog(catalog, config)
        assets = validate_catalog_assets(entry, options.assets_root)
        output = publish_no_replace(render_catalog(entry), options.output)
    except CatalogError as error:
        print("nxgenerator catalog: %s" % error, file=os.sys.stderr)
        return 1
    for logical, digest in sorted(assets.items()):
        print("ASSET_SHA256=%s PATH=%s" % (digest, logical))
    print("generated NextOS catalog %s for %s in %s (assets_verified=%d)" % (
        entry["id"], config["nxport"]["id"], output, len(assets),
    ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
