#!/usr/bin/env bash
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TOOL="$ROOT/nxrelease.py"
BOOTSTRAP_ROOT=$(CDPATH= cd -- "$ROOT/../nxbootstrap" && pwd -P)
NXEXTRACT_ROOT=$(CDPATH= cd -- "$ROOT/../../suportando_outros_devices/extrator-universal" && pwd -P)
BOOTSTRAP_VERSION=$(<"$BOOTSTRAP_ROOT/VERSION")
NXEXTRACT_VERSION=$(<"$NXEXTRACT_ROOT/VERSION")
NXSPLASH_VERSION=$(<"$ROOT/../nxsplash/VERSION")
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/nxrelease-test.XXXXXX")
trap 'rm -rf -- "$TEST_TMP"' EXIT INT TERM

fail() {
  printf 'nxrelease tests: FAIL: %s\n' "$*" >&2
  exit 1
}

expect_fail() {
  label=$1
  pattern=$2
  shift 2
  if "$@" >"$TEST_TMP/$label.output" 2>&1; then
    fail "$label unexpectedly passed"
  fi
  if ! grep -Eqi "$pattern" "$TEST_TMP/$label.output"; then
    sed -n '1,120p' "$TEST_TMP/$label.output" >&2
    fail "$label did not report /$pattern/"
  fi
}

for command_name in python3 readelf bash sh sha256sum cmp aarch64-linux-gnu-gcc clang ld.lld; do
  command -v "$command_name" >/dev/null 2>&1 ||
    fail "required command missing: $command_name"
done

grep -Fq 'generator.render_nxextract_block(nxport)' "$TOOL" ||
  fail 'NXRelease does not consume the canonical NXExtract launcher block'
grep -Fq 'generator.render_required_files_block(nxport)' "$TOOL" ||
  fail 'NXRelease does not consume the canonical required-files block'
if grep -Eq '^def expected_(nxextract_runtime|required_files_runtime)_block' "$TOOL"; then
  fail 'NXRelease retained a stale local copy of canonical launcher policy'
fi

# Exercise the whole release chain with no executable named ``stat`` in PATH.
# Populate a private PATH from the commands already available to this test,
# deliberately omitting only stat; absolute invocations are rejected by the
# source/ZIP audits below.
ORIGINAL_TEST_PATH=$PATH
NO_STAT_BIN="$TEST_TMP/path-without-stat"
mkdir -p -- "$NO_STAT_BIN"
IFS=: read -r -a test_path_entries <<<"$ORIGINAL_TEST_PATH"
for test_path_entry in "${test_path_entries[@]}"; do
  [ -d "$test_path_entry" ] || continue
  for test_executable in "$test_path_entry"/*; do
    [ -f "$test_executable" ] && [ -x "$test_executable" ] || continue
    test_command_name=${test_executable##*/}
    [ "$test_command_name" != stat ] || continue
    if [ ! -e "$NO_STAT_BIN/$test_command_name" ] &&
       [ ! -L "$NO_STAT_BIN/$test_command_name" ]; then
      ln -s -- "$test_executable" "$NO_STAT_BIN/$test_command_name"
    fi
  done
done
PATH=$NO_STAT_BIN
export PATH
if command -v stat >/dev/null 2>&1; then
  fail 'no-stat PATH unexpectedly contains stat'
fi

[ "$BOOTSTRAP_VERSION" = 0.8.4 ] ||
  fail "NXRelease tests require nxbootstrap 0.8.4"
[ "$NXEXTRACT_VERSION" = 1.3.0 ] ||
  fail "NXRelease 0.4.11 tests require NXExtract 1.3.0"
[ "$NXSPLASH_VERSION" = 0.1.2 ] ||
  fail "NXRelease 0.4.11 tests require nxsplash 0.1.2"
[ "$(python3 -B "$TOOL" --version)" = "nxrelease 0.4.11" ] ||
  fail "NXRelease tool/version file drifted from 0.4.11"

python3 -B "$ROOT/tests/test_human_authority.py"

# The credential scanner must understand Python's ``name: Type`` grammar
# without weakening real assignment or non-Python mapping detection.
python3 -B "$ROOT/tests/test_secret_literal_scan.py"
# 0.4.0: the structural scanner is wired; the adapter must stay additive.
python3 -B "$ROOT/tests/test_nxscan.py"
python3 -B "$ROOT/tests/test_preflight.py"
python3 -B "$ROOT/tests/test_sdl_floor_parity.py"
python3 -B "$ROOT/tests/test_face_layout_release.py"

# NXExtract 1.3.0 defaults an omitted top-level validate field to []; release
# must match that grammar while keeping extract/commit mandatory arrays.
python3 -B "$ROOT/tests/test_nxextract_recipe_validate.py"

# Mandatory V4 provider/candidate/video boundary.  Keeping this call in the
# already-classified nxrelease gate means the framework suite cannot stay green
# if a standalone directed test is accidentally left out of the matrix.
python3 -B "$ROOT/tests/test_provider_lock.py"

# nx-ship-port preserves legacy schema 1/2 ordering while schema 3 opts into
# build-then-seal. It judges the resulting archive, never a magic marker in a
# build helper: stale schema-3 bytes must reach and fail the independent
# verifier.
SHIP_FIXTURE="$TEST_TMP/ship-order"
SHIP_FRAMEWORK="$SHIP_FIXTURE/framework"
mkdir -p -- "$SHIP_FRAMEWORK/nxrelease"
cp -- "$ROOT/nx-ship-port.sh" "$SHIP_FRAMEWORK/nxrelease/nx-ship-port.sh"
cat >"$SHIP_FRAMEWORK/nxrelease/nx-refresh-pins.py" <<'PY'
#!/usr/bin/env python3
import os
with open(os.environ["SHIP_ORDER_LOG"], "a", encoding="utf-8") as stream:
    stream.write("refresh\n")
PY
chmod 0755 "$SHIP_FRAMEWORK/nxrelease/nx-refresh-pins.py"
cat >"$SHIP_FRAMEWORK/nxrelease/nxrelease.py" <<'PY'
#!/usr/bin/env python3
import os
import pathlib
import sys

with open(os.environ["SHIP_ORDER_LOG"], "a", encoding="utf-8") as stream:
    stream.write("verify\n")
try:
    archive = pathlib.Path(sys.argv[sys.argv.index("--archive") + 1])
except (ValueError, IndexError):
    raise SystemExit("missing --archive")
if archive.read_bytes() != b"zip\n":
    raise SystemExit("stale schema-3 archive")
PY

make_ship_fixture() {
  ship_port=$1 ship_schema=$2 ship_marker=$3
  mkdir -p -- "$ship_port/package"
  printf '{"nxport":{"schema_version":%s}}\n' "$ship_schema" \
    >"$ship_port/nxproject.json"
  printf '{}\n' >"$ship_port/nxrelease.json"
  {
    printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail'
    [ "$ship_marker" = yes ] && printf '%s\n' 'NXRELEASE_POST_BUILD_SEAL=1'
    printf '%s\n' 'printf "build\n" >>"$SHIP_ORDER_LOG"'
    [ "$ship_marker" = yes ] && printf '%s\n' \
      'python3 -B "$NX_FRAMEWORK_ROOT/nxrelease/nx-refresh-pins.py"'
    printf '%s\n' 'mkdir -p -- "$(dirname -- "$0")/../dist"'
    if [ "$ship_schema" = 3 ] && [ "$ship_marker" != yes ]; then
      printf '%s\n' \
        'printf "stale\n" >"$(dirname -- "$0")/../dist/fixture.zip"'
    else
      printf '%s\n' \
        'printf "zip\n" >"$(dirname -- "$0")/../dist/fixture.zip"'
    fi
  } >"$ship_port/package/build-package.sh"
  chmod 0755 "$ship_port/package/build-package.sh"
}

SHIP_ORDER_LOG="$SHIP_FIXTURE/order.log"
export SHIP_ORDER_LOG
LEGACY_SHIP_PORT="$SHIP_FIXTURE/legacy"
make_ship_fixture "$LEGACY_SHIP_PORT" 2 no
PATH=$ORIGINAL_TEST_PATH bash "$SHIP_FRAMEWORK/nxrelease/nx-ship-port.sh" \
  --port-dir "$LEGACY_SHIP_PORT" --framework-root "$SHIP_FRAMEWORK" \
  >"$SHIP_FIXTURE/legacy.output"
[ "$(tr '\n' ' ' <"$SHIP_ORDER_LOG")" = 'refresh build verify ' ] ||
  fail 'legacy ship helper did not preserve refresh-before-build ordering'

: >"$SHIP_ORDER_LOG"
V3_SHIP_PORT="$SHIP_FIXTURE/v3"
make_ship_fixture "$V3_SHIP_PORT" 3 yes
PATH=$ORIGINAL_TEST_PATH bash "$SHIP_FRAMEWORK/nxrelease/nx-ship-port.sh" \
  --port-dir "$V3_SHIP_PORT" --framework-root "$SHIP_FRAMEWORK" \
  >"$SHIP_FIXTURE/v3.output"
[ "$(tr '\n' ' ' <"$SHIP_ORDER_LOG")" = 'build refresh ' ] ||
  fail 'schema-3 ship flow repeated canonical ZIP verification'
printf 'nxrelease ship ordering regression passed: legacy=refresh-build-verify v3=build-seal-one-open\n'

# A recipe may invoke a hook owned by the port from
# {game_dir}/nxextract/<hook>. The canonical renderer must package that source
# alongside the four immutable NXExtract files; otherwise the device reaches
# extraction and fails with "can't open file".
python3 -B - "$ROOT/nx-render-manifest.py" "$TEST_TMP" "$ROOT/.." <<'PY'
import copy
import hashlib
import importlib.util
import json
import pathlib
import shutil
import sys

tool_raw, tmp_raw, framework_raw = sys.argv[1:]
tool = pathlib.Path(tool_raw)
port = pathlib.Path(tmp_raw) / "render-hook-port"
framework = pathlib.Path(framework_raw)
(port / "nxextract").mkdir(parents=True)
(port / "gamedata").mkdir()

nxport = {
    "id": "fixture",
    "launcher_name": "Fixture.sh",
    "executable": "fixture-nextos",
    "architecture": "aarch64",
    "nxextract": {"mode": "yes"},
}
(port / "nxproject.json").write_text(json.dumps({
    "version": "1.0.0",
    "nxport": nxport,
}), encoding="utf-8")
(port / "nxport.json").write_text(json.dumps(nxport), encoding="utf-8")

for relative in (
    "Fixture.sh", "fixture-nextos", "README.md", "INSTALLATION.md",
    "LICENSE", "port.json", "gameinfo.xml", "nxsplash-nextos",
    "extractor.json", "gamedata/README.txt", "nxextract/nxextract.py",
    "nxextract/run-extractor.sh", "nxextract/nxextract-runtime-env.sh",
    "nxextract/nxextract-ui",
):
    target = port / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(relative + "\n", encoding="utf-8")

(port / "nxextract-version.txt").write_text("1.3.0\n", encoding="utf-8")
(port / "nxextract/prepare-owner.py").write_text(
    "#!/usr/bin/env python3\n", encoding="utf-8")
(port / "nxextract/patch-policy.json").write_text("{}\n", encoding="utf-8")
(port / "nxextract/ignored.pyc").write_bytes(b"bytecode")
(port / "nxextract/ignored-link.py").symlink_to("prepare-owner.py")

