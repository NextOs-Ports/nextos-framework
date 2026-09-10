#!/usr/bin/env python3
"""Small source-level regression checks for Tearscape's device seam."""

from pathlib import Path
import hashlib
import re
import subprocess


PORT = Path(__file__).resolve().parent.parent
ENV = (PORT / "adapter-env.sh").read_text(encoding="utf-8")
CPP = (PORT / "src/godot_engine/v4-universal/platform/linuxbsd/fbdev/display_server_fbdev.cpp").read_text(encoding="utf-8")
HEADER = (PORT / "src/godot_engine/v4-universal/platform/linuxbsd/fbdev/display_server_fbdev.h").read_text(encoding="utf-8")
PATCH = (PORT / "src/godot_engine/godot-4.6.1-nextos.patch").read_text(encoding="utf-8")
GODOT_FRAME_PROOF = (PORT / "src/godot_engine/v4-universal/platform/linuxbsd/fbdev/nxgl_godot_frame_proof.h").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"TEARSCAPE RUNTIME CONTRACT: FAIL: {message}")


subprocess.run(["bash", "-n", str(PORT / "adapter-env.sh")], check=True)
subprocess.run(["bash", "-n", str(PORT / "build_low_glibc.sh")], check=True)
subprocess.run(["bash", str(PORT / "tests/video/run-host.sh")], check=True)
subprocess.run([
    "python3", "-B", str(PORT / "tests/release/test_one_shot_ledger.py"),
], check=True)
subprocess.run([
    "python3", "-B", str(PORT / "tests/release/test_engine_build_receipt.py"),
], check=True)
subprocess.run([
    "python3", "-B", str(PORT / "tests/release/test_geometry_proof.py"),
], check=True)
require(not re.search(r"(^|[;&|()]|\s)stat(?:\s|$)", ENV), "port-env invokes external stat")
for token in (
    '[ -c "$NX_TEARSCAPE_DRM_NODE" ]',
    '[ -r "$NX_TEARSCAPE_DRM_NODE" ]',
    '[ -w "$NX_TEARSCAPE_DRM_NODE" ]',
    'libEGL.so',
    'libGLESv2.so',
    'NX_TEARSCAPE_NATIVE_GLES3=0',
    'NX_TEARSCAPE_SDL_EGL=1',
    'SDL_VIDEO_EGL_DRIVER',
    'SDL_VIDEO_GL_DRIVER',
):
    require(token in ENV, f"graphics gate lacks {token}")
require('SConscript("fbdev/SCsub")' not in PATCH and
        all(f'"fbdev/{name}"' in PATCH for name in (
            "display_server_fbdev.cpp", "egl_manager_fbdev.cpp",
            "nx_sdl2_geometry.cpp",
            "nxgl_frame_proof_adapter.c", "nx_symver.c")),
        "pinned SCons path can return a None fbdev source list")

# --- WAYLAND_GEOMETRY_PROOF (0.2.16): with the SDL2 provider the window
# geometry is SDL's (display bounds -> FULLSCREEN_DESKTOP -> first
# authoritative configure -> live drawable per frame), never /dev/fb0's. The
# fbdev/EGL provider keeps the raw panel geometry. ---
GEOMETRY_H = (PORT / "src/godot_engine/v4-universal/platform/linuxbsd/fbdev/nx_sdl2_geometry.h").read_text(encoding="utf-8")
GEOMETRY_CPP = (PORT / "src/godot_engine/v4-universal/platform/linuxbsd/fbdev/nx_sdl2_geometry.cpp").read_text(encoding="utf-8")
ADAPTER_H = (PORT / "src/godot_engine/v4-universal/platform/linuxbsd/fbdev/nxgl_frame_proof_adapter.h").read_text(encoding="utf-8")
VIDEO_RUNNER = (PORT / "tests/video/run-host.sh").read_text(encoding="utf-8")
ENGINE_BUILD = (PORT / "build_low_glibc.sh").read_text(encoding="utf-8")
for name in ("nx_sdl2_geometry.cpp", "nx_sdl2_geometry.h"):
    require(f"platform/linuxbsd/fbdev/{name}" in ENGINE_BUILD,
            f"engine build does not install {name}")
    require(f"+++ b/platform/linuxbsd/fbdev/{name}" in PATCH,
            f"engine patch lacks {name}")
require('#include "nx_sdl2_geometry.h"' in CPP, "display server does not use the geometry authority")
require("nx_sdl2_geometry_prepare_app_id(&sdl_geometry);" in CPP and
        CPP.index("nx_sdl2_geometry_prepare_app_id(&sdl_geometry);") <
        CPP.index("sdl_api.InitSubSystem(NX_SDL_INIT_VIDEO) == 0"),
        "app id is not prepared before the SDL video subsystem starts")
require('"SDL_APP_ID"' in GEOMETRY_CPP and
        'setenv("SDL_VIDEO_WAYLAND_WMCLASS", g->app_id, 0)' in GEOMETRY_CPP and
        '"/proc/self/exe"' in GEOMETRY_CPP,
        "app id is not derived env -> wmclass -> executable basename")
sdl2_init = CPP.index("Error DisplayServerFBDev::_initialize_sdl2()")
sdl2_end = CPP.index("DisplayServerFBDev::DisplayServerFBDev(", sdl2_init)
sdl2_body = CPP[sdl2_init:sdl2_end]
require("nx_sdl2_geometry_initial_request(&sdl_geometry, &request_w, &request_h);" in sdl2_body and
        "fb_size.x > 0 ? fb_size.x" not in sdl2_body,
        "SDL2 window request still derives from the raw framebuffer")
require(sdl2_body.index("nx_sdl2_geometry_initial_request(") <
        sdl2_body.index("sdl_api.CreateWindow(") <
        sdl2_body.index("nx_sdl2_geometry_attach_window(&sdl_geometry, sdl_window") <
        sdl2_body.index("fb_size = Size2i(sdl_geometry.logical_w, sdl_geometry.logical_h);"),
        "SDL2 geometry order is not request -> window -> configure wait -> logical size")
require("NX_SDL_WINDOW_OPENGL | NX_SDL_WINDOW_FULLSCREEN_DESKTOP" in sdl2_body,
        "SDL2 window is not requested FULLSCREEN_DESKTOP")
for token in (
    "SDL_GetDisplayBounds", "SDL_GetDisplayUsableBounds", "SDL_GetCurrentDisplayMode",
    "SDL_GetWindowDisplayIndex", "SDL_PumpEvents", "SDL_PeepEvents",
    "SDL_GetWindowFlags", "SDL_SetWindowFullscreen", "SDL_SetHint",
    "SDL_GL_GetDrawableSize", "SDL_GetWindowSize",
):
    require(f'"{token}"' in GEOMETRY_CPP, f"geometry authority does not dlsym {token}")
require("NX_SDL_WINDOWEVENT_SIZE_CHANGED" in GEOMETRY_CPP and
        "NX_SDL_WINDOWEVENT_RESIZED" in GEOMETRY_CPP,
        "compositor resize events are not observed")
require("g->timed_out = true;" in GEOMETRY_CPP and
        "configure wait TIMED OUT" in GEOMETRY_CPP and
        '\\"timed_out\\":%s' in GEOMETRY_CPP,
        "configure timeout is not logged and recorded")
require('"nx-geometry-proof/1"' in GEOMETRY_H and
        '"NXGEOMETRY_RECEIPT"' in CPP and
        'NXGEOMETRY_RECEIPT="$GAMEDIR/nxgeometry-receipt.jsonl"' in ENV and
        ': > "$NXGEOMETRY_RECEIPT"' in ENV and
        "NXGEOMETRY_RECEIPT" in ENV.split("export NXC6_SEAM NXC6_RECEIPT", 1)[1].splitlines()[0],
        "geometry receipt is not exported and truncated by port-env")
# 0.2.17: the GPTK evidence receipt (nxinput-gptk-event-evidence/1 JSON lines)
# travels exactly like the geometry receipt: symlink-refused, truncated per
# launch, exported on the same line, read by the glue from NXGPTK_RECEIPT.
require('NXGPTK_RECEIPT="$GAMEDIR/nxgptk-receipt.jsonl"' in ENV and
        'if [ -L "$NXGPTK_RECEIPT" ]; then' in ENV and
        ': > "$NXGPTK_RECEIPT"' in ENV and
        "NXGPTK_RECEIPT" in ENV.split("export NXC6_SEAM NXC6_RECEIPT", 1)[1].splitlines()[0],
        "GPTK evidence receipt is not exported and truncated by port-env")
