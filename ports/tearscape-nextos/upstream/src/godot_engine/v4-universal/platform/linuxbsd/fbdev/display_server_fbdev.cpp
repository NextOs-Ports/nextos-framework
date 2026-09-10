#include "display_server_fbdev.h"

#include "drivers/sdl/nxinput_gptk_godot.h"
#include "nx_sdl2_geometry.h"
#include "nx_aspect_policy.h"

#ifdef FBDEV_ENABLED

#include "core/config/project_settings.h"
#include "core/os/os.h"

#include "core/string/print_string.h"

#ifdef GLES3_ENABLED
#include "drivers/gles3/rasterizer_gles3.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// NxSDL_Window is declared by nx_sdl2_geometry.h (shared with the geometry authority).
typedef void *NxSDL_GLContext;

enum {
	NX_SDL_INIT_VIDEO = 0x00000020u,
	NX_SDL_WINDOW_OPENGL = 0x00000002u,
	NX_SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001u,
	NX_SDL_WINDOWPOS_CENTERED = 0x2fff0000u,
	NX_SDL_GL_RED_SIZE = 0,
	NX_SDL_GL_GREEN_SIZE = 1,
	NX_SDL_GL_BLUE_SIZE = 2,
	NX_SDL_GL_ALPHA_SIZE = 3,
	NX_SDL_GL_DOUBLEBUFFER = 5,
	NX_SDL_GL_DEPTH_SIZE = 6,
	NX_SDL_GL_STENCIL_SIZE = 7,
	NX_SDL_GL_CONTEXT_MAJOR_VERSION = 17,
	NX_SDL_GL_CONTEXT_MINOR_VERSION = 18,
	NX_SDL_GL_CONTEXT_PROFILE_MASK = 21,
	NX_SDL_GL_CONTEXT_PROFILE_ES = 0x0004,
};

struct NxSdlApi {
	void *handle = nullptr;
	bool attempted = false;
	bool ok = false;
	int (*InitSubSystem)(uint32_t) = nullptr;
	void (*QuitSubSystem)(uint32_t) = nullptr;
	const char *(*GetError)(void) = nullptr;
	const char *(*GetCurrentVideoDriver)(void) = nullptr;
	int (*GL_SetAttribute)(int, int) = nullptr;
	NxSDL_Window *(*CreateWindow)(const char *, int, int, int, int, uint32_t) = nullptr;
	void (*DestroyWindow)(NxSDL_Window *) = nullptr;
	NxSDL_GLContext (*GL_CreateContext)(NxSDL_Window *) = nullptr;
	void (*GL_DeleteContext)(NxSDL_GLContext) = nullptr;
	int (*GL_MakeCurrent)(NxSDL_Window *, NxSDL_GLContext) = nullptr;
	void (*GL_SwapWindow)(NxSDL_Window *) = nullptr;
	int (*GL_SetSwapInterval)(int) = nullptr;
	void (*GL_GetDrawableSize)(NxSDL_Window *, int *, int *) = nullptr;
	void (*GetWindowSize)(NxSDL_Window *, int *, int *) = nullptr;
	int (*ShowCursor)(int) = nullptr;
};

NxSdlApi sdl_api;
NxSDL_Window *sdl_window = nullptr;
NxSDL_GLContext sdl_context = nullptr;
bool sdl_video_ready = false;

// Window geometry authority for the SDL2 provider (0.2.16). The raw /dev/fb0
// size is informational there: SDL's display bounds, the first authoritative
// configure and the live drawable decide the logical window size.
NxSdl2Geometry sdl_geometry;
bool sdl_geometry_ready = false;

void sdl_geometry_log(void *, const char *p_line) {
	print_line(String(p_line));
}

void *sdl_geometry_resolve(void *p_handle, const char *p_name) {
	return dlsym(p_handle, p_name);
}

void sdl_geometry_prepare() {
	if (sdl_geometry_ready) {
		return;
	}
	NxSdl2GeometryApi api;
	nx_sdl2_geometry_bind(&api, sdl_api.handle, sdl_geometry_resolve);
	nx_sdl2_geometry_init(&sdl_geometry, &api, sdl_geometry_log, nullptr);
	sdl_geometry_ready = true;
}

// Both FBDev/EGL and SDL2 contexts are resolved through Godot's loaded EGL
// entry point. The pinned Tearscape EGL shim then routes each gl* name to the
// GLES facade, firmware SDL provider or legacy blob that owns the live
// context. This is required because firmware SDL itself is deliberately
// opened RTLD_LOCAL and its GL symbols need not exist in RTLD_DEFAULT.
void *nxgl_godot_gl_resolve(const char *p_name) {
#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)
	if (eglGetProcAddress) {
		return (void *)eglGetProcAddress(p_name);
	}
#endif
	return nullptr;
}

const char *sdl_error() {
	const char *text = sdl_api.GetError ? sdl_api.GetError() : nullptr;
	return text && text[0] ? text : "unknown SDL2 error";
}

