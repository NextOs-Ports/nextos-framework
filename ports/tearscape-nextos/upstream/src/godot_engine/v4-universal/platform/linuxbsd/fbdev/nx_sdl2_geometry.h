/* SPDX-License-Identifier: GPL-3.0-only */
// NextOS SDL2 window geometry authority (Tearscape 0.2.16).
//
// On a compositor session (SDL2 video driver "wayland") the raw /dev/fb0
// geometry describes the PANEL (portrait on a Retroid-class handheld) while
// the compositor publishes a LOGICAL output (landscape). A display server that
// requests the raw panel size, never pumps SDL events and never waits for the
// first authoritative configure keeps rendering at the wrong geometry.
//
// This unit is the single owner of the SDL2 window geometry used by the fbdev
// display server when the SDL2 provider is active:
//
//   * the initial request comes from SDL's own display authority
//     (SDL_GetDisplayBounds / SDL_GetCurrentDisplayMode /
//     SDL_GetDisplayUsableBounds), never from /dev/fb0;
//   * the SDL_APP_ID hint and SDL_VIDEO_WAYLAND_WMCLASS are derived from the
//     environment or the executable basename before the video subsystem is
//     initialized;
//   * after the window exists, fullscreen is ensured from inside the runtime
//     and the first authoritative configure is awaited for a bounded time;
//   * once per frame the live drawable is re-read and a change is reported so
//     the engine can resize its viewport;
//   * a JSON-lines receipt ("nx-geometry-proof/1") records all of the above.
//
// Everything reaches SDL through the dlsym'd table below. Every entry is
// optional: a missing symbol degrades to a logged fallback, never a crash.
// The unit has no Godot, SDL header or libwayland dependency so a host test
// can drive it against a fake libSDL2. It is capability-based: it never tests
// device names, CFW names, VID/PID or fixed resolutions.
#pragma once

#include <stdint.h>
#include <stdio.h>

typedef struct NxSDL_Window NxSDL_Window;

struct NxSdl2GeometryRect {
	int x;
	int y;
	int w;
	int h;
};

// SDL2 SDL_DisplayMode layout (ABI-stable across SDL 2.0.x).
struct NxSdl2GeometryDisplayMode {
	uint32_t format;
	int w;
	int h;
	int refresh_rate;
	void *driverdata;
};

// Subset of the SDL2 C API the geometry authority consumes. Any pointer may be
// null; the unit logs the fallback it takes.
struct NxSdl2GeometryApi {
	const char *(*GetCurrentVideoDriver)(void) = nullptr;
	int (*SetHint)(const char *, const char *) = nullptr;
	int (*GetDisplayBounds)(int, NxSdl2GeometryRect *) = nullptr;
	int (*GetDisplayUsableBounds)(int, NxSdl2GeometryRect *) = nullptr;
	int (*GetCurrentDisplayMode)(int, NxSdl2GeometryDisplayMode *) = nullptr;
	int (*GetWindowDisplayIndex)(NxSDL_Window *) = nullptr;
	void (*PumpEvents)(void) = nullptr;
	int (*PeepEvents)(void *, int, int, uint32_t, uint32_t) = nullptr;
	void (*FlushEvents)(uint32_t, uint32_t) = nullptr;
	void (*GL_GetDrawableSize)(NxSDL_Window *, int *, int *) = nullptr;
	void (*GetWindowSize)(NxSDL_Window *, int *, int *) = nullptr;
	uint32_t (*GetWindowFlags)(NxSDL_Window *) = nullptr;
	int (*SetWindowFullscreen)(NxSDL_Window *, uint32_t) = nullptr;
};

typedef void (*NxSdl2GeometryLog)(void *userdata, const char *line);

enum {
	NX_SDL2_GEOMETRY_DEFAULT_TIMEOUT_MS = 1500,
	NX_SDL2_GEOMETRY_STEP_MS = 10,
	NX_SDL2_GEOMETRY_FALLBACK_W = 640,
	NX_SDL2_GEOMETRY_FALLBACK_H = 480,
};

#define NX_SDL2_GEOMETRY_RECEIPT_SCHEMA "nx-geometry-proof/1"

struct NxSdl2Geometry {
	NxSdl2GeometryApi api;
	NxSdl2GeometryLog log = nullptr;
	void *log_userdata = nullptr;
	NxSDL_Window *window = nullptr;

