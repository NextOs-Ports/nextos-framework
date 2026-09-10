#!/usr/bin/env python3
"""Prepare NXExtract-authenticated Freedom Planet 2 owner data.

NXExtract authenticates the ARM64 payload before this hook runs.  The hook
changes Vulkan to GLES2 in BuildSettings and derives GLES2 ShaderProgram blobs
from the owner's own Vulkan programs.  It never writes outside NXExtract's
disposable stage.  No APK, game asset, generated shader, or binary delta is
distributed.
"""

from __future__ import print_function

import argparse
import json
import os
import shutil
import stat
import struct
import subprocess
import sys


FORMAT = 2
PACKAGE = "com.GalaxyTrail.FP2"
PREPARATION = "fp2-vulkan-to-exact-gles2-v1"
MARKER = ".fp2-data.json"
TASK_ORDER_CONTRACT = "first-seen-unique-source-metadata-v1"
TASK_REPEAT_CONTRACT = "reuse-first-decision-and-cache-key"
TRANSLATOR_RELATIVE = "nxextract/bin/fp2-smolv-cross-nextos"
LZ4_RELATIVE = "nxextract/lib/aarch64/liblz4.so.1"
VULKAN = 21
GLES2 = 8


class PreparationError(RuntimeError):
    pass


def fail(message):
    raise PreparationError("Freedom Planet 2 preparation failed: " + message)


def json_no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            fail("duplicate policy key %s" % key)
        result[key] = value
    return result


def load_policy(path):
    try:
        with open(path, "r", encoding="utf-8") as stream:
            policy = json.load(stream, object_pairs_hook=json_no_duplicates)
    except (OSError, ValueError) as error:
        fail("cannot read policy: %s" % error)
    if policy.get("format") != FORMAT:
        fail("unsupported preparation policy format")
    if policy.get("package") != PACKAGE or policy.get("preparation") != PREPARATION:
        fail("preparation policy identifies another transformation")
    outputs = policy.get("logical_outputs")
    if not isinstance(outputs, list) or len(outputs) != 31:
        fail("preparation policy has an invalid logical output list")
    paths = []
    for record in outputs:
        if not isinstance(record, dict) or set(record) != {"path"}:
            fail("preparation policy has an invalid logical output")
        paths.append(safe_relative(record["path"], "logical output"))
    if paths != sorted(set(paths)):
        fail("preparation policy logical outputs are not unique and sorted")
    if "assets/bin/Data/globalgamemanagers" not in paths:
        fail("preparation policy omits BuildSettings")
    decisions = policy.get("task_decisions")
    if not isinstance(decisions, dict):
        fail("preparation policy has no task decision contract")
    required = {
        "contract", "omit_token", "repeat", "sequence", "translate_token",
    }
    if set(decisions) != required:
        fail("preparation policy task decision contract has unknown keys")
    if (decisions.get("contract") != TASK_ORDER_CONTRACT or
            decisions.get("repeat") != TASK_REPEAT_CONTRACT or
            decisions.get("translate_token") != "T" or
            decisions.get("omit_token") != "O"):
        fail("preparation policy task decision contract is unsupported")
    sequence = decisions.get("sequence")
    if not isinstance(sequence, str) or not sequence or set(sequence) - {"T", "O"}:
        fail("preparation policy task decision sequence is invalid")
    stats = policy.get("expected_stats")
    if not isinstance(stats, dict):
        fail("preparation policy has no expected statistics")
    if (len(sequence) != stats.get("tasks_seen") or
            sequence.count("T") != stats.get("tasks_accepted") or
            sequence.count("O") != stats.get("tasks_skipped")):
        fail("preparation policy task decisions disagree with expected statistics")
    return policy


def regular_file(path):
    try:
        return stat.S_ISREG(os.lstat(path).st_mode)
    except OSError:
        return False


def safe_relative(value, label):
    if not isinstance(value, str) or not value or "\\" in value or "\0" in value:
        fail("invalid %s" % label)
    parts = value.split("/")
    if value.startswith("/") or any(part in ("", ".", "..") for part in parts):
        fail("unsafe %s %r" % (label, value))
    return value


def inside(root, path):
    root = os.path.realpath(root)
    path = os.path.realpath(path)
    return path == root or path.startswith(root + os.sep)