bool sdl_load() {
	if (sdl_api.attempted) {
		return sdl_api.ok;
	}
	sdl_api.attempted = true;
	static const char *const candidates[] = {
		"libSDL2-2.0.so.0",
		"libSDL2.so",
		"libSDL2-2.0.so",
		nullptr,
	};
	for (int i = 0; candidates[i] && !sdl_api.handle; i++) {
		sdl_api.handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
	}
	if (!sdl_api.handle) {
		return false;
	}

#define NX_SDL_REQUIRED(field, symbol)                                  \
	do {                                                                  \
		*(void **)(&sdl_api.field) = dlsym(sdl_api.handle, symbol);         \
		if (!sdl_api.field) {                                               \
			dlclose(sdl_api.handle);                                          \
			sdl_api.handle = nullptr;                                         \
			return false;                                                      \
		}                                                                   \
	} while (0)

	NX_SDL_REQUIRED(InitSubSystem, "SDL_InitSubSystem");
	NX_SDL_REQUIRED(QuitSubSystem, "SDL_QuitSubSystem");
	NX_SDL_REQUIRED(GetError, "SDL_GetError");
	NX_SDL_REQUIRED(GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver");
	NX_SDL_REQUIRED(GL_SetAttribute, "SDL_GL_SetAttribute");
	NX_SDL_REQUIRED(CreateWindow, "SDL_CreateWindow");
	NX_SDL_REQUIRED(DestroyWindow, "SDL_DestroyWindow");
	NX_SDL_REQUIRED(GL_CreateContext, "SDL_GL_CreateContext");
	NX_SDL_REQUIRED(GL_DeleteContext, "SDL_GL_DeleteContext");
	NX_SDL_REQUIRED(GL_MakeCurrent, "SDL_GL_MakeCurrent");
	NX_SDL_REQUIRED(GL_SwapWindow, "SDL_GL_SwapWindow");
	NX_SDL_REQUIRED(GL_SetSwapInterval, "SDL_GL_SetSwapInterval");
#undef NX_SDL_REQUIRED

	*(void **)(&sdl_api.GL_GetDrawableSize) = dlsym(sdl_api.handle, "SDL_GL_GetDrawableSize");
	*(void **)(&sdl_api.GetWindowSize) = dlsym(sdl_api.handle, "SDL_GetWindowSize");
	*(void **)(&sdl_api.ShowCursor) = dlsym(sdl_api.handle, "SDL_ShowCursor");
	sdl_api.ok = true;
	return true;
}

bool sdl_init_video() {
	if (sdl_video_ready) {
		return true;
	}
	if (!sdl_load()) {
		return false;
	}
	setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0);
	// App id (hint + SDL_VIDEO_WAYLAND_WMCLASS) must be in place before the
	// video subsystem creates its compositor connection.
	sdl_geometry_prepare();
	nx_sdl2_geometry_prepare_app_id(&sdl_geometry);
	if (sdl_api.InitSubSystem(NX_SDL_INIT_VIDEO) == 0) {
		sdl_video_ready = true;
		print_line(vformat("nx/sdl2: video subsystem up, driver '%s'",
				sdl_api.GetCurrentVideoDriver() ? sdl_api.GetCurrentVideoDriver() : "(null)"));
		return true;
	}

	const char *inherited = getenv("SDL_VIDEODRIVER");
	print_line(vformat("nx/sdl2: video init failed with SDL_VIDEODRIVER='%s': %s",
			inherited ? inherited : "", sdl_error()));
	if (inherited && inherited[0]) {
		sdl_api.QuitSubSystem(NX_SDL_INIT_VIDEO);
		unsetenv("SDL_VIDEODRIVER");
		if (sdl_api.InitSubSystem(NX_SDL_INIT_VIDEO) == 0) {
			sdl_video_ready = true;
			print_line(vformat("nx/sdl2: firmware autodetection recovered, driver '%s'",
					sdl_api.GetCurrentVideoDriver() ? sdl_api.GetCurrentVideoDriver() : "(null)"));
			return true;
		}
		print_line(vformat("nx/sdl2: autodetection retry failed: %s", sdl_error()));
	}
	return false;
}

void sdl_reset_failed_video() {
	if (sdl_context && sdl_api.GL_DeleteContext) {
		sdl_api.GL_DeleteContext(sdl_context);
		sdl_context = nullptr;
	}
	if (sdl_window && sdl_api.DestroyWindow) {
		sdl_api.DestroyWindow(sdl_window);
		sdl_window = nullptr;
	}
	sdl_geometry.window = nullptr;
	if (sdl_video_ready && sdl_api.QuitSubSystem) {
		sdl_api.QuitSubSystem(NX_SDL_INIT_VIDEO);
		sdl_video_ready = false;
	}
}

} // namespace

Vector<String> DisplayServerFBDev::get_rendering_drivers_func_fbdev() {
	Vector<String> drivers;
#ifdef GLES3_ENABLED
	drivers.push_back("opengl3_es");
#endif
	drivers.push_back("dummy");
	return drivers;
}

DisplayServer *DisplayServerFBDev::create_func_fbdev(const String &p_rendering_driver, WindowMode p_mode, VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, Context p_context, int64_t p_parent_window, Error &r_error) {
	DisplayServer *ds = memnew(DisplayServerFBDev(p_rendering_driver, r_error));
	if (r_error != OK) {
		ERR_PRINT("Can't create the fbdev display server.");
	}
	return ds;
}

const char *DisplayServerFBDev::_provider_name(Provider p_provider) {
	switch (p_provider) {
		case PROVIDER_FBDEV:
			return "fbdev";
		case PROVIDER_SDL2:
			return "sdl2";
		default:
			return "none";
	}
}