require("nx_sdl2_geometry_write_fbdev_init(path, fb_size.x, fb_size.y)" in CPP,
        "fbdev provider does not write its non-regression geometry receipt")
require("void window_set_rect_changed_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override;" in HEADER and
        "Callable rect_changed_callback;" in HEADER and
        "Size2i raw_fb_size;" in HEADER,
        "display server lacks rect-changed callback storage or raw fb separation")
poll_body = CPP.split("void DisplayServerFBDev::_poll_sdl2_geometry()", 1)[1].split(
        "void DisplayServerFBDev::_write_geometry_receipt_init()", 1)[0]
require("nx_sdl2_geometry_poll(&sdl_geometry)" in poll_body and
        "fb_size = Size2i(sdl_geometry.logical_w, sdl_geometry.logical_h);" in poll_body and
        "nxgl_godot_frame_proof_resize(&frame_proof, fb_size.x, fb_size.y)" in poll_body and
        "rect_changed_callback.call(Rect2i(Point2i(), fb_size));" in poll_body and
        "nx_sdl2_geometry_write_resize(&sdl_geometry, notified);" in poll_body and
        "WINDOW_EVENT_DPI_CHANGE" not in poll_body.replace("WINDOW_EVENT_DPI_CHANGE is intentionally not raised", ""),
        "per-frame resize does not update size, frame proof, engine rect and receipt")
geometry_refresh_body = CPP.split("void DisplayServerFBDev::refresh_controls_before_events()", 1)[1].split(
        "void DisplayServerFBDev::_poll_sdl2_geometry()", 1)[0]
require(geometry_refresh_body.index("_poll_sdl2_geometry();") < geometry_refresh_body.index("nxgptk_godot_tick(0.0);"),
        "geometry is not refreshed before the per-frame input tick")
require("void nxgl_frame_proof_set_video_size(int width, int height);" in ADAPTER_H and
        "nxgl_godot_frame_proof_resize(" in GODOT_FRAME_PROOF and
        "nxgl_frame_proof_set_video_size(width, height);" in GODOT_FRAME_PROOF,
        "frame proof has no live resize entry point")
require('"$TEST_DIR/run-geometry-host.sh"' in VIDEO_RUNNER,
        "video host runner does not run the geometry gate")
for name in ("fake_sdl2.c", "test_sdl2_geometry_host.cpp", "run-geometry-host.sh"):
    require((PORT / "tests/video" / name).is_file(), f"geometry gate lacks {name}")
require("def read_geometry_proof(" in (PORT / "recipes/make_geometry_proof.py").read_text(encoding="utf-8"),
        "geometry gate consumer is missing")
# The fbdev/EGL provider path is byte-for-byte the raw panel: the constructor
# still reads /dev/fb0 and _initialize_fbdev consumes fb_size unchanged.
fbdev_init = CPP.split("Error DisplayServerFBDev::_initialize_fbdev()", 1)[1].split(
        "Error DisplayServerFBDev::_initialize_sdl2()", 1)[0]
require("fbdev_window.width = (uint16_t)fb_size.x;" in fbdev_init and
        "egl_manager->window_create(MAIN_WINDOW_ID, &nx_native_display, &nx_native_window, fb_size.x, fb_size.y)" in fbdev_init and
        "nx_sdl2_geometry" not in fbdev_init,
        "fbdev provider geometry changed")
require('open("/dev/fb0", O_RDWR | O_CLOEXEC)' in CPP and "fb_size = raw_fb_size;" in CPP,
        "raw framebuffer geometry no longer seeds the fbdev provider")

# The universal floor is a physical GLES2 context. The DRM path must keep the
# translation shims as the EGL/GLES pair and drive the firmware SDL2 through
# the shim facade; promoting the blob's native GLES3 to the main path proved
# black-screen-with-audio on DRM Mali devices (30/08/2026) while the context,
# page flip and shader log all looked healthy.
require('NX_TEARSCAPE_NATIVE_GLES3=1' not in ENV,
        "the forbidden native-GLES3 main path is back in port-env")
require(ENV.count('NX_TEARSCAPE_EGL_LIBRARY=') == 1 and
        ENV.count('NX_TEARSCAPE_GLES2_LIBRARY=') == 1,
        "port-env reassigns the atomic shim EGL/GLES pair")
SHIM_BUILD = (PORT / "src/shim/build_shim.sh").read_text(encoding="utf-8")
SHIM_EGL = (PORT / "src/shim/nx_egl.c").read_text(encoding="utf-8")
SHIM_GLES = (PORT / "src/shim/nx_gles3_core.c").read_text(encoding="utf-8")
SHIM_PROOF_STATE = (PORT / "src/shim/nx_gles3_frame_proof_state.h").read_text(encoding="utf-8")
# The GLES facade string must not contradict the live provider.
require('"OpenGL ES 3.0 (NextOS nxgles3 facade)"' in SHIM_GLES and
        "nxgles3 on Mali-450" not in SHIM_GLES,
        "GL_VERSION facade string names a physical GPU")
require('nx_egl_sdl.c' in SHIM_BUILD,
        "the release shim does not compile the SDL2/KMSDRM ES2 facade")
require('nx_sdl_egl_enabled' in SHIM_EGL,
        "the shim EGL entry points do not route through the SDL2 facade")
require('#include "nx_gles3_frame_proof_state.h"' in SHIM_GLES and
        "nx_gles3_frame_proof_state_query(" in SHIM_GLES and
        SHIM_GLES.index("nx_gles3_frame_proof_state_query(") <
        SHIM_GLES.index("p_glGetIntegerv(pname, data)"),
        "frame proof can still delegate facade-only state to physical GLES2")
for token in (
    "GL_FRAMEBUFFER_BINDING", "GL_READ_FRAMEBUFFER_BINDING",
    "GL_PIXEL_PACK_BUFFER_BINDING", "GL_PACK_ROW_LENGTH",
    "GL_PACK_SKIP_ROWS", "GL_PACK_SKIP_PIXELS",
):
    require(token in SHIM_PROOF_STATE,
            f"Mali-450 logical frame-proof state lacks {token}")
require("GL_PACK_SKIP_ROWS != 0x0D03" in SHIM_PROOF_STATE and
        "GL_PACK_SKIP_PIXELS != 0x0D04" in SHIM_PROOF_STATE,
        "pack skip constants are not pinned to the GLES specification")

require("bool health_published = false;" in HEADER, "health receipt lacks one-shot state")
require("std::atomic<bool> frame_presented{ false };" in HEADER,
        "health receipt lacks thread-safe presentation state")
require("NX_TEARSCAPE_NATIVE_GLES3" in CPP and
        "NX_SDL_GL_CONTEXT_MAJOR_VERSION, gles_major" in CPP,
        "SDL provider does not distinguish native GLES3 from shim GLES2")
for token in (
    "NXBOOTSTRAP_HEALTH_FILE",
    "NXBOOTSTRAP_HEALTH_SCHEMA",
    "NXBOOTSTRAP_HEALTH_SCHEMA_VERSION",
    "NXBOOTSTRAP_HEALTH_RUN_ID",
    "NXBOOTSTRAP_HEALTH_GENERATION",
    "NXBOOTSTRAP_HEALTH_PORT_ID",
    "O_EXCL | O_CLOEXEC",
    "fchmod(fd, 0600)",
    "rename(temporary, path)",
):
    require(token in CPP, f"health receipt lacks {token}")

swap = CPP.index("void DisplayServerFBDev::swap_buffers()")
publish = CPP.index("void DisplayServerFBDev::_publish_health_receipt()")
swap_body = CPP[swap:publish]
require("GL_SwapWindow(sdl_window);" in swap_body and
        "egl_manager->swap_buffers();" in swap_body and
        swap_body.count("frame_presented.store(true, std::memory_order_release);") == 2,
        "the providers do not record a real presentation")
refresh_at = PATCH.index("refresh_controls_before_events")
poll_at = PATCH.index("joypad_sdl->process_events();", refresh_at)
publish_at = PATCH.index("publish_health_after_iteration", poll_at)
iteration_at = PATCH.index("if (Main::iteration())", publish_at)
require(refresh_at < poll_at < publish_at < iteration_at,
		"main loop order is not context-refresh -> input-poll -> health -> iteration")
refresh_body = CPP.split("void DisplayServerFBDev::refresh_controls_before_events()", 1)[1].split(
		"void DisplayServerFBDev::publish_health_after_iteration()", 1)[0]
publish_body = CPP.split("void DisplayServerFBDev::publish_health_after_iteration()", 1)[1].split(
		"void DisplayServerFBDev::_publish_health_receipt()", 1)[0]