def stage_path(stage, relative):
    relative = safe_relative(relative, "stage path")
    path = os.path.abspath(os.path.join(stage, *relative.split("/")))
    if not inside(stage, path):
        fail("stage path escaped its root")
    return path


def validate_owner_input(stage, policy):
    assets = stage_path(stage, "assets")
    if not os.path.isdir(assets) or os.path.islink(assets):
        fail("NXExtract-authenticated owner asset tree is missing or unsafe")

    library_root = stage_path(stage, "lib")
    if not os.path.isdir(library_root) or os.path.islink(library_root):
        fail("ARM64 library directory is missing or unsafe")
    actual_names = sorted(os.listdir(library_root))
    expected_names = ["libil2cpp.so", "libmain.so", "libunity.so"]
    if actual_names != expected_names:
        fail("NXExtract-authenticated ARM64 library set changed")
    for name in expected_names:
        path = os.path.join(library_root, name)
        if not regular_file(path) or os.path.islink(path):
            fail("NXExtract-authenticated owner library is missing or unsafe: " + name)

    for record in policy["logical_outputs"]:
        path = stage_path(stage, record["path"])
        validate_logical_asset(path)


def split_parts(path):
    parts = []
    index = 0
    while regular_file("%s.split%d" % (path, index)):
        parts.append("%s.split%d" % (path, index))
        index += 1
    return parts


def validate_logical_asset(path):
    if regular_file(path) and not os.path.islink(path):
        parts = [path]
    else:
        parts = split_parts(path)
    if not parts:
        fail("logical asset is missing: %s" % path)
    for part in parts:
        if not regular_file(part) or os.path.islink(part):
            fail("logical asset has a symlink part")
    return parts


def atomic_bytes(path, payload):
    temporary = "%s.nxpart.%d" % (path, os.getpid())
    if os.path.lexists(temporary):
        fail("stale or unsafe temporary output")
    try:
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def patch_graphics_api(path, unitypy):
    environment = unitypy.load(path)
    objects = [obj for obj in environment.objects if obj.type.name == "BuildSettings"]
    if len(objects) != 1:
        fail("globalgamemanagers does not contain one BuildSettings object")
    obj = objects[0]
    tree = obj.read_typetree()
    apis = list(tree.get("m_GraphicsAPIs", []))
    if apis != [VULKAN]:
        fail("unexpected pristine graphics API list %r" % apis)
    raw = obj.get_raw_data()
    needle = struct.pack("<ii", 1, VULKAN)
    hits = [offset for offset in range(0, len(raw) - 7)
            if raw[offset:offset + 8] == needle]
    if len(hits) != 1:
        fail("BuildSettings graphics API encoding is ambiguous")
    with open(path, "rb") as stream:
        payload = bytearray(stream.read())
    absolute = obj.byte_start + hits[0] + 4
    if struct.unpack_from("<i", payload, absolute)[0] != VULKAN:
        fail("BuildSettings graphics API offset changed")
    struct.pack_into("<i", payload, absolute, GLES2)
    atomic_bytes(path, bytes(payload))
    verify = unitypy.load(path)
    values = [list(item.read_typetree().get("m_GraphicsAPIs", []))
              for item in verify.objects if item.type.name == "BuildSettings"]
    if values != [[GLES2]]:
        fail("patched graphics API did not read back as GLES2")