bool DisplayServerFBDev::_drm_capability_present() {
	for (int index = 0; index < 8; index++) {
		char node[64];
		snprintf(node, sizeof(node), "/dev/dri/card%d", index);
		int fd = open(node, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		close(fd);

		char prefix[32];
		snprintf(prefix, sizeof(prefix), "card%d-", index);
		DIR *dir = opendir("/sys/class/drm");
		if (!dir) {
			print_line(vformat("nx/video: DRM capability: %s opens, connector view unavailable", node));
			return true;
		}
		bool connected = false;
		struct dirent *entry;
		while ((entry = readdir(dir)) != nullptr) {
			if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) {
				continue;
			}
			char status_path[512];
			snprintf(status_path, sizeof(status_path), "/sys/class/drm/%s/status", entry->d_name);
			FILE *stream = fopen(status_path, "rb");
			if (!stream) {
				continue;
			}
			char status[32] = {};
			size_t got = fread(status, 1, sizeof(status) - 1, stream);
			fclose(stream);
			status[got] = 0;
			for (size_t i = 0; i < sizeof(status); i++) {
				if (status[i] == '\n' || status[i] == '\r') {
					status[i] = 0;
					break;
				}
			}
			if (strcmp(status, "disconnected") != 0) {
				print_line(vformat("nx/video: DRM capability: %s connector %s status '%s'",
						node, String(entry->d_name), String(status)));
				connected = true;
				break;
			}
		}
		closedir(dir);
		if (connected) {
			return true;
		}
	}
	return false;
}

void DisplayServerFBDev::window_set_vsync_mode(VSyncMode p_vsync_mode, WindowID p_window) {
#ifdef GLES3_ENABLED
	use_vsync = p_vsync_mode != VSYNC_DISABLED;
	if (provider == PROVIDER_SDL2 && sdl_api.GL_SetSwapInterval) {
		sdl_api.GL_SetSwapInterval(use_vsync ? 1 : 0);
	} else if (egl_manager) {
		egl_manager->set_use_vsync(p_vsync_mode != VSYNC_DISABLED);
	}
#endif
}

DisplayServerFBDev::VSyncMode DisplayServerFBDev::window_get_vsync_mode(WindowID p_window) const {
#ifdef GLES3_ENABLED
	if (provider == PROVIDER_SDL2) {
		return use_vsync ? VSYNC_ENABLED : VSYNC_DISABLED;
	}
	if (egl_manager) {
		return egl_manager->is_using_vsync() ? VSYNC_ENABLED : VSYNC_DISABLED;
	}
#endif
	return VSYNC_ENABLED;
}

void DisplayServerFBDev::window_set_window_event_callback(const Callable &p_callable, WindowID p_window) {
	ERR_FAIL_COND(p_window != MAIN_WINDOW_ID);
	window_event_callback = p_callable;
}

void DisplayServerFBDev::window_set_rect_changed_callback(const Callable &p_callable, WindowID p_window) {
	ERR_FAIL_COND(p_window != MAIN_WINDOW_ID);
	rect_changed_callback = p_callable;
}

void DisplayServerFBDev::request_close() {
	if (window_event_callback.is_valid()) {
		window_event_callback.call(WINDOW_EVENT_CLOSE_REQUEST);
	}
}

void DisplayServerFBDev::release_rendering_thread() {
#ifdef GLES3_ENABLED
	if (provider == PROVIDER_SDL2 && sdl_window) {
		sdl_api.GL_MakeCurrent(sdl_window, nullptr);
	} else if (egl_manager) {
		egl_manager->release_current();
	}
#endif
}

void DisplayServerFBDev::swap_buffers() {
#ifdef GLES3_ENABLED
	const int proof_status = nxgl_godot_frame_proof_before_swap(
			&frame_proof, fb_size.x, fb_size.y);
	if (proof_status != 0) {
		(void)nxgl_godot_frame_proof_consume_close(&frame_proof);
		int exit_status = nxgl_godot_frame_proof_exit_status(&frame_proof);
		if (exit_status == 0) {
			exit_status = EXIT_FAILURE;
		}
		_fail_runtime("nx/video: pre-present proof refused the frame; presentation stopped.", exit_status);
		return;
	}
	if (provider == PROVIDER_SDL2 && sdl_window) {
		sdl_api.GL_SwapWindow(sdl_window);
		frame_presented.store(true, std::memory_order_release);
	} else if (egl_manager) {
		egl_manager->swap_buffers();
		frame_presented.store(true, std::memory_order_release);
	}
	// FV3 (7A.2): the readback happens AFTER the engine applied and after the
	// FIRST present -- never before, or it would be describing an intention
	// instead of an effect.
	_readback_aspect_policy();
#endif
}

void DisplayServerFBDev::refresh_controls_before_events() {
	// Window geometry first: a compositor resize must reach the viewport before
	// this iteration renders.
	_poll_sdl2_geometry();
	// Resolve Tearscape scene/pause authority before SDL polls this batch.
	// A scene transition in the previous Main::iteration therefore cannot let
	// the next button use the old menu/gameplay context.
	nxgptk_godot_tick(0.0);
}

