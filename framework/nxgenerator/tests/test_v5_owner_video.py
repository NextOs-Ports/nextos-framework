#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V5 7A (nxgenerator 0.4.0): `video` (NEXTOS_SETTINGS/2) and `owner_runtime`
project blocks. Absence is a byte-identical no-op; presence is explicit,
total and refused when it cannot be honoured (never faked)."""
import importlib.util, json, os, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nxgenerator_v5", ROOT / "nxgenerator.py")
tool = importlib.util.module_from_spec(spec); spec.loader.exec_module(tool)
fails = 0
def check(c, m):
    global fails
    print(("ok   " if c else "FAIL ") + m)
    if not c: fails += 1
def refuses(fn, needle, m):
    try: fn()
    except tool.ProjectError as e:
        check(needle in str(e), "%s (%s)" % (m, e)); return
    check(False, m + " (accepted)")

check(tool.validate_video(None, 3) is None, "absent video block = no-op (V4 /1 seed unchanged)")
refuses(lambda: tool.validate_video({"authority": "nextos", "aspect_policies": ["preserve"], "invalid_policy": "fail_closed"}, 2), "schema_version 3", "video needs schema 3")
v = tool.validate_video({"authority": "nextos", "aspect_policies": ["auto", "preserve", "stretch"], "auto_algorithm": "ratio-threshold", "invalid_policy": "package_default"}, 3)
check(v["aspect"] == "auto" and v["output_size"] == "display" and v["filter"] == "engine" and v["native_config"] is None, "defaults: aspect = first declared policy, output_size display, filter engine")
v_stretch = tool.validate_video({"authority": "nextos", "aspect": "auto", "aspect_policies": ["auto", "preserve", "stretch"], "auto_algorithm": "stretch", "invalid_policy": "package_default"}, 3)
check(v_stretch["auto_algorithm"] == "stretch", "auto-stretch is an explicit total algorithm; preserve remains an owner policy")
refuses(lambda: tool.validate_video({"authority": "nextos", "aspect_policies": ["auto", "preserve"], "invalid_policy": "fail_closed"}, 3), "no total auto_algorithm", "MUTANT killed: auto without a declared total algorithm refused")
refuses(lambda: tool.validate_video({"authority": "nextos", "aspect_policies": ["auto", "preserve"], "auto_algorithm": "device-square", "invalid_policy": "fail_closed"}, 3), "auto_algorithm", "device/model heuristic is not an auto algorithm")
refuses(lambda: tool.validate_video({"authority": "nextos", "aspect_policies": ["preserve"], "auto_algorithm": "epsilon", "invalid_policy": "fail_closed"}, 3), "without auto", "auto_algorithm without auto refused")
refuses(lambda: tool.validate_video({"authority": "nextos", "aspect": "crop", "aspect_policies": ["preserve"], "invalid_policy": "fail_closed"}, 3), "not among the declared", "MUTANT killed: default aspect outside the implemented policies refused (no fake fallback)")
refuses(lambda: tool.validate_video({"authority": "engine", "aspect_policies": ["engine"], "invalid_policy": "fail_closed"}, 3), "native_config", "authority engine requires the owner-native config path")
refuses(lambda: tool.validate_video({"authority": "synchronized", "aspect_policies": ["preserve"], "invalid_policy": "fail_closed", "native_config": "/etc/x"}, 3), "relative path", "native_config must be relative")
refuses(lambda: tool.validate_video({"authority": "nextos", "aspect_policies": ["preserve"], "invalid_policy": "fail_closed", "output_size": "9000x100"}, 3), "8192", "output_size bound")
refuses(lambda: tool.validate_video({"authority": "nextos", "aspect_policies": ["preserve"], "invalid_policy": "ignore"}, 3), "invalid_policy", "invalid_policy enum")
refuses(lambda: tool.validate_video({"authority": "nextos", "aspect_policies": ["preserve"], "invalid_policy": "fail_closed", "cfw": "x"}, 3), "unknown field", "unknown field refused (no device/CFW selectors)")
v = tool.validate_video({"authority": "engine", "aspect_policies": ["engine", "preserve", "stretch", "auto"], "auto_algorithm": "ratio-threshold", "aspect": "auto", "invalid_policy": "fail_closed", "native_config": "game/override.cfg", "output_size": "800x600", "filter": "nearest"}, 3)
check(v["native_config"] == "game/override.cfg" and v["output_size"] == "800x600", "engine authority with native config and WxH accepted")

# the /2 seed renders every video key explicitly and parses under the schema-2 grammar
seed = tool.render_template("NEXTOSSETTINGS-2.txt.in", {"TITLE": "T", "VIDEO_AUTHORITY": v["authority"], "VIDEO_OUTPUT_SIZE": v["output_size"], "VIDEO_ASPECT": v["aspect"], "VIDEO_ASPECT_POLICIES": "|".join(v["aspect_policies"]), "VIDEO_FILTER": v["filter"], "VIDEO_INVALID_POLICY": v["invalid_policy"]}).decode()
lines = [l for l in seed.splitlines() if l and not l.startswith("#")]
check(seed.startswith("# NEXTOS_SETTINGS/2\n") and set(l.split("=")[0] for l in lines) == {"language", "video.authority", "video.output_size", "video.aspect", "video.filter", "video.invalid_policy"}, "seed is NEXTOS_SETTINGS/2 with every video key explicit (never hidden)")
check("video.aspect=auto" in lines and "video.output_size=800x600" in lines, "seed carries the declared defaults")
check(all(len(l.split("=")[1]) <= 32 and all(ch.isalnum() or ch in "._-" for ch in l.split("=")[1]) for l in lines), "seed values obey the settings grammar")
seed1 = tool.render_template("NEXTOSSETTINGS.txt.in", {"TITLE": "T"}).decode()
check(seed1.startswith("# NEXTOS_SETTINGS/1\n") and "video." not in seed1, "without video the /1 seed is unchanged")

# owner_runtime: both sides must agree; seed must be a regular file
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp); (src / "port-env.seed.sh").write_text("export X=1\n")
    nx0 = {"owner_runtime": None}; nx1 = {"owner_runtime": "1"}
    check(tool.validate_owner_runtime(None, nx0, 3, src) is None, "absent owner_runtime = V4 behaviour")
    refuses(lambda: tool.validate_owner_runtime(None, nx1, 3, src), "hook_seed", "nxport opt-in without the project seed refused")
    refuses(lambda: tool.validate_owner_runtime({"hook_seed": "port-env.seed.sh"}, nx0, 3, src), "nxport.owner_runtime", "project seed without the nxport opt-in refused")
    r = tool.validate_owner_runtime({"hook_seed": "port-env.seed.sh"}, nx1, 3, src)
    check(r["hook_seed"].name == "port-env.seed.sh", "owner_runtime accepted with a regular seed")
    os.symlink(src / "port-env.seed.sh", src / "link.sh")
    refuses(lambda: tool.validate_owner_runtime({"hook_seed": "link.sh"}, nx1, 3, src), "symlink", "symlinked seed refused")
    refuses(lambda: tool.validate_owner_runtime({"hook_seed": "port-env.seed.sh", "x": 1}, nx1, 3, src), "owner_runtime", "unknown owner_runtime field refused")
# --- NEXTOS_CONTROLLERS/4 (schema 4) rendering: single source, byte-pinned corpus
fp2 = json.loads((ROOT.parents[1] / "ports" / "fp2" / "nxproject.json").read_text(encoding="utf-8"))["controls"]
c4 = dict(fp2); c4["schema"] = 4; c4.pop("face_layout", None)
rendered = tool.render_template("NEXTOSCONTROLLERS.gptk.in", {"PORT_ID": "fp2", "GPTK_SCHEMA": "4", "GPTK_GUIDANCE": tool.GPTK_LIVE_GUIDANCE, "CONTROL_SECTIONS": tool.render_controls_sections(c4).rstrip()})
corpus = ROOT.parents[1] / "framework" / "nxinput" / "tests" / "v5" / "corpus" / "fp2-generated-v4.gptk"
check(corpus.read_bytes() == rendered, "schema-4 default for FP2 is byte-identical to the nxinput corpus (one generator source, C parser pinned on the other side)")
check("[base]\nA = action:fp2.attack" in rendered.decode() and "[override.menu]\nA = action:fp2.confirm\nB = action:fp2.cancel" in rendered.decode() and "GUIDE = native" in rendered.decode(), "unified base from gameplay; sparse menu override; GUIDE explicit")
check("[cursor]" not in rendered.decode() and "FACE_LAYOUT" not in rendered.decode(), "no V3 [cursor]/FACE_LAYOUT in a schema-4 owner")
def v4(controls):
    return tool.validate_controls(controls, 3)
bad = dict(c4); bad["face_layout"] = "auto"
refuses(lambda: v4(bad), "positional", "schema 4 refuses face_layout (positional Xbox)")
bad = dict(c4); bad.pop("runtime_mapping")
refuses(lambda: v4(bad), "runtime_mapping", "schema 4 requires the live runtime")
ok4 = v4(c4)
check(ok4["schema"] == 4 and "face_layout" not in ok4, "schema 4 validated")
print("nxgenerator-v5-owner-video: %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