class PolicyTranslator:
    def __init__(self, executable, work_dir, decisions, approved_skip):
        self.executable = executable
        self.work_dir = work_dir
        self.sequence = decisions["sequence"]
        self.approved_skip = approved_skip
        self.identities = {}
        self.decision_by_key = {}
        self.seen = set()
        self.invoked = set()

    def key(self, source, metadata):
        identity = (source, metadata)
        key = self.identities.get(identity)
        if key is None:
            index = len(self.identities)
            if index >= len(self.sequence):
                fail("shader task order exceeds the authenticated policy")
            key = "task-%04d" % (index + 1)
            self.identities[identity] = key
            self.decision_by_key[key] = self.sequence[index]
        self.seen.add(key)
        return key

    def translate(self, task):
        key = task["key"]
        decision = self.decision_by_key.get(key)
        if decision == "O":
            raise self.approved_skip("policy-selected GLES2 omission " + key)
        if decision != "T":
            fail("translator received a task outside the decision order")
        task_dir = os.path.join(self.work_dir, key)
        os.mkdir(task_dir, 0o700)
        source_path = os.path.join(task_dir, "source.bin")
        metadata_path = os.path.join(task_dir, "metadata.tsv")
        vertex_path = os.path.join(task_dir, "vertex.glsl")
        fragment_path = os.path.join(task_dir, "fragment.glsl")
        atomic_bytes(source_path, task["source"])
        atomic_bytes(metadata_path, task["metadata"].encode("utf-8"))
        try:
            result = subprocess.run(
                [self.executable, source_path, metadata_path,
                 vertex_path, fragment_path],
                cwd=task_dir,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
                timeout=120,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            fail("translator could not complete task %s: %s" % (key, error))
        if result.returncode != 0:
            fail("approved task %s was rejected by the translator: %s" %
                 (key, result.stdout.strip()[:400]))
        for label, path in (("vertex", vertex_path), ("fragment", fragment_path)):
            if not regular_file(path) or os.path.islink(path):
                fail("translator did not create a safe %s output" % label)
            size = os.path.getsize(path)
            if size < 32 or size > 1024 * 1024:
                fail("translator created an implausible %s output" % label)
            try:
                with open(path, "r", encoding="utf-8") as stream:
                    text = stream.read()
            except (OSError, UnicodeError) as error:
                fail("translator %s output is not UTF-8: %s" % (label, error))
            if (not text.startswith("#version 100\n") or "void main" not in text
                    or "\0" in text):
                fail("translator %s output is not a complete ESSL100 stage" % label)
        self.invoked.add(key)
        return {"key": key, "vertex": vertex_path, "fragment": fragment_path}


def safe_packaged_file(game_dir, relative, label, executable=False):
    relative = safe_relative(relative, label + " path")
    cursor = game_dir
    parts = relative.split("/")
    for index, part in enumerate(parts):
        cursor = os.path.join(cursor, part)
        try:
            mode = os.lstat(cursor).st_mode
        except OSError:
            fail("%s is missing or unsafe" % label)
        if stat.S_ISLNK(mode):
            fail("%s path contains a symbolic link" % label)
        if index < len(parts) - 1 and not stat.S_ISDIR(mode):
            fail("%s parent is not a directory" % label)
    if not regular_file(cursor) or os.path.islink(cursor):
        fail("%s is missing or unsafe" % label)
    if not inside(game_dir, cursor):
        fail("%s must be supplied under the port directory" % label)
    if executable and not os.access(cursor, os.X_OK):
        fail("%s is not executable" % label)
    return os.path.realpath(cursor)


def validate_tools(game_dir, policy):
    tool = policy["translator"]
    if not isinstance(tool, dict) or tool != {"path": TRANSLATOR_RELATIVE}:
        fail("preparation policy translator path is not canonical")
    translator = safe_packaged_file(
        game_dir, TRANSLATOR_RELATIVE, "packaged GLES2 translator", executable=True)

    tool = policy["lz4"]
    if not isinstance(tool, dict) or tool != {"path": LZ4_RELATIVE}:
        fail("preparation policy LZ4 path is not canonical")
    lz4_path = safe_packaged_file(
        game_dir, LZ4_RELATIVE, "packaged LZ4 runtime")
    return translator, lz4_path


def reject_stale_python_cache(game_dir):
    root = os.path.join(game_dir, "nxextract")
    if not os.path.isdir(root) or os.path.islink(root):
        fail("packaged Python root is missing or unsafe")
    for current, directories, files in os.walk(root, followlinks=False):
        for name in directories:
            path = os.path.join(current, name)
            if name == "__pycache__":
                fail("stale Python bytecode cache is forbidden")
            if os.path.islink(path):
                fail("packaged Python path contains a symbolic link")
        for name in files:
            if name.endswith(".pyc"):
                fail("stale Python bytecode is forbidden")


def configure_python(game_dir, lz4_path):
    reject_stale_python_cache(game_dir)
    vendor = os.path.join(game_dir, "nxextract", "vendor", "python")
    if not os.path.isdir(vendor) or os.path.islink(vendor):
        fail("vendored Python runtime is missing or unsafe")
    sys.path.insert(0, vendor)
    sys.path.insert(0, os.path.join(game_dir, "nxextract"))
    os.environ["PF2_LZ4_LIBRARY"] = lz4_path
    try:
        import UnityPy
        import exact_gles2
    except Exception as error:
        fail("could not load the pinned Unity preparation runtime: %s" % error)
    return UnityPy, exact_gles2


def validate_final_paths(stage, policy):
    for record in policy["logical_outputs"]:
        path = stage_path(stage, record["path"])
        validate_logical_asset(path)


def write_marker(stage, stats):
    document = {
        "format": FORMAT,
        "inputs": "nxextract-authenticated",
        "logical_outputs": 31,
        "package": PACKAGE,
        "preparation": PREPARATION,
        "shader_stats": stats,
    }
    payload = (json.dumps(document, ensure_ascii=True, sort_keys=True,
                          separators=(",", ":")) + "\n").encode("ascii")
    atomic_bytes(os.path.join(stage, MARKER), payload)


def prepare(stage, game_dir, policy):
    translator_path, lz4_path = validate_tools(game_dir, policy)
    unitypy, exact = configure_python(game_dir, lz4_path)

    print("[prepare] checking NXExtract-authenticated ARM64 inputs")
    validate_owner_input(stage, policy)
    graphics_path = stage_path(stage, "assets/bin/Data/globalgamemanagers")
    print("[prepare] selecting GLES2 in the staged BuildSettings")
    try:
        patch_graphics_api(graphics_path, unitypy)
    except PreparationError:
        raise
    except Exception as error:
        fail("could not patch staged BuildSettings: %s" % error)

    work_dir = os.path.join(stage, ".fp2-prepare-work")
    if os.path.lexists(work_dir):
        fail("stale or unsafe preparation work directory")
    os.mkdir(work_dir, 0o700)
    translator = PolicyTranslator(
        translator_path, work_dir, policy["task_decisions"], exact.ApprovedSkip)
    injector = exact.ExactInjector(translator)
    results = []
    try:
        shader_records = [record for record in policy["logical_outputs"]
                          if record["path"] != "assets/bin/Data/globalgamemanagers"]
        print("[prepare] deriving exact GLES2 shaders from the staged Vulkan programs")
        for index, record in enumerate(shader_records, 1):
            path = stage_path(stage, record["path"])
            try:
                result = exact.process_file(path, injector, dry_run=False)
            except PreparationError:
                raise
            except Exception as error:
                fail("could not prepare %s: %s" % (record["path"], error))
            if result is None:
                fail("expected shader asset was not transformed: " + record["path"])
            results.append(result)
            print("[prepare] shaders %d/%d: %s" %
                  (index, len(shader_records), record["path"].split("/")[-1]))
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)

    expected_keys = {
        "task-%04d" % index for index in range(1, len(translator.sequence) + 1)
    }
    if translator.seen != expected_keys:
        fail("shader task coverage differs from the authenticated task order")
    accepted_keys = {
        "task-%04d" % (index + 1)
        for index, decision in enumerate(translator.sequence)
        if decision == "T"
    }
    if translator.invoked != accepted_keys:
        fail("translated tasks differ from the authenticated decision order")
    stats = {
        "exact_entries": sum(shader["exact_entries"]
                             for result in results for shader in result["shaders"]),
        "files": len(results),
        "installed_records": sum(shader["installed_records"]
                                 for result in results for shader in result["shaders"]),
        "omitted_records": sum(shader["omitted_records"]
                               for result in results for shader in result["shaders"]),
        "shader_objects": sum(len(result["shaders"]) for result in results),
        "skipped_entries": sum(shader["skipped_entries"]
                               for result in results for shader in result["shaders"]),
        "tasks_accepted": len(translator.invoked),
        "tasks_seen": len(translator.seen),
        "tasks_skipped": len(translator.seen - translator.invoked),
    }
    if stats != policy["expected_stats"]:
        fail("shader preparation statistics changed: %r" % stats)

    validate_final_paths(stage, policy)
    write_marker(stage, stats)
    print("[prepare] FP2 owner data ready: 31/31 transformed paths present")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--game-dir", required=True)
    args = parser.parse_args(argv)

    if os.path.islink(args.stage) or not os.path.isdir(args.stage):
        fail("NXExtract stage is missing or unsafe")
    if os.path.islink(args.game_dir) or not os.path.isdir(args.game_dir):
        fail("port directory is missing or unsafe")
    stage = os.path.realpath(args.stage)
    game_dir = os.path.realpath(args.game_dir)
    policy_path = os.path.join(game_dir, "nxextract",
                               "fp2-preparation-policy.json")
    policy = load_policy(policy_path)
    prepare(stage, game_dir, policy)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PreparationError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