void DisplayServerFBDev::_poll_sdl2_geometry() {
	if (provider != PROVIDER_SDL2 || !sdl_window || !sdl_geometry_ready) {
		return;
	}
	if (!nx_sdl2_geometry_poll(&sdl_geometry)) {
		return;
	}
	fb_size = Size2i(sdl_geometry.logical_w, sdl_geometry.logical_h);
	(void)nxgl_godot_frame_proof_resize(&frame_proof, fb_size.x, fb_size.y);
	// Mirror DisplayServerX11/Wayland: the engine learns a new window rect
	// through rect_changed_callback (Window::_rect_changed_callback resizes
	// the root viewport). No DPI information exists on this path, so
	// WINDOW_EVENT_DPI_CHANGE is intentionally not raised.
	const bool notified = rect_changed_callback.is_valid();
	if (notified) {
		rect_changed_callback.call(Rect2i(Point2i(), fb_size));
	}
	nx_sdl2_geometry_write_resize(&sdl_geometry, notified);
	print_line(vformat("nx/sdl2: window resized to %dx%d (engine %s)",
			fb_size.x, fb_size.y, notified ? "notified" : "has no rect callback yet"));
}

void DisplayServerFBDev::_write_geometry_receipt_init() {
	const char *path = getenv("NXGEOMETRY_RECEIPT");
	if (!path || !path[0]) {
		return;
	}
	if (provider == PROVIDER_SDL2) {
		if (nx_sdl2_geometry_receipt_open(&sdl_geometry, path)) {
			nx_sdl2_geometry_write_init(&sdl_geometry, _provider_name(provider));
		}
	} else if (provider == PROVIDER_FBDEV) {
		if (!nx_sdl2_geometry_write_fbdev_init(path, fb_size.x, fb_size.y)) {
			print_line("nx/video: geometry receipt unavailable for the fbdev provider.");
		}
	}
}

// The logical source Tearscape renders: a fixed 640x360 viewport. Camera
// zoom follows its height (Camera::InitZoom = viewport.y/144), which is why
// the engine can only ever apply "keep"/"ignore".
#define NX_TEARSCAPE_SOURCE_W 640
#define NX_TEARSCAPE_SOURCE_H 360

