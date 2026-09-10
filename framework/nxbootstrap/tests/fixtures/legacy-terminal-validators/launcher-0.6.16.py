
import json
import os
import re
import stat
import sys

sys.tracebacklimit = 0
path, process_status_text = sys.argv[1:]
process_status = int(process_status_text)
top_keys = {
    "schema", "schema_version", "nxextract_version", "outcome", "code",
    "final_phase", "recipe", "package_id", "abi", "container",
    "validated", "logs", "duration_ms", "completed_unix", "error",
}
phase_ids = (
    "preparing", "scanning", "validating-packages", "selecting",
    "extracting", "processing", "validating-data", "installing", "ready",
)
sha256_re = re.compile(r"^[0-9a-f]{64}$")
safe_id_re = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")

def reject(message):
    raise ValueError(message)

def pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            reject("duplicate JSON member")
        result[key] = value
    return result

def finite_constant(_value):
    reject("non-finite JSON number")

def exact_object(value, keys, label):
    if not isinstance(value, dict) or set(value) != set(keys):
        reject("invalid " + label + " object")

def integer(value, label):
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        reject("invalid " + label)

def string(value, minimum, maximum, label):
    if not isinstance(value, str) or not minimum <= len(value) <= maximum:
        reject("invalid " + label)
    if any(ord(char) < 32 for char in value):
        reject("control byte in " + label)

def relative_path(value, label):
    string(value, 1, 512, label)
    if value.startswith("/") or "\\" in value or ".." in value.split("/"):
        reject("invalid " + label)

flags = (os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) |
         getattr(os, "O_NOFOLLOW", 0))
fd = os.open(path, flags)
try:
    opened = os.fstat(fd)
    linked = os.lstat(path)
    if (not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1 or
            (opened.st_dev, opened.st_ino) != (linked.st_dev, linked.st_ino)):
        reject("unsafe result identity")
    chunks = []
    total = 0
    while True:
        block = os.read(fd, 65536)
        if not block:
            break
        total += len(block)
        if total > 1048576:
            reject("result exceeds size limit")
        chunks.append(block)
finally:
    os.close(fd)

document = json.loads(
    b"".join(chunks).decode("utf-8", "strict"),
    object_pairs_hook=pairs,
    parse_constant=finite_constant,
)
exact_object(document, top_keys, "terminal result")
if (document["schema"] != "org.nextos.nxextract.terminal-result" or
        isinstance(document["schema_version"], bool) or
        document["schema_version"] != 1 or
        document["nxextract_version"] != "1.2.10"):
    reject("unknown terminal result schema")
if document["outcome"] not in ("success", "error"):
    reject("invalid terminal outcome")
if (process_status == 0) != (document["outcome"] == "success"):
    reject("process status and terminal outcome disagree")
if (not isinstance(document["code"], str) or
        not re.fullmatch(r"^NXE[0-9]{4}$", document["code"])):
    reject("invalid terminal code")

phase = document["final_phase"]
exact_object(phase, ("index", "id", "label"), "final phase")
integer(phase["index"], "phase index")
if phase["index"] > 8 or phase["id"] != phase_ids[phase["index"]]:
    reject("invalid final phase")
string(phase["label"], 1, 64, "phase label")

recipe = document["recipe"]
exact_object(recipe, ("id", "version", "digest"), "recipe")
if (not isinstance(recipe["id"], str) or
        not safe_id_re.fullmatch(recipe["id"])):
    reject("invalid recipe id")
string(recipe["version"], 1, 128, "recipe version")
if (not isinstance(recipe["digest"], str) or
        not sha256_re.fullmatch(recipe["digest"])):
    reject("invalid recipe digest")

if document["package_id"] is not None:
    string(document["package_id"], 1, 255, "package id")
abi = document["abi"]
if (abi is not None and
        (not isinstance(abi, str) or
         not re.fullmatch(r"^[A-Za-z0-9._-]{1,64}$", abi))):
    reject("invalid ABI")
container = document["container"]
exact_object(container, ("kind", "identity"), "container")
if container["kind"] not in (
        "apk-set", "bundle", "companion", "existing", None):
    reject("invalid container kind")
identity = container["identity"]
if (identity is not None and
        (not isinstance(identity, str) or not sha256_re.fullmatch(identity))):
    reject("invalid container identity")

validated = document["validated"]
exact_object(validated, ("items", "bytes", "critical_payloads"), "validated")
integer(validated["items"], "validated item count")
integer(validated["bytes"], "validated byte count")
critical = validated["critical_payloads"]
if not isinstance(critical, list) or len(critical) > 256:
    reject("invalid critical payload list")
for item in critical:
    exact_object(item, ("id", "items", "bytes"), "critical payload")
    if (not isinstance(item["id"], str) or
            not safe_id_re.fullmatch(item["id"])):
        reject("invalid critical payload id")
    integer(item["items"], "critical payload item count")
    integer(item["bytes"], "critical payload byte count")

logs = document["logs"]
exact_object(logs, ("summary", "detail"), "logs")
relative_path(logs["summary"], "summary log")
relative_path(logs["detail"], "detail log")
integer(document["duration_ms"], "duration")
integer(document["completed_unix"], "completion time")
error = document["error"]
if document["outcome"] == "success":
    if error is not None:
        reject("success result contains an error")
else:
    exact_object(error, ("class", "message"), "error")
    if (not isinstance(error["class"], str) or
            not re.fullmatch(r"^[A-Za-z][A-Za-z0-9_]{0,127}$",
                             error["class"])):
        reject("invalid error class")
    string(error["message"], 1, 512, "error message")

print(json.dumps(document, ensure_ascii=True, sort_keys=True,
                 separators=(",", ":")))
