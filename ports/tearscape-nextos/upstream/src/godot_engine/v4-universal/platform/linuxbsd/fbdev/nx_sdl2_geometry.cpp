/* SPDX-License-Identifier: GPL-3.0-only */
// NextOS SDL2 window geometry authority. See nx_sdl2_geometry.h.
#include "nx_sdl2_geometry.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {

// SDL2 ABI constants (SDL_events.h / SDL_video.h, stable across 2.0.x).
enum : uint32_t {
	NX_SDL_FIRSTEVENT = 0u,
	NX_SDL_WINDOWEVENT = 0x200u,
	NX_SDL_LASTEVENT = 0xFFFFu,
	NX_SDL_WINDOWEVENT_RESIZED = 5u,
	NX_SDL_WINDOWEVENT_SIZE_CHANGED = 6u,
	NX_SDL_GETEVENT = 2u,
	NX_SDL_WINDOW_FULLSCREEN = 0x00000001u,
	NX_SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001u,
	NX_SDL_EVENT_SIZE = 56u,
};

struct NxSdlEventView {
	uint32_t type;
	uint32_t timestamp;
	uint32_t windowID;
	uint8_t event;
	uint8_t padding1;
	uint8_t padding2;
	uint8_t padding3;
	int32_t data1;
	int32_t data2;
};

void logf(NxSdl2Geometry *g, const char *format, ...) {
	if (!g || !g->log) {
		return;
	}
	char line[512];
	va_list args;
	va_start(args, format);
	int written = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (written < 0) {
		return;
	}
	g->log(g->log_userdata, line);
}