// NEXTOS_SETTINGS/2 aspect (0.2.17, V5). The DECISION belongs to the
// framework: nxcompat_video_resolve_owner() elects the authority, applies the
// precedence (port-env.sh > NEXTOSSETTINGS.txt > an owner-EDITED override.cfg
// > the port's declared total `auto` algorithm > the package default) and
// computes the content rect. This adapter only reports what each source said,
// translates the effective policy into Godot's vocabulary and applies it
// BEFORE the SceneTree reads display/window/stretch/aspect for the root
// window. The logical viewport (640x360) is never touched: Tearscape derives
// camera zoom from viewport height, so only "keep"/"ignore" are ever applied.
void DisplayServerFBDev::_apply_aspect_policy() {
	// What each owner source said. adapter-env.sh exports the value parsed
	// from NEXTOSSETTINGS.txt separately, and port-env.sh runs AFTER it, so a
	// difference between the two is the owner's hook speaking last. Which of
	// them wins is the framework's call, not ours.
	const char *env_settings = getenv("NX_VIDEO_ASPECT_SETTINGS");
	const char *env_effective = getenv("NX_VIDEO_ASPECT");
	const char *env_authority = getenv("NX_VIDEO_AUTHORITY");
	String settings_aspect = env_settings ? String(env_settings).strip_edges().to_lower() : String();
	String hook_aspect;
	if (env_effective && env_effective[0]) {
		String v = String(env_effective).strip_edges().to_lower();
		if (v != settings_aspect) {
			hook_aspect = v;
		}
	}

	// The engine's native config, as the owner may have edited it in
	// game/override.cfg, translated into the schema the framework speaks.
	const String setting_path = "display/window/stretch/aspect";
	String project_value;
	if (ProjectSettings::get_singleton() && ProjectSettings::get_singleton()->has_setting(setting_path)) {
		project_value = String(ProjectSettings::get_singleton()->get_setting(setting_path));
	}
	CharString project_c = project_value.utf8();
	const char *native_schema = nx_godot_aspect_to_schema(project_c.get_data());
	const bool native_present = !project_value.is_empty();
	const bool native_edited = native_present && project_value != String(NX_ASPECT_SHIPPED_DEFAULT);

	CharString settings_c = settings_aspect.utf8();
	CharString hook_c = hook_aspect.utf8();
	nxcompat_video_owner_input in;
	memset(&in, 0, sizeof(in));
	in.settings_authority = (env_authority && env_authority[0]) ? env_authority : "nextos";
	in.settings_aspect = settings_aspect.is_empty() ? nullptr : settings_c.get_data();
	in.port_env_aspect = hook_aspect.is_empty() ? nullptr : hook_c.get_data();
	in.native_config_aspect = native_schema;
	in.native_config_present = native_present ? 1 : 0;
	in.native_config_edited = (native_edited && native_schema != nullptr) ? 1 : 0;
	// This port declares exactly ONE total auto algorithm: preserve the
	// established full-panel presentation on every drawable. Owners who want
	// letterbox select `preserve` explicitly in NEXTOSSETTINGS.txt.
	in.auto_algorithm_declared = 1;
	in.auto_algorithm_result = nxcompat_video_auto_stretch();
	in.package_default = NXCOMPAT_VIDEO_ASPECT_STRETCH; /* the shipped "ignore" */
	in.source_w = NX_TEARSCAPE_SOURCE_W;
	in.source_h = NX_TEARSCAPE_SOURCE_H;
	in.drawable_w = fb_size.x;
	in.drawable_h = fb_size.y;
	in.cas_generation = ++video_config_generation;

	nxcompat_video_owner_decision decision;
	if (nxcompat_video_resolve_owner(&in, &decision) != 0) {
		// Fail visibly and keep the owner's bytes and the project value: the
		// framework refuses rather than guessing (unknown token, overlap
		// outside the election, `auto` with no algorithm).
		WARN_PRINT(vformat("nx/video: aspect refused: %s (project value kept)",
				String::utf8(decision.reason)));
		return;
	}

	const char *apply = nx_godot_aspect(decision.geometry.effective);
	const bool changed = apply && project_value != String(apply);
	if (apply && changed && ProjectSettings::get_singleton()) {
		ProjectSettings::get_singleton()->set_setting(setting_path, String(apply));
	}

	char receipt[512];
	if (nxcompat_video_owner_receipt(&decision, receipt, sizeof(receipt)) > 0) {
		// Godot's vformat does not implement %u: using it made this whole line
		// fail to format and vanish silently -- the framework receipt, which is
		// the line the kit and the release gates grep for, was simply absent
		// from the device log while the readback line (plain %s) came through.
		print_line(vformat("nx/video: %s generation=%d", String::utf8(receipt),
				(int)video_config_generation));
	}
	print_line(vformat("nx/video: aspect requested=%s settings=%s hook=%s project=%s effective=%s source=%s changed=%s drawable=%dx%d content=%dx%d@%d,%d bars=%d,%d,%d,%d",
			String::utf8(nxcompat_video_aspect_name(decision.requested)),
			settings_aspect.is_empty() ? String("-") : settings_aspect,
			hook_aspect.is_empty() ? String("-") : hook_aspect,
			project_value.is_empty() ? String("-") : project_value,
			String::utf8(nxcompat_video_aspect_name(decision.geometry.effective)),
			String::utf8(nxcompat_video_source_name(decision.source)),
			changed ? "yes" : "no", fb_size.x, fb_size.y,
			decision.geometry.content.w, decision.geometry.content.h,
			decision.geometry.content.x, decision.geometry.content.y,
			decision.geometry.bar_left, decision.geometry.bar_right,
			decision.geometry.bar_top, decision.geometry.bar_bottom));

	video_decision = decision;
	video_decision_valid = true;

	const char *path = getenv("NXGEOMETRY_RECEIPT");
	if (path && path[0]) {
		(void)nx_sdl2_geometry_write_aspect(path, _provider_name(provider),
				nxcompat_video_aspect_name(decision.requested), project_c.get_data(),
				nxcompat_video_aspect_name(decision.geometry.effective),
				nxcompat_video_source_name(decision.source), changed,
				fb_size.x, fb_size.y, decision.geometry.content.x, decision.geometry.content.y,
				decision.geometry.content.w, decision.geometry.content.h);
	}
}

// FV3 readback: after the engine applied and the first frame was presented,
// MEASURE what really happened and compare it with the elected decision. A
// readback that measured nothing is a mismatch by construction, so "the config
// file was read" can never approve (mission 7A.2).
void DisplayServerFBDev::_readback_aspect_policy() {
	if (!video_decision_valid || video_readback_done) {
		return;
	}
	video_readback_done = true;
	const String setting_path = "display/window/stretch/aspect";
	String applied;
	if (ProjectSettings::get_singleton() && ProjectSettings::get_singleton()->has_setting(setting_path)) {
		applied = String(ProjectSettings::get_singleton()->get_setting(setting_path));
	}
	CharString applied_c = applied.utf8();
	const char *applied_schema = nx_godot_aspect_to_schema(applied_c.get_data());

	nxcompat_video_readback got;
	memset(&got, 0, sizeof(got));
	got.api_version = NXCOMPAT_VIDEO_OWNER_API_VERSION;
	got.drawable_w = fb_size.x;
	got.drawable_h = fb_size.y;
	got.cas_generation = video_config_generation;
	got.effective = video_decision.geometry.effective;
	// Under an elected `engine` (inherit) the port asserted NOTHING, so the
	// project's own value is not a measurement of our decision: reading it as
	// one would report a mismatch every time the shipped project says
	// "ignore". Measure the applied value only when we actually elected a
	// concrete policy.
	if (applied_schema && video_decision.geometry.effective != NXCOMPAT_VIDEO_ASPECT_ENGINE) {
		nxcompat_video_aspect measured;
		if (nxcompat_video_aspect_from_string(applied_schema, &measured) == 0) {
			got.effective = measured;
		}
	}
	// The content rect the engine really presents for this effective policy on
	// the real drawable, recomputed from the measured values (never copied
	// from the decision).
	nxcompat_video_decision measured_rect;
	if (nxcompat_video_content_rect(got.effective, NX_TEARSCAPE_SOURCE_W, NX_TEARSCAPE_SOURCE_H,
				fb_size.x, fb_size.y, &measured_rect) == 0) {
		got.content = measured_rect.content;
	}

	char line[512];
	const int match = nxcompat_video_readback_check(&video_decision, &got, line, sizeof(line));
	print_line(vformat("nx/video: %s", String::utf8(line)));
	if (match != 0) {
		WARN_PRINT("nx/video: aspect readback does NOT match the elected decision.");
	}
	const char *path = getenv("NXGEOMETRY_RECEIPT");
	if (path && path[0]) {
		(void)nx_sdl2_geometry_write_line(path, line);
	}
}

