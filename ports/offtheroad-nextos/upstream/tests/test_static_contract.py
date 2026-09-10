#!/usr/bin/env python3
"""Fail-closed static checks for the OTR promotion seams."""

import hashlib
import json
import pathlib
import re
import sys


PORT = pathlib.Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise SystemExit("test_static_contract: " + message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


project = json.loads((PORT / "nxproject.json").read_text(encoding="utf-8"))
adapter = json.loads(
    (PORT / "adapter/adapter-contract.json").read_text(encoding="utf-8")
)
static = json.loads(
    (PORT / "references/graphics-contract-static-v1.json").read_text(
        encoding="utf-8"
    )
)
egl_contract = json.loads(
    (PORT / "references/egl-import-contract-v1.json").read_text(
        encoding="utf-8"
    )
)
recipe = json.loads((PORT / "extractor.json").read_text(encoding="utf-8"))
main = (PORT / "src/main.c").read_text(encoding="utf-8")
imports = (PORT / "src/imports.c").read_text(encoding="utf-8")
egl_imports = (PORT / "src/otr_egl_imports.c").read_text(encoding="utf-8")
so_util = (PORT / "src/so_util.c").read_text(encoding="utf-8")
undefined_symbols = (PORT / "src/und_symbols.txt").read_text(
    encoding="utf-8"
)
gptk = (PORT / "src/otr_gptk.c").read_text(encoding="utf-8")
audio = (PORT / "src/opensles_shim.c").read_text(encoding="utf-8")
audio_recovery = (PORT / "src/otr_audio_recovery.c").read_text(
    encoding="utf-8"
)
exit_monitor = (PORT / "src/otr_exit_monitor.c").read_text(encoding="utf-8")
util = (PORT / "src/util.c").read_text(encoding="utf-8")
runtime_evidence = (PORT / "src/otr_runtime_evidence.c").read_text(
    encoding="utf-8"
)
build = (PORT / "build_universal.sh").read_text(encoding="utf-8")
framework_pin = json.loads(
    (PORT / "FRAMEWORK-PIN.json").read_text(encoding="utf-8")
)
build_inputs = json.loads(
    (PORT / "tools/BUILD-INPUTS.json").read_text(encoding="utf-8")
)
release = json.loads((PORT / "nxrelease.json").read_text(encoding="utf-8"))
version = (PORT / "version.txt").read_text(encoding="utf-8").strip()

require(project["schema_version"] == 3, "nxproject is not schema v3")
require(project["documentation"]["status"] == "authored" and
        project["adapter"]["skeleton"] == "contract-only",
        "schema-v3 authored documentation opt-in is incomplete")
claims = project["promotion"]["claims"]
require(claims == {
    "adapter_lifecycle_implemented": True,
    "physical_support_proven": False,
    "release_ready": True,
}, "promotion claims are dishonest or incomplete")
require(adapter["status"] == "implemented_release" and
        adapter["release_ready"] is True,
        "adapter remains a scaffold")
require(adapter["input"]["actions"] == project["controls"]["actions"] and
        adapter["input"]["contexts"] == project["controls"]["contexts"],
        "project and adapter input contracts diverge")
adapter_graphics = dict(adapter["graphics"])
adapter_graphics.pop("adapter", None)
require(adapter_graphics == project["graphics"],
        "project and adapter graphics contracts diverge")
require(project["nxport"]["executable"] == "offtheroad-nextos",
        "public executable lacks the canonical name")
require(project["runtime_root"] == "." and
        project["nxport"]["schema_version"] == 3,
        "generation-v2 schema-3 opt-in is incomplete")
expected_runtime = [
    ("executable", "offtheroad-nextos", "0755"),
    ("nxextract-recipe", "extractor.json", "0644"),
    ("nxextract-engine", "nxextract/nxextract.py", "0644"),
    ("nxextract-runner", "nxextract/run-extractor.sh", "0644"),
    ("nxextract-runtime-env", "nxextract/nxextract-runtime-env.sh", "0644"),
    ("nxextract-ui", "nxextract/nxextract-ui", "0755"),
]
generation_runtime = project["nxport"]["generation_runtime"]
require(all(set(member) == {"role", "path", "mode", "sha256"}
            for member in generation_runtime),
        "generation runtime member shape is not exact")
require([(member["role"], member["path"], member["mode"])
         for member in generation_runtime] == expected_runtime,
        "generation runtime closure/order is not exact")
for member in generation_runtime:
    source = PORT / member["path"]
    require(source.is_file() and not source.is_symlink(),
            "generation runtime source is missing or linked: " +
            member["path"])
    require("%04o" % (source.stat().st_mode & 0o777) == member["mode"],
            "generation runtime mode diverged: " + member["path"])
    require(sha256(source) == member["sha256"],
            "generation runtime SHA-256 diverged: " + member["path"])
require("nxsplash-nextos" in project["nxport"]["required_files"] and
        all(member["role"] != "nxsplash" for member in generation_runtime),
        "canonical NXSplash is not delegated to nxbootstrap append")
require(project["controls"]["tuning"] == {
    "camera": {"authority": "native"},
    "cursor": {
        "acceleration": 0.35,
        "deadzone": 0.15,
        "response_curve": 1.35,
        "smoothing_ms": 45,
        "speed": 1.25,
    },
}, "authored cursor/camera tuning is not exact")
expected_package_payload = [
    ("FRAMEWORK-PIN.json", "payload", "0644"),
    ("INSTALLATION.md", "payload", "0644"),
    ("NOTICE.md", "license-notice", "0644"),
    ("README.md", "payload", "0644"),
    ("alsoft.conf", "payload", "0644"),
    ("licenses/NXExtract-MIT.txt", "license-notice", "0644"),
    ("nxextract-version.txt", "payload", "0644"),
    ("tools/BUILD-INPUTS.json", "payload", "0644"),
    ("version.txt", "payload", "0644"),
]
package_payload = project["package_payload"]
require(all(set(member) == {"path", "kind", "mode", "sha256"}
            for member in package_payload),
        "package payload member shape is not exact")
require([(member["path"], member["kind"], member["mode"])
         for member in package_payload] == expected_package_payload,
        "package payload closure/order is not exact")
for member in package_payload:
    source = PORT / member["path"]
    require(source.is_file() and not source.is_symlink(),
            "package payload source is missing or linked: " + member["path"])
    require("%04o" % (source.stat().st_mode & 0o777) == member["mode"],
            "package payload mode diverged: " + member["path"])
    require(sha256(source) == member["sha256"],
            "package payload SHA-256 diverged: " + member["path"])

roles = {
    member["member"]: member.get("role")
    for member in recipe["compatibility"]["required_members"]
}
require(roles.get("assets/AVConfig.json") == "optional",
        "AVConfig.json regressed to a required payload")
extract_by_id = {item["id"]: item for item in recipe["extract"]}
game_validation = extract_by_id["engine-libgame-arm64"]["validate"]
cxx_validation = extract_by_id["android-libcpp-shared-arm64"]["validate"]
require(recipe["version"] == "1.18.2-arm64-4" and
        game_validation.get("size") == 9501104 and
        game_validation.get("sha256") ==
        "5258d644cbb023b137719901fa84acf8a8ae3da01c573b52a7a31255245628dd" and
        cxx_validation.get("size") == 1292904 and
        cxx_validation.get("sha256") ==
        "4397241b4bd20a8e579bfb41d21107857e12985f6a01ca0c2a5f83380d1270b4",
        "critical internal runtime payload identity is not fail-closed")
require("egl-import-contract-v1" in
        recipe["compatibility"]["required_symbols_or_interfaces"][0],
        "recipe no longer binds compatibility to the measured EGL ABI")
require(project["nxport"]["nxextract"]["version"] == "1.2.21" and
        release["nxextract"]["version"] == "1.2.21",
        "NXExtract 1.2.21 is not pinned consistently")
require(version == "1.0.4" and release["package"]["version"] == version,
        "port version bump is not explicit and consistent")

contract_call = main.index("otr_graphics_contract_start(cwd)")
require(contract_call < main.index("init_cursor_renderer();") and
        contract_call < main.index("load_module(CXX_SO") and
        contract_call < main.index("e_nativeInit(g_env"),
        "graphics barrier no longer precedes shaders/engine")
native_render = main.index("e_nativeRender(g_env, NULL);")
pre_present = main.index("otr_graphics_pre_present", native_render)
cursor = main.index("draw_cursor();", pre_present)
swap = main.index("SDL_GL_SwapWindow", cursor)
require(native_render < pre_present < cursor < swap,
        "native RGB is not proved before the cursor/present")
require("g_cursor_mode" not in main and "L3 liga/desliga" not in main,
        "legacy L3 cursor toggle remains compiled")
require("_ZN10cSingletonI6cMultiE10mSingletonE" in main and
        "_ZN6cMulti14getLocalPlayerEv" in main,
        "runtime context is not derived from cMulti symbols")
require("nxinput_gptk_load_at" in gptk and
        "nxinput_gptk_load_receipt_json" in gptk and
        "nxinput_gptk_dispatcher_feed_source" in gptk and
        "nxinput_gptk_dispatcher_set_primary_mask" in gptk,
        "nxinput 0.5.1 owner/source-guard route is incomplete")
require("void offtheroad_input_digital_sink" in main and
        "void offtheroad_input_vector_sink" in main,
        "port-owned GPTK sinks are absent")
require("exit_monitor_start_runtime()" in main and
        "nx_evdev_chord_set_primary_active(authoritative)" in main and
        "if (!authoritative) sample->fallback_fired = nx_evdev_chord_poll()" in main and
        "SDL_JoystickGetButton(joystick, back.value.button)" in main and
        "SDL_JoystickGetButton(joystick, start.value.button)" in main and
        "nx_exit_chord_log_controller(controller, 0)" in main and
        "otr_exit_monitor_start" in exit_monitor and
        "independent_guest_loop=1" in exit_monitor,
        "independent SDL/evdev terminal watcher is incomplete")
pump = main[main.index("static void pump_input(float dt)"):
            main.index("/* ------------------------------------------------------------------ main --")]
require("nxinput_exit_chord" not in pump and
        "nx_evdev_chord_poll" not in pump and
        'trigger_exit("SELECT+START' not in pump,
        "SELECT+START still depends on pump_input")
require("void offtheroad_audio_device_callback" in audio and
        "nxaudio_liveness_tick" in audio and
        "otr_audio_recovery_run_callback_stalled" in audio and
        "otr_audio_recovery_note_callback" in audio and
        "nxaudio_receipt_format" in audio and
        "recovery_line" in audio and
        "NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED" in audio_recovery and
        "adapter_outcome=%s" in audio_recovery and
        "otr_audio_recovery_mark_callback_timeout" in audio and
        "NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE" not in audio_recovery,
        "nxaudio receipt/liveness/bounded recovery route is incomplete")
require("shader-cache/%s/%s" in util and "unlink(" not in util,
        "shader cache is not isolated or owner data may be deleted")
require("nxgl_graphics_contract_evidence_receipt" in runtime_evidence and
        'fprintf(stdout, "%s\\n", line)' in runtime_evidence,
        "canonical key=value graphics receipt is not emitted")
require(static["contract"] == {
    "api": "gles",
    "profile": "es",
    "shader_dialect": "essl100",
    "version": "2.0",
    "version_policy": "minimum",
}, "static game evidence and declared GLES contract diverge")

expected_egl_imports = {
    "eglChooseConfig", "eglCreateContext", "eglCreateWindowSurface",
    "eglDestroyContext", "eglDestroySurface", "eglGetConfigAttrib",
    "eglGetCurrentContext", "eglGetDisplay", "eglGetProcAddress",
    "eglInitialize", "eglMakeCurrent", "eglQueryString", "eglSwapBuffers",
    "eglSwapInterval", "eglTerminate",
}
require(egl_contract["schema"] == "offtheroad-egl-import-contract-v1" and
        set(egl_contract["guest_imports"]) == expected_egl_imports and
        set(egl_contract["provider_imports"]) ==
        expected_egl_imports - {"eglGetProcAddress"},
        "measured guest EGL import contract is incomplete")
provider_names = set(re.findall(r'^\s*"(egl[A-Za-z0-9_]+)",?$',
                                egl_imports, re.MULTILINE))
require(provider_names == expected_egl_imports - {"eglGetProcAddress"} and
        '{"eglGetProcAddress", (uintptr_t)&my_eglGetProcAddress}' in imports,
        "runtime EGL table does not cover the exact measured guest imports")
documented_egl_imports = {
    line.split("@", 1)[0] for line in undefined_symbols.splitlines()
    if line.startswith("egl")
}
require(documented_egl_imports == expected_egl_imports,
        "undefined-symbol inventory omitted measured EGL imports")
require(main.index("if (prepare_guest_egl_imports() != 0)") <
        main.index("load_module(CXX_SO") and
        "otr_egl_provider_open_and_promote" in main and
        "RTLD_NOW | RTLD_LOCAL" in egl_imports and
        "RTLD_NOW | RTLD_GLOBAL" in egl_imports and
        egl_imports.index("RTLD_NOW | RTLD_LOCAL") <
        egl_imports.index("RTLD_NOW | RTLD_GLOBAL") and
        '"libEGL.so.1", "libEGL.so"' in main and
        "SDL_GL_GetProcAddress(\"eglGetCurrentContext\")" in main and
        "dladdr(sdl_current_context_symbol" in main and
        "g_egl_imports.functions" in main and
        "current_context=1" in main,
        "EGL provider proof/table no longer precedes guest loading")
require('fatal_error("so_resolve(%s) deixou imports sem resolver"' in main and
        "return unresolved == 0 ? 0 : -unresolved;" in so_util,
        "guest import resolution is no longer fail-closed")

require({name: value["version"] for name, value in
         framework_pin["components"].items()} == {
             "nxaudio": "0.3.1", "nxgl": "0.2.17", "nxinput": "0.5.1"
         } and
        {value["commit"] for value in
         framework_pin["components"].values()} == {
             "5ab765d95d76d12fb425f5667f79d71c055118f2"
         } and
        {name: value["tree_sha256"] for name, value in
         framework_pin["components"].items()} == {
             "nxaudio": "5d67454d2254b614bd2a876fe26898865d771acf5155d87817bfc802270f6cf3",
             "nxgl": "50b002bdca0e6370f190589998129e159c8f4e516b25d31c5040292c43818c23",
             "nxinput": "05890441ce05769a1b33101081b271c421f6dae27b1403d5039560cc2d68ec72",
         }, "framework RC3 pin is not exact")
require(build_inputs["schema"] == "nextos-port-build-inputs-v1" and
        build_inputs["framework_pin_sha256"] ==
        "90eedfc90f92b64d14b98586daba8a2405a5c41413255fa89ccd8e6f06cbca90" and
        build_inputs["headers"]["nextos_sysroot"]["source_identity"] ==
        "build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4" and
        build_inputs["headers"]["nextos_sysroot"]["tree_sha256"] ==
        "256edb96dc7cfc2ba40b3efb68079cf5b0dda7c7069da6d85e5add5ae84a9851",
        "builder/sysroot inputs are not pinned to measured identities")
require('"$PIN_TOOL" materialize' in build and
        '"$PIN_TOOL" verify' in build and
        '"$FRAMEWORK_SOURCE":/framework:ro' in build and
        '"$REPOSITORY_ROOT/framework":/framework:ro' not in build and
        "candidate_digest" in build and
        "EXPECTED_NEXTOS_IDENTITY" in build,
        "build does not materialize and verify its immutable inputs")
release_sources = {entry["source"]: entry for entry in release["files"]}
for member in package_payload:
    record = release_sources.get(member["path"])
    require(record is not None and
            record["kind"] == member["kind"] and
            record["mode"] == member["mode"] and
            record["sha256"] == member["sha256"],
            "release manifest differs from package payload: " +
            member["path"])
require(release_sources["FRAMEWORK-PIN.json"]["sha256"] ==
        build_inputs["framework_pin_sha256"] and
        release_sources["tools/BUILD-INPUTS.json"]["sha256"] ==
        sha256(PORT / "tools/BUILD-INPUTS.json") and
        release_sources["offtheroad-nextos"]["sha256"] ==
        generation_runtime[0]["sha256"] and
        "FRAMEWORK-PIN.json" in
        release_sources["offtheroad-nextos"]["provenance"] and
        "tools/BUILD-INPUTS.json" in
        release_sources["offtheroad-nextos"]["provenance"],
        "release manifest/provenance omitted immutable build inputs")
require("OUTPUT=${OT_UNIVERSAL_OUTPUT:-offtheroad-nextos}" in build and
        "offtheroad-nextos|*/offtheroad-nextos" in build and
        release_sources["offtheroad-nextos"]["target"] ==
        "offtheroad/offtheroad-nextos",
        "build/release flow accepts a noncanonical executable name")

print("test_static_contract: PASS")