require("nxgptk_godot_tick(0.0);" in refresh_body and
		"nxgptk_godot_tick" not in publish_body,
		"scene context is not refreshed exactly once before the SDL event batch")
require("void DisplayServerFBDev::publish_health_after_iteration()" in CPP and
        "frame_presented.load(std::memory_order_acquire)" in CPP and
        "_publish_health_receipt();" in CPP,
        "health publication is not gated by a presented frame")
require("uint light_count = uint(nx_vnine.w);" in PATCH and
        "int light_count = int(nx_vnine.w);" not in PATCH,
        "the canvas light count mixes signed and unsigned shader operands")

# The frame-proof adapter is compiled as C. The canonical Godot wrapper keeps
# C linkage, fixes launch/context/present/stop order and never creates a
# synthetic present. The engine-major caller still owns the live resolver.
require('#include "nxgl_godot_frame_proof.h"' in HEADER and
		'extern "C" {' in GODOT_FRAME_PROOF and
		'#include "nxgl_frame_proof_adapter.h"' in GODOT_FRAME_PROOF,
        "Godot frame-proof wrapper does not retain C linkage")
require('"nxgl-godot-frame-proof/2"' in GODOT_FRAME_PROOF,
		"Godot frame-proof wrapper marker is missing")
require("nxgl_godot_frame_proof_marker()" in CPP and
		'frame-proof runtime=%s' in CPP,
		"Godot frame-proof wrapper identity is not retained in the final engine")
require("void *nxgl_godot_gl_resolve(const char *p_name)" in CPP and
        "eglGetProcAddress(p_name)" in CPP,
        "frame proof does not resolve GL through the live EGL provider")
provider_ok = CPP.index("if (error == OK)", CPP.index("for (int i = 0; i < order_count; i++)"))
context_set = CPP.index("nxgl_godot_frame_proof_context(", provider_ok)
require(provider_ok < context_set and
		"&frame_proof, nxgl_godot_gl_resolve" in CPP[context_set:context_set + 240],
		"live provider is not handed to the canonical frame-proof context")
resolver_set = GODOT_FRAME_PROOF.index("nxgl_frame_proof_set_resolver(resolver);")
context_metadata = GODOT_FRAME_PROOF.index("nxgl_frame_proof_set_video_context(", resolver_set)
require(resolver_set < context_metadata,
		"canonical wrapper does not install resolver before video context")
proof_call = CPP.index("nxgl_godot_frame_proof_before_swap(")
proof_refusal = CPP.index("if (proof_status != 0)", proof_call)
sdl_present = CPP.index("sdl_api.GL_SwapWindow", proof_refusal)
egl_present = CPP.index("egl_manager->swap_buffers", proof_refusal)
require(proof_call < proof_refusal < sdl_present and proof_refusal < egl_present and
		CPP.count("nxgl_godot_frame_proof_before_swap(") == 1 and
		"nxgl_godot_frame_proof_consume_close(&frame_proof)" in CPP[proof_refusal:sdl_present] and
		"nxgl_godot_frame_proof_exit_status(&frame_proof)" in CPP[proof_refusal:sdl_present] and
		"return;" in CPP[proof_refusal:sdl_present],
		"nonzero frame proof does not block both real presents and close nonzero")
require("nxgl_godot_frame_proof_health_allowed(&frame_proof)" in publish_body,
		"conclusive frame failure can still publish generation health")
constructor = CPP.index("DisplayServerFBDev::DisplayServerFBDev(")
destructor = CPP.index("DisplayServerFBDev::~DisplayServerFBDev()")
require(CPP.index("nxgl_godot_frame_proof_begin(&frame_proof)", constructor) <
		CPP.index("r_error = OK;", constructor),
		"frame-proof launch receipt is not the first constructor action")
require(CPP.index("nxgl_godot_frame_proof_stop(&frame_proof)", destructor) <
		CPP.index("memdelete(egl_manager)", destructor),
		"frame proof is not published before context destruction")

# --- TEARSCAPE-CONTROLS-LIVE: NEXTOSCONTROLLERS.gptk is a RUNTIME, not
# documentation. The engine glue must load the owner/default file through the
# canonical nxinput loader, decide before the native path, keep the sovereign
# chord out-of-band, and its embedded allowlist must equal the declarative
# adapter-contract actions exactly. ---
import json

GLUE = (PORT / "src/godot_engine/v4-universal/drivers/sdl/nxinput_gptk_godot.cpp").read_text(encoding="utf-8")
JOYPAD = (PORT / "src/godot_engine/v4-universal/drivers/sdl/joypad_sdl.cpp").read_text(encoding="utf-8")
SCSUB = (PORT / "src/godot_engine/v4-universal/drivers/sdl/SCsub").read_text(encoding="utf-8")
NXC6 = (PORT / "src/nxinput/nxc6_glue.c").read_text(encoding="utf-8")
PM_SOURCE = (PORT / "src/nxinput/nxinput_portmaster.c").read_text(encoding="utf-8")
PM_HEADER = (PORT / "src/nxinput/nxinput_portmaster.h").read_text(encoding="utf-8")
BUILD = (PORT / "build_low_glibc.sh").read_text(encoding="utf-8")
BUILDER = (PORT / "recipes/low_glibc_container.sh").read_text(encoding="utf-8")
PACKAGER = (PORT / "recipes/build_public_byo.py").read_text(encoding="utf-8")
REFRESH = (PORT / "recipes/refresh_release_inputs.py").read_text(encoding="utf-8")
PROOF = (PORT / "recipes/make_input_proof.py").read_text(encoding="utf-8")
POLICY = (PORT / "src/godot_engine/v4-universal/drivers/sdl/tearscape_gptk_context_policy.h").read_text(encoding="utf-8")
INPUT_POLICY = (PORT / "src/godot_engine/v4-universal/drivers/sdl/tearscape_gptk_input_policy.h").read_text(encoding="utf-8")
LIVE_HEADER = (PORT / "src/nxinput/nxinput_gptk_live.h").read_text(encoding="utf-8")
GODOT_RUNTIME = (PORT / "src/nxinput/nxinput_godot_runtime.h").read_text(encoding="utf-8")
CONTROLLER_PROFILES = (PORT / "controllers.nxb").read_text(encoding="utf-8")
CONTROLLER_EVIDENCE = json.loads(
	(PORT / "evidence/controller-profile-source.json").read_text(encoding="utf-8"))
NXPROJECT = json.loads((PORT / "nxproject.json").read_text(encoding="utf-8"))
ADAPTER = json.loads((PORT / "adapter/adapter-contract.json").read_text(encoding="utf-8"))

for token in (
		"require_clean_port", "git -C \"$REPOSITORY\" archive",
		"pwd.getpwuid", "nextos-engine-attempts", "causal_key",
		"one-shot engine build already consumed for these causal inputs",
		"renameat2", "reserved engine output changed during the one-shot build",
		"require_released_ref()",
		"engine output publication allowlist or directory modes differ",
		"NXINPUT_FRAMEWORK_COMMIT=ba1da0e7b0482c4dbad76b7e36afe04de57fa5c3",
		"NXINPUT_FRAMEWORK_TAG=nxinput-v0.9.0",
		"NXINPUT_FRAMEWORK_TAG_OBJECT=32ed6553e16bc4195baed988ea9a9d3849fd86b1",
		"NXINPUT_FRAMEWORK_TAG_OBJECT_SHA256=2accbb023971fa47c4e73d23ee2e46472572b857f94911cabca3d9cf814c95b2",
		"cat-file -t \"$NXINPUT_TAG_REF\"",
		"component-tag-pinned", "candidate-commit-pinned",
		"NXINPUT_0_9_PIN_PENDING",
		"vendored nxinput differs from released tag",
		"committed nxinput vendor file set differs from the release map"):
	require(token in BUILD, f"one-shot engine build gate lacks {token}")

declared = sorted(action["id"] for action in NXPROJECT["controls"]["actions"])
glue_actions = sorted(re.findall(r'^\t"([a-z]+\.[a-z_.]+)",$', GLUE, re.M))
require(glue_actions == declared,
        "the glue allowlist differs from the adapter-contract actions: "
        f"{sorted(set(glue_actions) ^ set(declared))}")

expected_live_contract = {
	"schema": "nxinput-gptk-live/1",
	"context_initial": "unproven",
	"unproven_policy": "native-passthrough",
	"sink_coverage": "all-actions-before-activation",
	"delivery_ack": "required",
}
require(NXPROJECT.get("promotion", {}).get("adapter_contract") ==
		"adapter/adapter-contract.json",
		"nxproject does not promote the port-owned adapter contract")
require(ADAPTER.get("status") == "implemented_release" and
		ADAPTER.get("release_ready") is True,
		"promoted adapter is not release-ready")