void DisplayServerFBDev::publish_health_after_iteration() {
	// Drain failures and mapped quit after the current input batch. Health is
	// published only after a real frame from the preceding completed iteration.
	if (!nxgl_godot_frame_proof_health_allowed(&frame_proof)) {
		(void)nxgl_godot_frame_proof_consume_close(&frame_proof);
		_fail_runtime("nx/video: conclusive frame-proof failure blocks health.",
				nxgl_godot_frame_proof_exit_status(&frame_proof));
		return;
	}
	if (nxgptk_godot_consume_fatal_request()) {
		_fail_runtime("nx/input: GPTK delivery failed closed.", EXIT_FAILURE);
		return;
	}
	if (nxgptk_godot_consume_quit_request()) {
		request_close();
	}
	if (!health_blocked && frame_presented.load(std::memory_order_acquire)) {
		_publish_health_receipt();
	}
}

void DisplayServerFBDev::_fail_runtime(const char *p_reason, int p_status) {
	const bool first_failure = !health_blocked;
	health_blocked = true;
	_revoke_health_receipt();
	OS::get_singleton()->set_exit_code(p_status > 0 ? p_status : EXIT_FAILURE);
	if (first_failure) {
		ERR_PRINT(String(p_reason));
		request_close();
	}
}

void DisplayServerFBDev::_publish_health_receipt() {
	if (health_published || health_blocked) {
		return;
	}
	health_published = true;

	const char *path = getenv("NXBOOTSTRAP_HEALTH_FILE");
	const char *schema = getenv("NXBOOTSTRAP_HEALTH_SCHEMA");
	const char *schema_version = getenv("NXBOOTSTRAP_HEALTH_SCHEMA_VERSION");
	const char *run_id = getenv("NXBOOTSTRAP_HEALTH_RUN_ID");
	const char *generation = getenv("NXBOOTSTRAP_HEALTH_GENERATION");
	const char *port_id = getenv("NXBOOTSTRAP_HEALTH_PORT_ID");
	if (!path || !path[0] || !schema || !schema_version || !run_id || !generation || !port_id) {
		return;
	}

	char line[1024];
	int written = snprintf(line, sizeof(line),
			"{\"schema\":\"%s\",\"schema_version\":%s,\"run_id\":\"%s\","
			"\"generation\":\"%s\",\"port_id\":\"%s\",\"status\":\"ready\"}\n",
			schema, schema_version, run_id, generation, port_id);
	if (written <= 0 || written >= (int)sizeof(line)) {
		ERR_PRINT("nx/health: receipt does not fit its buffer.");
		return;
	}

	char temporary[1152];
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%d", path, (int)getpid()) >= (int)sizeof(temporary)) {
		ERR_PRINT("nx/health: receipt temporary path does not fit.");
		return;
	}
	mode_t previous_mask = umask(077);
	int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	umask(previous_mask);
	if (fd < 0) {
		ERR_PRINT(vformat("nx/health: cannot create the receipt (%d).", (int)errno));
		return;
	}
	bool ok = true;
	int offset = 0;
	while (offset < written) {
		ssize_t chunk = write(fd, line + offset, written - offset);
		if (chunk <= 0) {
			ok = false;
			break;
		}
		offset += (int)chunk;
	}
	if (fchmod(fd, 0600) != 0) {
		ok = false;
	}
	close(fd);
	if (!ok || rename(temporary, path) != 0) {
		unlink(temporary);
		ERR_PRINT("nx/health: could not publish the run-bound receipt.");
		return;
	}
	if (snprintf(health_receipt_path, sizeof(health_receipt_path), "%s", path) >=
			(int)sizeof(health_receipt_path)) {
		/* The path already fit the larger temporary buffer, so this is only a
		 * defensive invariant. Never retain an unrevocable success receipt. */
		unlink(path);
		health_receipt_path[0] = '\0';
		health_blocked = true;
		ERR_PRINT("nx/health: published path cannot be retained for revocation.");
		return;
	}
	print_line("nx/health: run-bound ready receipt published.");
}

void DisplayServerFBDev::_revoke_health_receipt() {
	/* Never reread the environment here: game code could have changed it.
	 * Revoke only the exact path retained after our successful atomic rename. */
	if (!health_published || health_receipt_path[0] == '\0') {
		return;
	}
	if (unlink(health_receipt_path) != 0 && errno != ENOENT) {
		ERR_PRINT(vformat("nx/health: could not revoke failed-run receipt (%d).", (int)errno));
	} else {
		print_line("nx/health: failed-run receipt revoked.");
	}
	health_receipt_path[0] = '\0';
}

void DisplayServerFBDev::register_fbdev_driver() {
	register_create_function("fbdev", create_func_fbdev, get_rendering_drivers_func_fbdev);
}