spec = importlib.util.spec_from_file_location("nx_render_manifest", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.elf_needed = lambda _path: []
module.elf_soname = lambda _path: None
old_argv = sys.argv
try:
    sys.argv = [str(tool), "--port-dir", str(port),
                "--framework-root", str(framework)]
    assert module.main() == 0
finally:
    sys.argv = old_argv

manifest = json.loads((port / "nxrelease.json").read_text(encoding="utf-8"))
records = {item["target"]: item for item in manifest["files"]}
hook = records["fixture/nxextract/prepare-owner.py"]
data = records["fixture/nxextract/patch-policy.json"]
assert hook["kind"] == "payload" and hook["mode"] == "0755"
assert data["kind"] == "payload" and data["mode"] == "0644"
assert "fixture/nxextract/ignored.pyc" not in records
assert "fixture/nxextract/ignored-link.py" not in records
assert records["fixture/nxextract/nxextract.py"]["kind"] == "nxextract"
assert sum(item["target"] == "fixture/nxextract/nxextract.py"
           for item in manifest["files"]) == 1
assert "fixture/GENERATION.json" not in records
try:
    sys.argv = [str(tool), "--port-dir", str(port),
                "--framework-root", str(framework), "--public-final"]
    module.main()
except SystemExit as error:
    assert "requires a regular GENERATION.json" in str(error)
else:
    raise AssertionError("--public-final accepted a missing GENERATION.json")
finally:
    sys.argv = old_argv
(port / "GENERATION.json").write_text("{}\n", encoding="utf-8")
try:
    sys.argv = [str(tool), "--port-dir", str(port),
                "--framework-root", str(framework), "--public-final"]
    assert module.main() == 0
finally:
    sys.argv = old_argv
final_manifest = json.loads(
    (port / "nxrelease.json").read_text(encoding="utf-8"))
final_records = {item["target"]: item for item in final_manifest["files"]}
assert final_records["fixture/GENERATION.json"]["kind"] == "payload"
assert final_records["fixture/GENERATION.json"]["mode"] == "0644"

# Schema 3 stops discovering helpers by suffix/basename and packages the exact
# declared closure. A nested helper named like a core file and a spec carrying
# a historically ignored suffix must still enter; unrelated legacy extras do
# not.
(port / "nxextract/deep").mkdir()
(port / "nxextract/deep/nxextract.py").write_text(
    "declared nested helper\n", encoding="utf-8")
(port / "nxextract/deep/spec.pyc").write_bytes(b"declared opaque spec\n")
shutil.copy2(
    framework / "nxsplash/release/aarch64/nxsplash-nextos",
    port / "nxextract/deep/translator-nextos",
)
for relative in ("Fixture.sh", "fixture-nextos", "nxextract/nxextract-ui",
                 "nxextract/deep/translator-nextos", "nxsplash-nextos"):
    (port / relative).chmod(0o755)
for relative in ("extractor.json", "nxextract/nxextract.py",
                 "nxextract/run-extractor.sh",
                 "nxextract/nxextract-runtime-env.sh",
                 "nxextract/deep/nxextract.py", "nxextract/deep/spec.pyc"):
    (port / relative).chmod(0o644)

def generation_member(role, relative, mode):
    return {
        "role": role,
        "path": relative,
        "mode": mode,
        "sha256": hashlib.sha256((port / relative).read_bytes()).hexdigest(),
    }

nxport["schema_version"] = 3
nxport["generation_runtime"] = [
    generation_member("executable", "fixture-nextos", "0755"),
    generation_member("nxextract-recipe", "extractor.json", "0644"),
    generation_member("nxextract-engine", "nxextract/nxextract.py", "0644"),
    generation_member("nxextract-runner", "nxextract/run-extractor.sh", "0644"),
    generation_member("nxextract-runtime-env",
                      "nxextract/nxextract-runtime-env.sh", "0644"),
    generation_member("nxextract-ui", "nxextract/nxextract-ui", "0755"),
    generation_member("nxextract-helper",
                      "nxextract/deep/nxextract.py", "0644"),
    generation_member("nxextract-helper",
                      "nxextract/deep/translator-nextos", "0755"),
    generation_member("nxextract-spec", "nxextract/deep/spec.pyc", "0644"),
    generation_member("nxsplash", "nxsplash-nextos", "0755"),
]
(port / "nxport.json").write_text(json.dumps(nxport), encoding="utf-8")
(port / "nxproject.json").write_text(json.dumps({
    "version": "1.0.0", "nxport": nxport,
}), encoding="utf-8")
try:
    sys.argv = [str(tool), "--port-dir", str(port),
                "--framework-root", str(framework)]
    assert module.main() == 0
finally:
    sys.argv = old_argv
schema3_manifest = json.loads(
    (port / "nxrelease.json").read_text(encoding="utf-8"))
schema3_records = {item["target"]: item for item in schema3_manifest["files"]}
assert "fixture/nxextract/deep/nxextract.py" in schema3_records
assert "fixture/nxextract/deep/spec.pyc" in schema3_records
helper_elf = schema3_records["fixture/nxextract/deep/translator-nextos"]
assert helper_elf["kind"] == "project-linux"
assert helper_elf["mode"] == "0755"
assert helper_elf["architecture"] == "aarch64"
assert helper_elf["build_profile"] == "universal-low-glibc"
assert schema3_records["fixture/nxextract/deep/spec.pyc"]["kind"] == "payload"
assert "fixture/nxextract/prepare-owner.py" not in schema3_records
assert "fixture/nxextract/patch-policy.json" not in schema3_records

def write_schema3(value):
    (port / "nxport.json").write_text(json.dumps(value), encoding="utf-8")
    (port / "nxproject.json").write_text(json.dumps({
        "version": "1.0.0", "nxport": value,
    }), encoding="utf-8")

def render_failure(label, value, needle):
    write_schema3(value)
    try:
        sys.argv = [str(tool), "--port-dir", str(port),
                    "--framework-root", str(framework)]
        module.main()
    except SystemExit as error:
        assert needle.lower() in str(error).lower(), (label, str(error))
    else:
        raise AssertionError(label + " unexpectedly passed")
    finally:
        sys.argv = old_argv

elf_path = port / "nxextract/deep/translator-nextos"
elf_path.chmod(0o644)
for hidden_role in ("nxextract-spec", "runtime-data", "runtime-hook"):
    hidden_elf = copy.deepcopy(nxport)
    elf_record = next(member for member in hidden_elf["generation_runtime"]
                      if member["path"] ==
                      "nxextract/deep/translator-nextos")
    elf_record["role"] = hidden_role
    elf_record["mode"] = "0644"
    render_failure("ELF hidden as " + hidden_role, hidden_elf,
                   "cannot carry an ELF")

elf_helper_bad_mode = copy.deepcopy(nxport)
elf_record = next(member for member in elf_helper_bad_mode["generation_runtime"]
                  if member["path"] == "nxextract/deep/translator-nextos")
elf_record["mode"] = "0644"
render_failure("ELF helper without executable mode", elf_helper_bad_mode,
               "mode must be 0755")

elf_path.chmod(0o755)
write_schema3(nxport)
print("nxrelease port hook packaging regression passed: "
      "legacy_included=2 legacy_excluded=2 schema3_declared=3 "
      "elf_helper=project-linux hidden_elf_negatives=4")
PY

# The canonical UI release manifest is executable policy, not documentation.
# Exercise both supported package ABIs and fail closed on a forged source hash,
# GLIBC row or cross-architecture ELF even when the forged row self-pins bytes.
python3 -B - "$TOOL" "$NXEXTRACT_ROOT" <<'PY'
import hashlib
import importlib.util
import json
import pathlib
import shutil
import tempfile
import sys

tool, canonical_root_raw = sys.argv[1:]
spec = importlib.util.spec_from_file_location("nxrelease_ui_contract", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
canonical_root = pathlib.Path(canonical_root_raw)

for architecture in ("aarch64", "armv7"):
    contract = module.canonical_nxextract_ui_contract(architecture)
    assert contract["architecture"] == architecture
    assert contract["version"] == "1.2.16"
    assert contract["engine_version"] == "1.3.0"

def expect_failure(label, mutate, expected):
    with tempfile.TemporaryDirectory(prefix="nxrelease-ui-contract-") as raw:
        root = pathlib.Path(raw)
        shutil.copytree(canonical_root / "ui", root / "ui")
        manifest_path = root / "ui/release/manifest-v1.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        mutate(root, manifest)
        manifest_path.write_text(
            json.dumps(manifest, sort_keys=True, indent=2) + "\n",
            encoding="utf-8",
        )
        module.NXEXTRACT_ROOT = root
        module.NXEXTRACT_UI_MANIFEST_PATH = manifest_path
        try:
            module.canonical_nxextract_ui_contract("aarch64")
        except module.ReleaseError as error:
            if expected.lower() not in str(error).lower():
                raise AssertionError(
                    "%s reported %r, expected %r" % (label, str(error), expected)
                )
        else:
            raise AssertionError("%s unexpectedly passed" % label)

def forged_source(_root, manifest):
    manifest["source_sha256"] = "0" * 64

def forged_glibc(_root, manifest):
    manifest["artifacts"]["aarch64"]["glibc_max"] = "2.16"

def forged_version(_root, manifest):
    manifest["version"] = "1.2.10"

def cross_architecture(root, manifest):
    target = root / "ui/release/aarch64/nxextract-ui"
    source = root / "ui/release/armv7/nxextract-ui"
    target.write_bytes(source.read_bytes())
    record = manifest["artifacts"]["aarch64"]
    record["sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
    record["size"] = target.stat().st_size
    record["glibc_max"] = manifest["artifacts"]["armv7"]["glibc_max"]

expect_failure("source hash", forged_source, "source hash")
expect_failure("GLIBC row", forged_glibc, "GLIBC maximum")
expect_failure("UI version", forged_version, "header")
expect_failure("ELF class/machine", cross_architecture, "ELF class")
PY

# Mixed-ABI is an explicit execution-role contract: the host UI may remain
# AArch64 while splash/game are ARMHF.  Inspect the real canonical ELFs and
# reject ABI, interpreter, closure and receipt drift without launching them.
python3 -B - "$TOOL" \
  "$NXEXTRACT_ROOT/ui/release/aarch64/nxextract-ui" \
  "$ROOT/../nxsplash/release/armv7/nxsplash-nextos" <<'PY'
import copy
import hashlib
import importlib.util
import json
import pathlib
import tempfile
import sys

tool, ui_raw, armhf_raw = sys.argv[1:]
spec = importlib.util.spec_from_file_location("nxrelease_mixed_abi", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

needed = ["libc.so.6", "libdl.so.2"]
ui = module.elf_information(
    pathlib.Path(ui_raw), "fixture/nxextract/nxextract-ui",
    "nxextract-ui-linux", "aarch64", "2.30",
    "universal-low-glibc", needed, None, "canonical mixed-ABI fixture",
)
splash = module.elf_information(
    pathlib.Path(armhf_raw), "fixture/nxsplash-nextos",
    "nxsplash-linux", "armv7", "2.30", "universal-low-glibc",
    needed, None, "canonical mixed-ABI fixture",
)
game = module.elf_information(
    pathlib.Path(armhf_raw), "fixture/bin/armv7/game-nextos",
    "project-linux", "armv7", "2.30", "universal-low-glibc",
    needed, None, "canonical mixed-ABI fixture",
)
roles = {
    "extractor": {
        "architecture": "aarch64",
        "executable": "nxextract/nxextract-ui",
        "executor": "native",
        "interpreter": "/lib/ld-linux-aarch64.so.1",
        "closure": "host",
    },
    "splash": {
        "architecture": "armv7",
        "executable": "nxsplash-nextos",
        "executor": "native-or-loader",
        "interpreter": "/lib/ld-linux-armhf.so.3",
        "closure": "firmware",
    },
    "game": {
        "architecture": "armv7",
        "executable": "bin/armv7/game-nextos",
        "executor": "native-or-loader",
        "interpreter": "/lib/ld-linux-armhf.so.3",
        "closure": "firmware-and-port",
    },
    "helpers": [],
}
dependencies = [
    {"namespace": "linux", "architecture": architecture,
     "soname": soname, "provider": "glibc-base"}
    for architecture in ("aarch64", "armv7")
    for soname in needed
]
config = {
    "port_dir": "fixture",
    "execution_roles": roles,
    "dependencies": dependencies,
    "nxextract": {
        "ui_architecture": "aarch64",
        "ui_path": "fixture/nxextract/nxextract-ui",
    },
    "nxsplash": {"architecture": "armv7"},
}
elfs = [ui, splash, game]
module.validate_dependency_closure(elfs, config)
module.validate_execution_role_elfs(elfs, config)

def expect_role_failure(label, candidate_elfs, candidate_config, expected):
    try:
        module.validate_execution_role_elfs(candidate_elfs, candidate_config)
    except module.ReleaseError as error:
        assert expected.lower() in str(error).lower(), (label, error)
    else:
        raise AssertionError(label + " unexpectedly passed")

wrong_arch = copy.deepcopy(elfs)
wrong_arch[-1]["architecture"] = "aarch64"
expect_role_failure("game ABI", wrong_arch, config, "architecture differs")
wrong_interp = copy.deepcopy(elfs)
wrong_interp[-1]["interpreter"] = "none"
expect_role_failure("game interpreter", wrong_interp, config, "PT_INTERP")
missing_game = elfs[:-1]
expect_role_failure("missing game", missing_game, config, "does not resolve")
package_closure = copy.deepcopy(config)
next(item for item in package_closure["dependencies"]
     if item["architecture"] == "armv7" and
     item["soname"] == "libc.so.6").update({
         "provider": "package", "path": "fixture/lib/libc.so.6"})
expect_role_failure("firmware contamination", elfs, package_closure,
                    "depends on packaged")

receipt = {
    "schema": "nxgenerator-receipt-v1",
    "schema_version": 1,
    "generator": {"name": "nxgenerator", "version": "0.4.5"},
    "project_manifest_sha256": "1" * 64,
    "source_pins": {
        "nxbootstrap": {"version": "0.8.4"},
        "nxsplash": {"version": "0.1.2"},
        "nxextract": {"version": "1.3.0"},
        "portmaster": {},
    },
    "artifacts": [],
    "claims": {
        "deterministic_scaffold": True,
        "release_ready": False,
        "physical_support_proven": False,
        "adapter_lifecycle_implemented": False,
    },
    "execution_roles": roles,
}
with tempfile.TemporaryDirectory(prefix="nxrelease-generation-") as raw:
    root = pathlib.Path(raw)
    project_path = root / "nxproject.json"
    project_path.write_text('{"schema_version":3}\n', encoding="utf-8")
    project_sha256 = hashlib.sha256(project_path.read_bytes()).hexdigest()
    receipt["project_manifest_sha256"] = project_sha256
    receipt["artifacts"] = [{
        "path": "fixture/nxproject.json", "mode": "0644",
        "sha256": project_sha256,
    }]
    path = root / "GENERATION.json"
    path.write_text(json.dumps(receipt), encoding="utf-8")
    project_record = {
        "target": "fixture/nxproject.json", "actual_path": project_path,
        "kind": "payload", "mode": 0o644, "sha256": project_sha256,
    }
    generation_record = {
        "target": "fixture/GENERATION.json", "actual_path": path,
        "kind": "payload", "mode": 0o644,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }
    records = [generation_record, project_record]
    module.validate_generation_receipt(records, config)
    mismatch = copy.deepcopy(receipt)
    mismatch["execution_roles"]["game"]["architecture"] = "aarch64"
    path.write_text(json.dumps(mismatch), encoding="utf-8")
    try:
        module.validate_generation_receipt(records, config)
    except module.ReleaseError as error:
        assert "execution_roles differ" in str(error), error
    else:
        raise AssertionError("GENERATION.json role drift unexpectedly passed")

print("nxrelease mixed-ABI gate passed: actual_elf=3 negative=5 receipt=2")

# ---- V3 generation_id branch (auditoria V3, ponto 4): a generation carrying a
# generation_id MUST ship a valid defaults gptk, a valid defaults settings and a
# present, well-formed adapter-contract; absent/malformed fails closed.
import hashlib
GPTK_OK = ("format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n"
           "[gameplay]\nA = player.jump\n")
SETTINGS_OK = "# NEXTOS_SETTINGS/1\nlanguage=auto\nquality=high\n"
ADAPTER_OK = {
    "input": {"mapping": None, "touch": None, "actions": []},
    "release_ready": False,
    "language_access": {"mode": "native-menu",
                        "supported": ["en-US", "pt-BR"],
                        "fallback": "en-US",
                        "sinks": ["Locale.getLanguage"]},
}
with tempfile.TemporaryDirectory(prefix="nxrelease-v3gen-") as v3raw:
    v3root = pathlib.Path(v3raw)
    def gen_records(idx, gptk=GPTK_OK, settings=SETTINGS_OK,
                    adapter=None, adapter_raw=None, drop=(), claims=None):
        base = v3root / ("case%d" % idx)
        gen_receipt = copy.deepcopy(receipt)
        gen_receipt["generation_id"] = "a" * 32
        if claims is not None:
            gen_receipt["claims"] = claims
        def wr(rel, data):
            p = base / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data if isinstance(data, bytes)
                          else data.encode("utf-8"))
            return p
        files = {
            "fixture/nxproject.json":
                wr("nxproject.json", '{"schema_version":3}\n'),
            "fixture/defaults/NEXTOSCONTROLLERS.gptk":
                wr("defaults/NEXTOSCONTROLLERS.gptk", gptk),
            "fixture/defaults/NEXTOSSETTINGS.txt":
                wr("defaults/NEXTOSSETTINGS.txt", settings),
        }
        if adapter_raw is not None:
            files["fixture/adapter/adapter-contract.json"] = wr(
                "adapter/adapter-contract.json", adapter_raw)
        else:
            files["fixture/adapter/adapter-contract.json"] = wr(
                "adapter/adapter-contract.json",
                json.dumps(ADAPTER_OK if adapter is None else adapter))
        files = {
            target: path for target, path in files.items()
            if target not in drop
        }
        project_path = files["fixture/nxproject.json"]
        gen_receipt["project_manifest_sha256"] = hashlib.sha256(
            project_path.read_bytes()
        ).hexdigest()
        gen_receipt["artifacts"] = sorted(({
            "path": target, "mode": "0644",
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        } for target, path in files.items()), key=lambda item: item["path"])
        files["fixture/GENERATION.json"] = wr(
            "GENERATION.json", json.dumps(gen_receipt)
        )
        return [
            {"target": target, "actual_path": path, "kind": "payload",
             "mode": 0o644,
             "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            for target, path in files.items()
        ]

    module.validate_generation_receipt(gen_records(0), config)  # positive

    gen_neg = 0
    def expect_gen_fail(label, records, needle):
        global gen_neg
        try:
            module.validate_generation_receipt(records, config)
        except module.ReleaseError as error:
            assert needle.lower() in str(error).lower(), (label, error)
            gen_neg += 1
        else:
            raise AssertionError(label + " unexpectedly passed")

    expect_gen_fail("adapter absent",
                    gen_records(1, drop=("fixture/adapter/adapter-contract.json",)),
                    "declares its adapter contract")
    expect_gen_fail("adapter malformed",
                    gen_records(2, adapter_raw="{not json"),
                    "not valid JSON")
    expect_gen_fail("language_access null",
                    gen_records(3, adapter={**ADAPTER_OK, "language_access": None}),
                    "language_access must not be null")
    expect_gen_fail("settings absent",
                    gen_records(4, drop=("fixture/defaults/NEXTOSSETTINGS.txt",)),
                    "NEXTOS_SETTINGS/1 default")
    expect_gen_fail("settings unknown key",
                    gen_records(5, settings="# NEXTOS_SETTINGS/1\nresolution=auto\n"),
                    "unknown key")
    expect_gen_fail("release_ready without actions",
                    gen_records(6, adapter={**ADAPTER_OK, "release_ready": True}),
                    "must declare input.actions")

    # ---- V3-GRAPHICS-02 release gate (steps 3-5) ----
    # gen_records pins generation_id to "a"*32; every physical proof must link
    # to a GRAPHICS-EVIDENCE line carrying that generation and a real build_id.
    GEN = "a" * 32
    GRAPHICS = {"api": "gles", "profile": "es", "version": "2.0",
                "version_policy": "exact", "shader_dialect": "essl100",
                "drawable_ready_timeout_ms": 5000}

    def gfx_evidence(generation=GEN, run_id="porttest-1700000000-77-3",
                     build_id="deadbeef12", obtained_api="gles",
                     obtained_profile="es", obtained_version="2.0",
                     drawable_w=640, drawable_h=480, shader_probe="pass",
                     verdict="OK", reason="ok", commit="abc123", extra=""):
        return ("GRAPHICS-EVIDENCE: run_id=%s generation=%s commit=%s "
                "cfw=darkos sdl=2 provider_egl=/usr/lib/libmali.so "
                "provider_gles=/usr/lib/libmali.so build_id=%s "
                "requested=gles/es/2.0/exact obtained=%s/%s/%s drawable=%dx%d "
                "shader_probe=%s verdict=%s reason=%s%s" % (
                    run_id, generation, commit, build_id, obtained_api,
                    obtained_profile, obtained_version, drawable_w, drawable_h,
                    shader_probe, verdict, reason, extra))

    def gfx_proof(device="mali-g31", evidence=None, **over):
        f = dict(obtained_api="gles", obtained_profile="es",
                 obtained_version="2.0", drawable_w=640, drawable_h=480,
                 shader_probe="pass", verdict="OK", reason="ok")
        f.update(over)
        proof = {"device": device, **f}
        # By default the evidence line MIRRORS the summary (a consistent proof);
        # a caller passes evidence=... to make them disagree on purpose.
        proof["evidence"] = evidence if evidence is not None else gfx_evidence(
            obtained_api=f["obtained_api"], obtained_profile=f["obtained_profile"],
            obtained_version=f["obtained_version"], drawable_w=f["drawable_w"],
            drawable_h=f["drawable_h"], shader_probe=f["shader_probe"],
            verdict=f["verdict"], reason=f["reason"])
        return proof

    PROOF_OK = gfx_proof()
    RELEASE_CLAIMS = {"deterministic_scaffold": True, "release_ready": True,
                      "physical_support_proven": True,
                      "adapter_lifecycle_implemented": True}
    def rel_adapter(**over):
        a = {**ADAPTER_OK, "release_ready": True,
             "input": {"mapping": None, "touch": None,
                       "actions": ["player.jump"]},
             "graphics": dict(GRAPHICS), "graphics_proofs": [dict(PROOF_OK)]}
        a.update(over)
        return a

    # positive: a release-ready GLES2 port with a valid, evidence-backed proof.
    module.validate_generation_receipt(
        gen_records(10, adapter=rel_adapter(), claims=RELEASE_CLAIMS), config)

    expect_gen_fail("graphics api invalid",
                    gen_records(11, adapter=rel_adapter(
                        graphics={**GRAPHICS, "api": "vulkan"}),
                        claims=RELEASE_CLAIMS),
                    "graphics.api must be gles or gl")
    expect_gen_fail("release graphics without proofs",
                    gen_records(12, adapter=rel_adapter(graphics_proofs=[]),
                                claims=RELEASE_CLAIMS),
                    "must carry graphics_proofs")
    expect_gen_fail("graphics proof verdict FAIL",
                    gen_records(13, adapter=rel_adapter(graphics_proofs=[
                        gfx_proof(verdict="FAIL",
                                  reason="desktop-gl-for-gles-contract")]),
                        claims=RELEASE_CLAIMS),
                    "is not OK")
    expect_gen_fail("graphics proof desktop-GL for a GLES contract",
                    gen_records(14, adapter=rel_adapter(graphics_proofs=[
                        gfx_proof(obtained_api="gl")]),
                        claims=RELEASE_CLAIMS),
                    "non-GLES context")
    expect_gen_fail("graphics proof 1x1 drawable",
                    gen_records(15, adapter=rel_adapter(graphics_proofs=[
                        gfx_proof(drawable_w=1, drawable_h=1)]),
                        claims=RELEASE_CLAIMS),
                    "1x1 drawable")
    expect_gen_fail("graphics proof shader probe failed",
                    gen_records(16, adapter=rel_adapter(graphics_proofs=[
                        gfx_proof(shader_probe="fail")]),
                        claims=RELEASE_CLAIMS),
                    "shader probe of the")

    # ---- item 5: the proof MUST link to a physical structured receipt ----
    expect_gen_fail("graphics proof without an evidence line",
                    gen_records(17, adapter=rel_adapter(graphics_proofs=[
                        {k: v for k, v in PROOF_OK.items()
                         if k != "evidence"}]),
                        claims=RELEASE_CLAIMS),
                    "GRAPHICS-EVIDENCE")
    expect_gen_fail("graphics proof evidence for another generation",
                    gen_records(18, adapter=rel_adapter(graphics_proofs=[
                        gfx_proof(evidence=gfx_evidence(generation="b" * 32))]),
                        claims=RELEASE_CLAIMS),
                    "does not match this generation")
    expect_gen_fail("graphics proof evidence with no build_id",
                    gen_records(19, adapter=rel_adapter(graphics_proofs=[
                        gfx_proof(evidence=gfx_evidence(build_id="-"))]),
                        claims=RELEASE_CLAIMS),
                    "no provider build-id")
    expect_gen_fail("graphics proof summary disagreeing with evidence",
                    gen_records(22, adapter=rel_adapter(graphics_proofs=[
                        gfx_proof(evidence=gfx_evidence(drawable_w=320,
                                                        drawable_h=240))]),
                        claims=RELEASE_CLAIMS),
                    "summary disagrees")
    expect_gen_fail("graphics duplicate device proof",
                    gen_records(23, adapter=rel_adapter(graphics_proofs=[
                        gfx_proof(), gfx_proof()]),
                        claims=RELEASE_CLAIMS),
                    "duplicate proof")

    # ---- V4-GRAPHICS-04: the post-first-present declarative opt-in ----
    PFP_GFX = {**GRAPHICS, "evidence_boundary": "post-first-present"}
    PFP_EXTRA = (" phase=post-first-present first_present=1 pre_drawable=1x1"
                 " port_id=fixture port_version=1.0.0 egl_build_id=beef01")

    # positive: a post-present receipt with phase, first_present and the
    # pre-present diagnosis satisfies the armed boundary.
    module.validate_generation_receipt(
        gen_records(60, adapter=rel_adapter(
            graphics=PFP_GFX,
            graphics_proofs=[gfx_proof(evidence=gfx_evidence(
                extra=PFP_EXTRA))]),
            claims=RELEASE_CLAIMS), config)

    # the legacy boundary (no opt-in) still accepts the classic receipt with
    # the extension fields absent -- bytes and behavior preserved.
    module.validate_generation_receipt(
        gen_records(61, adapter=rel_adapter(), claims=RELEASE_CLAIMS), config)

    expect_gen_fail("boundary with an unknown value",
                    gen_records(62, adapter=rel_adapter(
                        graphics={**GRAPHICS,
                                  "evidence_boundary": "pre-present"}),
                        claims=RELEASE_CLAIMS),
                    "evidence_boundary must be post-first-present")
    expect_gen_fail("boundary armed but pre-present receipt",
                    gen_records(63, adapter=rel_adapter(
                        graphics=PFP_GFX,
                        graphics_proofs=[gfx_proof()]),
                        claims=RELEASE_CLAIMS),
                    "pre-present receipt never promotes")
    expect_gen_fail("boundary armed but first_present missing",
                    gen_records(64, adapter=rel_adapter(
                        graphics=PFP_GFX,
                        graphics_proofs=[gfx_proof(evidence=gfx_evidence(
                            extra=" phase=post-first-present"
                                  " pre_drawable=1x1"))]),
                        claims=RELEASE_CLAIMS),
                    "first_present=1")
    expect_gen_fail("boundary armed but pre_drawable missing",
                    gen_records(65, adapter=rel_adapter(
                        graphics=PFP_GFX,
                        graphics_proofs=[gfx_proof(evidence=gfx_evidence(
                            extra=" phase=post-first-present"
                                  " first_present=1"))]),
                        claims=RELEASE_CLAIMS),
                    "pre-present drawable diagnosis")
    expect_gen_fail("boundary armed but commit identity absent",
                    gen_records(66, adapter=rel_adapter(
                        graphics=PFP_GFX,
                        graphics_proofs=[gfx_proof(evidence=gfx_evidence(
                            commit="-", extra=PFP_EXTRA))]),
                        claims=RELEASE_CLAIMS),
                    "no framework commit identity")
    # 1x1 stays refused under the new boundary exactly as before.
    expect_gen_fail("boundary armed but 1x1 drawable",
                    gen_records(67, adapter=rel_adapter(
                        graphics=PFP_GFX,
                        graphics_proofs=[gfx_proof(
                            drawable_w=1, drawable_h=1,
                            evidence=gfx_evidence(
                                drawable_w=1, drawable_h=1,
                                extra=PFP_EXTRA))]),
                        claims=RELEASE_CLAIMS),
                    "1x1 drawable")
    # a one-shot receipt can never be reused across proofs.
    PFP_COV = {**PFP_GFX, "version_policy": "minimum",
               "required_devices": ["mali-g31", "mali-450"]}
    expect_gen_fail("boundary armed with a reused receipt",
                    gen_records(68, adapter=rel_adapter(
                        graphics=PFP_COV,
                        graphics_proofs=[
                            gfx_proof(evidence=gfx_evidence(extra=PFP_EXTRA)),
                            gfx_proof(device="mali-450",
                                      evidence=gfx_evidence(
                                          extra=PFP_EXTRA))]),
                        claims=RELEASE_CLAIMS),
                    "one-shot")
    # two devices, two runs, one commit: promotes.
    module.validate_generation_receipt(
        gen_records(69, adapter=rel_adapter(
            graphics=PFP_COV,
            graphics_proofs=[
                gfx_proof(evidence=gfx_evidence(extra=PFP_EXTRA)),
                gfx_proof(device="mali-450",
                          evidence=gfx_evidence(
                              run_id="porttest-1700000000-77-4",
                              extra=PFP_EXTRA))]),
            claims=RELEASE_CLAIMS), config)
    # divergent framework commits across proofs never promote.
    expect_gen_fail("boundary armed with divergent commits",
                    gen_records(70, adapter=rel_adapter(
                        graphics=PFP_COV,
                        graphics_proofs=[
                            gfx_proof(evidence=gfx_evidence(extra=PFP_EXTRA)),
                            gfx_proof(device="mali-450",
                                      evidence=gfx_evidence(
                                          run_id="porttest-1700000000-77-4",
                                          commit="def456",
                                          extra=PFP_EXTRA))]),
                        claims=RELEASE_CLAIMS),
                    "disagree on the framework commit")

    # ---- item 5: version policy is enforced against the evidence ----
    MIN_GFX = {**GRAPHICS, "version_policy": "minimum"}
    module.validate_generation_receipt(  # minimum 2.0 accepts obtained 3.2
        gen_records(24, adapter=rel_adapter(
            graphics=MIN_GFX,
            graphics_proofs=[gfx_proof(obtained_version="3.2")]),
            claims=RELEASE_CLAIMS), config)
    expect_gen_fail("graphics exact policy rejects a higher version",
                    gen_records(25, adapter=rel_adapter(graphics_proofs=[
                        gfx_proof(obtained_version="3.2")]),
                        claims=RELEASE_CLAIMS),
                    "not exactly")

    # ---- item 5: coverage of required_devices ----
    COV_GFX = {**GRAPHICS, "version_policy": "minimum",
               "required_devices": ["mali-g31", "mali-450"]}
    module.validate_generation_receipt(  # both devices proven -> pass
        gen_records(26, adapter=rel_adapter(
            graphics=COV_GFX,
            graphics_proofs=[gfx_proof(device="mali-g31"),
                             gfx_proof(device="mali-450")]),
            claims=RELEASE_CLAIMS), config)
    expect_gen_fail("graphics coverage misses a required device",
                    gen_records(27, adapter=rel_adapter(
                        graphics=COV_GFX,
                        graphics_proofs=[gfx_proof(device="mali-g31")]),
                        claims=RELEASE_CLAIMS),
                    "miss required device")

    # ---- Named field fixtures (step 4) ----
    # FF4 / FF4A: approved GLES2 ports proven on ROCKNIX/Mali-G52 -> positive.
    FF4_PROOF = gfx_proof(device="rocknix-mali-g52", drawable_w=1280,
                          drawable_h=720)
    module.validate_generation_receipt(
        gen_records(20, adapter=rel_adapter(graphics_proofs=[FF4_PROOF]),
                    claims=RELEASE_CLAIMS), config)  # FF4 positive
    # Beach Buggy 1/2: a GLES2 port whose ROCKNIX context resolved to desktop GL
    # 3.1 (Mesa) -> the physical proof verdict is FAIL -> release refused.
    BEACH_PROOF = gfx_proof(device="rocknix-mali-g52", obtained_api="gl",
                            obtained_profile="compat", obtained_version="3.1",
                            drawable_w=1, drawable_h=1, shader_probe="fail",
                            verdict="FAIL", reason="desktop-gl-for-gles-contract")
    expect_gen_fail("Beach Buggy desktop-GL field fixture",
                    gen_records(21, adapter=rel_adapter(
                        graphics_proofs=[BEACH_PROOF]), claims=RELEASE_CLAIMS),
                    "is not OK")

print("nxrelease v3-generation gate passed: positive=3 negatives=%d" % gen_neg)
PY

# Every allowlisted shell is audited, even an extensionless payload helper.
# Cover the four public roles explicitly and the supported portable shebangs.
python3 -B - "$TOOL" <<'PY'
import importlib.util
import pathlib
import tempfile
import sys

tool = sys.argv[1]
spec = importlib.util.spec_from_file_location("nxrelease_shell_gate", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
cases = (
    ("prepare", "fixture/prepare.sh", "script",
     "#!/bin/ash\nstat file\n"),
    ("runner", "fixture/nxextract/run-extractor.sh", "nxextract-runner",
     "#!/bin/mksh\nenv stat file\n"),
    ("runtime", "fixture/nxextract/nxextract-runtime-env.sh",
     "nxextract-runtime-env", "#!/bin/hush\ncommand stat file\n"),
    ("helper", "fixture/helper", "payload",
     "#!/usr/bin/env busybox sh\nbuiltin stat file\n"),
)
with tempfile.TemporaryDirectory(prefix="nxrelease-shell-stat-") as raw:
    root = pathlib.Path(raw)
    for label, target, kind, text in cases:
        path = root / label
        path.write_text(text, encoding="utf-8")
        record = {"kind": kind, "target": target, "actual_path": path}
        assert module.record_is_shell(record), label
        try:
            module.audit_script(
                path, target, {"exception_map": {}}, set(),
            )
        except module.ReleaseError as error:
            assert "external stat" in str(error), (label, error)
        else:
            raise AssertionError(label + " external stat unexpectedly passed")

    safe = root / "safe.sh"
    safe.write_text(
        "#!/bin/sh\ncommand -v stat\nprintf '%s\\n' 'stat file'\n"
        "read value </proc/1/stat\n",
        encoding="utf-8",
    )
    module.audit_script(safe, "fixture/safe.sh", {"exception_map": {}}, set())

    # Regra #9b: port NUNCA cria/gerencia swap nem mexe em page-cache.
    swap_cases = (
        ("mkswap", "mkswap /storage/swapfile\n"),
        ("swapon", "swapon /storage/swapfile\n"),
        ("zramctl", "zramctl --find --size 256M\n"),
        ("swapfile", "dd if=/dev/zero of=/swapfile bs=1M count=256\n"),
        ("drop-caches", "echo 3 > /proc/sys/vm/drop_caches\n"),
    )
    for label, line in swap_cases:
        hostile = root / ("swap-" + label + ".sh")
        hostile.write_text("#!/bin/sh\n" + line, encoding="utf-8")
        try:
            module.audit_script(
                hostile, "fixture/swap-" + label + ".sh",
                {"exception_map": {}}, set(),
            )
        except module.ReleaseError as error:
            assert "swap" in str(error), (label, error)
        else:
            raise AssertionError(label + " swap guard unexpectedly passed")
    # Palavras parecidas em contexto legitimo NAO reprovam (zero falso-positivo):
    benign = root / "swap-benign.sh"
    benign.write_text(
        "#!/bin/sh\n"
        "printf '%s\\n' 'texture swap done'\n"
        "SWAPPED=1\n",
        encoding="utf-8",
    )
    module.audit_script(benign, "fixture/swap-benign.sh",
                        {"exception_map": {}}, set())
    # E4: piso de simbolos por familia (classe Mix_PlayChannel do spruce).
    floors = module.load_symbol_floors()
    assert floors, "symbol-floors/ vazio ou ausente"
    mixer = floors.get("libSDL2_mixer-2.0.so.0")
    assert mixer and "Mix_PlayChannelTimed" in mixer, "piso do mixer incompleto"
    assert "Mix_PlayChannel" not in mixer, (
        "Mix_PlayChannel entrou no piso: o caso de campo do spruce deixaria de ser pego")

    def floor_item(path, undefined, needed, soname=None, defined=()):
        module._DYNAMIC_SYMBOLS[path] = (set(undefined), set(defined))
        return {"path": path, "needed": tuple(needed), "soname": soname}

    module._DYNAMIC_SYMBOLS.clear()
    # 1. Caso de campo real: loader NEEDa o mixer e importa Mix_PlayChannel.
    bad = [floor_item("fixture/loader-v113", {"Mix_PlayChannel", "SDL_Init"},
                      ["libSDL2-2.0.so.0", "libSDL2_mixer-2.0.so.0"])]
    try:
        module.validate_symbol_floor(bad, {})
    except module.ReleaseError as error:
        assert "Mix_PlayChannel" in str(error) and "floor" in str(error), error
    else:
        raise AssertionError("piso de simbolos deixou passar Mix_PlayChannel")
    # 2. API antiga passa limpa.
    ok = [floor_item("fixture/loader-v114", {"Mix_PlayChannelTimed", "SDL_Init"},
                     ["libSDL2-2.0.so.0", "libSDL2_mixer-2.0.so.0"])]
    module.validate_symbol_floor(ok, {})
    # 3. Biblioteca EMPACOTADA cobre o import (bundle e conserto legitimo).
    module._DYNAMIC_SYMBOLS.clear()
    bundled = [
        floor_item("fixture/loader", {"Mix_PlayChannel"},
                   ["libSDL2_mixer-2.0.so.0"]),
        floor_item("fixture/libSDL2_mixer-2.0.so.0", set(),
                   [], soname="libSDL2_mixer-2.0.so.0",
                   defined={"Mix_PlayChannel"}),
    ]
    module.validate_symbol_floor(bundled, {})
    # 4. Simbolo fora dos namespaces com piso NUNCA reprova (glibc/GL tem
    #    gates proprios): zero falso-positivo por construcao.
    module._DYNAMIC_SYMBOLS.clear()
    outside = [floor_item("fixture/loader-glibc",
                          {"__isoc23_sscanf", "eglChooseConfig"},
                          ["libSDL2-2.0.so.0"])]
    module.validate_symbol_floor(outside, {})
    # 5. dlopen (sem NEEDED da familia) fica fora do gate de proposito.
    module._DYNAMIC_SYMBOLS.clear()
    dlopened = [floor_item("fixture/loader-dlopen", {"Mix_PlayChannel"}, [])]
    module.validate_symbol_floor(dlopened, {})
    module._DYNAMIC_SYMBOLS.clear()

    # V4-03B: a familia CORE da SDL2 decide por VERSAO DE NASCIMENTO, lida da
    # autoridade unica framework/nxabi/sdl2-symbol-floor.tsv.
    assert "libSDL2-2.0.so.0" not in floors, (
        "lista .syms paralela do core da SDL2 voltou ao symbol-floors/")
    authority = module.load_sdl_symbol_authority()
    assert authority["authority"] == "nx-sdl-symbol-floor/1", authority
    assert len(authority["sha256"]) == 64, authority
    # Consistencia entre consumidores: o nxabi abre os MESMOS bytes pela
    # MESMA rota de parser; id, hash e mapa inteiro devem coincidir.
    nxabi_path = (pathlib.Path(tool).resolve().parents[1] / "nxabi"
                  / "nxabi.py")
    nxabi_spec = importlib.util.spec_from_file_location(
        "nxabi_gate", str(nxabi_path))
    nxabi_module = importlib.util.module_from_spec(nxabi_spec)
    nxabi_spec.loader.exec_module(nxabi_module)
    nxabi_authority = nxabi_module.load_sdl_authority_for_policy(
        nxabi_module.load_policy(nxabi_module.DEFAULT_POLICY),
        nxabi_module.DEFAULT_POLICY)
    assert nxabi_authority["sha256"] == authority["sha256"], (
        nxabi_authority["sha256"], authority["sha256"])
    assert nxabi_authority["authority"] == authority["authority"]
    assert nxabi_authority["table"] == authority["table"], (
        "nxabi e nxrelease interpretaram a autoridade de forma diferente")
    # 6. Vendor/Product (SDL 2.0.6) reprovam com ELF, simbolo, versao exigida
    #    e piso declarado nomeados -- o controle negativo exato da antiga
    #    aceitacao indevida da closure.
    for symbol in ("SDL_JoystickGetVendor", "SDL_JoystickGetProduct"):
        module._DYNAMIC_SYMBOLS.clear()
        direct = [floor_item("fixture/loader-" + symbol,
                             {symbol, "SDL_Init"}, ["libSDL2-2.0.so.0"])]
        try:
            module.validate_symbol_floor(direct, {})
        except module.ReleaseError as error:
            message = str(error)
            assert ("fixture/loader-" + symbol) in message, message
            assert symbol in message, message
            assert "2.0.6" in message, message
            assert "2.0.4" in message, message
            assert authority["sha256"] in message, message
        else:
            raise AssertionError(
                symbol + " (SDL 2.0.6) passou acima do piso 2.0.4")
    # 7. Imports do piso continuam passando limpos.
    module._DYNAMIC_SYMBOLS.clear()
    module.validate_symbol_floor(
        [floor_item("fixture/loader-floor",
                    {"SDL_Init", "SDL_JoystickOpen", "SDL_CreateWindow"},
                    ["libSDL2-2.0.so.0"])], {})
    # 8. Simbolo SDL fora da autoridade nao prova o piso: reprova.
    module._DYNAMIC_SYMBOLS.clear()
    try:
        module.validate_symbol_floor(
            [floor_item("fixture/loader-unknown", {"SDL_TotallyUnknown"},
                        ["libSDL2-2.0.so.0"])], {})
    except module.ReleaseError as error:
        assert "absent from" in str(error), error
    else:
        raise AssertionError("simbolo fora da autoridade passou")
    # 9. SDL2 core EMPACOTADA continua fora do gate de piso, como antes.
    module._DYNAMIC_SYMBOLS.clear()
    module.validate_symbol_floor([
        floor_item("fixture/loader-bundled-sdl", {"SDL_JoystickGetVendor"},
                   ["libSDL2-2.0.so.0"]),
        floor_item("fixture/libSDL2-2.0.so.0", set(), [],
                   soname="libSDL2-2.0.so.0",
                   defined={"SDL_JoystickGetVendor"}),
    ], {})
    # 10. Lista legada paralela do core reaparecendo = falha fechada.
    with tempfile.TemporaryDirectory(
            prefix="nxrelease-legacy-syms-") as legacy_raw:
        legacy_root = pathlib.Path(legacy_raw)
        (legacy_root / "libSDL2-2.0.so.0.syms").write_text(
            "SDL_JoystickGetVendor\n", encoding="utf-8")
        try:
            module.load_symbol_floors(directory=str(legacy_root))
        except module.ReleaseError as error:
            assert "parallel" in str(error), error
        else:
            raise AssertionError("lista legada paralela do core foi aceita")
    # 11. Autoridade ausente, symlink, linha malformada, versao invalida,
    #     duplicata ambigua e id errado falham fechado no parser unico.
    good_bytes = pathlib.Path(
        nxabi_module.SDL_AUTHORITY_TABLE).read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(
            prefix="nxrelease-authority-") as auth_raw:
        auth_root = pathlib.Path(auth_raw)

        def expect_authority_failure(label, content=None, link_to=None):
            candidate = auth_root / (label + ".tsv")
            if link_to is not None:
                candidate.symlink_to(link_to)
            else:
                candidate.write_text(content, encoding="utf-8")
            try:
                nxabi_module.load_symbol_authority(candidate)
            except nxabi_module.AbiError:
                return
            raise AssertionError("autoridade adulterada aceita: " + label)

        expect_authority_failure(
            "missing-directive", content="SDL_Init\t2.0.0\tsdl2-headers\n")
        expect_authority_failure(
            "malformed-row",
            content="#% authority: nx-sdl-symbol-floor/1\n"
                    "SDL_Init 2.0.0 sdl2-headers\n")
        expect_authority_failure(
            "bad-version",
            content="#% authority: nx-sdl-symbol-floor/1\n"
                    "SDL_Init\tnew\tsdl2-headers\n")
        expect_authority_failure(
            "ambiguous-duplicate",
            content="#% authority: nx-sdl-symbol-floor/1\n"
                    "SDL_Init\t2.0.0\tsdl2-headers\n"
                    "SDL_Init\t2.0.6\tsdl2-headers\n")
        expect_authority_failure(
            "wrong-id",
            content="#% authority: someone-else/9\n"
                    "SDL_Init\t2.0.0\tsdl2-headers\n")
        real_copy = auth_root / "real-copy.tsv"
        real_copy.write_text(good_bytes, encoding="utf-8")
        expect_authority_failure("symlinked", link_to=real_copy)
        try:
            nxabi_module.load_symbol_authority(
                auth_root / "does-not-exist.tsv")
        except nxabi_module.AbiError:
            pass
        else:
            raise AssertionError("autoridade ausente aceita")
    module._DYNAMIC_SYMBOLS.clear()

    # D2: loader com frame-proof antigo (sem recibo VIDEO:) reprova; com o
    # recibo 0.2.8, sem adapter nenhum ou sem marcador registrado passa.
    module._RECEIPT_MARKERS.clear()
    module._RECEIPT_MARKERS["fixture/loader-old-nxgl"] = (True, False)
    try:
        module.validate_video_receipt(
            [{"path": "fixture/loader-old-nxgl"}], {})
    except module.ReleaseError as error:
        assert "VIDEO:" in str(error), error
    else:
        raise AssertionError("frame-proof sem recibo VIDEO: passou")
    module._RECEIPT_MARKERS["fixture/loader-028"] = (True, True)
    module._RECEIPT_MARKERS["fixture/no-adapter"] = (False, False)
    module.validate_video_receipt(
        [{"path": "fixture/loader-028"}, {"path": "fixture/no-adapter"},
         {"path": "fixture/unscanned"}], {})
    module._RECEIPT_MARKERS.clear()

    # D2: capability audio.embedded-openal exige o escudo no launcher.
    shield_config = {"package_id": "org.example.port"}
    shield_nxport = {"required_capabilities": ["audio.embedded-openal"],
                     "enabled_quirks": [], "runtime_report": "r.json"}
    bare = "# PORTMASTER: org.example.port, Port.sh\n"
    try:
        module.verify_self_contained_wrapper(
            bare, "Port.sh", shield_config, "aarch64", shield_nxport)
    except module.ReleaseError as error:
        assert "ALSOFT_DRIVERS=opensl" in str(error), error
    else:
        raise AssertionError("launcher sem escudo de audio passou")
    shielded = bare + "export ALSOFT_DRIVERS=opensl\necho 'ENV RECEIPT: x'\n"
    try:
        module.verify_self_contained_wrapper(
            shielded, "Port.sh", shield_config, "aarch64", shield_nxport)
    except module.ReleaseError as error:
        assert "ALSOFT" not in str(error) and "ENV RECEIPT" not in str(error), (
            "escudo presente ainda reprovou pelo motivo errado: %s" % error)
print("nxrelease shell stat gate passed: prepare=1 runner=1 runtime=1 helper=1 swap_guard=5 swap_benign=1 symbol_floor=5 video_receipt=2 audio_shield=2")
PY

# Keep the three real recipe shapes that motivated flexible container policy:
# two approved Angry Birds identities, Retro Highway's internal ELF SHA and the
# transformed ScourgeBringer container anchored by required content trees.
python3 -B - "$TOOL" \
  "$ROOT/../nxgenerator/tests/fixtures/apk-variant-recipes" <<'PY'
import importlib.util
import json
import pathlib
import sys

tool, fixtures_raw = sys.argv[1:]
spec = importlib.util.spec_from_file_location("nxrelease_variant_gate", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
fixtures = pathlib.Path(fixtures_raw)
expected = ("angrybirds", "retrohighway", "scourgebringer")
for name in expected:
    recipe = json.loads((fixtures / (name + ".json")).read_text(encoding="utf-8"))
    module.validate_apk_variant_policy(recipe)
scourge = json.loads(
    (fixtures / "scourgebringer.json").read_text(encoding="utf-8")
)
container = next(
    item for item in scourge["extract"] if item["source"]["kind"] == "container"
)
assert not ({"min_size", "max_size", "magic_hex", "magic_ascii"} &
            set(container["validate"]))
legacy = json.loads(
    (fixtures / "angrybirds-legacy-identity.json").read_text(encoding="utf-8")
)
try:
    module.validate_apk_variant_policy(legacy)
except module.ReleaseError as error:
    assert "NXA0001" in str(error), str(error)
else:
    raise AssertionError("legacy container sha whitelist passed at release")
for label, mutate, code in (
    (
        "source_validate_sha",
        lambda value: value["extract"][0].update(
            {"source_validate": {"sha256": "c" * 64}}
        ),
        "NXA0001",
    ),
    (
        "literal_container_name",
        lambda value: value["extract"][0]["source"].update(
            {"patterns": ["OriginalOwnerCopy.apk"]}
        ),
        "NXA0005",
    ),
    (
        "signing_member",
        lambda value: value.update(
            {"compatibility": {"required_members": ["META-INF/CERT.RSA"]}}
        ),
        "NXA0004",
    ),
):
    candidate = json.loads(json.dumps(
        json.loads((fixtures / "retrohighway.json").read_text(encoding="utf-8"))
    ))
    mutate(candidate)
    try:
        module.validate_apk_variant_policy(candidate)
    except module.ReleaseError as error:
        assert code in str(error), (label, str(error))
    else:
        raise AssertionError("{} compatibility bypass passed".format(label))
print("nxrelease APK variant regressions passed: angry=1 retro=1 scourge=1 "
      "legacy_identity_negative=1 bypass_negatives=3")
PY

# Release scans the packaged hook source itself, not only recipe argv/env.
python3 -B - "$TOOL" "$TEST_TMP" <<'PY'
import importlib.util
import pathlib
import sys

tool, root_raw = sys.argv[1:]
spec = importlib.util.spec_from_file_location("nxrelease_hook_source_gate", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
root = pathlib.Path(root_raw)
source = root / "cert-hook.py"
source.write_text(
    "certificate = open('META-INF/CERT.RSA', 'rb').read()\n",
    encoding="utf-8",
)
contract = {
    "schema": "org.nextos.apk-compat.hook-contract",
    "schema_version": 1,
    "hook_id": "cert-gate",
    "inputs": ["game.apk"],
    "predicates": [{"class": "compatibility", "checks": "package structure"}],
    "fallback": "generic-symbolic-path",
    "error_codes": ["NXH0001"],
}
recipe = {
    "compatibility": {"required_members": []},
    "hooks": [{
        "id": "cert-gate",
        "argv": ["python3", "{game_dir}/hooks/cert-hook.py"],
        "contract": contract,
    }],
}
records = [{
    "target": "fixture/hooks/cert-hook.py",
    "actual_path": source,
    "kind": "script",
}]
try:
    module.validate_recipe_hooks_static(recipe, records, "fixture/extractor.json")
except module.ReleaseError as error:
    assert "NXA0055" in str(error), str(error)
else:
    raise AssertionError("packaged certificate-gating hook source passed")
print("nxrelease packaged hook source gate passed: certificate_member=1")
PY

# The focused gate covers the complete transitive static closure: no digest is
# trusted because of a patch-profile or arbitrary output key, and JSON content
# receives the same decision under .json and an otherwise unknown suffix.
python3 -B "$ROOT/tests/test_hook_closure_provenance.py"

mkdir -p "$TEST_TMP/source/fixture/nxextract" "$TEST_TMP/source/payload"

cat >"$TEST_TMP/nxport-input.json" <<'JSON'
{
  "schema_version": 1,
  "id": "fixture",
  "title": "NXRelease Fixture",
  "launcher_name": "Game.sh",
  "architecture": "aarch64",
  "executable": "bin/aarch64/loader",
  "argument_mode": "game-dir-and-passthrough",
  "home_mode": "preserve",
  "nxextract": "yes",
  "required_files": ["bin/aarch64/loader"],
  "extra_library_paths": [],
  "prepare_script": ""
}
JSON
python3 "$BOOTSTRAP_ROOT/tools/generate-port.py" \
  "$TEST_TMP/nxport-input.json" --output "$TEST_TMP/source" >/dev/null

cp "$NXEXTRACT_ROOT/nxextract.py" \
  "$TEST_TMP/source/fixture/nxextract/nxextract.py"
cp "$NXEXTRACT_ROOT/run-extractor.sh" \
  "$TEST_TMP/source/fixture/nxextract/run-extractor.sh"
cp "$NXEXTRACT_ROOT/nxextract-runtime-env.sh" \
  "$TEST_TMP/source/fixture/nxextract/nxextract-runtime-env.sh"
cp "$NXEXTRACT_ROOT/ui/release/aarch64/nxextract-ui" \
  "$TEST_TMP/source/fixture/nxextract/nxextract-ui"
chmod 0755 "$TEST_TMP/source/fixture/nxextract/nxextract-ui"
printf '%s\n' "$NXEXTRACT_VERSION" \
  >"$TEST_TMP/source/fixture/nxextract-version.txt"
cat >"$TEST_TMP/source/fixture/extractor.json" <<'JSON'
{
  "schema": 1,
  "id": "fixture",
  "version": "test",
  "title": "NXRelease Fixture",
  "abi_order": ["arm64-v8a"],
  "input": {},
  "extract": [],
  "hooks": [],
  "validate": [],
  "commit": [],
  "marker": ".nxextract-fixture.json"
}
JSON

printf '%s\n' 'fixture payload' >"$TEST_TMP/source/payload/README.txt"
printf '%s\n' \
  '# NXRelease Fixture — installation / instalação' \
  '' \
  '## English' \
  'Synthetic owner-data fixture. Put owner data in ports/fixture/gamedata/.' \
  '' \
  '## Português' \
  'Fixture sintética. Coloque os dados do dono em ports/fixture/gamedata/.' \
  >"$TEST_TMP/source/fixture/INSTALLATION.md"
printf '%s\n' 'BIN="$GAMEDIR/bin/aarch64/loader"' \
  >"$TEST_TMP/source/fixture/port-env.sh"
mkdir -p -- "$TEST_TMP/source/fixture/gamedata"
printf '%s\n' \
  '[PT-BR] Coloque aqui os dados que voce possui legalmente. Ver ../INSTALLATION.md' \
  '[EN] Put here the data you lawfully own. See ../INSTALLATION.md' \
  >"$TEST_TMP/source/fixture/gamedata/README.txt"
printf '%s\n' '{"schema_version":3,"fixture":"nxrelease"}' \
  >"$TEST_TMP/source/fixture/nxproject.json"

# A real V3 generation makes every happy-path stage/build/bundle exercise the
# generation-default pin boundary.  This is the exact path that 0.2.35 broke:
# source validation retained sha256, but verify_stage reconstructed the same
# records from authenticated metadata without it.
python3 -B - "$TEST_TMP/source/fixture" "$BOOTSTRAP_VERSION" \
  "$NXEXTRACT_VERSION" "$NXSPLASH_VERSION" <<'PY'
import hashlib
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
bootstrap_version, nxextract_version, nxsplash_version = sys.argv[2:]
project_sha256 = hashlib.sha256(
    (root / "nxproject.json").read_bytes()
).hexdigest()
generation_roots = sorted(
    path for path in (root / ".nxruntime/generations").iterdir()
    if path.is_dir() and not path.is_symlink()
)
assert len(generation_roots) == 1, generation_roots
generation_id = generation_roots[0].name
(root / "defaults").mkdir()
(root / "adapter").mkdir()
(root / "defaults/NEXTOSCONTROLLERS.gptk").write_text(
    "format = NEXTOS_CONTROLLERS/1\n"
    "[menu]\nA = ui.confirm\n"
    "[gameplay]\nA = player.jump\n",
    encoding="utf-8",
)
(root / "defaults/NEXTOSSETTINGS.txt").write_text(
    "# NEXTOS_SETTINGS/1\nlanguage=auto\nquality=high\n",
    encoding="utf-8",
)
(root / "adapter/adapter-contract.json").write_text(
    json.dumps({
        "input": {"mapping": None, "touch": None, "actions": []},
        "language_access": {
            "mode": "native-menu",
            "supported": ["en-US", "pt-BR"],
            "fallback": "en-US",
            "sinks": ["Locale.getLanguage"],
        },
        "release_ready": False,
    }, sort_keys=True, indent=2) + "\n",
    encoding="utf-8",
)
(root / "GENERATION.json").write_text(
    json.dumps({
        "schema": "nxgenerator-receipt-v1",
        "schema_version": 1,
        "generator": {"name": "nxgenerator", "version": "0.4.5"},
        "project_manifest_sha256": project_sha256,
        "source_pins": {
            "nxbootstrap": {"version": bootstrap_version},
            "nxsplash": {"version": nxsplash_version},
            "nxextract": {"version": nxextract_version},
            "portmaster": {},
        },
        "artifacts": [],
        "claims": {
            "deterministic_scaffold": True,
            "release_ready": False,
            "physical_support_proven": False,
            "adapter_lifecycle_implemented": False,
        },
        "generation_id": generation_id,
    }, sort_keys=True, indent=2) + "\n",
    encoding="utf-8",
)
PY

printf '%s\n' \
  '{' \
  '  "version": 4,' \
  '  "name": "fixture.zip",' \
  '  "items": ["Game.sh", "fixture/"],' \
  '  "items_opt": [],' \
  '  "attr": {' \
  '    "title": "NXRelease Fixture",' \
  '    "arch": ["aarch64"],' \
  '    "min_glibc": "2.30",' \
  '    "image": {"cover": "cover.png"}' \
  '  }' \
  '}' \
  >"$TEST_TMP/source/port.json"

printf '%s\n' \
  '<?xml version="1.0" encoding="utf-8"?>' \
  '<gameList>' \
  '  <game>' \
  '    <path>./Game.sh</path>' \
  '    <name>NXRelease Fixture</name>' \
  '    <image>./fixture/cover.png</image>' \
  '  </game>' \
  '</gameList>' \
  >"$TEST_TMP/source/gameinfo.xml"

python3 - "$TEST_TMP/source/cover.png" <<'PY'
import sys
with open(sys.argv[1], "wb") as handle:
    handle.write(b"\x89PNG\r\n\x1a\n" + b"offline-fixture")
PY

# Real dependency-free, loadable AArch64 ELF. A synthetic 64-byte header is
# deliberately not a valid fixture: the gate requires PT_LOAD.
cat >"$TEST_TMP/start.S" <<'ASM'
.global _start
_start:
    mov x0, #0
    mov x8, #93
    svc #0
ASM
aarch64-linux-gnu-gcc -nostdlib -static -Wl,-e,_start \
  "$TEST_TMP/start.S" -o "$TEST_TMP/source/loader"
chmod 0755 "$TEST_TMP/source/loader"

# Additional real ELFs exercise dependency closure and strict dynamic tags.
cat >"$TEST_TMP/dep.c" <<'C'
int dep(void) { return 7; }
C
aarch64-linux-gnu-gcc -fPIC -nostdlib -shared \
  -Wl,-soname,libdep.so "$TEST_TMP/dep.c" \
  -o "$TEST_TMP/source/libdep.so"
cat >"$TEST_TMP/android-log.c" <<'C'
int android_log_stub(void) { return 0; }
C
aarch64-linux-gnu-gcc -fPIC -nostdlib -shared \
  -Wl,-soname,liblog.so "$TEST_TMP/android-log.c" \
  -o "$TEST_TMP/source/android-liblog.so"
cat >"$TEST_TMP/android-game.c" <<'C'
extern int android_log_stub(void);
int game_entry(void) { return android_log_stub(); }
C
aarch64-linux-gnu-gcc -fPIC -nostdlib -shared \
  -Wl,-soname,libgame.so "$TEST_TMP/android-game.c" \
  -L"$TEST_TMP/source" -Wl,--no-as-needed -l:android-liblog.so \
  -o "$TEST_TMP/source/android-game.so"
cat >"$TEST_TMP/consumer.S" <<'ASM'
.global _start
.extern dep
_start:
    bl dep
    mov x0, #0
    mov x8, #93
    svc #0
ASM
aarch64-linux-gnu-gcc -nostdlib -Wl,-e,_start \
  -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1 \
  "$TEST_TMP/consumer.S" -L"$TEST_TMP/source" -Wl,--no-as-needed -ldep \
  -o "$TEST_TMP/source/consumer"

aarch64-linux-gnu-gcc -c "$TEST_TMP/start.S" -o "$TEST_TMP/source/reloc.o"
aarch64-linux-gnu-gcc -nostdlib -Wl,-e,_start \
  -Wl,--dynamic-linker=/lib/wrong-loader.so \
  "$TEST_TMP/start.S" -o "$TEST_TMP/source/wrong-interp"
aarch64-linux-gnu-gcc -nostdlib -Wl,-e,_start \
  -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1 \
  -Wl,-rpath,'$ORIGIN/lib' "$TEST_TMP/start.S" \
  -o "$TEST_TMP/source/with-runpath"

cat >"$TEST_TMP/arm-start.S" <<'ASM'
.syntax unified
.global _start
_start:
    mov r0, #0
    mov r7, #1
    svc #0
ASM
clang --target=armv7-linux-gnueabi -fuse-ld=lld -nostdlib -static \
  -Wl,-e,_start "$TEST_TMP/arm-start.S" -o "$TEST_TMP/source/arm-softfp"

python3 - "$TEST_TMP/source/no-load" "$TEST_TMP/source/class-mismatch" <<'PY'
import struct
import sys

# Complete headers that readelf accepts, but one has no program headers and
# the other deliberately declares ELF32 for an AArch64 manifest.
ident64 = bytearray(16)
ident64[:4] = b"\x7fELF"
ident64[4:7] = bytes((2, 1, 1))
header64 = struct.pack(
    "<16sHHIQQQIHHHHHH", bytes(ident64), 2, 183, 1, 0x400000,
    0, 0, 0, 64, 56, 0, 64, 0, 0,
)
with open(sys.argv[1], "wb") as handle:
    handle.write(header64)

ident32 = bytearray(16)
ident32[:4] = b"\x7fELF"
ident32[4:7] = bytes((1, 1, 1))
header32 = struct.pack(
    "<16sHHIIIIIHHHHHH", bytes(ident32), 2, 183, 1, 0x10000,
    52, 0, 0, 52, 32, 1, 40, 0, 0,
)
phdr32 = struct.pack("<IIIIIIII", 1, 0, 0x10000, 0x10000, 84, 84, 5, 0x1000)
with open(sys.argv[2], "wb") as handle:
    handle.write(header32 + phdr32)
PY

# A real license/notice file exercises the license-notice kind on the happy path.
printf '%s\n' 'Test license notice for the offline deterministic fixture.' \
  > "$TEST_TMP/source/LICENSE"
chmod 0644 "$TEST_TMP/source/LICENSE"

# Sectionless ELF: the same loadable AArch64 ELF with its section header table
# stripped, proving the gate audits Linux ELFs that only expose program headers.
aarch64-linux-gnu-gcc -nostdlib -static -Wl,-e,_start \
  "$TEST_TMP/start.S" -o "$TEST_TMP/source/loader-sectionless"
aarch64-linux-gnu-objcopy --strip-section-headers "$TEST_TMP/source/loader-sectionless"
chmod 0755 "$TEST_TMP/source/loader-sectionless"

python3 - "$TEST_TMP/source" "$TEST_TMP/manifest.json" \
  "$BOOTSTRAP_VERSION" "$NXEXTRACT_VERSION" <<'PY'
import hashlib
import json
import os
import sys

root, output, bootstrap_version, nxextract_version = sys.argv[1:]
bootstrap_tuple = tuple(int(part) for part in bootstrap_version.split("."))
self_contained = bootstrap_tuple >= (0, 6, 0)
bootstrap_name = (
    "nxbootstrap-{}.sh".format(bootstrap_version)
    if bootstrap_tuple >= (0, 5, 0)
    else "nxbootstrap.sh"
)

def digest(relative):
    h = hashlib.sha256()
    with open(os.path.join(root, relative), "rb") as handle:
        h.update(handle.read())
    return h.hexdigest()

manifest = {
    "schema_version": 2,
    "source_root": "source",
    "package": {
        "id": "fixture",
        "version": "1.0.0",
        "profile": "universal-portmaster",
        "launcher": "Game.sh",
        "launcher_chain": (
            ["Game.sh"] if self_contained
            else ["Game.sh", "fixture/" + bootstrap_name]
        ),
        "launcher_contract": {
            "generator": "nxbootstrap",
            "version": bootstrap_version,
            "config_path": "fixture/nxport.json",
            "config_sha256": digest("fixture/nxport.json"),
        },
        "port_dir": "fixture",
        "license": {
            "spdx_id": "LicenseRef-Test",
            "source_url": "https://example.invalid/fixture",
            "file": "fixture/LICENSE",
        },
    },
    "release": {
        "source_date_epoch": 1785542400,
        "max_glibc": "2.30",
        "compression": "deflated",
    },
    "nxextract": {
        "path": "fixture/nxextract/nxextract.py",
        "version": nxextract_version,
        "minimum_version": "1.2.2",
        "sha256": digest("fixture/nxextract/nxextract.py"),
        "runner_path": "fixture/nxextract/run-extractor.sh",
        "runner_sha256": digest("fixture/nxextract/run-extractor.sh"),
        "runtime_env_path": "fixture/nxextract/nxextract-runtime-env.sh",
        "runtime_env_sha256": digest("fixture/nxextract/nxextract-runtime-env.sh"),
        "ui_path": "fixture/nxextract/nxextract-ui",
        "ui_sha256": digest("fixture/nxextract/nxextract-ui"),
        "recipe_path": "fixture/extractor.json",
        "recipe_sha256": digest("fixture/extractor.json"),
    },
    "portmaster_metadata": {
        "port_json": {
            "path": "fixture/port.json", "sha256": digest("port.json"),
        },
        "gameinfo_xml": {
            "path": "fixture/gameinfo.xml", "sha256": digest("gameinfo.xml"),
        },
        "images": [{
            "path": "fixture/cover.png", "role": "cover",
            "sha256": digest("cover.png"),
        }],
    },
    "dependencies": [
        {
            "namespace": "linux", "architecture": "aarch64",
            "soname": "libc.so.6", "provider": "glibc-base",
        },
        {
            "namespace": "linux", "architecture": "aarch64",
            "soname": "libdl.so.2", "provider": "glibc-base",
        },
    ],
    "files": [
        {
            "source": "Game.sh", "target": "Game.sh",
            "kind": "launcher", "mode": "0755", "sha256": digest("Game.sh"),
        },
        {
            "source": "fixture/nxport.json", "target": "fixture/nxport.json",
            "kind": "nxbootstrap-config", "mode": "0644",
            "sha256": digest("fixture/nxport.json"),
        },
        {
            "source": "fixture/nxsplash-nextos",
            "target": "fixture/nxsplash-nextos",
            "kind": "nxsplash-linux", "mode": "0755",
            "architecture": "aarch64",
            "build_profile": "universal-low-glibc",
            "provenance": "NextOS nxsplash immutable offline fixture",
            "sha256": digest("fixture/nxsplash-nextos"),
            "needed": ["libc.so.6", "libdl.so.2"], "soname": None,
        },
        {
            "source": "fixture/port-env.sh", "target": "fixture/port-env.sh",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/port-env.sh"),
        },
        {
            "source": "fixture/INSTALLATION.md",
            "target": "fixture/INSTALLATION.md",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/INSTALLATION.md"),
        },
        {
            "source": "fixture/nxproject.json",
            "target": "fixture/nxproject.json",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/nxproject.json"),
        },
        {
            "source": "fixture/gamedata/README.txt",
            "target": "fixture/gamedata/README.txt",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/gamedata/README.txt"),
        },
        {
            "source": "fixture/GENERATION.json",
            "target": "fixture/GENERATION.json",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/GENERATION.json"),
        },
        {
            "source": "fixture/defaults/NEXTOSCONTROLLERS.gptk",
            "target": "fixture/defaults/NEXTOSCONTROLLERS.gptk",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/defaults/NEXTOSCONTROLLERS.gptk"),
        },
        {
            "source": "fixture/defaults/NEXTOSSETTINGS.txt",
            "target": "fixture/defaults/NEXTOSSETTINGS.txt",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/defaults/NEXTOSSETTINGS.txt"),
        },
        {
            "source": "fixture/adapter/adapter-contract.json",
            "target": "fixture/adapter/adapter-contract.json",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/adapter/adapter-contract.json"),
        },
        {
            "source": "loader", "target": "fixture/bin/aarch64/loader",
            "kind": "project-linux", "mode": "0755",
            "architecture": "aarch64",
            "build_profile": "universal-low-glibc",
            "provenance": "offline deterministic test fixture",
            "sha256": digest("loader"), "needed": [], "soname": None,
        },
        {
            "source": "loader-sectionless",
            "target": "fixture/bin/aarch64/loader-sectionless",
            "kind": "project-linux", "mode": "0755",
            "architecture": "aarch64",
            "build_profile": "universal-low-glibc",
            "provenance": "offline deterministic sectionless fixture",
            "sha256": digest("loader-sectionless"), "needed": [], "soname": None,
        },
        {
            "source": "LICENSE", "target": "fixture/LICENSE",
            "kind": "license-notice", "mode": "0644", "sha256": digest("LICENSE"),
        },
        {
            "source": "fixture/nxextract/nxextract.py",
            "target": "fixture/nxextract/nxextract.py",
            "kind": "nxextract", "mode": "0644",
            "sha256": digest("fixture/nxextract/nxextract.py"),
        },
        {
            "source": "fixture/nxextract/run-extractor.sh",
            "target": "fixture/nxextract/run-extractor.sh",
            "kind": "nxextract-runner", "mode": "0644",
            "sha256": digest("fixture/nxextract/run-extractor.sh"),
        },
        {
            "source": "fixture/nxextract/nxextract-runtime-env.sh",
            "target": "fixture/nxextract/nxextract-runtime-env.sh",
            "kind": "nxextract-runtime-env", "mode": "0644",
            "sha256": digest("fixture/nxextract/nxextract-runtime-env.sh"),
        },
        {
            "source": "fixture/nxextract/nxextract-ui",
            "target": "fixture/nxextract/nxextract-ui",
            "kind": "nxextract-ui-linux", "mode": "0755",
            "architecture": "aarch64",
            "build_profile": "universal-low-glibc",
            "provenance": "NXExtract 1.2.16 canonical low-glibc UI",
            "sha256": digest("fixture/nxextract/nxextract-ui"),
            "needed": ["libc.so.6", "libdl.so.2"], "soname": None,
        },
        {
            "source": "fixture/nxextract-version.txt",
            "target": "fixture/nxextract-version.txt",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/nxextract-version.txt"),
        },
        {
            "source": "fixture/extractor.json",
            "target": "fixture/extractor.json",
            "kind": "nxextract-recipe", "mode": "0644",
            "sha256": digest("fixture/extractor.json"),
        },
        {
            "source": "payload", "target": "fixture/assets",
            "kind": "payload",
        },
        {
            "source": "port.json", "target": "fixture/port.json",
            "kind": "portmaster-metadata", "mode": "0644",
            "sha256": digest("port.json"),
        },
        {
            "source": "gameinfo.xml", "target": "fixture/gameinfo.xml",
            "kind": "portmaster-metadata", "mode": "0644",
            "sha256": digest("gameinfo.xml"),
        },
        {
            "source": "cover.png", "target": "fixture/cover.png",
            "kind": "portmaster-image", "mode": "0644",
            "sha256": digest("cover.png"),
        },
    ],
}
if not self_contained:
    manifest["files"].extend([
        {
            "source": "fixture/nxbootstrap.sh",
            "target": "fixture/nxbootstrap.sh",
            "kind": "script", "mode": "0644",
            "sha256": digest("fixture/nxbootstrap.sh"),
        },
        {
            "source": "fixture/" + bootstrap_name,
            "target": "fixture/" + bootstrap_name,
            "kind": "script", "mode": "0644",
            "sha256": digest("fixture/" + bootstrap_name),
        },
        {
            "source": "fixture/nxdeployment.json",
            "target": "fixture/nxdeployment.json",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/nxdeployment.json"),
        },
    ])

# generate-port already published the canonical v1 generation before the live
# launcher.  Include that exact five-file store in the release fixture: a
# receipt without its store would be a corrupt split state, not a generation.
generation_root = os.path.join(root, "fixture/.nxruntime/generations")
generation_ids = sorted(
    name for name in os.listdir(generation_root)
    if os.path.isdir(os.path.join(generation_root, name))
)
assert len(generation_ids) == 1, generation_ids
generation_prefix = "fixture/.nxruntime/generations/" + generation_ids[0]
for relative, mode in (
    ("commit", "0644"),
    ("components.sha256", "0644"),
    ("manifest.json", "0644"),
    ("files/launcher/Game.sh", "0755"),
    ("files/nxport.json", "0644"),
):
    target = generation_prefix + "/" + relative
    manifest["files"].append({
        "source": target,
        "target": target,
        "kind": "nxruntime-generation",
        "mode": mode,
        "sha256": digest(target),
    })

# Seal the hand-built fixture exactly like nxgenerator 0.3.0: every staged
# target except GENERATION.json itself is ordered and bound by path/mode/SHA.
# The one directory source is expanded to the same per-file targets that
# nxrelease loads from the manifest.
artifacts = []
for item in manifest["files"]:
    if item["target"] == "fixture/GENERATION.json":
        continue
    source = os.path.join(root, item["source"])
    if os.path.isdir(source):
        for directory, _dirs, names in os.walk(source):
            for name in sorted(names):
                member = os.path.join(directory, name)
                relative = os.path.relpath(member, source).replace(os.sep, "/")
                target = item["target"].rstrip("/") + "/" + relative
                artifacts.append({
                    "path": target,
                    "mode": item.get("mode", "0644"),
                    "sha256": hashlib.sha256(open(member, "rb").read()).hexdigest(),
                })
    else:
        artifacts.append({
            "path": item["target"],
            "mode": item["mode"],
            "sha256": hashlib.sha256(open(source, "rb").read()).hexdigest(),
        })
artifacts.sort(key=lambda item: item["path"])
generation_path = os.path.join(root, "fixture/GENERATION.json")
with open(generation_path, "r", encoding="utf-8") as handle:
    generation = json.load(handle)
generation["artifacts"] = artifacts
with open(generation_path, "w", encoding="utf-8") as handle:
    json.dump(generation, handle, sort_keys=True, indent=2)
    handle.write("\n")
next(item for item in manifest["files"]
     if item["target"] == "fixture/GENERATION.json")["sha256"] = \
    digest("fixture/GENERATION.json")
with open(output, "w", encoding="utf-8") as handle:
    json.dump(manifest, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY

python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json" |
  grep -q 'NXRELEASE VALIDATE: PASS' || fail 'positive validate did not pass'

# A valid pin survives the full round-trip below; absent or forged input pins
# remain fail-closed and never reach staging.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP" <<'PY'
import copy
import json
import pathlib
import sys

manifest = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
root = pathlib.Path(sys.argv[2])
target = "fixture/defaults/NEXTOSCONTROLLERS.gptk"

missing = copy.deepcopy(manifest)
next(item for item in missing["files"] if item["target"] == target).pop(
    "sha256"
)
(root / "generation-defaults-pin-missing.json").write_text(
    json.dumps(missing, sort_keys=True, indent=2) + "\n", encoding="utf-8"
)

wrong = copy.deepcopy(manifest)
next(item for item in wrong["files"] if item["target"] == target)[
    "sha256"
] = "0" * 64
(root / "generation-defaults-pin-wrong.json").write_text(
    json.dumps(wrong, sort_keys=True, indent=2) + "\n", encoding="utf-8"
)
PY
expect_fail generation-defaults-pin-missing \
  'NEXTOSCONTROLLERS[.]gptk must be a pinned 0644 payload' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/generation-defaults-pin-missing.json"
expect_fail generation-defaults-pin-wrong \
  'source hash mismatch.*NEXTOSCONTROLLERS[.]gptk' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/generation-defaults-pin-wrong.json"

# Public PortMaster metadata and the bilingual installation contract are
# mandatory at manifest time, before any stage or archive can be produced.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP" <<'PY'
import copy
import json
import pathlib
import sys

manifest_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
original = json.loads(manifest_path.read_text(encoding="utf-8"))

variants = {}
missing_metadata = copy.deepcopy(original)
missing_metadata["portmaster_metadata"] = None
variants["portmaster-metadata-missing"] = missing_metadata

missing_port_json = copy.deepcopy(original)
missing_port_json["portmaster_metadata"]["port_json"] = None
variants["port-json-missing"] = missing_port_json

missing_installation = copy.deepcopy(original)
missing_installation["files"] = [
    item for item in missing_installation["files"]
    if item["target"] != "fixture/INSTALLATION.md"
]
variants["installation-missing"] = missing_installation

for label, value in variants.items():
    (root / (label + ".json")).write_text(
        json.dumps(value, sort_keys=True, indent=2) + "\n", encoding="utf-8"
    )
PY
expect_fail portmaster-metadata-missing 'portmaster_metadata.*required' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/portmaster-metadata-missing.json"
expect_fail port-json-missing 'portmaster_metadata[.]port_json.*required' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/port-json-missing.json"
expect_fail installation-missing 'INSTALLATION[.]md' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/installation-missing.json"

cp -- "$TEST_TMP/source/fixture/INSTALLATION.md" \
  "$TEST_TMP/source/fixture/INSTALLATION.md.saved"
printf '%s\n' '# Installation' '## English' 'English only document.' \
  >"$TEST_TMP/source/fixture/INSTALLATION.md"
python3 - "$TEST_TMP/manifest.json" \
  "$TEST_TMP/source/fixture/INSTALLATION.md" \
  "$TEST_TMP/installation-portuguese-missing.json" <<'PY'
import hashlib
import json
import pathlib
import sys

manifest = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
digest = hashlib.sha256(pathlib.Path(sys.argv[2]).read_bytes()).hexdigest()
record = next(
    item for item in manifest["files"]
    if item["target"] == "fixture/INSTALLATION.md"
)
record["sha256"] = digest
pathlib.Path(sys.argv[3]).write_text(
    json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8"
)
PY
expect_fail installation-portuguese-missing 'lacks the Portuguese section' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/installation-portuguese-missing.json"
mv -- "$TEST_TMP/source/fixture/INSTALLATION.md.saved" \
  "$TEST_TMP/source/fixture/INSTALLATION.md"

python3 -B - "$TOOL" "$TEST_TMP/manifest.json" "$TEST_TMP/source" <<'PY'
import copy
import hashlib
import importlib.util
import json
import pathlib
import sys

tool_raw, manifest_raw, source_raw = sys.argv[1:]
manifest_path = pathlib.Path(manifest_raw)
source_root = pathlib.Path(source_raw)
spec = importlib.util.spec_from_file_location("nxrelease_portmaster_v3", tool_raw)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
original = json.loads(manifest_path.read_text(encoding="utf-8"))
port_json = json.loads((source_root / "port.json").read_text(encoding="utf-8"))

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def expect_metadata_failure(label, mutate, expected):
    candidate = copy.deepcopy(port_json)
    mutate(candidate)
    source = source_root / ("port-" + label + ".json")
    source.write_text(
        json.dumps(candidate, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )
    manifest = copy.deepcopy(original)
    checksum = digest(source)
    manifest["portmaster_metadata"]["port_json"]["sha256"] = checksum
    record = next(
        item for item in manifest["files"]
        if item["target"] == "fixture/port.json"
    )
    record["source"] = source.name
    record["sha256"] = checksum
    selected = manifest_path.parent / ("manifest-" + label + ".json")
    selected.write_text(
        json.dumps(manifest, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )
    try:
        config = module.load_manifest(selected)
        module.validate_sources(config)
    except module.ReleaseError as error:
        if expected.lower() not in str(error).lower():
            raise AssertionError(
                "%s reported %r, expected %r" % (label, str(error), expected)
            )
    else:
        raise AssertionError("%s unexpectedly passed" % label)

if module.validate_portmaster_runtime({}) != []:
    raise AssertionError("absent PortMaster runtime did not normalize empty")
if module.validate_portmaster_runtime(
        {"runtime": ["mono.squashfs"]}) != ["mono.squashfs"]:
    raise AssertionError("nonempty PortMaster runtime was not preserved")

cases = (
    ("runtime-empty", lambda value: value["attr"].update({"runtime": []}),
     "empty attr.runtime array must be omitted"),
    ("runtime-null", lambda value: value["attr"].update({"runtime": None}),
     "runtime must be an array"),
    ("runtime-string", lambda value: value["attr"].update({"runtime": "mono"}),
     "runtime must be an array"),
    ("runtime-duplicate", lambda value: value["attr"].update(
        {"runtime": ["mono", "mono"]}), "runtime contains duplicates"),
    ("items-opt-type", lambda value: value.update({"items_opt": None}),
     "items_opt must be an array"),
    ("items-overlap", lambda value: value.update({"items_opt": ["Game.sh"]}),
     "items/items_opt overlap"),
    ("title-divergence", lambda value: value["attr"].update({"title": "Wrong"}),
     "title differs"),
    ("arch-overclaim", lambda value: value["attr"].update(
        {"arch": ["aarch64", "armhf"]}), "arch differs"),
    ("glibc-missing", lambda value: value["attr"].pop("min_glibc"),
     "missing field"),
    ("metadata-v3", lambda value: value.update({"version": 3}),
     "version must be 4"),
)
for label, mutate, expected in cases:
    expect_metadata_failure(label, mutate, expected)

strict_manifest = manifest_path.read_text(encoding="utf-8")
strict_cases = {
    "duplicate": strict_manifest.replace(
        '"schema_version": 2',
        '"schema_version": 2,\n  "schema_version": 2', 1),
    "nan": strict_manifest.replace('"schema_version": 2',
                                   '"schema_version": NaN', 1),
    "trailing": strict_manifest[:-2] + ",\n}\n",
    "truncated": strict_manifest[:-2],
    "bom": "\ufeff" + strict_manifest,
}
for label, payload in strict_cases.items():
    selected = manifest_path.parent / ("manifest-strict-" + label + ".json")
    selected.write_text(payload, encoding="utf-8")
    try:
        module.load_manifest(selected)
    except module.ReleaseError:
        pass
    else:
        raise AssertionError("strict manifest %s unexpectedly passed" % label)

print("nxrelease PortMaster v3 regressions passed: metadata=10 strict_json=5 publication_contract=4")
PY

# NXExtract UI is a mandatory canonical low-glibc ELF, not an optional payload.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/missing-extract-ui.json" \
  "$TEST_TMP/relabelled-extract-ui.json" <<'PY'
import copy
import json
import sys

base_path, missing_path, relabelled_path = sys.argv[1:]
with open(base_path, encoding="utf-8") as handle:
    base = json.load(handle)
target = "fixture/nxextract/nxextract-ui"
missing = copy.deepcopy(base)
missing["files"] = [item for item in missing["files"]
                    if item["target"] != target]
with open(missing_path, "w", encoding="utf-8") as handle:
    json.dump(missing, handle, sort_keys=True, indent=2)
    handle.write("\n")
relabelled = copy.deepcopy(base)
next(item for item in relabelled["files"]
     if item["target"] == target)["kind"] = "third-party-linux"
with open(relabelled_path, "w", encoding="utf-8") as handle:
    json.dump(relabelled, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
expect_fail missing-extract-ui 'ui_path.*nxextract-ui-linux|UI is absent' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/missing-extract-ui.json"
expect_fail relabelled-extract-ui 'ui_path.*nxextract-ui-linux|wrong inventory kind' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/relabelled-extract-ui.json"

# The UI row is selected by nxport architecture.  A valid ARMv7 release
# artifact cannot be self-pinned into an AArch64 package.
cp -a -- "$TEST_TMP/source" "$TEST_TMP/source-cross-arch-ui"
cp "$NXEXTRACT_ROOT/ui/release/armv7/nxextract-ui" \
  "$TEST_TMP/source-cross-arch-ui/fixture/nxextract/nxextract-ui"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source-cross-arch-ui" \
  "$TEST_TMP/cross-arch-ui.json" <<'PY'
import hashlib
import json
import pathlib
import sys

base_path, root_raw, output_path = sys.argv[1:]
root = pathlib.Path(root_raw)
ui = root / "fixture/nxextract/nxextract-ui"
digest = hashlib.sha256(ui.read_bytes()).hexdigest()
with open(base_path, encoding="utf-8") as handle:
    data = json.load(handle)
data["source_root"] = root.name
data["nxextract"]["ui_sha256"] = digest
entry = next(item for item in data["files"]
             if item["target"] == "fixture/nxextract/nxextract-ui")
entry["sha256"] = digest
entry["architecture"] = "armv7"
with open(output_path, "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
expect_fail cross-arch-extract-ui 'architecture differs from nxport' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/cross-arch-ui.json"

# A modified runner cannot bless itself by updating both manifest hashes.
cp -a -- "$TEST_TMP/source" "$TEST_TMP/source-self-pinned-runner"
python3 - "$TEST_TMP/manifest.json" \
  "$TEST_TMP/source-self-pinned-runner" \
  "$TEST_TMP/self-pinned-extract-runner.json" <<'PY'
import hashlib
import json
import pathlib
import sys

manifest_path, source_root_raw, output_path = sys.argv[1:]
source_root = pathlib.Path(source_root_raw)
runner = source_root / "fixture/nxextract/run-extractor.sh"
runner.write_bytes(runner.read_bytes() + b"# self-pinned tamper\n")
runner_hash = hashlib.sha256(runner.read_bytes()).hexdigest()
with open(manifest_path, encoding="utf-8") as handle:
    data = json.load(handle)
data["source_root"] = source_root.name
data["nxextract"]["runner_sha256"] = runner_hash
next(item for item in data["files"]
     if item["target"] == "fixture/nxextract/run-extractor.sh")["sha256"] = runner_hash
with open(output_path, "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
# A mensagem deixou de dizer "canonical" de proposito: o hash cobrado e' o da
# VERSAO DECLARADA pelo pacote, que a partir do registro de identidades pode
# nao ser a canonica. Chamar de canonico seria mentira assim que houver duas.
expect_fail self-pinned-extract-runner 'runner_sha256.*NXExtract 1.3.0 identity' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/self-pinned-extract-runner.json"

# Container compatibility is content-driven and survives APK packaging SHA drift.
cp -a -- "$TEST_TMP/source" "$TEST_TMP/source-flex-apk"
cp -a -- "$TEST_TMP/source" "$TEST_TMP/source-exact-apk"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source-flex-apk" \
  "$TEST_TMP/flexible-apk.json" "$TEST_TMP/source-exact-apk" \
  "$TEST_TMP/exact-apk.json" <<'PY'
import copy
import hashlib
import json
import pathlib
import sys

base_path, flex_root_raw, flex_out, exact_root_raw, exact_out = sys.argv[1:]
with open(base_path, encoding="utf-8") as handle:
    base = json.load(handle)

def publish(root_raw, output, container_validation):
    root = pathlib.Path(root_raw)
    recipe_path = root / "fixture" / "extractor.json"
    recipe = {
        "schema": 1, "id": "fixture", "version": "variant-policy",
        "input": {"packages": ["com.example.fixture"]},
        "extract": [
            {
                "id": "owner-apk", "source": {"kind": "container"},
                "destination": "game.apk", "validate": container_validation,
            },
            {
                "id": "runtime-anchor",
                "source": {"kind": "entry", "patterns": ["lib/{abi}/libgame.so"]},
                "destination": "libgame.so",
                "validate": {"sha256": "a" * 64},
            },
        ],
        "validate": [], "commit": ["game.apk", "libgame.so"],
    }
    recipe_path.write_text(json.dumps(recipe, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
    digest = hashlib.sha256(recipe_path.read_bytes()).hexdigest()
    manifest = copy.deepcopy(base)
    manifest["source_root"] = root.name
    manifest["nxextract"]["recipe_sha256"] = digest
    next(item for item in manifest["files"]
         if item["target"] == "fixture/extractor.json")["sha256"] = digest
    generation_path = root / "fixture" / "GENERATION.json"
    generation = json.loads(generation_path.read_text(encoding="utf-8"))
    next(item for item in generation["artifacts"]
         if item["path"] == "fixture/extractor.json")["sha256"] = digest
    generation_path.write_text(
        json.dumps(generation, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )
    generation_digest = hashlib.sha256(
        generation_path.read_bytes()
    ).hexdigest()
    next(item for item in manifest["files"]
         if item["target"] == "fixture/GENERATION.json")["sha256"] = \
        generation_digest
    with open(output, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, sort_keys=True, indent=2)
        handle.write("\n")

publish(flex_root_raw, flex_out, {
    "type": "file", "min_size": 1024, "max_size": 1048576,
    "magic_hex": "504b0304",
})
publish(exact_root_raw, exact_out, {
    "type": "file", "size": 2048, "sha256": "b" * 64,
    "magic_hex": "504b0304",
})
PY
python3 "$TOOL" validate --manifest "$TEST_TMP/flexible-apk.json" |
  grep -q 'NXRELEASE VALIDATE: PASS' ||
  fail 'content-driven APK container recipe was rejected'
expect_fail exact-apk 'NXA0003|NXA0001' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/exact-apk.json"

# The framework startup helper is a first-class audited Linux ELF, not a
# generic payload that can be omitted, relabelled or replaced by a port.
cp -a -- "$TEST_TMP/source" "$TEST_TMP/source-altered-splash"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/missing-splash.json" \
  "$TEST_TMP/relabelled-splash.json" "$TEST_TMP/altered-splash.json" \
  "$TEST_TMP/source-altered-splash" <<'PY'
import copy
import hashlib
import json
import pathlib
import sys

base_path, missing_path, relabelled_path, altered_path, altered_root = sys.argv[1:]
with open(base_path, encoding="utf-8") as handle:
    base = json.load(handle)

def splash_entry(document):
    return next(item for item in document["files"]
                if item["target"] == "fixture/nxsplash-nextos")

missing = copy.deepcopy(base)
missing["files"] = [item for item in missing["files"]
                    if item["target"] != "fixture/nxsplash-nextos"]
with open(missing_path, "w", encoding="utf-8") as handle:
    json.dump(missing, handle, sort_keys=True, indent=2)
    handle.write("\n")

relabelled = copy.deepcopy(base)
splash_entry(relabelled)["kind"] = "project-linux"
with open(relabelled_path, "w", encoding="utf-8") as handle:
    json.dump(relabelled, handle, sort_keys=True, indent=2)
    handle.write("\n")

altered = copy.deepcopy(base)
root = pathlib.Path(altered_root)
path = root / "fixture" / "nxsplash-nextos"
path.write_bytes(path.read_bytes() + b"altered")
digest = hashlib.sha256(path.read_bytes()).hexdigest()
altered["source_root"] = root.name
splash_entry(altered)["sha256"] = digest
with open(altered_path, "w", encoding="utf-8") as handle:
    json.dump(altered, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
expect_fail missing-splash 'exactly one classified nxsplash' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/missing-splash.json"
expect_fail relabelled-splash 'exactly one classified nxsplash' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/relabelled-splash.json"
expect_fail altered-splash 'differs from the canonical release' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/altered-splash.json"

# Optional language support remains canonical end-to-end and is absent from
# ports that do not opt in. Regenerate only launcher+nxport in an isolated copy.
cp -a -- "$TEST_TMP/source" "$TEST_TMP/source-language"
python3 - "$TEST_TMP/source-language/fixture/nxport.json" \
  "$TEST_TMP/language-nxport.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    config = json.load(handle)
config["language"] = {
    "default": "auto",
    "supported": ["en", "es", "pt-br"],
}
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(config, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
python3 "$BOOTSTRAP_ROOT/tools/generate-port.py" \
  "$TEST_TMP/language-nxport.json" \
  --output "$TEST_TMP/source-language" --force >/dev/null
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/manifest-language.json" \
  "$TEST_TMP/source-language" <<'PY'
import hashlib
import json
import pathlib
import sys

base, output, source = sys.argv[1:]
root = pathlib.Path(source)
with open(base, encoding="utf-8") as handle:
    manifest = json.load(handle)
manifest["source_root"] = root.name

def digest(relative):
    return hashlib.sha256((root / relative).read_bytes()).hexdigest()

manifest["package"]["launcher_contract"]["config_sha256"] = digest(
    "fixture/nxport.json"
)
for record in manifest["files"]:
    if record["target"] in ("Game.sh", "fixture/nxport.json"):
        record["sha256"] = digest(record["source"])

# --force publishes a new immutable generation and deliberately leaves the
# previous one available as rollback source.  This variant must package the
# generation whose stored launcher/nxport are byte-identical to the new live
# pair, never keep the old store under a freshly resealed receipt.
store_root = root / "fixture/.nxruntime/generations"
matching_generations = [
    path for path in store_root.iterdir()
    if path.is_dir() and not path.is_symlink() and
    (path / "files/launcher/Game.sh").read_bytes() ==
        (root / "Game.sh").read_bytes() and
    (path / "files/nxport.json").read_bytes() ==
        (root / "fixture/nxport.json").read_bytes()
]
assert len(matching_generations) == 1, matching_generations
generation_id = matching_generations[0].name
generation_prefix = "fixture/.nxruntime/generations/" + generation_id
manifest["files"] = [
    record for record in manifest["files"]
    if not record["target"].startswith(
        "fixture/.nxruntime/generations/"
    )
]
for relative, mode in (
    ("commit", "0644"),
    ("components.sha256", "0644"),
    ("manifest.json", "0644"),
    ("files/launcher/Game.sh", "0755"),
    ("files/nxport.json", "0644"),
):
    target = generation_prefix + "/" + relative
    manifest["files"].append({
        "source": target,
        "target": target,
        "kind": "nxruntime-generation",
        "mode": mode,
        "sha256": digest(target),
    })

generation_path = root / "fixture/GENERATION.json"
generation = json.loads(generation_path.read_text(encoding="utf-8"))
generation["generation_id"] = generation_id
artifacts = []
for record in manifest["files"]:
    if record["target"] == "fixture/GENERATION.json":
        continue
    source_path = root / record["source"]
    if source_path.is_dir():
        for member in sorted(path for path in source_path.rglob("*")
                             if path.is_file()):
            relative = member.relative_to(source_path).as_posix()
            artifacts.append({
                "path": record["target"].rstrip("/") + "/" + relative,
                "mode": record.get("mode", "0644"),
                "sha256": hashlib.sha256(member.read_bytes()).hexdigest(),
            })
    else:
        artifacts.append({
            "path": record["target"],
            "mode": record["mode"],
            "sha256": hashlib.sha256(source_path.read_bytes()).hexdigest(),
        })
generation["artifacts"] = sorted(artifacts, key=lambda item: item["path"])
generation_path.write_text(
    json.dumps(generation, sort_keys=True, indent=2) + "\n",
    encoding="utf-8",
)
next(record for record in manifest["files"]
     if record["target"] == "fixture/GENERATION.json")["sha256"] = digest(
         "fixture/GENERATION.json"
     )
with open(output, "w", encoding="utf-8") as handle:
    json.dump(manifest, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-language.json" |
  grep -q 'NXRELEASE VALIDATE: PASS' ||
  fail 'language-aware positive validate did not pass'
grep -Fq 'GAME_LANGUAGE="auto"' "$TEST_TMP/source-language/Game.sh" ||
  fail 'language-aware release lacks its visible edit line'

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/pre-deployment-bootstrap.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["package"]["launcher_contract"]["version"] = "0.8.2"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
expect_fail pre-deployment-bootstrap 'must be canonical nxbootstrap 0[.]8[.]4' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/pre-deployment-bootstrap.json"
# 0.6.0: the single launcher carries the whole PortMaster integration.
for token in 'control.txt' 'get_controls' 'pm_platform_helper' \
             'nxbootstrap_finish' 'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"' \
             'command ls -Lldn /proc/self/fd/9' \
             '"$NXBOOTSTRAP_LOCK_FILE" -ef /proc/self/fd/9' \
             'NXBOOTSTRAP_CHILD_STARTTIME=${20}' \
             'NXBOOTSTRAP_SHUTDOWN_TICKS=10' \
             'builtin kill -KILL "$game_pid"' "trap '' INT TERM HUP" \
             'NXBOOTSTRAP_SPLASH="$GAMEDIR/nxsplash-nextos"' \
             'mandatory handoff complete'; do
  grep -Fq "$token" "$TEST_TMP/source/Game.sh" ||
    fail "self-contained launcher fixture is missing: $token"
done

# A benign secondary hop used to survive validation because every script was
# audited individually but the public shell layout was never counted.
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$TEST_TMP/source/benign.sh"
chmod 0755 "$TEST_TMP/source/benign.sh"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source/benign.sh" \
  "$TEST_TMP/secondary-run.json" "$TEST_TMP/second-top-level.json" <<'PY'
import copy
import hashlib
import json
import sys

manifest_path, script_path, run_output, top_output = sys.argv[1:]
with open(manifest_path, encoding="utf-8") as handle:
    base = json.load(handle)
with open(script_path, "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()

secondary_run = copy.deepcopy(base)
secondary_run["files"].append({
    "source": "benign.sh",
    "target": "fixture/RUN.sh",
    "kind": "script",
    "mode": "0755",
    "sha256": digest,
})
with open(run_output, "w", encoding="utf-8") as handle:
    json.dump(secondary_run, handle, sort_keys=True, indent=2)
    handle.write("\n")

second_top_level = copy.deepcopy(base)
second_top_level["files"].append({
    "source": "benign.sh",
    "target": "Legacy.sh",
    "kind": "script",
    "mode": "0755",
    "sha256": digest,
})
with open(top_output, "w", encoding="utf-8") as handle:
    json.dump(second_top_level, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
expect_fail secondary-run 'forbidden secondary.*run[.]sh' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/secondary-run.json"
expect_fail second-top-level 'exactly one top-level [.]sh' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/second-top-level.json"

# Public release consumes only canonical nxport v2 and proves the generated
# The single public launcher carries every declarative field. Legacy input is upgraded by the
# generator, never guessed by the release gate.
for contract_case in legacy unknown capability capability-order quirk \
                     divergence runtime-contract required-files \
                     nxextract-optional required-check dead-payload-gates \
                     inode-bound-lock unbounded-termination unguarded-finish \
                     splash-handoff splash-order splash-disable; do
  cp -a -- "$TEST_TMP/source" "$TEST_TMP/source-$contract_case"
  python3 - "$TEST_TMP/manifest.json" \
    "$TEST_TMP/manifest-$contract_case.json" \
    "$TEST_TMP/source-$contract_case" "$contract_case" <<'PY'
import hashlib
import json
import pathlib
import shlex
import sys

base, output, source_root, mode = sys.argv[1:]
with open(base, encoding="utf-8") as handle:
    data = json.load(handle)
data["source_root"] = pathlib.Path(source_root).name

root = pathlib.Path(source_root)
config_path = root / "fixture" / "nxport.json"
launcher_path = root / "Game.sh"
if mode == "legacy":
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["schema_version"] = 1
    config_path.write_text(json.dumps(config, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
elif mode == "unknown":
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["process_names"] = ["forbidden"]
    config_path.write_text(json.dumps(config, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
elif mode == "capability":
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["required_capabilities"] = ["host.unregistered-capability"]
    config_path.write_text(json.dumps(config, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
elif mode == "capability-order":
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["required_capabilities"] = ["graphics.gles2", "host.portmaster"]
    config_path.write_text(json.dumps(config, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
elif mode == "quirk":
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["enabled_quirks"] = ["engine.unregistered-quirk"]
    config_path.write_text(json.dumps(config, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
elif mode == "divergence":
    text = launcher_path.read_text(encoding="utf-8")
    old = 'GAMEDIR="/$directory/ports/fixture"'
    if text.count(old) != 1:
        raise SystemExit("GAMEDIR assignment fixture changed")
    launcher_path.write_text(
        text.replace(old, 'GAMEDIR="/$directory/ports/other"'),
        encoding="utf-8")
elif mode == "runtime-contract":
    text = launcher_path.read_text(encoding="utf-8")
    old = "export NXCOMPAT_RUNTIME_REPORT=log-and-logo"
    if text.count(old) != 1:
        raise SystemExit("runtime report export fixture changed")
    launcher_path.write_text(
        text.replace(old, "export NXCOMPAT_RUNTIME_REPORT=log"),
        encoding="utf-8")
elif mode == "required-files":
    text = launcher_path.read_text(encoding="utf-8")
    config = json.loads(config_path.read_text(encoding="utf-8"))
    old = "NXBOOTSTRAP_REQUIRED_FILES=" + shlex.quote(
        "\n".join(config["required_files"])
    )
    if text.count(old) != 1:
        raise SystemExit("required_files assignment fixture changed")
    launcher_path.write_text(
        text.replace(old, "NXBOOTSTRAP_REQUIRED_FILES=missing/payload", 1),
        encoding="utf-8")
elif mode == "nxextract-optional":
    text = launcher_path.read_text(encoding="utf-8")
    old = "NXEXTRACT_REQUESTED=1"
    if text.count(old) != 1:
        raise SystemExit("mode=yes NXExtract fixture changed")
    launcher_path.write_text(
        text.replace(old, "NXEXTRACT_REQUESTED=0", 1),
        encoding="utf-8")
elif mode == "required-check":
    text = launcher_path.read_text(encoding="utf-8")
    old = 'required_path=$(readlink -f "$GAMEDIR/$required_file"'
    if text.count(old) != 1:
        raise SystemExit("required file readlink fixture changed")
    launcher_path.write_text(
        text.replace(
            old,
            'required_path=$(printf %s "$GAMEDIR/$required_file"',
            1,
        ),
        encoding="utf-8")
elif mode == "dead-payload-gates":
    text = launcher_path.read_text(encoding="utf-8")
    first = "# NXExtract owner-data phase"
    last = "unset NXBOOTSTRAP_REQUIRED_FILES required_file required_path"
    if text.count(first) != 1 or text.count(last) != 1:
        raise SystemExit("payload gate boundaries changed")
    text = text.replace(first, "if false; then\n" + first, 1)
    launcher_path.write_text(
        text.replace(last, last + "\nfi", 1), encoding="utf-8"
    )
elif mode == "inode-bound-lock":
    text = launcher_path.read_text(encoding="utf-8")
    old = 'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"'
    if text.count(old) != 1:
        raise SystemExit("stable lock fixture changed")
    launcher_path.write_text(
        text.replace(old, 'exec 9<"$NXBOOTSTRAP_EXECUTABLE"', 1),
        encoding="utf-8")
elif mode == "unbounded-termination":
    text = launcher_path.read_text(encoding="utf-8")
    old = 'builtin kill -KILL "$game_pid"'
    if text.count(old) != 1:
        raise SystemExit("forced termination fixture changed")
    launcher_path.write_text(
        text.replace(old, 'builtin kill -TERM "$game_pid"', 1),
        encoding="utf-8")
elif mode == "unguarded-finish":
    text = launcher_path.read_text(encoding="utf-8")
    old = '[ "$NXBOOTSTRAP_FINISHED" = 0 ] || return 0'
    if text.count(old) != 1:
        raise SystemExit("finish guard fixture changed")
    launcher_path.write_text(
        text.replace(old, "true", 1), encoding="utf-8")
elif mode == "splash-handoff":
    text = launcher_path.read_text(encoding="utf-8")
    old = 'NXBOOTSTRAP_SPLASH="$GAMEDIR/nxsplash-nextos"'
    if text.count(old) != 1:
        raise SystemExit("mandatory splash fixture changed")
    launcher_path.write_text(
        text.replace(old, 'NXBOOTSTRAP_SPLASH="$GAMEDIR/missing-splash"', 1),
        encoding="utf-8",
    )
elif mode == "splash-order":
    text = launcher_path.read_text(encoding="utf-8")
    first = "# Mandatory framework identity handoff"
    last = "NXBOOTSTRAP_SPLASH_PREFIX NXBOOTSTRAP_SPLASH_INTERP d"
    required = "# Manifest-owned payload gate"
    start = text.find(first)
    end = text.find(last)
    required_index = text.find(required)
    if min(start, end, required_index) < 0:
        raise SystemExit("splash/order fixture changed")
    end += len(last)
    block = text[start:end]
    text = text[:start] + text[end:]
    required_index = text.find(required)
    launcher_path.write_text(
        text[:required_index] + block + "\n" + text[required_index:],
        encoding="utf-8",
    )
elif mode == "splash-disable":
    text = launcher_path.read_text(encoding="utf-8")
    marker = "# Mandatory framework identity handoff"
    if text.count(marker) != 1:
        raise SystemExit("mandatory splash marker changed")
    launcher_path.write_text(
        text.replace(marker, "NXSPLASH_SKIP=1\n" + marker, 1),
        encoding="utf-8",
    )
else:
    raise SystemExit("unknown contract case")

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

if mode in ("legacy", "unknown", "capability", "capability-order", "quirk"):
    config_sha = digest(config_path)
    data["package"]["launcher_contract"]["config_sha256"] = config_sha
    entry = next(item for item in data["files"]
                 if item["target"] == "fixture/nxport.json")
    entry["sha256"] = config_sha
else:
    launcher_sha = digest(launcher_path)
    entry = next(item for item in data["files"]
                 if item["target"] == "Game.sh")
    entry["sha256"] = launcher_sha

with open(output, "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
done
expect_fail nxport-legacy 'schema_version must be 2|regenerate legacy' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-legacy.json"
expect_fail nxport-unknown 'unknown field.*process_names' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-unknown.json"
expect_fail nxport-capability 'required_capabilities has an unknown name' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-capability.json"
expect_fail nxport-capability-order 'required_capabilities is not in canonical' \
  python3 "$TOOL" validate --manifest \
    "$TEST_TMP/manifest-capability-order.json"
expect_fail nxport-quirk 'enabled_quirks has an unknown name' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-quirk.json"
expect_fail nxport-divergence 'does not derive GAMEDIR' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-divergence.json"
expect_fail nxport-runtime-contract \
  'NXCOMPAT_RUNTIME_REPORT differs from nxport[.]json' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-runtime-contract.json"
expect_fail nxport-required-files 'required_files gate differs' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-required-files.json"
expect_fail nxport-nxextract-optional 'NXExtract policy differs' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-nxextract-optional.json"
expect_fail nxport-required-check 'required_files gate differs' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-required-check.json"
expect_fail nxport-dead-payload-gates 'canonical nxbootstrap render' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-dead-payload-gates.json"
expect_fail nxport-inode-bound-lock 'is missing|canonical nxbootstrap render' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-inode-bound-lock.json"
expect_fail nxport-unbounded-termination 'is missing|canonical nxbootstrap render' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-unbounded-termination.json"
expect_fail nxport-unguarded-finish 'is missing|canonical nxbootstrap render' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-unguarded-finish.json"
expect_fail nxport-splash-handoff 'nxsplash handoff differs' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-splash-handoff.json"
expect_fail nxport-splash-order 'payload/splash phases are out of order' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-splash-order.json"
expect_fail nxport-splash-disable 'nxsplash removal switch' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-splash-disable.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/schema-v1.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["schema_version"] = 1
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail schema-v1 'v1 did not close dependencies|schema_version must be 2' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/schema-v1.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/traversal.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["files"][-1]["target"] = "../escape"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail manifest-traversal 'not a safe relative path' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/traversal.json"

ln -s ../README.txt "$TEST_TMP/source/payload/hostile-link"
expect_fail source-symlink 'contains symlink|traverses a symlink|non-regular file' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json"
rm -f -- "$TEST_TMP/source/payload/hostile-link"

# V4-03B: o piso SDL por versao decide na PRIMEIRA fronteira somente leitura.
# Um ELF real com import direto de SDL_JoystickGetVendor (SDL 2.0.6) reprova
# no validate com simbolo/versao/piso nomeados, e a tentativa de stage falha
# ANTES de criar qualquer arquivo. O recibo do validate bom carrega a
# identidade (id + sha256) da autoridade unica.
python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json" \
  > "$TEST_TMP/validate-receipt.output"
grep -q 'sdl_floor=2\.0\.4' "$TEST_TMP/validate-receipt.output" ||
  fail 'validate receipt lacks the declared SDL floor'
grep -q 'sdl_authority=nx-sdl-symbol-floor/1' "$TEST_TMP/validate-receipt.output" ||
  fail 'validate receipt lacks the SDL authority id'
grep -Eq 'sdl_authority_sha256=[0-9a-f]{64}' "$TEST_TMP/validate-receipt.output" ||
  fail 'validate receipt lacks the SDL authority sha256'
RECEIPT_AUTHORITY_SHA=$(sed -n \
  's/.*sdl_authority_sha256=\([0-9a-f]\{64\}\).*/\1/p' \
  "$TEST_TMP/validate-receipt.output")
[ "$RECEIPT_AUTHORITY_SHA" = \
  "$(sha256sum "$ROOT/../nxabi/sdl2-symbol-floor.tsv" | cut -d' ' -f1)" ] ||
  fail 'validate receipt sha256 is not the exact authority bytes on disk'

cat >"$TEST_TMP/fake-sdl2.c" <<'C'
void SDL_Init(void) {}
unsigned short SDL_JoystickGetVendor(void *joystick);
unsigned short SDL_JoystickGetVendor(void *joystick) {
  (void)joystick;
  return 0;
}
C
aarch64-linux-gnu-gcc -fPIC -nostdlib -shared \
  -Wl,-soname,libSDL2-2.0.so.0 "$TEST_TMP/fake-sdl2.c" \
  -o "$TEST_TMP/libfake-sdl2.so"
cat >"$TEST_TMP/sdl-vendor.c" <<'C'
extern void SDL_Init(void);
extern unsigned short SDL_JoystickGetVendor(void *joystick);
void _start(void);
void _start(void) {
  SDL_Init();
  SDL_JoystickGetVendor(0);
}
C
aarch64-linux-gnu-gcc -nostdlib -Wl,-e,_start \
  -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1 \
  "$TEST_TMP/sdl-vendor.c" -Wl,--no-as-needed "$TEST_TMP/libfake-sdl2.so" \
  -o "$TEST_TMP/source/loader-sdl-vendor"
chmod 0755 "$TEST_TMP/source/loader-sdl-vendor"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/sdl-floor.json" \
  "$TEST_TMP/source/loader-sdl-vendor" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
data["files"].append({
    "source": "loader-sdl-vendor",
    "target": "fixture/bin/aarch64/loader-sdl-vendor",
    "kind": "project-linux", "mode": "0755",
    "architecture": "aarch64",
    "build_profile": "universal-low-glibc",
    "provenance": "offline SDL floor negative fixture",
    "sha256": digest,
    "needed": ["libSDL2-2.0.so.0"], "soname": None,
})
data["dependencies"].append({
    "namespace": "linux", "architecture": "aarch64",
    "soname": "libSDL2-2.0.so.0", "provider": "portmaster",
})
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail sdl-floor-preflight \
  'loader-sdl-vendor.*SDL_JoystickGetVendor.*SDL 2\.0\.6.*floor SDL 2\.0\.4' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/sdl-floor.json"
python3 - "$TEST_TMP/source/loader" "$TEST_TMP/sdl-candidate-lock.json" <<'PY'
import hashlib, json, pathlib, sys
executable = pathlib.Path(sys.argv[1])
document = {
    "schema": "nxrelease-candidate-lock-v1",
    "schema_version": 1,
    "executable": "fixture/bin/aarch64/loader",
    "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
}
pathlib.Path(sys.argv[2]).write_text(
    json.dumps(document, sort_keys=True, indent=2) + "\n", encoding="utf-8")
PY
chmod 0444 "$TEST_TMP/sdl-candidate-lock.json"
if python3 "$TOOL" stage --manifest "$TEST_TMP/sdl-floor.json" \
    --candidate-lock "$TEST_TMP/sdl-candidate-lock.json" \
    --stage "$TEST_TMP/stage-sdl-floor" \
    >"$TEST_TMP/sdl-floor-stage.output" 2>&1; then
  fail 'stage com violacao de piso SDL passou'
fi
grep -Eqi 'SDL_JoystickGetVendor.*2\.0\.6' "$TEST_TMP/sdl-floor-stage.output" || {
  sed -n '1,40p' "$TEST_TMP/sdl-floor-stage.output" >&2
  fail 'stage nao reprovou pelo piso SDL'
}
[ ! -e "$TEST_TMP/stage-sdl-floor" ] ||
  fail 'preflight reprovado ainda criou artefato de stage'
ls "$TEST_TMP" | grep -q '\.zip$' &&
  fail 'preflight reprovado criou um ZIP' || true
rm -f "$TEST_TMP/source/loader-sdl-vendor" "$TEST_TMP/sdl-floor.json" \
  "$TEST_TMP/fake-sdl2.c" "$TEST_TMP/sdl-vendor.c" "$TEST_TMP/libfake-sdl2.so" \
  "$TEST_TMP/sdl-candidate-lock.json"

# Freeze the independently supplied candidate identity only after every source
# mutation above.  It lives outside source_root and is read-only, matching the
# real post-proof boundary.
CANDIDATE_LOCK="$TEST_TMP/candidate-lock.json"
python3 - "$TEST_TMP/source/loader" "$CANDIDATE_LOCK" <<'PY'
import hashlib, json, pathlib, sys
executable = pathlib.Path(sys.argv[1])
target = pathlib.Path(sys.argv[2])
document = {
    "schema": "nxrelease-candidate-lock-v1",
    "schema_version": 1,
    "executable": "fixture/bin/aarch64/loader",
    "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
}
target.write_text(json.dumps(document, sort_keys=True, indent=2) + "\n",
                  encoding="utf-8")
PY
chmod 0444 "$CANDIDATE_LOCK"

python3 "$TOOL" build \
  --manifest "$TEST_TMP/manifest.json" \
  --candidate-lock "$CANDIDATE_LOCK" \
  --stage "$TEST_TMP/stage-one" \
  --output "$TEST_TMP/fixture-one.zip" \
  | grep -q 'NXRELEASE BUILD: PASS' || fail 'positive build did not pass'

[ -f "$TEST_TMP/stage-one/fixture/.nxrelease/MANIFEST.sha256" ] || fail 'stage has no scoped MANIFEST.sha256'
[ -f "$TEST_TMP/stage-one/fixture/.nxrelease/NXRELEASE-METADATA.json" ] || fail 'stage has no scoped release metadata'
[ ! -e "$TEST_TMP/stage-one/MANIFEST.sha256" ] || fail 'shared ZIP root contains colliding MANIFEST.sha256'
[ -f "$TEST_TMP/fixture-one.zip.sha256" ] || fail 'archive has no external SHA-256'

python3 "$TOOL" verify \
  --archive "$TEST_TMP/fixture-one.zip" \
  --sha256-file "$TEST_TMP/fixture-one.zip.sha256" \
  | grep -q 'NXRELEASE VERIFY: PASS' || fail 'archive verify did not pass'

# The internal receipt is parsed with the same strict JSON boundary as the
# input manifest.  Repacking cannot legitimize duplicate keys, NaN or a BOM.
python3 - "$TEST_TMP/fixture-one.zip" "$TEST_TMP" <<'PY'
import json
import pathlib
import sys
import zipfile

source_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])

def rewrite(label, mutate):
    destination = root / ("archive-metadata-" + label + ".zip")
    with zipfile.ZipFile(source_path, "r") as source:
        with zipfile.ZipFile(destination, "w") as target:
            for info in source.infolist():
                payload = source.read(info.filename)
                if info.filename.endswith("/.nxrelease/NXRELEASE-METADATA.json"):
                    payload = mutate(payload)
                target.writestr(info, payload)

rewrite("duplicate", lambda value: value.replace(
    b'"schema_version": 2',
    b'"schema_version": 2,\n  "schema_version": 2', 1,
))
rewrite("nan", lambda value: value.replace(
    b'"schema_version": 2', b'"schema_version": NaN', 1,
))
rewrite("bom", lambda value: b"\xef\xbb\xbf" + value)


def remove_candidate_lock(value):
    document = json.loads(value.decode("utf-8"))
    document.pop("candidate_lock", None)
    return (json.dumps(
        document, sort_keys=True, indent=2) + "\n").encode("utf-8")

rewrite("candidate-lock-missing", remove_candidate_lock)
PY
expect_fail archive-metadata-duplicate 'archive release metadata contains duplicate key' \
  python3 "$TOOL" verify --archive "$TEST_TMP/archive-metadata-duplicate.zip"
expect_fail archive-metadata-nan 'archive release metadata contains non-JSON constant' \
  python3 "$TOOL" verify --archive "$TEST_TMP/archive-metadata-nan.zip"
expect_fail archive-metadata-bom 'archive release metadata cannot contain a UTF-8 BOM' \
  python3 "$TOOL" verify --archive "$TEST_TMP/archive-metadata-bom.zip"
expect_fail archive-candidate-lock-missing 'candidate-lock release metadata lacks its candidate lock' \
  python3 "$TOOL" verify --archive "$TEST_TMP/archive-metadata-candidate-lock-missing.zip"

# verify_archive must hand the exact ZIP bytes/path to the independent public
# auditor, rather than auditing only a reconstructed stage.
python3 -B - "$TOOL" "$TEST_TMP/fixture-one.zip" <<'PY'
import importlib.util
import pathlib
import sys

tool, archive_raw = sys.argv[1:]
archive = pathlib.Path(archive_raw)
spec = importlib.util.spec_from_file_location("nxrelease_real_zip_audit", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
calls = []
real_audit = module.audit_public_portmaster_zip

def capture(path):
    calls.append(pathlib.Path(path).resolve())
    return real_audit(path)

module.audit_public_portmaster_zip = capture
module.verify_archive(archive)
assert calls == [archive.resolve()], calls
print("nxrelease independent real-ZIP audit passed: calls=1")
PY

python3 "$TOOL" verify-stage --stage "$TEST_TMP/stage-one" \
  | grep -q 'NXRELEASE VERIFY-STAGE: PASS' || fail 'stage verify did not pass'

# A second build from identical inputs must be byte-for-byte identical.
python3 "$TOOL" build \
  --manifest "$TEST_TMP/manifest.json" \
  --candidate-lock "$CANDIDATE_LOCK" \
  --stage "$TEST_TMP/stage-two" \
  --output "$TEST_TMP/fixture-two.zip" >/dev/null
cmp "$TEST_TMP/fixture-one.zip" "$TEST_TMP/fixture-two.zip" ||
  fail 'deterministic archives differ'

python3 "$TOOL" verify \
  --archive "$TEST_TMP/fixture-two.zip" \
  --previous-archive "$TEST_TMP/fixture-one.zip" \
  | grep -q 'NXRELEASE VERIFY: PASS' ||
  fail 'real PortMaster update cycle did not pass'

python3 "$TOOL" bundle \
  --manifest "$TEST_TMP/manifest.json" \
  --candidate-lock "$CANDIDATE_LOCK" \
  --stage "$TEST_TMP/stage-bundle" \
  --destination "$TEST_TMP/publication-bundle" \
  --archive-name fixture.zip \
  | grep -q 'NXRELEASE BUNDLE: PASS' || fail 'atomic bundle did not pass'
[ -f "$TEST_TMP/publication-bundle/fixture.zip" ] || fail 'bundle lacks ZIP'
[ -f "$TEST_TMP/publication-bundle/fixture.zip.sha256" ] || fail 'bundle lacks SHA-256'
cmp "$TEST_TMP/fixture-one.zip" "$TEST_TMP/publication-bundle/fixture.zip" ||
  fail 'bundle ZIP is not deterministic'
python3 - "$TEST_TMP/stage-bundle" <<'PY'
import hashlib
import json
import pathlib
import sys

stage = pathlib.Path(sys.argv[1])
target = "fixture/defaults/NEXTOSCONTROLLERS.gptk"
payload = stage / target
metadata = json.loads(
    (stage / "fixture/.nxrelease/NXRELEASE-METADATA.json").read_text(
        encoding="utf-8"
    )
)
record = next(item for item in metadata["inventory"]
              if item["path"] == target)
assert record["kind"] == "payload" and record["mode"] == "0644"
assert record["sha256"] == hashlib.sha256(payload.read_bytes()).hexdigest()
print("nxrelease generation stage round-trip passed: build=2 bundle=1 negatives=2")
PY

python3 - "$TEST_TMP/fixture-one.zip" "$BOOTSTRAP_VERSION" \
  "$NXEXTRACT_ROOT/ui/release/manifest-v1.json" <<'PY'
import hashlib
import json
import stat
import sys
import time
import zipfile

bootstrap_version = sys.argv[2]
ui_manifest_path = sys.argv[3]
with open(ui_manifest_path, "rb") as handle:
    ui_manifest_bytes = handle.read()
ui_release = json.loads(ui_manifest_bytes.decode("utf-8"))
ui_artifact = ui_release["artifacts"]["aarch64"]
bootstrap_tuple = tuple(int(part) for part in bootstrap_version.split("."))
bootstrap_name = (
    "nxbootstrap-{}.sh".format(bootstrap_version)
    if bootstrap_tuple >= (0, 5, 0)
    else "nxbootstrap.sh"
)

with zipfile.ZipFile(sys.argv[1]) as archive:
    infos = archive.infolist()
    names = [item.filename for item in infos]
    assert names == sorted(names)
    assert names.index("fixture/nxextract-version.txt") < names.index(
        "fixture/nxextract/nxextract.py"
    )
    metadata = json.loads(archive.read("fixture/.nxrelease/NXRELEASE-METADATA.json").decode("utf-8"))
    expected_chain = (
        ["Game.sh"] if bootstrap_tuple >= (0, 6, 0)
        else ["Game.sh", "fixture/" + bootstrap_name]
    )
    assert metadata["package"]["launcher_chain"] == expected_chain
    assert metadata["portmaster_metadata"]["port_json"]["path"] == "fixture/port.json"
    assert metadata["nxsplash"]["path"] == "fixture/nxsplash-nextos"
    assert metadata["nxsplash"]["duration_ms"] == 5000
    assert metadata["nxsplash"]["architecture"] == "aarch64"
    assert metadata["nxextract"]["ui_path"] == "fixture/nxextract/nxextract-ui"
    assert metadata["nxextract"]["ui_architecture"] == "aarch64"
    assert metadata["nxextract"]["ui_sha256"] == ui_artifact["sha256"]
    assert metadata["nxextract"]["ui_glibc_max"] == ui_artifact["glibc_max"]
    assert metadata["nxextract"]["ui_source_sha256"] == ui_release["source_sha256"]
    assert metadata["nxextract"]["ui_version"] == ui_release["version"] == "1.2.16"
    assert metadata["nxextract"]["ui_release_manifest_sha256"] == hashlib.sha256(
        ui_manifest_bytes).hexdigest()
    splash_inventory = next(
        item for item in metadata["inventory"]
        if item["path"] == "fixture/nxsplash-nextos"
    )
    assert splash_inventory["kind"] == "nxsplash-linux"
    assert splash_inventory["sha256"] == metadata["nxsplash"]["sha256"]
    splash_elf = next(
        item for item in metadata["elf_audit"]["files"]
        if item["path"] == "fixture/nxsplash-nextos"
    )
    assert splash_elf["needed"] == ["libc.so.6", "libdl.so.2"]
    assert tuple(map(int, splash_elf["glibc_max"].split("."))) <= (2, 30)
    extract_ui_inventory = next(
        item for item in metadata["inventory"]
        if item["path"] == "fixture/nxextract/nxextract-ui"
    )
    assert extract_ui_inventory["kind"] == "nxextract-ui-linux"
    extract_ui_elf = next(
        item for item in metadata["elf_audit"]["files"]
        if item["path"] == "fixture/nxextract/nxextract-ui"
    )
    assert extract_ui_elf["needed"] == ["libc.so.6", "libdl.so.2"]
    assert extract_ui_elf["architecture"] == "aarch64"
    assert extract_ui_elf["class"] == "ELF64"
    assert extract_ui_elf["machine"] == "AArch64"
    assert tuple(map(int, extract_ui_elf["glibc_max"].split("."))) <= (2, 17)
    assert metadata["elf_audit"]["files"][0]["needed"] == []
    assert metadata["elf_audit"]["files"][0]["soname"] is None
    epoch = metadata["archive"]["source_date_epoch"]
    stamp = list(time.gmtime(epoch)[:6])
    stamp[5] -= stamp[5] % 2
    assert all(item.date_time == tuple(stamp) for item in infos)
    launcher = archive.getinfo("Game.sh")
    assert ((launcher.external_attr >> 16) & 0o7777) == 0o755
    splash = archive.getinfo("fixture/nxsplash-nextos")
    assert ((splash.external_attr >> 16) & 0o7777) == 0o755
    extract_ui = archive.getinfo("fixture/nxextract/nxextract-ui")
    assert ((extract_ui.external_attr >> 16) & 0o7777) == 0o755
    manifest = archive.read("fixture/.nxrelease/MANIFEST.sha256").decode("utf-8")
    assert "fixture/.nxrelease/NXRELEASE-METADATA.json" in manifest
    assert "fixture/.nxrelease/MANIFEST.sha256" not in manifest
PY

# The immutable public ceiling cannot be raised.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/ceiling.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["release"]["max_glibc"] = "2.31"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail ceiling 'public ceiling' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/ceiling.json"

# A current-host/current-glibc profile is rejected even for a static ELF.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/current.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
entry = next(item for item in data["files"] if item["target"].endswith("/loader"))
entry["build_profile"] = "nextos-current"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail current 'current-host|build_profile' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/current.json"

# NXExtract is a content pin, not just a filename/version claim.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/nxhash.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["nxextract"]["sha256"] = "0" * 64
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail nxhash 'NXExtract|pin' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/nxhash.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/nxfloor.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["nxextract"]["minimum_version"] = "1.2.1"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail nxfloor 'below tool floor' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/nxfloor.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/nxmissing.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["files"] = [
    item for item in data["files"]
    if item["target"] != "fixture/nxextract/nxextract-runtime-env.sh"
]
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail nxmissing 'runtime_env_path|runtime helper' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/nxmissing.json"

printf '%s\n' '{"schema":2,"id":"fixture","extract":[],"validate":[],"commit":[]}' \
  >"$TEST_TMP/source/bad-extractor.json"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/nxrecipe.json" \
  "$TEST_TMP/source/bad-extractor.json" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
data["nxextract"]["recipe_sha256"] = digest
entry = next(item for item in data["files"] if item["kind"] == "nxextract-recipe")
entry["source"] = "bad-extractor.json"
entry["sha256"] = digest
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail nxrecipe 'recipe schema must be 1' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/nxrecipe.json"

# No ELF may hide in a generic payload tree.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/unclassified.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
entry = next(item for item in data["files"] if item["target"].endswith("/loader"))
entry["kind"] = "payload"
entry.pop("architecture")
entry.pop("build_profile")
entry.pop("provenance")
entry.pop("needed")
entry.pop("soname")
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail unclassified 'ELF.*unclassified' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/unclassified.json"

# DT_NEEDED is an exact per-ELF contract, not an informational report.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/needed.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
entry = next(item for item in data["files"] if item["target"].endswith("/loader"))
entry["needed"] = ["libc.so.6"]
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail needed 'DT_NEEDED differs' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/needed.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/soname.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
entry = next(item for item in data["files"] if item["target"].endswith("/loader"))
entry["soname"] = "libfixture.so"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail soname 'DT_SONAME differs' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/soname.json"

# Every readelf phase is mandatory and loadability/ABI/interpreter/search paths
# are hard gates, not best-effort reports.
cat >"$TEST_TMP/check-elf.py" <<'PY'
import importlib.util
import pathlib
import sys

tool, elf_path, arch = sys.argv[1:]
spec = importlib.util.spec_from_file_location("nxrelease_test_module", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
try:
    module.elf_information(
        pathlib.Path(elf_path), pathlib.Path(elf_path).name,
        "project-linux", arch, "2.30", "universal-low-glibc", [], None,
    )
except module.ReleaseError as error:
    print("NXRELEASE EXPECTED FAIL: {}".format(error), file=sys.stderr)
    raise SystemExit(1)
raise SystemExit(0)
PY
expect_fail elf-rel 'ET_EXEC/ET_DYN|non-loadable type' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/reloc.o" aarch64
expect_fail elf-no-load 'no PT_LOAD' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/no-load" aarch64
expect_fail elf-class 'class.*disagrees|ELF32' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/class-mismatch" aarch64
expect_fail elf-softfp 'soft-float|hard-float' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/arm-softfp" armv7
expect_fail elf-interp 'PT_INTERP must be exactly' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/wrong-interp" aarch64
expect_fail elf-runpath 'RPATH/RUNPATH' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/with-runpath" aarch64

mkdir -p "$TEST_TMP/fakebin"
cat >"$TEST_TMP/fakebin/readelf" <<SH
#!/usr/bin/env bash
if [ "\${1:-}" = "\${NX_TEST_READELF_FAIL_ARG:--lW}" ]; then
  printf 'forced readelf phase failure: %s\n' "\${1:-}" >&2
  exit 9
fi
exec "$(command -v readelf)" "\$@"
SH
chmod 0755 "$TEST_TMP/fakebin/readelf"
expect_fail readelf-phase 'readelf -lW rejected|forced program-header' \
  env PATH="$TEST_TMP/fakebin:$PATH" \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json"
expect_fail readelf-dynamic 'readelf -dW rejected|forced readelf phase' \
  env PATH="$TEST_TMP/fakebin:$PATH" NX_TEST_READELF_FAIL_ARG=-dW \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json"
expect_fail readelf-versions 'readelf --version-info --wide rejected|forced readelf phase' \
  env PATH="$TEST_TMP/fakebin:$PATH" NX_TEST_READELF_FAIL_ARG=--version-info \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json"

# DT_NEEDED closes over an explicit (namespace, ABI, SONAME) provider map.
cp "$TEST_TMP/source/libdep.so" "$TEST_TMP/source/libdep2.so"
cp -a -- "$TEST_TMP/source" "$TEST_TMP/source-dependency-package"
cat >"$TEST_TMP/dependency-manifest.py" <<'PY'
import hashlib
import json
import os
import sys

base, output, source_root, mode = sys.argv[1:]
with open(base, encoding="utf-8") as handle:
    data = json.load(handle)
base_dependencies = list(data["dependencies"])

def digest(relative):
    with open(os.path.join(source_root, relative), "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()

def elf_entry(source, target, needed, soname):
    return {
        "source": source,
        "target": target,
        "kind": "third-party-linux",
        "mode": "0755",
        "architecture": "aarch64",
        "build_profile": "universal-low-glibc",
        "provenance": "offline dependency closure fixture",
        "sha256": digest(source),
        "needed": needed,
        "soname": soname,
    }

if mode != "bad-name":
    data["files"].append(elf_entry(
        "consumer", "fixture/tests/consumer", ["libdep.so"], None,
    ))

if mode == "external":
    data["dependencies"] = base_dependencies + [{
        "namespace": "linux", "architecture": "aarch64",
        "soname": "libdep.so", "provider": "portmaster",
    }]
elif mode in ("package", "duplicate", "two-package"):
    data["files"].append(elf_entry(
        "libdep.so", "fixture/lib/aarch64/libdep.so", [], "libdep.so",
    ))
    data["dependencies"] = base_dependencies + [{
        "namespace": "linux", "architecture": "aarch64",
        "soname": "libdep.so",
        "provider": "package" if mode != "duplicate" else "portmaster",
        **({"path": "fixture/lib/aarch64/libdep.so"}
           if mode != "duplicate" else {}),
    }]
    if mode == "two-package":
        data["files"].append(elf_entry(
            "libdep2.so", "fixture/lib/aarch64/libdep-copy.so",
            [], "libdep.so",
        ))
elif mode == "bad-glibc":
    data["dependencies"] = base_dependencies + [{
        "namespace": "linux", "architecture": "aarch64",
        "soname": "libdep.so", "provider": "glibc-base",
    }]
elif mode == "bad-name":
    data["dependencies"] = base_dependencies + [{
        "namespace": "linux", "architecture": "aarch64",
        "soname": "../libevil.so", "provider": "firmware",
    }]

if mode == "package":
    data["source_root"] = os.path.basename(source_root)
    generation_path = os.path.join(
        source_root, "fixture", "GENERATION.json"
    )
    with open(generation_path, encoding="utf-8") as handle:
        generation = json.load(handle)
    by_path = {item["path"]: item for item in generation["artifacts"]}
    for source, target in (
            ("consumer", "fixture/tests/consumer"),
            ("libdep.so", "fixture/lib/aarch64/libdep.so")):
        by_path[target] = {
            "path": target, "mode": "0755", "sha256": digest(source),
        }
    generation["artifacts"] = [by_path[path] for path in sorted(by_path)]
    with open(generation_path, "w", encoding="utf-8") as handle:
        json.dump(generation, handle, sort_keys=True, indent=2)
        handle.write("\n")
    next(item for item in data["files"]
         if item["target"] == "fixture/GENERATION.json")["sha256"] = digest(
             "fixture/GENERATION.json"
         )

with open(output, "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY

for dependency_mode in unresolved external package duplicate two-package bad-glibc bad-name; do
  dependency_source="$TEST_TMP/source"
  if [ "$dependency_mode" = package ]; then
    dependency_source="$TEST_TMP/source-dependency-package"
  fi
  python3 "$TEST_TMP/dependency-manifest.py" \
    "$TEST_TMP/manifest.json" "$TEST_TMP/dependency-$dependency_mode.json" \
    "$dependency_source" "$dependency_mode"
done
expect_fail dep-unresolved 'unresolved DT_NEEDED.*libdep[.]so' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-unresolved.json"
# A portmaster/firmware dependency that is NOT in the guaranteed CFW baseline
# (here the fixture soname libdep.so) must be REJECTED: leaving it unbundled is
# a status-127 "cannot open shared object" on the CFWs that lack it, exactly the
# libzip.so.5-on-ArkOS failure. The port must bundle it (provider=package).
expect_fail dep-external-not-baseline \
  'not in the guaranteed PortMaster runtime baseline' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-external.json"
python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-package.json" \
  | grep -q 'NXRELEASE VALIDATE: PASS' || fail 'explicit package provider failed'
python3 "$TOOL" stage --manifest "$TEST_TMP/dependency-package.json" \
  --candidate-lock "$CANDIDATE_LOCK" \
  --stage "$TEST_TMP/dependency-package-stage" \
  | grep -q 'NXRELEASE STAGE: PASS' || fail 'package-provider metadata round-trip failed'
expect_fail dep-duplicate 'duplicate providers' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-duplicate.json"
expect_fail dep-package-duplicate 'duplicate packaged ELF provider' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-two-package.json"
expect_fail dep-glibc-label 'cannot label.*glibc-base' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-bad-glibc.json"
expect_fail dep-portable-name 'portable ELF library basename' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-bad-name.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/not-glibc-base.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["dependencies"] = [
    {"namespace": "linux", "architecture": "aarch64",
     "soname": "libgcc_s.so.1", "provider": "glibc-base"},
    {"namespace": "linux", "architecture": "aarch64",
     "soname": "libstdc++.so.6", "provider": "glibc-base"},
]
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail dep-cxx-not-base 'libgcc_s[.]so[.]1.*glibc-base|cannot label' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/not-glibc-base.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source" \
  "$TEST_TMP/android-package.json" "$TEST_TMP/android-mislabelled.json" <<'PY'
import hashlib, json, os, sys
base, root, output, mislabelled = sys.argv[1:]
with open(base, encoding="utf-8") as handle:
    data = json.load(handle)
with open(os.path.join(root, "android-game.so"), "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
data["files"].append({
    "source": "android-game.so",
    "target": "fixture/android/libgame.so",
    "kind": "android-upstream",
    "mode": "0644",
    "architecture": "aarch64",
    "provenance": "offline original Android namespace fixture",
    "sha256": digest,
    "needed": ["liblog.so"],
    "soname": "libgame.so",
})
base_dependencies = list(data["dependencies"])
data["dependencies"] = base_dependencies + [{
    "namespace": "android", "architecture": "aarch64",
    "soname": "liblog.so", "provider": "nxloader-import-registry",
}]
with open(output, "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
data["files"][-1]["kind"] = "third-party-linux"
data["files"][-1]["build_profile"] = "universal-low-glibc"
data["dependencies"] = base_dependencies + [{
    "namespace": "linux", "architecture": "aarch64",
    "soname": "liblog.so", "provider": "firmware",
}]
with open(mislabelled, "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail android-byo 'kind is unsupported.*android-upstream' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/android-package.json"
expect_fail android-mislabelled 'Android/Bionic dependencies.*cannot be packaged as Linux' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/android-mislabelled.json"

# Declared PortMaster metadata is parsed and tied to the actual wrapper path.
printf '%s\n' \
  '<?xml version="1.0" encoding="utf-8"?>' \
  '<gameList><game><path>./Wrong.sh</path><name>Broken</name></game></gameList>' \
  >"$TEST_TMP/source/bad-gameinfo.xml"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/bad-gameinfo.json" "$TEST_TMP/source/bad-gameinfo.xml" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
data["portmaster_metadata"]["gameinfo_xml"]["sha256"] = digest
entry = next(item for item in data["files"] if item["target"].endswith("/gameinfo.xml"))
entry["source"] = "bad-gameinfo.xml"
entry["sha256"] = digest
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail gameinfo 'gameinfo.xml path does not match' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/bad-gameinfo.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source" \
  "$TEST_TMP/items-double.json" "$TEST_TMP/items-file-slash.json" <<'PY'
import hashlib, json, os, sys
base, root, double_manifest, file_manifest = sys.argv[1:]
with open(base, encoding="utf-8") as handle:
    original = json.load(handle)

def variant(items, source_name, output):
    data = json.loads(json.dumps(original))
    with open(os.path.join(root, "port.json"), encoding="utf-8") as handle:
        port_json = json.load(handle)
    port_json["items"] = items
    source_path = os.path.join(root, source_name)
    with open(source_path, "w", encoding="utf-8") as handle:
        json.dump(port_json, handle, sort_keys=True, indent=2)
        handle.write("\n")
    with open(source_path, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    data["portmaster_metadata"]["port_json"]["sha256"] = digest
    entry = next(item for item in data["files"] if item["target"].endswith("/port.json"))
    entry["source"] = source_name
    entry["sha256"] = digest
    with open(output, "w", encoding="utf-8") as handle:
        json.dump(data, handle, sort_keys=True, indent=2)

variant(["Game.sh", "fixture//"], "port-double.json", double_manifest)
variant(["Game.sh/", "fixture/"], "port-file-slash.json", file_manifest)
PY
expect_fail port-item-double 'more than one trailing' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/items-double.json"
expect_fail port-item-file 'wrong directory suffix' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/items-file-slash.json"

# Use a real host-linked executable with e_machine relabeled AArch64 so the
# test exercises version-info without needing a cross compiler.
if command -v cc >/dev/null 2>&1; then
  printf '%s\n' 'int main(void) { return 0; }' >"$TEST_TMP/host.c"
  cc "$TEST_TMP/host.c" -o "$TEST_TMP/source/high-glibc"
  python3 - "$TEST_TMP/source/high-glibc" <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as handle:
    handle.seek(18)
    handle.write(struct.pack("<H", 183))
PY
  python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/high.json" <<'PY'
import hashlib, json, re, subprocess, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
entry = next(item for item in data["files"] if item["target"].endswith("/loader"))
entry["source"] = "high-glibc"
with open(sys.argv[1].rsplit("/", 1)[0] + "/source/high-glibc", "rb") as handle:
    entry["sha256"] = hashlib.sha256(handle.read()).hexdigest()
dynamic = subprocess.check_output(
    ["readelf", "-dW", sys.argv[1].rsplit("/", 1)[0] + "/source/high-glibc"],
    text=True, stderr=subprocess.STDOUT,
)
entry["needed"] = sorted(set(re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic)))
sonames = sorted(set(re.findall(r"\(SONAME\).*?\[([^\]]+)\]", dynamic)))
entry["soname"] = sonames[0] if sonames else None
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
  if readelf --version-info --wide "$TEST_TMP/source/high-glibc" 2>/dev/null | grep -q 'GLIBC_'; then
    expect_fail glibc 'requires GLIBC_' \
      python3 "$TOOL" validate --manifest "$TEST_TMP/high.json" --max-glibc 0.1
  fi
fi

# PortMaster integration is a release contract, not launcher folklore.
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'source "${controlfolder}/control.txt"' \
  'get_controls' \
  '"${directory}/pm_platform_helper" "${GAMEDIR}/game"' \
  >"$TEST_TMP/source/Bad.sh"
chmod 0755 "$TEST_TMP/source/Bad.sh"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/bad-launcher.json" "$TEST_TMP/source/Bad.sh" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["package"]["launcher"] = "Bad.sh"
data["package"]["launcher_chain"][0] = "Bad.sh"
data["files"][0]["source"] = "Bad.sh"
data["files"][0]["target"] = "Bad.sh"
with open(sys.argv[3], "rb") as handle:
    data["files"][0]["sha256"] = hashlib.sha256(handle.read()).hexdigest()
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail portmaster 'canonical|nxbootstrap config|launcher_name' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/bad-launcher.json"

printf '%s\n' \
  '#!/usr/bin/env bash' \
  'case x in' \
  '  x) true & ;;' \
  'esac' \
  >"$TEST_TMP/source/background.sh"
chmod 0755 "$TEST_TMP/source/background.sh"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/background.json" \
  "$TEST_TMP/source/background.sh" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
data["files"].append({
    "source": "background.sh", "target": "fixture/background.sh",
    "kind": "script", "mode": "0755", "sha256": digest,
})
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail background-case 'backgrounds a child' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/background.json"

python3 - "$TEST_TMP/source/Game.sh" "$TEST_TMP/source/wrong-exec.sh" <<'PY'
import sys
text = open(sys.argv[1], encoding="utf-8").read()
assert "flock -n 9" in text
text = text.replace("flock -n 9", "true", 1)
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    handle.write(text)
PY
chmod 0755 "$TEST_TMP/source/wrong-exec.sh"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/wrong-exec.json" \
  "$TEST_TMP/source/wrong-exec.sh" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
entry = next(item for item in data["files"] if item["kind"] == "launcher")
entry["source"] = "wrong-exec.sh"
entry["sha256"] = digest
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail launcher-exec 'self-contained launcher is missing' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/wrong-exec.json"

python3 - "$TEST_TMP/source/Game.sh" \
  "$TEST_TMP/source/dead-portmaster.sh" <<'PY'
import sys
text = open(sys.argv[1], encoding="utf-8").read()
text = text.replace("control.txt", "control.dead")
text = text.replace("get_controls", "ignored_controls")
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    handle.write(text)
PY
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/dead-portmaster.json" \
  "$TEST_TMP/source/dead-portmaster.sh" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
entry = next(item for item in data["files"] if item["kind"] == "launcher")
entry["source"] = "dead-portmaster.sh"
entry["sha256"] = digest
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail launcher-dead-token 'self-contained launcher is missing|missing control' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dead-portmaster.json"

# Adversarial payload scans: the gate rejects proprietary/temp suffixes
# (.jar), saves/tmp directory parts, embedded credential literals, funding
# advocacy and FUNDING.* members; and it refuses a license pinned to a
# non-license file.
printf 'evil jar payload\n' > "$TEST_TMP/source/evil.jar"
printf 'save data\n' > "$TEST_TMP/source/save-secret"
printf 'password=supersecret123\n' > "$TEST_TMP/source/secret.txt"
printf 'please donate via patreon\n' > "$TEST_TMP/source/donate.txt"
printf 'github: patreon\n' > "$TEST_TMP/source/FUNDING.yml"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source" \
  "$TEST_TMP/forbidden-jar.json" "$TEST_TMP/forbidden-saves.json" \
  "$TEST_TMP/forbidden-secret.json" "$TEST_TMP/forbidden-advocacy.json" \
  "$TEST_TMP/forbidden-funding.json" "$TEST_TMP/license-bad.json" <<'PY'
import copy, hashlib, json, os, sys
base, source_root = sys.argv[1], sys.argv[2]
jar_out, saves_out, secret_out, advocacy_out, funding_out, license_out = sys.argv[3:9]
with open(base, encoding="utf-8") as handle:
    data = json.load(handle)

def digest(rel):
    with open(os.path.join(source_root, rel), "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()

def with_extra(source_rel, target):
    d = copy.deepcopy(data)
    d["files"].append({
        "source": source_rel, "target": target,
        "kind": "license-notice", "mode": "0644", "sha256": digest(source_rel),
    })
    return d

specs = {
    jar_out: with_extra("evil.jar", "fixture/evil.jar"),
    saves_out: with_extra("save-secret", "fixture/saves/save-secret"),
    secret_out: with_extra("secret.txt", "fixture/secret.txt"),
    advocacy_out: with_extra("donate.txt", "fixture/donate.txt"),
    funding_out: with_extra("FUNDING.yml", "fixture/FUNDING.yml"),
}
for path, payload in specs.items():
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, sort_keys=True, indent=2)
        handle.write("\n")

bad = copy.deepcopy(data)
bad["package"]["license"]["file"] = "fixture/nxextract/run-extractor.sh"
with open(license_out, "w", encoding="utf-8") as handle:
    json.dump(bad, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY

expect_fail forbidden-jar 'proprietary/temp/log suffix|forbidden release data' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-jar.json"
expect_fail forbidden-saves 'private/temp/cache data' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-saves.json"
expect_fail forbidden-secret 'credential/secret' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-secret.json"
expect_fail forbidden-advocacy 'funding/donations' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-advocacy.json"
expect_fail forbidden-funding 'forbidden release artifact|funding/donations' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-funding.json"
expect_fail license-bad 'license.file must match a license-notice' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/license-bad.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/license-missing.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["package"].pop("license")
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail license-missing 'package.license is required' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/license-missing.json"

printf '%s\n' 'hostname=private-build-node' > "$TEST_TMP/source/hostname.txt"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source/hostname.txt" \
  "$TEST_TMP/forbidden-hostname.json" <<'PY'
import hashlib, json, pathlib, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
source = pathlib.Path(sys.argv[2])
data["files"].append({
    "source": source.name,
    "target": "fixture/hostname.txt",
    "kind": "payload",
    "mode": "0644",
    "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
})
with open(sys.argv[3], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail forbidden-hostname 'hostname literal|private host information' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-hostname.json"

for android_suffix in apk obb dex; do
  python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source/payload/README.txt" \
    "$TEST_TMP/forbidden-$android_suffix.json" "$android_suffix" <<'PY'
import hashlib, json, pathlib, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
source = pathlib.Path(sys.argv[2])
suffix = sys.argv[4]
data["files"].append({
    "source": "payload/README.txt",
    "target": "fixture/data/game." + suffix,
    "kind": "payload",
    "mode": "0644",
    "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
})
with open(sys.argv[3], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
  expect_fail "forbidden-$android_suffix" 'proprietary/temp/log suffix|forbidden release data' \
    python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-$android_suffix.json"
done

# Re-opening the artifact catches a payload changed without refreshing its
# internal MANIFEST.sha256.
python3 - "$TEST_TMP/fixture-one.zip" "$TEST_TMP/tampered.zip" <<'PY'
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1], "r") as source:
    with zipfile.ZipFile(sys.argv[2], "w") as target:
        for info in source.infolist():
            data = source.read(info.filename)
            if info.filename == "fixture/assets/README.txt":
                data += b"tampered\n"
            target.writestr(info, data)
PY
expect_fail tamper 'MANIFEST.sha256 verification failed|hash mismatch' \
  python3 "$TOOL" verify --archive "$TEST_TMP/tampered.zip"

# Adversarial ZIP members must fail before extraction or metadata trust: path
# traversal, Unix symlink entries and NFC/case-fold collisions are independent
# attack classes.
python3 - "$TEST_TMP/fixture-one.zip" "$TEST_TMP" <<'PY'
import shutil
import stat
import sys
import zipfile

source, root = sys.argv[1:]

traversal = root + "/adversarial-traversal.zip"
shutil.copyfile(source, traversal)
with zipfile.ZipFile(traversal, "a") as archive:
    archive.writestr("../escape", b"escape")

symlink = root + "/adversarial-symlink.zip"
shutil.copyfile(source, symlink)
with zipfile.ZipFile(symlink, "a") as archive:
    info = zipfile.ZipInfo("fixture/hostile-link")
    info.create_system = 3
    info.external_attr = (stat.S_IFLNK | 0o777) << 16
    archive.writestr(info, "../../outside")

collision = root + "/adversarial-unicode.zip"
shutil.copyfile(source, collision)
with zipfile.ZipFile(collision, "a") as archive:
    for name in ("fixture/Caf\u00e9.txt", "fixture/Cafe\u0301.txt"):
        info = zipfile.ZipInfo(name)
        info.create_system = 3
        info.external_attr = (stat.S_IFREG | 0o644) << 16
        archive.writestr(info, b"collision")
PY
expect_fail zip-traversal 'ZIP member.*safe relative path|not a safe relative path' \
  python3 "$TOOL" verify --archive "$TEST_TMP/adversarial-traversal.zip"
expect_fail zip-symlink 'archive contains symlink' \
  python3 "$TOOL" verify --archive "$TEST_TMP/adversarial-symlink.zip"
expect_fail zip-unicode 'case-insensitive collision' \
  python3 "$TOOL" verify --archive "$TEST_TMP/adversarial-unicode.zip"

# A source changed after validation but before copy is caught by the staged
# hash. The barrier makes the race deterministic instead of timing-dependent.
python3 - "$TOOL" "$TEST_TMP/manifest.json" "$TEST_TMP/toctou-stage" \
  "$CANDIDATE_LOCK" <<'PY'
import importlib.util
import pathlib
import sys
import threading

tool, manifest, destination, candidate_lock = sys.argv[1:]
spec = importlib.util.spec_from_file_location("nxrelease_toctou", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
config = module.load_manifest(manifest, candidate_lock_path=candidate_lock)
payload = pathlib.Path(manifest).parent / "source" / "payload" / "README.txt"
original_payload = payload.read_bytes()
real_copy = module.shutil.copyfile
copy_reached = threading.Event()
continue_copy = threading.Event()
errors = []

def barrier_copy(source, target):
    if pathlib.Path(source) == payload:
        copy_reached.set()
        if not continue_copy.wait(10):
            raise RuntimeError("TOCTOU test barrier timed out")
    return real_copy(source, target)

def worker():
    try:
        module.stage_release(config, destination)
    except BaseException as error:
        errors.append(error)

module.shutil.copyfile = barrier_copy
thread = threading.Thread(target=worker)
thread.start()
if not copy_reached.wait(10):
    raise SystemExit("copy barrier was never reached")
payload.write_bytes(original_payload + b"raced\n")
continue_copy.set()
thread.join(10)
payload.write_bytes(original_payload)
if thread.is_alive():
    raise SystemExit("stage worker did not finish")
if len(errors) != 1 or not isinstance(errors[0], module.ReleaseError):
    raise SystemExit("TOCTOU mutation was not rejected: {!r}".format(errors))
if "changed" not in str(errors[0]) and "pin" not in str(errors[0]):
    raise SystemExit("unexpected TOCTOU error: {}".format(errors[0]))
if pathlib.Path(destination).exists():
    raise SystemExit("TOCTOU failure published a stage")
PY

# Pair publication installs checksum first, uses hard-link O_EXCL semantics and
# rolls back only its own inode if either final name loses a race.
python3 - "$TOOL" "$TEST_TMP" <<'PY'
import importlib.util
import pathlib
import sys

tool, root_value = sys.argv[1:]
root = pathlib.Path(root_value)
spec = importlib.util.spec_from_file_location("nxrelease_publish", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def run_race(label, collide_on):
    archive_temp = root / (label + "-archive.tmp")
    checksum_temp = root / (label + "-checksum.tmp")
    output = root / (label + ".zip")
    checksum_output = root / (label + ".zip.sha256")
    archive_temp.write_bytes(b"candidate archive")
    checksum_temp.write_bytes(b"candidate checksum")
    real_link = module.os.link
    competitor = b"concurrent owner"

    def racing_link(source, destination):
        destination_path = pathlib.Path(destination)
        if destination_path == collide_on(output, checksum_output):
            destination_path.write_bytes(competitor)
        return real_link(source, destination)

    module.os.link = racing_link
    try:
        try:
            module.publish_archive_pair(
                archive_temp, checksum_temp, output, checksum_output,
            )
        except module.ReleaseError:
            pass
        else:
            raise SystemExit(label + " race unexpectedly published")
    finally:
        module.os.link = real_link

    collision_path = collide_on(output, checksum_output)
    if collision_path.read_bytes() != competitor:
        raise SystemExit(label + " overwrote/deleted the concurrent destination")
    other = checksum_output if collision_path == output else output
    if other.exists():
        raise SystemExit(label + " left a partial release pair")

run_race("race-archive", lambda archive, checksum: archive)
run_race("race-checksum", lambda archive, checksum: checksum)

bundle_destination = root / "race-bundle"
real_rename = module.rename_noreplace

def racing_rename(source, destination):
    pathlib.Path(destination).mkdir()
    (pathlib.Path(destination) / "owner.txt").write_bytes(b"concurrent bundle")
    return real_rename(source, destination)

module.rename_noreplace = racing_rename
try:
    try:
        module.create_release_bundle(
            root / "stage-one", bundle_destination, "fixture.zip",
        )
    except module.ReleaseError:
        pass
    else:
        raise SystemExit("bundle destination race unexpectedly published")
finally:
    module.rename_noreplace = real_rename
if (bundle_destination / "owner.txt").read_bytes() != b"concurrent bundle":
    raise SystemExit("bundle race overwrote the concurrent directory")
if list(root.glob(".nxrelease-bundle-*")):
    raise SystemExit("bundle race leaked a hidden publication directory")
PY

before_hash=$(sha256sum "$TEST_TMP/fixture-one.zip")
expect_fail no-overwrite 'already exists' \
  python3 "$TOOL" build --manifest "$TEST_TMP/manifest.json" \
    --candidate-lock "$CANDIDATE_LOCK" \
    --stage "$TEST_TMP/unused-stage" --output "$TEST_TMP/fixture-one.zip"
after_hash=$(sha256sum "$TEST_TMP/fixture-one.zip")
[ "$before_hash" = "$after_hash" ] || fail 'existing publication was overwritten'
[ ! -e "$TEST_TMP/unused-stage" ] || fail 'preflight collision still created a stage'

if command -v stat >/dev/null 2>&1; then
  fail 'stat appeared in PATH during the release chain'
fi


# --- CFW runtime baseline gate (libzip.so.5 on ArkOS = status 127) ---------
python3 -B - "$TOOL" <<'PYEOF'
import importlib.util, sys
tool=sys.argv[1]
spec=importlib.util.spec_from_file_location("nxr_baseline", tool)
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
def elf(soname_needed):
    return {"namespace":"linux","architecture":"aarch64","path":"fixture/bin/loader",
            "soname":None,"needed":list(soname_needed)}
def dep(so,pv,path=None):
    d={"namespace":"linux","architecture":"aarch64","soname":so,"provider":pv}
    if path is not None: d["path"]=path
    return d
def run(needed, deps):
    cfg={"dependencies":deps}
    elfs=[elf(needed)]
    try:
        m.validate_dependency_closure(elfs, cfg); return None
    except SystemExit as e: return str(e)
    except Exception as e: return str(e)
fails=[]
# baseline portmaster/firmware NEEDed and declared -> PASS
if run(["libSDL2-2.0.so.0","libgcc_s.so.1","libz.so.1"],
       [dep("libSDL2-2.0.so.0","portmaster"),dep("libgcc_s.so.1","firmware"),
        dep("libz.so.1","firmware")]) is not None:
    fails.append("baseline set rejected")
# libzip.so.5 as portmaster, unbundled -> FAIL
if run(["libzip.so.5"],[dep("libzip.so.5","portmaster")]) is None:
    fails.append("libzip.so.5 portmaster unbundled NOT rejected")
# libzip.so.5 as firmware, unbundled -> FAIL
if run(["libzip.so.5"],[dep("libzip.so.5","firmware")]) is None:
    fails.append("libzip.so.5 firmware unbundled NOT rejected")
if fails:
    for f in fails: print("BASELINE GATE FAIL:", f)
    sys.exit(1)
print("nxrelease cfw-baseline gate: PASS")
PYEOF

printf '%s\n' 'nxrelease archive strict JSON regressions passed: cases=3'
# --- V3 GAMEDATA-DIR-01: negative boundaries for the owner-data marker ---
python3 -B - "$TEST_TMP/manifest.json" "$TEST_TMP" <<'PY'
import copy
import json
import pathlib
import sys

manifest_raw, tmp_raw = sys.argv[1:]
tmp = pathlib.Path(tmp_raw)
base = json.loads(pathlib.Path(manifest_raw).read_text(encoding="utf-8"))


def find(doc):
    for item in doc["files"]:
        if item["target"] == "fixture/gamedata/README.txt":
            return item
    raise AssertionError("marker entry missing from fixture manifest")


missing = copy.deepcopy(base)
missing["files"] = [item for item in missing["files"]
                    if item["target"] != "fixture/gamedata/README.txt"]
(tmp / "gamedata-missing.json").write_text(
    json.dumps(missing, sort_keys=True, indent=2), encoding="utf-8")

wrong_kind = copy.deepcopy(base)
find(wrong_kind)["kind"] = "license-notice"
(tmp / "gamedata-wrong-kind.json").write_text(
    json.dumps(wrong_kind, sort_keys=True, indent=2), encoding="utf-8")

wrong_mode = copy.deepcopy(base)
find(wrong_mode)["mode"] = "0755"
(tmp / "gamedata-wrong-mode.json").write_text(
    json.dumps(wrong_mode, sort_keys=True, indent=2), encoding="utf-8")

stale = copy.deepcopy(base)
find(stale)["sha256"] = "0" * 64
(tmp / "gamedata-stale-hash.json").write_text(
    json.dumps(stale, sort_keys=True, indent=2), encoding="utf-8")

nested = copy.deepcopy(base)
entry = find(nested)
entry["target"] = "fixture/gamedata/extra/README.txt"
(tmp / "gamedata-extra-level.json").write_text(
    json.dumps(nested, sort_keys=True, indent=2), encoding="utf-8")
print("gamedata negative manifests written")
PY
expect_fail gamedata-missing 'gamedata/README.txt' \
  python3 -B "$TOOL" validate --manifest "$TEST_TMP/gamedata-missing.json"
expect_fail gamedata-wrong-kind 'kind payload' \
  python3 -B "$TOOL" validate --manifest "$TEST_TMP/gamedata-wrong-kind.json"
expect_fail gamedata-wrong-mode 'mode 0644' \
  python3 -B "$TOOL" validate --manifest "$TEST_TMP/gamedata-wrong-mode.json"
expect_fail gamedata-stale-hash 'sha256|hash|match' \
  python3 -B "$TOOL" validate --manifest "$TEST_TMP/gamedata-stale-hash.json"
expect_fail gamedata-extra-level 'gamedata/README.txt' \
  python3 -B "$TOOL" validate --manifest "$TEST_TMP/gamedata-extra-level.json"
printf '%s\n' 'nxrelease gamedata gate passed: negatives=5'

python3 -B "$ROOT/tests/test_public_final.py"

printf '%s\n' 'nxrelease tests: PASS no_external_stat=1'