adapter_evidence = ADAPTER.get("lifecycle", {}).get("source_evidence", [])
require("ports/tearscape/src/nxinput/nxc6_glue.c" in adapter_evidence and
		"ports/tearscape/src/nxinput/nxinput_portmaster.c" in adapter_evidence,
		"promoted adapter does not cite both runtime normalizer implementers")
require(ADAPTER.get("input_sdl3_portmaster") == {
		"enabled": False, "private_sdl3_sha256": ""} and
		any("joystick subset statically" in quirk and
				"no private libSDL shared object" in quirk
				for quirk in ADAPTER.get("quirks", [])),
		"adapter does not distinguish embedded Godot SDL3 from private SDL in the ZIP")
adapter_input = ADAPTER.get("input", {})
require(adapter_input.get("actions") == NXPROJECT["controls"]["actions"] and
		adapter_input.get("contexts") == NXPROJECT["controls"]["contexts"],
		"promoted adapter actions/contexts differ from nxproject")
require(adapter_input.get("runtime_mapping") == "nxinput-gptk" and
			adapter_input.get("runtime_contract") == expected_live_contract,
				"promoted adapter relaxes the canonical live GPTK contract")
controller_profiles = NXPROJECT["controls"]["controller_profiles"]
require(ADAPTER.get("input_controller_profiles") == controller_profiles and
		controller_profiles.get("enabled") is True and
		controller_profiles.get("bundle") == "controllers.nxb" and
		controller_profiles.get("sha256") == hashlib.sha256(
			(PORT / "controllers.nxb").read_bytes()).hexdigest(),
		"promoted adapter controller-profile bundle/hash differs from nxproject")
bindings = [
	(context, control, action)
	for context, mapping in NXPROJECT["controls"]["contexts"].items()
	for control, action in mapping.items()
	if action not in ("native", "null")
]
require(len(declared) == 16 and len(bindings) == 15,
		"the action/binding closure is not the audited 16-action/15-binding "
		"contract (player.heal added in 0.2.15 for the Y=heal flask)")
require('(\"player.zoom_map\", \"button\", \"engine.input.zoom_map\")' in REFRESH and
		'(\"player.zoom_map\", \"vector\", \"engine.input.zoom_map\")' not in REFRESH,
		"refresh recipe would restore analog zoom semantics")
require('system.start_coop' not in REFRESH,
		"refresh recipe would restore synthetic device-sensitive co-op input")
require('\"RIGHT_STICK\": \"player.zoom_map\"' not in REFRESH,
		"refresh recipe would restore the repeating right-stick default")
require('\"adapter_contract\": \"adapter/adapter-contract.json\"' in REFRESH,
		"refresh recipe would drop adapter promotion")
PREPARE_PIPELINE = PACKAGER.split("def prepare_candidate", 1)[1].split(
	"def bundle_candidate", 1)[0]
BUNDLE_PIPELINE = PACKAGER.split("def bundle_candidate", 1)[1].split(
	"def parse_arguments", 1)[0]
for token in (
	'"adapter/adapter-contract.json": 0o644',
	'".gptk"',
	'"controllers.nxb"',
	'f"{PORT_ID}/adapter/adapter-contract.json"',
	'"prepare", help="generate and preserve one scaffold plus manifest"',
	'"bundle", help="bundle the already prepared scaffold exactly once"',
	'"--prepared-root"',
	'"PREPARED.json"',
	'RENAME_NOREPLACE',
):
	require(token in PACKAGER, f"persistent one-tree pipeline lacks {token}")
require("--candidate-lock-output" not in PACKAGER and
		"--compose-check" not in PACKAGER and
		"make_input_proof.py" not in PACKAGER,
		"packager still creates its own proof/lock or exposes the monolithic path")
require('logical.name in ("LICENSE", "controllers.nxb")' in PACKAGER and
		'"controllers-modern.nxb"' not in PACKAGER and
		'".nxb", ".py"' not in PACKAGER,
		"public audit does not distinguish the single text bundle from binary runtime .nxb")
require(PREPARE_PIPELINE.count("str(generator)") == 1 and
		PREPARE_PIPELINE.count("str(renderer)") == 1,
		"prepare does not generate/render exactly one persistent scaffold")
for forbidden in (
	"stage_source_root(", "str(generator)", "str(renderer)",
	"test_runtime_contract.py", "refresh_release_inputs.py", "run.sh",
):
	require(forbidden not in BUNDLE_PIPELINE,
		f"bundle repeats preparation/proof work: {forbidden}")
require(BUNDLE_PIPELINE.count('str(release_tool), "bundle"') == 1,
		"bundle does not retain exactly one nxrelease bundle call")
require('RELEASE_FRAMEWORK_COMMIT = "275a8fb47c2f2bf9a452734dec5c33ceb4bd549f"' in PACKAGER and
		'NXRELEASE_COMPONENT_VERSION = "0.4.9"' in PACKAGER and
		'"--release-framework-root"' in PACKAGER and
		"verify_release_tool(release_framework)" in BUNDLE_PIPELINE,
		"bundle does not separately pin the release-only nxrelease composition")
require('"--authority", "human"' in BUNDLE_PIPELINE and
		'"--candidate-lock"' not in BUNDLE_PIPELINE and
		"claim_bundle_attempt(" not in BUNDLE_PIPELINE and
		"frozen_candidate_lock(" not in BUNDLE_PIPELINE,
		"bundle did not adopt the V5 human authority path")
require("SOURCE_TWO=" not in BUILD and
		"TEARSCAPE_FINAL_OUT must name a new, explicit output directory" in BUILD and
		'[ ! -e "$FINAL_OUT" ] && [ ! -L "$FINAL_OUT" ]' in BUILD and
		"os.mkdir(final_out, 0o500)" in BUILD and
		'"final_out_reservation"' in BUILD and
		"one-shot engine build already consumed for these causal inputs" in BUILD and
		"os.chmod(root, 0o755)" in BUILD and
		'mkdir -p "$FINAL_OUT' not in BUILD,
		"engine build can repeat or replace a prior final output")
for token in (
	'SNAPSHOT_ROOT="$WORK_DIR/repository-snapshot"',
	'BUILD_PORT_DIR="$SNAPSHOT_ROOT/${PORT_PREFIX%/}"',
	'git -C "$REPOSITORY" archive --format=tar "$PORT_HEAD"',
	'"$BUILD_PORT_DIR:/port:ro"',
	'"$BUILD_INPUT_MANIFEST:/build-causal-inputs.json:ro"',
	'BUILD-RECEIPT.json',
	'org.nextos.tearscape.engine-build-receipt',
):
	require(token in BUILD or token in BUILDER,
		f"single engine build lacks causal receipt boundary {token}")
require("def verify_engine_build_receipt(" in REFRESH and
		"verify_engine_build_receipt(engine_dir)" in REFRESH and
		'"engine_build_receipt_sha256": build_receipt_sha256' in REFRESH,
		"release refresh does not reject old/mixed engine bytes")
require("tests/controls/run.sh" not in PACKAGER and
		'["bash", str(harness), str(mapping)]' in PROOF,
		"exact generated GPTK harness is not confined to the external proof step")
for token in (
	'port / "defaults/NEXTOSCONTROLLERS.gptk"',
	'port / "adapter/adapter-contract.json"',
	'port / "GENERATION.json"',
	'"--prepared-root"',
	'"--video-proof"',
	'payload != producer_bytes',
	'"video_proof": video_proof',
	'"video_proof_receipt_sha256": video_proof_sha256',
	'stat.S_IMODE(metadata_before.st_mode) != 0o600',
	'is_within(video_path, repository)',
	'stat.S_IMODE(parent_metadata.st_mode) != 0o700',
	'GODOT_RUNTIME_MARKER = "nxinput-godot-runtime/1"',
	'FRAME_PROOF_MARKER = "nxgl-godot-frame-proof/2"',
	'"nxgl_frame_proof_consume_fatal"',
	'"nxgl_frame_proof_is_fatal"',
	'"nxinput_gptk_live_clear_context_checked"',
	'"nxinput_gptk_live_is_fatal"',
	'parent_metadata.st_uid != os.geteuid()',
	'metadata.st_nlink != 1',
	'os.fchmod(descriptor, 0o444)',
	'def parse_harness_evidence(',
	'context_proofs, cases, safety = parse_harness_evidence(result.stdout)',
	'if context_proofs != expected_context_proofs:',
	'if cases != expected_cases:',
):
	require(token in PROOF, f"input proof lacks exact/frozen boundary {token}")