Error DisplayServerFBDev::_initialize_fbdev() {
#ifdef GLES3_ENABLED
	if (fb_size.x <= 0 || fb_size.y <= 0) {
		print_line("fbdev: no framebuffer geometry available.");
		return ERR_UNAVAILABLE;
	}
	fbdev_window.width = (uint16_t)fb_size.x;
	fbdev_window.height = (uint16_t)fb_size.y;
	static EGLNativeDisplayType nx_native_display = EGL_DEFAULT_DISPLAY;
	egl_manager = memnew(GLManagerEGL_FBDev);
	if (egl_manager->initialize(&nx_native_display) != OK || egl_manager->open_display(&nx_native_display) != OK) {
		memdelete(egl_manager);
		egl_manager = nullptr;
		return ERR_UNAVAILABLE;
	}
	static void *nx_native_window = nullptr;
	nx_native_window = &fbdev_window;
	if (egl_manager->window_create(MAIN_WINDOW_ID, &nx_native_display, &nx_native_window, fb_size.x, fb_size.y) != OK) {
		memdelete(egl_manager);
		egl_manager = nullptr;
		return ERR_UNAVAILABLE;
	}
	egl_manager->window_make_current(MAIN_WINDOW_ID);
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

Error DisplayServerFBDev::_initialize_sdl2() {
#ifdef GLES3_ENABLED
	if (!sdl_init_video()) {
		return ERR_UNAVAILABLE;
	}
	const char *native_gles3 = getenv("NX_TEARSCAPE_NATIVE_GLES3");
	const int gles_major = native_gles3 && strcmp(native_gles3, "1") == 0 ? 3 : 2;
	struct SdlFormat {
		int alpha;
		int depth;
		int stencil;
	};
	static const SdlFormat ladder[] = {
		{ 8, 24, 8 },
		{ 8, 16, 8 },
		{ 8, 16, 0 },
		{ 0, 16, 0 },
		{ 0, 0, 0 },
	};

	// The raw framebuffer size is NOT the window geometry on a compositor:
	// request what SDL's display authority publishes for this session.
	sdl_geometry_prepare();
	nx_sdl2_geometry_set_raw_fb(&sdl_geometry, raw_fb_size.x, raw_fb_size.y);
	int request_w = 0;
	int request_h = 0;
	nx_sdl2_geometry_initial_request(&sdl_geometry, &request_w, &request_h);
	for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
		const SdlFormat &format = ladder[i];
		sdl_api.GL_SetAttribute(NX_SDL_GL_CONTEXT_PROFILE_MASK, NX_SDL_GL_CONTEXT_PROFILE_ES);
		sdl_api.GL_SetAttribute(NX_SDL_GL_CONTEXT_MAJOR_VERSION, gles_major);
		sdl_api.GL_SetAttribute(NX_SDL_GL_CONTEXT_MINOR_VERSION, 0);
		sdl_api.GL_SetAttribute(NX_SDL_GL_RED_SIZE, 8);
		sdl_api.GL_SetAttribute(NX_SDL_GL_GREEN_SIZE, 8);
		sdl_api.GL_SetAttribute(NX_SDL_GL_BLUE_SIZE, 8);
		sdl_api.GL_SetAttribute(NX_SDL_GL_ALPHA_SIZE, format.alpha);
		sdl_api.GL_SetAttribute(NX_SDL_GL_DEPTH_SIZE, format.depth);
		sdl_api.GL_SetAttribute(NX_SDL_GL_STENCIL_SIZE, format.stencil);
		sdl_api.GL_SetAttribute(NX_SDL_GL_DOUBLEBUFFER, 1);
		sdl_window = sdl_api.CreateWindow("Tearscape",
				NX_SDL_WINDOWPOS_CENTERED, NX_SDL_WINDOWPOS_CENTERED,
				request_w, request_h,
				NX_SDL_WINDOW_OPENGL | NX_SDL_WINDOW_FULLSCREEN_DESKTOP);
		if (!sdl_window) {
			print_line(vformat("nx/sdl2: window a%d/d%d/s%d refused: %s",
					format.alpha, format.depth, format.stencil, String(sdl_error())));
			continue;
		}
		sdl_context = sdl_api.GL_CreateContext(sdl_window);
		if (!sdl_context) {
			print_line(vformat("nx/sdl2: ES%d context a%d/d%d/s%d refused: %s",
					gles_major, format.alpha, format.depth, format.stencil, String(sdl_error())));
			sdl_api.DestroyWindow(sdl_window);
			sdl_window = nullptr;
			continue;
		}
		print_line(vformat("nx/sdl2: ES%d config alpha=%d depth=%d stencil=%d",
				gles_major, format.alpha, format.depth, format.stencil));
		break;
	}
	if (!sdl_window || !sdl_context || sdl_api.GL_MakeCurrent(sdl_window, sdl_context) != 0) {
		print_line(vformat("nx/sdl2: no usable GL context: %s", String(sdl_error())));
		sdl_reset_failed_video();
		return ERR_UNAVAILABLE;
	}
	if (sdl_api.ShowCursor) {
		sdl_api.ShowCursor(0);
	}

	// Ensure fullscreen from inside the runtime and wait (bounded) for the
	// first authoritative configure. On drivers whose first query is already
	// authoritative (KMSDRM) the wait returns at once with configure_events=0.
	if (nx_sdl2_geometry_attach_window(&sdl_geometry, sdl_window,
				NX_SDL2_GEOMETRY_DEFAULT_TIMEOUT_MS) != 0) {
		sdl_reset_failed_video();
		return ERR_UNAVAILABLE;
	}
	fb_size = Size2i(sdl_geometry.logical_w, sdl_geometry.logical_h);
	sdl_api.GL_SetSwapInterval(use_vsync ? 1 : 0);

#if defined(GLAD_ENABLED) && !defined(EGL_STATIC)
	// SDL owns the context, but Godot's GLES loader still needs the process EGL
	// resolver. The port-local EGL shim forwards it to the same firmware stack.
	if (!gladLoaderLoadEGL(EGL_NO_DISPLAY)) {
		print_line("nx/sdl2: could not load the EGL resolver for Godot GLAD.");
		sdl_reset_failed_video();
		return ERR_UNAVAILABLE;
	}
#endif
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

DisplayServerFBDev::DisplayServerFBDev(const String &p_rendering_driver, Error &r_error) {
	(void)nxgl_godot_frame_proof_begin(&frame_proof);
	print_line(vformat("nx/video: frame-proof runtime=%s",
			nxgl_godot_frame_proof_marker()));
	r_error = OK;
	// Raw panel geometry. Authoritative for the fbdev/EGL provider only; the
	// SDL2 provider replaces fb_size with SDL's configured drawable.
	raw_fb_size = Size2i(0, 0);
	int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		struct fb_var_screeninfo vinfo;
		if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
			raw_fb_size = Size2i(vinfo.xres, vinfo.yres);
		}
		close(fd);
	}
	fb_size = raw_fb_size;
	if (raw_fb_size.x > 0 && raw_fb_size.y > 0) {
		print_line(vformat("nx/video: framebuffer geometry %dx%d", raw_fb_size.x, raw_fb_size.y));
	}