int64_t monotonic_ms() {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(int ms) {
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
}

void copy_bounded(char *dst, size_t cap, const char *src) {
	size_t i = 0;
	if (src) {
		for (; src[i] && i + 1 < cap; i++) {
			dst[i] = src[i];
		}
	}
	dst[i] = 0;
}

// Pump once and drain the queue. Window size events are counted; everything
// else is discarded (the engine's input never comes through this SDL2 queue).
int drain_events(NxSdl2Geometry *g) {
	int size_events = 0;
	if (g->api.PumpEvents) {
		g->api.PumpEvents();
	}
	if (g->api.PeepEvents) {
		unsigned char buffer[16 * NX_SDL_EVENT_SIZE];
		for (int guard = 0; guard < 256; guard++) {
			int got = g->api.PeepEvents(buffer, 16, (int)NX_SDL_GETEVENT,
					NX_SDL_WINDOWEVENT, NX_SDL_WINDOWEVENT);
			if (got <= 0) {
				break;
			}
			for (int i = 0; i < got; i++) {
				NxSdlEventView view;
				memcpy(&view, buffer + (size_t)i * NX_SDL_EVENT_SIZE, sizeof(view));
				if (view.type == NX_SDL_WINDOWEVENT &&
						(view.event == NX_SDL_WINDOWEVENT_RESIZED ||
								view.event == NX_SDL_WINDOWEVENT_SIZE_CHANGED)) {
					size_events++;
				}
			}
			if (got < 16) {
				break;
			}
		}
	}
	if (g->api.FlushEvents) {
		g->api.FlushEvents(NX_SDL_FIRSTEVENT, NX_SDL_LASTEVENT);
	} else if (g->api.PeepEvents) {
		unsigned char buffer[64 * NX_SDL_EVENT_SIZE];
		for (int guard = 0; guard < 1024; guard++) {
			int got = g->api.PeepEvents(buffer, 64, (int)NX_SDL_GETEVENT,
					NX_SDL_FIRSTEVENT, NX_SDL_LASTEVENT);
			if (got < 64) {
				break;
			}
		}
	}
	g->configure_events += size_events;
	return size_events;
}

void query_sizes(NxSdl2Geometry *g, int *window_w, int *window_h,
		int *drawable_w, int *drawable_h) {
	*window_w = *window_h = *drawable_w = *drawable_h = 0;
	if (!g->window) {
		return;
	}
	if (g->api.GetWindowSize) {
		g->api.GetWindowSize(g->window, window_w, window_h);
	}
	if (g->api.GL_GetDrawableSize) {
		g->api.GL_GetDrawableSize(g->window, drawable_w, drawable_h);
	}
	if (*drawable_w <= 0 || *drawable_h <= 0) {
		*drawable_w = *window_w;
		*drawable_h = *window_h;
	}
	if (*window_w <= 0 || *window_h <= 0) {
		*window_w = *drawable_w;
		*window_h = *drawable_h;
	}
}

bool refresh_display_bounds(NxSdl2Geometry *g) {
	NxSdl2GeometryRect rect = {};
	NxSdl2GeometryDisplayMode mode = {};
	const char *source = nullptr;
	if (g->api.GetDisplayBounds && g->api.GetDisplayBounds(g->display_index, &rect) == 0 &&
			rect.w > 0 && rect.h > 0) {
		source = "SDL_GetDisplayBounds";
	} else if (g->api.GetCurrentDisplayMode &&
			g->api.GetCurrentDisplayMode(g->display_index, &mode) == 0 &&
			mode.w > 0 && mode.h > 0) {
		rect.w = mode.w;
		rect.h = mode.h;
		source = "SDL_GetCurrentDisplayMode";
	} else if (g->api.GetDisplayUsableBounds &&
			g->api.GetDisplayUsableBounds(g->display_index, &rect) == 0 &&
			rect.w > 0 && rect.h > 0) {
		source = "SDL_GetDisplayUsableBounds";
	}
	if (!source) {
		g->display_w = 0;
		g->display_h = 0;
		logf(g, "nx/sdl2: no display authority available (display %d)", g->display_index);
		return false;
	}
	g->display_w = rect.w;
	g->display_h = rect.h;
	logf(g, "nx/sdl2: display %d bounds %dx%d via %s", g->display_index,
			g->display_w, g->display_h, source);
	return true;
}

void ensure_fullscreen(NxSdl2Geometry *g) {
	if (!g->window || !g->api.GetWindowFlags) {
		logf(g, "nx/sdl2: SDL_GetWindowFlags unavailable; fullscreen state unverified");
		return;
	}
	uint32_t flags = g->api.GetWindowFlags(g->window);
	g->fullscreen = (flags & NX_SDL_WINDOW_FULLSCREEN) != 0;
	if (g->fullscreen) {
		return;
	}
	if (!g->api.SetWindowFullscreen) {
		logf(g, "nx/sdl2: window is not fullscreen and SDL_SetWindowFullscreen is unavailable");
		return;
	}
	int rc = g->api.SetWindowFullscreen(g->window, NX_SDL_WINDOW_FULLSCREEN_DESKTOP);
	flags = g->api.GetWindowFlags(g->window);
	g->fullscreen = (flags & NX_SDL_WINDOW_FULLSCREEN) != 0;
	g->fullscreen_forced = true;
	logf(g, "nx/sdl2: fullscreen was not set by the compositor; SDL_SetWindowFullscreen(DESKTOP) rc=%d now %s",
			rc, g->fullscreen ? "fullscreen" : "windowed");
}

void write_json_string(FILE *stream, const char *value) {
	char escaped[512];
	nx_sdl2_geometry_json_escape(value ? value : "", escaped, sizeof(escaped));
	fputc('"', stream);
	fputs(escaped, stream);
	fputc('"', stream);
}

void write_env_string_field(FILE *stream, const char *key, const char *env_name) {
	const char *value = getenv(env_name);
	fprintf(stream, ",\"%s\":", key);
	write_json_string(stream, value ? value : "");
}

FILE *open_receipt(const char *path) {
	if (!path || !path[0]) {
		return nullptr;
	}
	FILE *stream = fopen(path, "a");
	if (!stream) {
		return nullptr;
	}
	setvbuf(stream, nullptr, _IOLBF, 0);
	return stream;
}

void write_init_line(FILE *stream, const char *provider, const char *video_driver,
		const char *app_id, const char *app_id_source, int raw_w, int raw_h,
		int display_w, int display_h, int requested_w, int requested_h,
		int configured_w, int configured_h, int drawable_w, int drawable_h,
		bool fullscreen, int configure_events, int wait_ms, bool timed_out) {
	fputs("{\"schema\":\"" NX_SDL2_GEOMETRY_RECEIPT_SCHEMA "\",\"kind\":\"init\",\"provider\":", stream);
	write_json_string(stream, provider);
	fputs(",\"video_driver\":", stream);
	write_json_string(stream, video_driver);
	fputs(",\"app_id\":", stream);
	write_json_string(stream, app_id);
	fputs(",\"app_id_source\":", stream);
	write_json_string(stream, app_id_source);
	fprintf(stream,
			",\"raw_fb_w\":%d,\"raw_fb_h\":%d,\"display_w\":%d,\"display_h\":%d,"
			"\"requested_w\":%d,\"requested_h\":%d,\"configured_w\":%d,\"configured_h\":%d,"
			"\"drawable_w\":%d,\"drawable_h\":%d,\"fullscreen\":%s,\"configure_events\":%d,"
			"\"wait_ms\":%d,\"timed_out\":%s",
			raw_w, raw_h, display_w, display_h, requested_w, requested_h,
			configured_w, configured_h, drawable_w, drawable_h,
			fullscreen ? "true" : "false", configure_events, wait_ms,
			timed_out ? "true" : "false");
	write_env_string_field(stream, "run_id", "NXBOOTSTRAP_HEALTH_RUN_ID");
	write_env_string_field(stream, "generation", "NXBOOTSTRAP_HEALTH_GENERATION");
	write_env_string_field(stream, "port_id", "NXBOOTSTRAP_HEALTH_PORT_ID");
	fputs("}\n", stream);
	fflush(stream);
}

} // namespace