for token in (
	# V5 (nxinput 0.11.8): the glue no longer opens the directories, calls the
	# loader and formats the receipt itself -- those sixty lines were copied in
	# four ports and are now nxinput_gptk4_preinit_load(). What the glue must
	# still prove is that it goes through that UNIVERSAL boundary.
	"nxinput_gptk4_preinit_load",
	"nxgptk_godot_preinit",
	"nxinput_gptk_live_init",
	"nxinput_gptk_live_register",
	"nxinput_gptk_live_register_vector",
	"nxinput_gptk_live_seal",
	"nxinput_gptk_live_set_context",
	"nxinput_gptk_live_clear_context",
	"nxinput_gptk_live_clear_context_checked",
	"nxinput_gptk_live_is_fatal",
	"nxinput_gptk_live_should_consume",
	"nxinput_gptk_live_feed",
	"nxinput_gptk_live_feed_vector",
	"NXINPUT_GPTK4_RUNTIME_MARKER",
	"nxinput_gptk_event_evidence_schema",
	"NXCOMPAT_GAME_DIR",
	"InputMap::get_singleton",
	"has_action",
	"context_proven",
	"decision=PASSTHROUGH suppressed=false delivery=0",
	"decision=ACTION context=",
	"tearscape_gptk_inputmap_sink",
	"tearscape_gptk_inputmap_vector_sink",
	"tearscape_gptk_resolve_context",
	"nx/gptk: InputMap sink contract proven",
):
    require(token in GLUE, f"controls runtime glue lacks {token}")
require('#include "scene/main/node.h"' in GLUE and
		GLUE.index('#include "scene/main/node.h"') <
		GLUE.index('#include "scene/main/scene_tree.h"'),
		"FBDEV context resolver uses an incomplete Godot Node type")
require('"nxinput-gptk-runtime/3"' in LIVE_HEADER,
		"canonical live runtime marker is not version 3 (V3-capable "
		"runtimes must never reuse /2; sealed oracle case N28)")
require('"nxinput-gptk-event-evidence/1"' in LIVE_HEADER,
		"canonical input evidence schema is missing")
for token in (
	'"nxinput-godot-runtime/1"',
	"nxinput_godot_action_preview",
	"nxinput_godot_split_vector",
	"nxinput_godot_vector_alias_update",
	"nxinput_godot_handoff_snapshot",
	"nxinput_godot_handoff_partition",
	"nxinput_godot_lifecycle_fail",
	"nxinput_godot_lifecycle_consume_close",
	"nxinput_godot_lifecycle_health_allowed",
):
	require(token in GODOT_RUNTIME, f"canonical Godot runtime lacks {token}")
require('install -m 0644 "$BUILD_PORT_DIR"/src/nxinput/*.[ch]' in BUILD,
		"build recipe does not install the canonical Godot runtime header")
require('"controller_profiles"' in REFRESH and '"controllers.nxb"' in REFRESH and
		'"controllers.nxb": 0o644' in PACKAGER,
		"authority-3 controller bundle is not generated and staged canonically")
for token in (
		"INVARIANT BASE",
		"source_file_sha256=4238023705e73ea94b468975d1d15f71cc8d63f8880b3cd205aebbe2a75f2a0f",
		"source_line_number=1110",
		"source_line_sha256=da0447deece5800c7c845652cb1a73893e706cbdb08601e33a2b74c5da3daa64",
		"no universal fallback is claimed"):
	require(token in CONTROLLER_PROFILES,
			"invariant base bundle lacks bounded, content-addressed provenance")
controller_lines = [line for line in CONTROLLER_PROFILES.splitlines()
		if line and not line.startswith("#") and
		not line.startswith("NXCONTROLLER_PROFILES/")]
require(len(controller_lines) == 1 and
		controller_lines[0].startswith(
			"190000004b4800000011000000010000,GO-Super Gamepad,"),
		"the invariant base must retain ONLY the GO-Super line: the mutable "
		"muOS modern/retro line is a user preference and never freezes here")
require("19000000010000000100000000010000," not in CONTROLLER_PROFILES,
		"the mutable muOS GUID re-entered the invariant base bundle")
require(CONTROLLER_EVIDENCE.get("schema") ==
		"org.nextos.controller-profile-source" and
		CONTROLLER_EVIDENCE.get("scope", {}).get(
			"candidate_admission_required") is True and
		CONTROLLER_EVIDENCE.get("scope", {}).get(
			"universal_fallback_claimed") is False,
		"controller evidence lost the measured-source provenance")
require("nxgptk_godot_gamepad_button(joy_id, sdl_event.gbutton.button" in JOYPAD,
		"button governor lost the physical pad identity")
require("nxgptk_godot_gamepad_connected(joy_id, gamepad)" in JOYPAD,
		"initial P1 adoption is not tied to the already-open SDL gamepad")
require("nxc6_declare_port_bundle(game_dir) < 0" in JOYPAD and
		"nxc6_declare_port_bundle_for_layout" not in JOYPAD and
		JOYPAD.index("nxc6_declare_port_bundle(game_dir)") <
		JOYPAD.index("SDL_Init("),
		"the positional bundle (schema 4) is not declared before SDL initialization")
require("nxgptk_godot_preinit()" in JOYPAD and
		JOYPAD.index("nxgptk_godot_preinit()") <
		JOYPAD.index("nxc6_declare_port_bundle(game_dir)"),
		"the single pre-init GPTK read does not precede the declaration")
require("nxc6_declare_port_bundle(game_dir) <= 0" not in JOYPAD,
		"the benign 0 of the bundle declaration must never fail the driver")
require("nxc6_declare_static_provider((const void *)&SDL_Init" in JOYPAD and
		JOYPAD.index("nxc6_declare_static_provider(") <
		JOYPAD.index("nxc6_declare_port_bundle(game_dir)"),
		"the built-in SDL3 must be declared as the static provider (V5/5.1) before the bundle")
remove_body = JOYPAD.split("case SDL_EVENT_JOYSTICK_REMOVED:", 1)[1].split(
		"case SDL_EVENT_JOYSTICK_AXIS_MOTION:", 1)[0]
require(remove_body.index("nxgptk_godot_gamepad_disconnected(joy_id)") <
		remove_body.index("close_joypad(joy_id)") <
		remove_body.index("remaining.get_sdl_gamepad()") <
		remove_body.index("nxgptk_godot_gamepad_connected("),
		"hotplug promotion must disconnect, close, then snapshot an already-open remaining pad")
require("nxgptk_godot_tick" in CPP and "nxgptk_godot_consume_quit_request" in CPP,
        "the display server does not tick the controls runtime")
fatal_health = CPP.split("if (nxgptk_godot_consume_fatal_request())", 1)[1].split(
		"if (nxgptk_godot_consume_quit_request())", 1)[0]
require('_fail_runtime("nx/input: GPTK delivery failed closed.", EXIT_FAILURE)' in
		fatal_health,
		"GPTK fatal does not enter the shared nonzero runtime failure boundary")
fail_runtime = CPP.split("void DisplayServerFBDev::_fail_runtime(", 1)[1].split(
		"void DisplayServerFBDev::_publish_health_receipt()", 1)[0]
require(fail_runtime.index("health_blocked = true") <
		fail_runtime.index("_revoke_health_receipt()") <
		fail_runtime.index("set_exit_code(") <
		fail_runtime.index("request_close()"),
		"runtime fatal does not revoke health before nonzero close")
revoke_body = CPP.split("void DisplayServerFBDev::_revoke_health_receipt()", 1)[1].split(
		"void DisplayServerFBDev::register_fbdev_driver()", 1)[0]
require("getenv(" not in revoke_body and "health_receipt_path" in revoke_body and
		"unlink(health_receipt_path)" in revoke_body,
		"health revocation is not bound to the exact path retained at publication")
for token in ("nxinput_gptk.c", "nxinput_gptk_live.c", "nxinput_gptk_loader.c", "nxinput_gptk_motion.c"):
    require(token in SCSUB, f"SCsub does not compile {token}")
require("nxinput_portmaster.c" in SCSUB,
		"SCsub does not compile the PortMaster domain normalizer")
require('#include "nxinput_decision.h"' in NXC6 and
		"nxinput_decision_decide(" in NXC6 and
		"nxc6_decide_line(" in NXC6 and
		"ops->normalize_source_fn = nxc6_normalize_source;" in NXC6,
		"C6 admission does not run the V5 decision machine per source line before authority")
for token in (
		"pm_legacy_code", 'pm_find_single_button(mapping, "volumedown"',
		'pm_find_single_button(mapping, "volumeup"', "KEY_VOLUMEDOWN",
		"KEY_VOLUMEUP", "NXINPUT_PM_NOT_APPLICABLE"):
	require(token in PM_SOURCE,
			f"PortMaster normalizer lacks positive capability gate {token}")