#ifdef GLES3_ENABLED
	if (p_rendering_driver == "opengl3_es") {
		const char *forced = getenv("NX_TEARSCAPE_VIDEO");
		String choice = forced ? String(forced).strip_edges().to_lower() : String("auto");
		if (choice.is_empty()) {
			choice = "auto";
		}
		Provider order[2];
		int order_count = 0;
		if (choice == "fbdev") {
			order[order_count++] = PROVIDER_FBDEV;
		} else if (choice == "sdl2") {
			order[order_count++] = PROVIDER_SDL2;
		} else {
			if (choice != "auto") {
				WARN_PRINT(vformat("nx/video: unknown NX_TEARSCAPE_VIDEO='%s', using auto.", choice));
			}
			bool drm = _drm_capability_present();
			bool sdl = sdl_load();
			print_line(vformat("nx/video: capability probe drm=%s sdl2=%s",
					drm ? "yes" : "no", sdl ? "yes" : "no"));
			if (drm && sdl) {
				order[order_count++] = PROVIDER_SDL2;
				order[order_count++] = PROVIDER_FBDEV;
			} else {
				order[order_count++] = PROVIDER_FBDEV;
				if (sdl) {
					order[order_count++] = PROVIDER_SDL2;
				}
			}
		}

		for (int i = 0; i < order_count; i++) {
			// Each attempt starts from the raw panel geometry; a failed SDL2
			// attempt must not leave its size behind for the fbdev provider.
			fb_size = raw_fb_size;
			Error error = order[i] == PROVIDER_SDL2 ? _initialize_sdl2() : _initialize_fbdev();
			if (error == OK) {
				provider = order[i];
				if (nxgl_godot_frame_proof_context(
						&frame_proof, nxgl_godot_gl_resolve, fb_size.x, fb_size.y,
						_provider_name(provider), nullptr, nullptr) != 0) {
					print_line("nx/video: frame-proof lifecycle rejected the live context.");
					if (provider == PROVIDER_SDL2) {
						sdl_reset_failed_video();
					} else if (egl_manager) {
						memdelete(egl_manager);
						egl_manager = nullptr;
					}
					provider = PROVIDER_NONE;
					continue;
				}
				print_line(vformat("nx/video: provider '%s' drawable %dx%d",
						String(_provider_name(provider)), fb_size.x, fb_size.y));
				_write_geometry_receipt_init();
				_apply_aspect_policy();
				RasterizerGLES3::make_current(false);
				return;
			}
			print_line(vformat("nx/video: provider '%s' unavailable.", String(_provider_name(order[i]))));
		}
		r_error = ERR_UNAVAILABLE;
		ERR_PRINT("nx/video: no presentation provider could be initialized.");
	}
#endif
}

DisplayServerFBDev::~DisplayServerFBDev() {
	if (nxgl_godot_frame_proof_stop(&frame_proof) == NXGL_GODOT_FRAME_PROOF_FATAL) {
		health_blocked = true;
		_revoke_health_receipt();
		OS::get_singleton()->set_exit_code(
				nxgl_godot_frame_proof_exit_status(&frame_proof));
	}
#ifdef GLES3_ENABLED
	if (egl_manager) {
		memdelete(egl_manager);
		egl_manager = nullptr;
	}
#endif
	nx_sdl2_geometry_receipt_close(&sdl_geometry);
}

#endif // FBDEV_ENABLED