void nx_sdl2_geometry_json_escape(const char *in, char *out, size_t cap) {
	size_t o = 0;
	if (cap == 0) {
		return;
	}
	for (size_t i = 0; in && in[i]; i++) {
		unsigned char c = (unsigned char)in[i];
		char piece[8];
		size_t len;
		if (c == '"' || c == '\\') {
			piece[0] = '\\';
			piece[1] = (char)c;
			len = 2;
		} else if (c < 0x20) {
			len = (size_t)snprintf(piece, sizeof(piece), "\\u%04x", c);
		} else {
			piece[0] = (char)c;
			len = 1;
		}
		if (o + len + 1 > cap) {
			break;
		}
		memcpy(out + o, piece, len);
		o += len;
	}
	out[o] = 0;
}

void nx_sdl2_geometry_bind(NxSdl2GeometryApi *api, void *handle,
		void *(*resolve)(void *handle, const char *name)) {
	if (!api || !resolve) {
		return;
	}
#define NX_BIND(field, symbol) *(void **)(&api->field) = resolve(handle, symbol)
	NX_BIND(GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver");
	NX_BIND(SetHint, "SDL_SetHint");
	NX_BIND(GetDisplayBounds, "SDL_GetDisplayBounds");
	NX_BIND(GetDisplayUsableBounds, "SDL_GetDisplayUsableBounds");
	NX_BIND(GetCurrentDisplayMode, "SDL_GetCurrentDisplayMode");
	NX_BIND(GetWindowDisplayIndex, "SDL_GetWindowDisplayIndex");
	NX_BIND(PumpEvents, "SDL_PumpEvents");
	NX_BIND(PeepEvents, "SDL_PeepEvents");
	NX_BIND(FlushEvents, "SDL_FlushEvents");
	NX_BIND(GL_GetDrawableSize, "SDL_GL_GetDrawableSize");
	NX_BIND(GetWindowSize, "SDL_GetWindowSize");
	NX_BIND(GetWindowFlags, "SDL_GetWindowFlags");
	NX_BIND(SetWindowFullscreen, "SDL_SetWindowFullscreen");
#undef NX_BIND
}

void nx_sdl2_geometry_init(NxSdl2Geometry *g, const NxSdl2GeometryApi *api,
		NxSdl2GeometryLog log, void *log_userdata) {
	if (!g) {
		return;
	}
	FILE *receipt = g->receipt;
	*g = NxSdl2Geometry();
	g->receipt = receipt;
	if (api) {
		g->api = *api;
	}
	g->log = log;
	g->log_userdata = log_userdata;
}

void nx_sdl2_geometry_set_raw_fb(NxSdl2Geometry *g, int w, int h) {
	if (!g) {
		return;
	}
	g->raw_fb_w = w > 0 ? w : 0;
	g->raw_fb_h = h > 0 ? h : 0;
}

void nx_sdl2_geometry_prepare_app_id(NxSdl2Geometry *g) {
	if (!g) {
		return;
	}
	const char *from_env = getenv("SDL_APP_ID");
	const char *from_wmclass = getenv("SDL_VIDEO_WAYLAND_WMCLASS");
	if (from_env && from_env[0]) {
		copy_bounded(g->app_id, sizeof(g->app_id), from_env);
		copy_bounded(g->app_id_source, sizeof(g->app_id_source), "SDL_APP_ID");
	} else if (from_wmclass && from_wmclass[0]) {
		copy_bounded(g->app_id, sizeof(g->app_id), from_wmclass);
		copy_bounded(g->app_id_source, sizeof(g->app_id_source), "SDL_VIDEO_WAYLAND_WMCLASS");
	} else {
		char exe[1024];
		ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
		if (len > 0) {
			exe[len] = 0;
			const char *base = strrchr(exe, '/');
			base = base ? base + 1 : exe;
			copy_bounded(g->app_id, sizeof(g->app_id), base);
			copy_bounded(g->app_id_source, sizeof(g->app_id_source), "exe");
		}
	}
	if (!g->app_id[0]) {
		copy_bounded(g->app_id_source, sizeof(g->app_id_source), "none");
		logf(g, "nx/sdl2: no app id source (SDL_APP_ID, SDL_VIDEO_WAYLAND_WMCLASS, /proc/self/exe)");
		return;
	}
	if (g->api.SetHint) {
		g->app_id_hint_set = g->api.SetHint("SDL_APP_ID", g->app_id) != 0;
	} else {
		logf(g, "nx/sdl2: SDL_SetHint unavailable; SDL_APP_ID hint not set");
	}
	// overwrite=0: an inherited value always wins.
	g->app_id_env_set = setenv("SDL_VIDEO_WAYLAND_WMCLASS", g->app_id, 0) == 0;
	logf(g, "nx/sdl2: app id '%s' from %s (hint %s, wmclass env %s)", g->app_id,
			g->app_id_source, g->app_id_hint_set ? "set" : "not set",
			g->app_id_env_set ? "exported" : "unchanged");
}

void nx_sdl2_geometry_initial_request(NxSdl2Geometry *g, int *w, int *h) {
	if (!w || !h) {
		return;
	}
	*w = NX_SDL2_GEOMETRY_FALLBACK_W;
	*h = NX_SDL2_GEOMETRY_FALLBACK_H;
	if (!g) {
		return;
	}
	g->display_index = 0;
	if (refresh_display_bounds(g)) {
		*w = g->display_w;
		*h = g->display_h;
	} else {
		logf(g, "nx/sdl2: requesting fallback %dx%d (raw fb %dx%d is not a window authority)",
				*w, *h, g->raw_fb_w, g->raw_fb_h);
	}
	g->requested_w = *w;
	g->requested_h = *h;
}

int nx_sdl2_geometry_attach_window(NxSdl2Geometry *g, NxSDL_Window *window,
		int timeout_ms) {
	if (!g || !window) {
		return -1;
	}
	g->window = window;
	g->configure_events = 0;
	g->timed_out = false;
	g->configured = false;
	copy_bounded(g->video_driver, sizeof(g->video_driver),
			g->api.GetCurrentVideoDriver ? g->api.GetCurrentVideoDriver() : nullptr);

	if (g->api.GetWindowDisplayIndex) {
		int index = g->api.GetWindowDisplayIndex(window);
		if (index >= 0 && index != g->display_index) {
			g->display_index = index;
			refresh_display_bounds(g);
		}
	}
	ensure_fullscreen(g);

	if (timeout_ms < 0) {
		timeout_ms = NX_SDL2_GEOMETRY_DEFAULT_TIMEOUT_MS;
	}
	const int64_t started = monotonic_ms();
	int prev_w = -1;
	int prev_h = -1;
	int window_w = 0;
	int window_h = 0;
	int drawable_w = 0;
	int drawable_h = 0;
	for (;;) {
		drain_events(g);
		query_sizes(g, &window_w, &window_h, &drawable_w, &drawable_h);
		const bool nonzero = drawable_w > 0 && drawable_h > 0;
		const bool matches_display = nonzero && g->display_w > 0 &&
				drawable_w == g->display_w && drawable_h == g->display_h;
		const bool stable = nonzero && drawable_w == prev_w && drawable_h == prev_h;
		if (matches_display || (stable && (g->configure_events > 0 || g->display_w <= 0))) {
			g->configured = true;
			break;
		}
		const int64_t elapsed = monotonic_ms() - started;
		if (elapsed >= timeout_ms) {
			g->timed_out = true;
			break;
		}
		prev_w = drawable_w;
		prev_h = drawable_h;
		sleep_ms(NX_SDL2_GEOMETRY_STEP_MS);
	}
	g->wait_ms = (int)(monotonic_ms() - started);
	g->configured_w = window_w;
	g->configured_h = window_h;
	g->drawable_w = drawable_w;
	g->drawable_h = drawable_h;
	g->logical_w = drawable_w;
	g->logical_h = drawable_h;

	if (g->timed_out) {
		logf(g, "nx/sdl2: configure wait TIMED OUT after %d ms (display %dx%d, drawable %dx%d, configure_events=%d)",
				g->wait_ms, g->display_w, g->display_h, drawable_w, drawable_h,
				g->configure_events);
	}
	logf(g, "nx/sdl2: driver '%s' display %dx%d requested %dx%d configured %dx%d drawable %dx%d configure_events=%d",
			g->video_driver, g->display_w, g->display_h, g->requested_w, g->requested_h,
			g->configured_w, g->configured_h, g->drawable_w, g->drawable_h,
			g->configure_events);
	if (g->logical_w <= 0 || g->logical_h <= 0) {
		logf(g, "nx/sdl2: firmware reported no drawable size.");
		return -1;
	}
	return 0;
}

bool nx_sdl2_geometry_poll(NxSdl2Geometry *g) {
	if (!g || !g->window) {
		return false;
	}
	drain_events(g);
	int window_w, window_h, drawable_w, drawable_h;
	query_sizes(g, &window_w, &window_h, &drawable_w, &drawable_h);
	if (drawable_w <= 0 || drawable_h <= 0) {
		return false;
	}
	if (drawable_w == g->logical_w && drawable_h == g->logical_h) {
		return false;
	}
	logf(g, "nx/sdl2: drawable changed %dx%d -> %dx%d (window %dx%d)",
			g->logical_w, g->logical_h, drawable_w, drawable_h, window_w, window_h);
	g->configured_w = window_w;
	g->configured_h = window_h;
	g->drawable_w = drawable_w;
	g->drawable_h = drawable_h;
	g->logical_w = drawable_w;
	g->logical_h = drawable_h;
	g->resize_count++;
	return true;
}

bool nx_sdl2_geometry_receipt_open(NxSdl2Geometry *g, const char *path) {
	if (!g) {
		return false;
	}
	nx_sdl2_geometry_receipt_close(g);
	g->receipt = open_receipt(path);
	if (!g->receipt) {
		if (path && path[0]) {
			logf(g, "nx/sdl2: geometry receipt unavailable (%d)", errno);
		}
		return false;
	}
	return true;
}

void nx_sdl2_geometry_receipt_close(NxSdl2Geometry *g) {
	if (g && g->receipt) {
		fclose(g->receipt);
		g->receipt = nullptr;
	}
}

void nx_sdl2_geometry_write_init(NxSdl2Geometry *g, const char *provider) {
	if (!g || !g->receipt || g->init_written) {
		return;
	}
	g->init_written = true;
	write_init_line(g->receipt, provider ? provider : "sdl2", g->video_driver,
			g->app_id, g->app_id_source, g->raw_fb_w, g->raw_fb_h,
			g->display_w, g->display_h, g->requested_w, g->requested_h,
			g->configured_w, g->configured_h, g->drawable_w, g->drawable_h,
			g->fullscreen, g->configure_events, g->wait_ms, g->timed_out);
}

void nx_sdl2_geometry_write_resize(NxSdl2Geometry *g, bool notified) {
	if (!g || !g->receipt) {
		return;
	}
	fprintf(g->receipt,
			"{\"schema\":\"" NX_SDL2_GEOMETRY_RECEIPT_SCHEMA "\",\"kind\":\"resize\","
			"\"w\":%d,\"h\":%d,\"drawable_w\":%d,\"drawable_h\":%d,\"notified\":%s}\n",
			g->logical_w, g->logical_h, g->drawable_w, g->drawable_h,
			notified ? "true" : "false");
	fflush(g->receipt);
}

bool nx_sdl2_geometry_write_aspect(const char *path, const char *provider,
		const char *requested, const char *project_value, const char *effective,
		const char *source, int changed, int drawable_w, int drawable_h,
		int content_x, int content_y, int content_w, int content_h) {
	FILE *stream = open_receipt(path);
	if (!stream) {
		return false;
	}
	char provider_e[64], requested_e[64], project_e[64], effective_e[64], source_e[64];
	nx_sdl2_geometry_json_escape(provider ? provider : "", provider_e, sizeof provider_e);
	nx_sdl2_geometry_json_escape(requested ? requested : "", requested_e, sizeof requested_e);
	nx_sdl2_geometry_json_escape(project_value ? project_value : "", project_e, sizeof project_e);
	nx_sdl2_geometry_json_escape(effective ? effective : "", effective_e, sizeof effective_e);
	nx_sdl2_geometry_json_escape(source ? source : "", source_e, sizeof source_e);
	fprintf(stream,
			"{\"schema\":\"nx-geometry-proof/1\",\"kind\":\"aspect\","
			"\"policy_schema\":\"nx-aspect-policy/2\",\"provider\":\"%s\","
			"\"requested\":\"%s\",\"project_value\":\"%s\",\"effective\":\"%s\","
			"\"source\":\"%s\",\"changed\":%s,\"drawable_w\":%d,\"drawable_h\":%d,"
			"\"content_x\":%d,\"content_y\":%d,\"content_w\":%d,\"content_h\":%d}\n",
			provider_e, requested_e, project_e, effective_e, source_e,
			changed ? "true" : "false", drawable_w, drawable_h,
			content_x, content_y, content_w, content_h);
	fclose(stream);
	return true;
}

/* FV3 readback: the NX-VIDEO-READBACK/1 line, verbatim, as one receipt
 * record. The measured verdict is the framework's; this only stores it. */
bool nx_sdl2_geometry_write_line(const char *path, const char *line) {
	FILE *stream = open_receipt(path);
	if (!stream) {
		return false;
	}
	char escaped[512];
	nx_sdl2_geometry_json_escape(line ? line : "", escaped, sizeof escaped);
	fprintf(stream,
			"{\"schema\":\"nx-geometry-proof/1\",\"kind\":\"aspect_readback\","
			"\"receipt\":\"%s\"}\n", escaped);
	fclose(stream);
	return true;
}

bool nx_sdl2_geometry_write_fbdev_init(const char *path, int raw_w, int raw_h) {
	FILE *stream = open_receipt(path);
	if (!stream) {
		return false;
	}
	write_init_line(stream, "fbdev", "", "", "", raw_w, raw_h, raw_w, raw_h,
			raw_w, raw_h, raw_w, raw_h, raw_w, raw_h, true, 0, 0, false);
	fclose(stream);
	return true;
}