# V5 schema 4: the PortMaster joydev conversion API stays vendored (the source
# still ships) but is NO LONGER the live input boundary. The schema-4 bridge
# (nxinput_gptk4) projects the owner onto the live V3 dispatch, so the input
# proof and the stripped ELF require the bridge entry points instead of the
# joydev converter (which the V5 engine legitimately does not retain).
for symbol in (
		"nxinput_pm_convert_joydev_mapping",
		"nxinput_pm_normalize_source",
):
	require(symbol in PM_HEADER and symbol in PM_SOURCE,
			f"vendored PortMaster API lacks {symbol}")
for symbol in (
		"nxinput_gptk4_load_project_at",
		"nxinput_gptk4_bridge_receipt_json",
):
	require(f'"{symbol}"' in PROOF,
			f"candidate input proof does not require the schema-4 bridge {symbol}")
require('"nxinput_portmaster.c": "src/nxinput_portmaster.c"' in REFRESH and
		'"nxinput_portmaster.h": "include/nxinput_portmaster.h"' in REFRESH and
		'"nxinput": {' in REFRESH and 'NXINPUT_COMPONENT_VERSION' in REFRESH and
		'"tag": NXINPUT_FRAMEWORK_TAG' in REFRESH and
		'"tag_object": NXINPUT_FRAMEWORK_TAG_OBJECT' in REFRESH and
		'"tag_object_sha256": NXINPUT_FRAMEWORK_TAG_OBJECT_SHA256' in REFRESH and
		'"nxinput_livedb.c": "src/nxinput_livedb.c"' in REFRESH and
		'"nxinput_gptk_preinit.c": "src/nxinput_gptk_preinit.c"' in REFRESH and
		'pinned framework nxinput version is not {NXINPUT_COMPONENT_VERSION}'
		in REFRESH,
		"release provenance does not bind the full nxinput 0.10.0 vendor")
require("drivers/sdl/nxinput_gptk_godot.cpp" in BUILD and
		"drivers/sdl/nxinput_gptk_godot.h" in BUILD and
		"drivers/sdl/tearscape_gptk_context_policy.h" in BUILD and
		"drivers/sdl/tearscape_gptk_input_policy.h" in BUILD and
		"drivers/sdl/tearscape_padset.cpp" in BUILD and
		"drivers/sdl/tearscape_padset.h" in BUILD,
		"build recipe does not install the controls runtime glue")
# 0.2.17: the pad set travels like nx_sdl2_geometry.*: new-file hunks in the
# engine patch (byte-identical to v4-universal) AND the install list. The
# drivers/sdl SCsub compiles every *.cpp by glob, so no source list changes.
require("+++ b/drivers/sdl/tearscape_padset.cpp" in PATCH and
		"+++ b/drivers/sdl/tearscape_padset.h" in PATCH and
		'+#include "tearscape_padset.h"' in PATCH and
		"+++ b/drivers/sdl/joypad_sdl.cpp" in PATCH,
		"engine patch does not carry the pad set and the joypad driver")
require('add_source_files(driver_obj, "*.cpp")' in SCSUB,
		"drivers/sdl SCsub no longer compiles the pad set by glob")
# 0.2.17: the GPTK evidence receipt unit travels the same way (install list +
# new-file hunks byte-identical to v4-universal).
RECEIPT_H = (PORT / "src/godot_engine/v4-universal/drivers/sdl/tearscape_gptk_receipt.h").read_text(encoding="utf-8")
RECEIPT_CPP = (PORT / "src/godot_engine/v4-universal/drivers/sdl/tearscape_gptk_receipt.cpp").read_text(encoding="utf-8")
require("drivers/sdl/tearscape_gptk_receipt.cpp" in BUILD and
		"drivers/sdl/tearscape_gptk_receipt.h" in BUILD,
		"build recipe does not install the GPTK evidence receipt unit")
for name, source in (("tearscape_gptk_receipt.cpp", RECEIPT_CPP),
		("tearscape_gptk_receipt.h", RECEIPT_H)):
	header = f"+++ b/drivers/sdl/{name}\n"
	require(header in PATCH and
			PATCH.split(f"diff --git a/drivers/sdl/{name} b/drivers/sdl/{name}\n", 1)[1].startswith(
				"new file mode 100644\n"),
			f"engine patch lacks the new-file hunk for {name}")
	hunk = PATCH.split(header, 1)[1].split("\ndiff --git ", 1)[0].splitlines()[1:]
	require(all(line.startswith("+") for line in hunk) and
			"\n".join(line[1:] for line in hunk) + "\n" == source,
			f"engine patch and v4-universal {name} are not byte-identical")
require("#include" not in RECEIPT_CPP.replace('#include "tearscape_gptk_receipt.h"', "").replace("#include <string.h>", "") and
		all(f"#include <{name}>" in RECEIPT_H for name in ("stdbool.h", "stddef.h", "stdio.h")) and
		RECEIPT_H.count("#include") == 3 and
		all(token not in RECEIPT_H + RECEIPT_CPP for token in (
			'#include "core/', "#include <SDL3/", '#include "joystick/linux/')),
		"the receipt unit is no longer a pure host-testable unit")
# ONE authority for the `sink` field: the receipt table == nxproject sinks[0].
receipt_rows = re.findall(r'^\t\{ "([a-z]+\.[a-z_.]+)", "([a-z]+\.[a-z_.]+)" \},$', RECEIPT_CPP, re.M)
require(sorted(receipt_rows) == sorted(
			(action["id"], action["sinks"][0]) for action in NXPROJECT["controls"]["actions"]) and
		all(len(action["sinks"]) == 1 for action in NXPROJECT["controls"]["actions"]),
		"the adapter sink-id table differs from nxproject controls.actions[].sinks")
require(set(sink for _action, sink in receipt_rows) ==
		set(re.findall(r'^    "([a-z]+\.[a-z_.]+)": \($', PROOF, re.M)),
		"the adapter sink-id table differs from make_input_proof SINKS")
# The glue feeds the unit: every line kind, the env, the edge/attribution
# helpers; no per-frame vector line; stdout sink= carries the adapter sink id.
for token in (
	'#include "tearscape_gptk_receipt.h"',
	'getenv("NXGPTK_RECEIPT")',
	"tears_gptk_receipt_open(", "tears_gptk_receipt_write(",
	"tears_gptk_receipt_runtime_line(", "tears_gptk_receipt_context_line(",
	"tears_gptk_receipt_delivery_line(", "tears_gptk_receipt_suppressed_line(",
	"tears_gptk_press_owner_edge(", "tears_gptk_vector_gesture_feed(",
	"tears_gptk_vector_gesture_close(", "tears_gptk_adapter_sink_id(",
	"TEARS_GPTK_VECTOR_EDGE_START", "TEARS_GPTK_VECTOR_EDGE_STOP",
	'nxgptk_receipt_delivery(control, "press", p_action, p_pressed)',
	'"motion", p_sink->action, true)', '"motion", p_sink->action, false)',
):
	require(token in GLUE, f"controls runtime glue lacks receipt token {token}")
require("sink=godot-inputmap-vector" not in GLUE and
		"p_action, sink->input_map_action));" not in GLUE and
		GLUE.count("nxgptk_adapter_sink(p_action)") >= 1 and
		GLUE.count("nxgptk_adapter_sink(p_sink->action)") == 2,
		"stdout evidence lines no longer name the adapter sink id")
release_all_body = GLUE.split("static bool nxgptk_vector_release_all() {", 1)[1].split(
		"static bool nxgptk_vector_feed(", 1)[0]
require("nxgptk_vector_gesture_close((int)source);" in release_all_body,
		"release-all does not close open stick gestures on the evidence")
commit_body = GLUE.split("static int nxgptk_commit_action_edge(", 1)[1].split(
		"static const NxGptkActionSink *nxgptk_action_sink_for(", 1)[0]
require("if (effect == NXINPUT_GODOT_ACTION_DELIVER) {" in commit_body.split(
			"nxinput_godot_action_commit(&latch, p_pressed)", 1)[1] and
		"&nxgptk.button_owner[index], p_pressed, nxgptk.current_control" in commit_body,
		"a delivered press/release is not attributed to the pressing control")
for feed in (
	"\tnxgptk.current_control = control;\n\tconst nxinput_gptk_live_result result = nxinput_gptk_live_feed(\n",
	"\t\tnxgptk.current_control = control;\n\t\tconst nxinput_gptk_live_result result = nxinput_gptk_live_feed(\n",
	"\tnxgptk.current_control = p_control;\n\tconst nxinput_gptk_live_result result = nxinput_gptk_live_feed_vector(\n",
):
	require(feed in GLUE and "nxgptk.current_control = -1;" in GLUE.split(feed, 1)[1][:400],
			"the feeding control is not published around every live feed")