	char video_driver[32] = {};
	char app_id[128] = {};
	char app_id_source[32] = {};
	bool app_id_hint_set = false;
	bool app_id_env_set = false;

	// Raw panel geometry as reported by the framebuffer node. Informational
	// for the SDL2 provider: it is NEVER used as the logical window size.
	int raw_fb_w = 0;
	int raw_fb_h = 0;

	int display_index = 0;
	int display_w = 0;
	int display_h = 0;
	int requested_w = 0;
	int requested_h = 0;
	int configured_w = 0; // SDL_GetWindowSize after the configure wait
	int configured_h = 0;
	int drawable_w = 0; // SDL_GL_GetDrawableSize after the configure wait
	int drawable_h = 0;
	int logical_w = 0; // live authoritative size the engine must use
	int logical_h = 0;

	bool fullscreen = false;
	bool fullscreen_forced = false;
	int configure_events = 0;
	int wait_ms = 0;
	bool timed_out = false;
	bool configured = false;
	int resize_count = 0;

	FILE *receipt = nullptr;
	bool init_written = false;
};

// Bind the optional table from an already-open SDL2 handle. `resolve` is the
// caller's symbol lookup (a dlsym wrapper); the unit itself never links dlfcn.
void nx_sdl2_geometry_bind(NxSdl2GeometryApi *api, void *handle,
		void *(*resolve)(void *handle, const char *name));

void nx_sdl2_geometry_init(NxSdl2Geometry *g, const NxSdl2GeometryApi *api,
		NxSdl2GeometryLog log, void *log_userdata);

void nx_sdl2_geometry_set_raw_fb(NxSdl2Geometry *g, int w, int h);

// Before SDL_InitSubSystem(VIDEO). Resolves the app id from env SDL_APP_ID,
// else env SDL_VIDEO_WAYLAND_WMCLASS, else the executable basename; sets the
// SDL_APP_ID hint when SDL_SetHint exists and exports
// SDL_VIDEO_WAYLAND_WMCLASS without overwriting an inherited value.
void nx_sdl2_geometry_prepare_app_id(NxSdl2Geometry *g);

// After the video subsystem is up, before SDL_CreateWindow: the size to
// request, from SDL's display authority (fallback 640x480).
void nx_sdl2_geometry_initial_request(NxSdl2Geometry *g, int *w, int *h);

// After the window and context exist. Records the driver, refreshes the
// display bounds for the window's display, ensures fullscreen and waits up to
// `timeout_ms` for the first authoritative configure. Returns 0 when a usable
// non-zero size is known (possibly after a logged timeout), -1 otherwise.
int nx_sdl2_geometry_attach_window(NxSdl2Geometry *g, NxSDL_Window *window,
		int timeout_ms);

// Once per frame. Pumps SDL events (this process owns the SDL2 queue: the
// engine's joypad path lives in a separately linked SDL3) and re-reads the
// drawable. Returns true when logical_w/logical_h changed.
bool nx_sdl2_geometry_poll(NxSdl2Geometry *g);

// Receipt ("nx-geometry-proof/1", JSON lines, append, line-buffered).
bool nx_sdl2_geometry_receipt_open(NxSdl2Geometry *g, const char *path);
void nx_sdl2_geometry_receipt_close(NxSdl2Geometry *g);
void nx_sdl2_geometry_write_init(NxSdl2Geometry *g, const char *provider);
void nx_sdl2_geometry_write_resize(NxSdl2Geometry *g, bool notified);

// The raw fbdev/EGL provider proves non-regression with the same schema: the
// panel geometry is authoritative there, so every size equals the raw one.
bool nx_sdl2_geometry_write_fbdev_init(const char *path, int raw_w, int raw_h);

/* NEXTOS_SETTINGS/2 aspect decision (nx-aspect-policy/1): one "aspect" line
 * appended to the same receipt, for either provider, right after the
 * provider is accepted and before the engine builds the root window. */
bool nx_sdl2_geometry_write_aspect(const char *path, const char *provider,
		const char *requested, const char *project_value, const char *effective,
		const char *source, int changed, int drawable_w, int drawable_h,
		int content_x, int content_y, int content_w, int content_h);

/* FV3: one NX-VIDEO-READBACK/1 line stored verbatim in the same receipt. */
bool nx_sdl2_geometry_write_line(const char *path, const char *line);

// Escapes a string for a JSON literal (without quotes). Exposed for tests.
void nx_sdl2_geometry_json_escape(const char *in, char *out, size_t cap);