RUN_SH = (PORT / "tests/controls/run.sh").read_text(encoding="utf-8")
require("test_tearscape_gptk_receipt.cpp" in RUN_SH and
		"tearscape_gptk_receipt.cpp" in RUN_SH,
		"tests/controls/run.sh does not run the receipt host gate")
patched_joypad = []
for hunk_line in PATCH.split("+++ b/drivers/sdl/joypad_sdl.cpp", 1)[1].split("\ndiff --git ", 1)[0].splitlines()[1:]:
	if hunk_line.startswith("+"):
		patched_joypad.append(hunk_line[1:])
	elif hunk_line.startswith(" "):
		patched_joypad.append(hunk_line[1:])
for token in (
		"const bool admitted_removed =",
		"(void)nxgptk_godot_gamepad_connected(",
		"nx/input: SELECT+START: lifecycle exit requested (exit-chord on pad %d)"):
	require(any(token in line for line in patched_joypad) and token in JOYPAD,
			f"patch and v4-universal joypad_sdl.cpp disagree on {token}")
require("nice -n 10 python3 -m SCons" in BUILDER and
		'--jobs="${TEARSCAPE_BUILD_JOBS:-2}"' in BUILDER,
		"Godot compiler processes are not constrained to nice-10 and two workers")
require("platform/linuxbsd/fbdev/nxgl_godot_frame_proof.h" in BUILD,
		"build recipe does not install the canonical Godot frame-proof wrapper")
require("NXINPUT_GPTK_LIVE_PASSTHROUGH" in GLUE and
		"NXINPUT_GPTK_LIVE_FATAL" in GLUE,
		"live results do not preserve native passthrough/fatal no-replay")
for token in (
	'"res://scenes/ui/"',
	'"scene:ui:start-coop"',
	'"res://scenes/map.tscn"',
	'"res://scenes/level/"',
	"TEARS_GPTK_CONTEXT_UNPROVEN",
):
	require(token in POLICY, f"context policy lacks {token}")
require("tears_gptk_context_source_for_scene" in POLICY and
		"tears_gptk_context_source_for_scene" in GLUE and
		"nxgptk_context_source_for_scene" not in GLUE,
		"start-coop authority epoch is not owned by the testable policy helper")
require("tree->is_paused()" in GLUE,
		"Tearscape overlays are not proven from the UiManager pause boundary")
require("gui_get_focus_owner" not in GLUE,
		"custom GUI focus survived as a false Tearscape context authority")
# 0.2.17 (logical-player pad set): every C6-admitted gamepad drives ONE
# logical player through tearscape_padset.{h,cpp}; "primary pad" is retired.
# Co-op (the game's own attack2 InputMap) still keeps every pad native.
require("tears_padset_slot(&nxgptk.pads" in GLUE and
			"tears_gptk_pad_is_primary" not in GLUE and
			"native_coop" in GLUE and 'has_action(StringName("attack2"))' in GLUE,
			"P2/co-op is not preserved on Tearscape's native per-device path")
PADSET_H = (PORT / "src/godot_engine/v4-universal/drivers/sdl/tearscape_padset.h").read_text(encoding="utf-8")
PADSET_CPP = (PORT / "src/godot_engine/v4-universal/drivers/sdl/tearscape_padset.cpp").read_text(encoding="utf-8")
require("TEARS_PADSET_MAX = 4" in PADSET_H and
		'"chord denied: SELECT and START on different pads (cross-pad)"' in PADSET_CPP and
		"#include" not in PADSET_H.replace("#include <stdbool.h>", "").replace("#include <stdint.h>", "") and
		"#include" not in PADSET_CPP.replace('#include "tearscape_padset.h"', "").replace("#include <string.h>", ""),
		"the pad set is not a pure unit (cap 4, exact denial text, no Godot/SDL header)")
for token in (
		"tears_padset_union_controls(&nxgptk.pads",
		"tears_padset_axis(&nxgptk.pads",
		"tears_padset_chord(&nxgptk.pads",
		'print_line(vformat("nx/input: %s", tears_padset_cross_pad_denial()))',
		'"nx/input: controller: %s (%04x:%04x) mapping=%s"',
		"SDL_GetGamepadMapping(p_gamepad)", "SDL_free(mapping)",
		'"nx/input: pad slot=%d instance=%d sdl_index=%d"',
		'"nx/input: controller-removed instance=%d',
		"nxgptk_clear_live_context(\"pad-disconnect-release-failed\")",
):
	require(token in GLUE, f"logical-player pad set glue lacks {token}")
require(GLUE.count("SDL_GetGamepadMapping(") == 1 and
		"a:b" not in GLUE and "platform:" not in GLUE,
		"the admission mapping must be SDL's real string, never synthesized")
connect_body = GLUE.split("bool nxgptk_godot_gamepad_connected(", 1)[1].split(
		"bool nxgptk_godot_gamepad_disconnected(", 1)[0]
require("SDL_GetGamepadButton" in connect_body and
		connect_body.count("nxgptk_snapshot_stick_axis(") == 4 and
		connect_body.count("nxgptk_snapshot_trigger_axis(") == 2 and
		"nxinput_godot_handoff_snapshot" in connect_body,
		"hotplug pad admission lacks a complete buttons/six-axis SDL snapshot")
require(connect_body.index("SDL_GetGamepadButton") <
		connect_body.index("tears_padset_admit(&nxgptk.pads") <
		connect_body.index("nxinput_godot_handoff_snapshot"),
		"hotplug snapshot must be queried before admission and armed before admission returns")
require(connect_body.index("tears_padset_admit(&nxgptk.pads") <
		connect_body.index("nx/input: controller: "),
		"the admission receipt must follow the actual admission")
disconnect_body = GLUE.split("bool nxgptk_godot_gamepad_disconnected(", 1)[1].split(
		"static bool nxgptk_result_consumes(", 1)[0]
require(disconnect_body.index("tears_padset_remove(&nxgptk.pads") <
		disconnect_body.index("nxgptk_clear_live_context(") <
		disconnect_body.index("nxinput_godot_handoff_snapshot(") <
		disconnect_body.index("controller-removed"),
		"pad removal must drop the pad, release every latch, re-arm the remaining pads natively, then log")
require("tears_gptk_clear_axis_cache" in GLUE and
			"tears_gptk_clear_axis_cache" in INPUT_POLICY,
			"co-op transition can replay a stale governed stick vector")
button_body = GLUE.split("bool nxgptk_godot_gamepad_button(", 1)[1].split(
		"bool nxgptk_godot_gamepad_axis(", 1)[0]
require("nxgptk_godot_gamepad_connected" not in button_body,
		"button event can still lazily adopt a pad without a state snapshot")
require(button_body.index("nxgptk.physical_down_controls") <
		button_body.index("if (!nxgptk.active)"),
		"pre-init native button state is not captured before GPTK activation")
require(button_body.index("nxinput_godot_handoff_button") <
		button_body.index("nxgptk.force_start_coop_native") and
		"nxgptk.handoff.controls |= bit" in button_body,
		"native start-coop press is not latched through its release")
button_suppressed = button_body.index("&nxgptk.suppressed_handoff")
button_native = button_body.index("&nxgptk.handoff", button_suppressed)
button_coop = button_body.index("if (nxgptk.native_coop)", button_native)
require(button_suppressed < button_native < button_coop,
		"button handoff must consume old governed input, pass old native input, then evaluate co-op")
axis_body = GLUE.split("bool nxgptk_godot_gamepad_axis(", 1)[1].split(
		"static bool nxgptk_vector_release_all()", 1)[0]
require("nxgptk_godot_gamepad_connected" not in axis_body,
		"axis event can still lazily adopt a pad without a state snapshot")
non_fbdev = GLUE.split("#else /* !FBDEV_ENABLED */", 1)[1]
require("bool nxgptk_godot_gamepad_connected(int p_joy_id, SDL_Gamepad *p_gamepad)" in non_fbdev and
		"bool nxgptk_godot_gamepad_disconnected(int p_joy_id)" in non_fbdev,
		"non-FBDEV controls stubs differ from their public signatures")
# 0.2.17: the stick cache is the pad set's largest-magnitude aggregate, still
# written (nxgptk_refresh_from_pads) before the activation check.
require(axis_body.index("tears_padset_set_axis(&nxgptk.pads") <
		axis_body.index("nxgptk_refresh_from_pads()") <
		axis_body.index("if (!nxgptk.active)"),
		"pre-init native stick state is not captured before GPTK activation")
require("nxgptk.left_x = tears_padset_axis(&nxgptk.pads, TEARS_PADSET_AXIS_LEFT_X)" in GLUE and
		"trigger_now ? logical_value : 0.0f" in axis_body,
		"the runtime does not read the logical player's aggregate axes")
button_body_padset = GLUE.split("bool nxgptk_godot_gamepad_button(", 1)[1].split(
		"bool nxgptk_godot_gamepad_axis(", 1)[0]
require(button_body_padset.index("tears_padset_set_control(&nxgptk.pads") <
		button_body_padset.index("nxgptk_refresh_from_pads()") <
		button_body_padset.index("nxgptk_observe_chord()") <
		button_body_padset.index("nxgptk_control_consumed_without_edge(control)") <
		button_body_padset.index("if (!nxgptk.active)"),
		"button events must update the pad set, the union and the chord before any GPTK edge")
coop_transition = GLUE.split("if (coop_now != nxgptk.native_coop)", 1)[1].split(
		"if (nxgptk.native_coop)", 1)[0]
require("tears_gptk_clear_axis_cache" not in coop_transition,
		"leaving native co-op erases held-stick truth before neutral handoff")
require("nxgptk_snapshot_native_handoff" not in GLUE and
		"nxgptk_snapshot_suppressed_handoff" not in GLUE and
		GLUE.count("nxgptk_partition_handoff(") == 4,
		"scene transitions do not rebuild both authority masks together")
governed_body = GLUE.split("static uint32_t nxgptk_governed_controls(", 1)[1].split(
		"static void nxgptk_partition_handoff(", 1)[0]
require("nxinput_gptk_decide" in governed_body and
		"NXINPUT_GPTK_DECIDE_ACTION" in governed_body and
		"NXINPUT_GPTK_DECIDE_SUPPRESS" in governed_body,
		"held controls are not classified by the old GPTK authority")
context_transition = GLUE.split(
		"if (!nxgptk.context_proven || wanted != nxgptk.context", 1)[1].split(
		"if (((nxgptk.handoff.controls", 1)[0]
require(context_transition.index("nxgptk_partition_handoff(") <
		context_transition.index("nxinput_gptk_live_set_context("),
		"old-context handoff is not armed before the new context")
vector_tick = GLUE.split(
		"if (((nxgptk.handoff.controls | nxgptk.suppressed_handoff.controls)", 1)[1]
require(vector_tick.count(
		"nxgptk.handoff.controls | nxgptk.suppressed_handoff.controls") >= 1 and
		"NXINPUT_GPTK_LEFT_STICK" in vector_tick and
		"NXINPUT_GPTK_RIGHT_STICK" in vector_tick,
		"held stick crosses into the new context before release/neutral")
require("nxinput_godot_vector_alias vector_alias" in GLUE and
		"nxinput_godot_vector_alias_update" in GLUE and
		"vector_edge_down[" in GLUE and
		"[NXINPUT_GODOT_VECTOR_ALIAS_SOURCES]" in GLUE,
		"two sticks mapped to one action do not retain independent alias state")
vector_feed_body = GLUE.split("static bool nxgptk_vector_feed(", 1)[1].split(
		'extern "C" int tearscape_gptk_inputmap_vector_sink', 1)[0]
edge_body = vector_feed_body.split("if (p_sink->edge_action)", 1)[1].split(
		"float strengths[4]", 1)[0]
require("nxgptk_commit_action_edge" in edge_body and
		"nxgptk_deliver(p_sink->up" not in edge_body,
		"button and stick aliases do not share one semantic action refcount")
require(GLUE.count("nxgptk_feed_vector_control(") == 3 and
		"nxinput_gptk_live_feed_vector(" in
		GLUE.split("static nxinput_gptk_live_result nxgptk_feed_vector_control(", 1)[1].split(
			"extern \"C\" int tearscape_gptk_resolve_context", 1)[0],
		"stick identity is not carried through the live vector callback")
require("NXINPUT_GODOT_HANDOFF_NEUTRAL 0.20f" in GODOT_RUNTIME and
		"x * x + y * y" in GODOT_RUNTIME,
		"handoff neutral is not radial/drift-safe")
require("Crossing a\n * native co-op boundary must never replay" not in INPUT_POLICY,
		"input policy still tells future adapters to erase co-op physical state")
require("tears_gptk_edge_next" in GLUE and "tears_gptk_edge_next" in INPUT_POLICY,
		"legacy stick zoom lacks one-edge radial hysteresis")
for token in (
	"nxinput_godot_lifecycle_fail(&nxgptk.lifecycle)",
	"nxinput_godot_lifecycle_consume_close(&nxgptk.lifecycle)",
	"nxinput_gptk_live_clear_context_checked(&nxgptk.live)",
	"nxinput_gptk_live_is_fatal(&nxgptk.live)",
):
	require(token in GLUE, f"failed sink ACK lifecycle lacks {token}")
require("session_fatal" not in GLUE,
		"obsolete port-local fatal lifecycle survived canonical integration")
fatal_at = CPP.index("if (nxgptk_godot_consume_fatal_request())")
quit_at = CPP.index("if (nxgptk_godot_consume_quit_request())", fatal_at)
health_at = CPP.index("if (!health_blocked && frame_presented", quit_at)
require(fatal_at < quit_at < health_at and
		'_fail_runtime("nx/input: GPTK delivery failed closed.", EXIT_FAILURE)' in
		CPP[fatal_at:quit_at] and
		"health_blocked = true;" in fail_runtime and
		"set_exit_code(" in fail_runtime,
		"fatal sink ACK is not handled before clean quit/health with nonzero exit")
require("system.start_coop" not in declared,
		"device-sensitive start_coop must remain a native Joypad event")
require("RIGHT_STICK" not in NXPROJECT["controls"]["contexts"]["gameplay"],
		"default mapping still invents analog zoom on the button-only action")
zoom = next(action for action in NXPROJECT["controls"]["actions"]
		if action["id"] == "player.zoom_map")
require(zoom["kind"] == "button",
			"zoom_map differs from Tearscape's button-only InputMap contract")
require('{ "system.pause", "menu", nullptr }' in GLUE and
		'{ "system.pause", "pause"' not in GLUE,
		"START is not wired to UiManager's assembly-confirmed menu action")
for action in ("move_up", "move_down", "move_left", "move_right"):
	require(f'"{action}"' in GLUE, f"player movement sink lacks real InputMap action {action}")
for forbidden in ('"move_upD"', '"move_downD"', '"move_leftD"', '"move_rightD"'):
	require(forbidden not in GLUE, f"binary-header byte was mistaken for an InputMap suffix: {forbidden}")
for symbol in (
	"nxinput_gptk_load_at", "nxinput_gptk_load_receipt_json",
	"nxinput_gptk_parse", "nxinput_gptk_decide",
	"nxinput_gptk_live_init", "nxinput_gptk_live_register",
	"nxinput_gptk_live_register_vector", "nxinput_gptk_live_seal",
	"nxinput_gptk_live_set_context", "nxinput_gptk_live_clear_context",
	"nxinput_gptk_live_clear_context_checked", "nxinput_gptk_live_is_fatal",
	"nxinput_gptk_live_should_consume", "nxinput_gptk_live_feed",
	"nxinput_gptk_live_feed_vector", "nxinput_gptk4_load_project_at",
	"nxinput_gptk4_bridge_receipt_json", "nxc6_declare_static_provider", "nxinput_gptk_runtime_marker",
	"nxinput_gptk_event_evidence_schema", "tearscape_gptk_inputmap_sink",
	"tearscape_gptk_inputmap_vector_sink", "tearscape_gptk_quit_sink",
	"tearscape_gptk_resolve_context", "nxgl_frame_proof_set_resolver",
	"nxgl_frame_proof_is_fatal", "nxgl_frame_proof_consume_fatal",
):
	require(f"--export-dynamic-symbol={symbol}" in BUILDER or
			symbol in BUILDER.split("for symbol in", 1)[-1],
			f"final stripped ELF does not export {symbol}")
require("def verify_nxgl_vendor(framework: Path)" in REFRESH and
		"verify_nxgl_vendor(framework)" in REFRESH and
		'"nxgl": {"version": "0.3.5"' in REFRESH and
		'"nxgl_godot_frame_proof.h": "engine-glue/nxgl_godot_frame_proof.h"' in REFRESH,
		"release provenance does not bind the Godot frame proof to nxgl 0.3.5")

print("TEARSCAPE RUNTIME CONTRACT: PASS")
