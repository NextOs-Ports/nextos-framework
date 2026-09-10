/* SPDX-License-Identifier: GPL-3.0-only */
/* Fake firmware libSDL2-2.0.so.0 for the Tearscape geometry host gate.
 *
 * Models the two shapes the port meets in the field:
 *   - a compositor session ("wayland"): the display publishes a landscape
 *     logical output, the window's drawable stays at a stale (portrait,
 *     panel-shaped) size until the first configure arrives N pumps later,
 *     which flips the drawable and queues SDL_WINDOWEVENT_SIZE_CHANGED;
 *   - a direct driver ("kmsdrm"): the drawable equals the display at once.
 *
 * Behaviour is configured through FAKE_SDL_* environment variables read at
 * SDL_InitSubSystem, so one test process can run several scenarios. Nothing in
 * here is a real SDL: only the ABI the geometry unit consumes is modelled.
 * Compiled with -DFAKE_SDL_MINIMAL every optional symbol is omitted, so the
 * unit's graceful degradation can be measured. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_EVENT_SIZE 56
#define FAKE_QUEUE_CAP 64
#define FAKE_WINDOWEVENT 0x200u
#define FAKE_WINDOWEVENT_SIZE_CHANGED 6u
#define FAKE_WINDOW_FULLSCREEN 0x1u
#define FAKE_WINDOW_FULLSCREEN_DESKTOP 0x1001u

struct FakeWindow {
	int w;
	int h;
	uint32_t flags;
};

static struct FakeWindow g_window;
static int g_window_alive;
static char g_driver[32] = "wayland";
static int g_display_w = 1920, g_display_h = 1080;
static int g_initial_w = 1080, g_initial_h = 1920;
static int g_configure_after = 5; /* pumps; <0 = never */
static int g_late_resize_at = -1; /* pumps; <0 = none */
static int g_late_w = 0, g_late_h = 0;
static int g_initial_fullscreen = 0;
static int g_no_display_bounds = 0;
static int g_pumps;
static int g_video_inited;
static int g_hint_calls;
static int g_hint_before_init = -1;
static char g_hint_app_id[128];
static int g_set_fullscreen_calls;
static uint32_t g_set_fullscreen_last;
static unsigned char g_queue[FAKE_QUEUE_CAP][FAKE_EVENT_SIZE];
static int g_queue_len;

static int env_int(const char *name, int fallback) {
	const char *value = getenv(name);
	return value && value[0] ? atoi(value) : fallback;
}

static void queue_size_changed(int w, int h) {
	unsigned char *event;
	if (g_queue_len >= FAKE_QUEUE_CAP)
		return;
	event = g_queue[g_queue_len++];
	memset(event, 0, FAKE_EVENT_SIZE);
	{
		uint32_t type = FAKE_WINDOWEVENT, window_id = 1;
		uint8_t kind = FAKE_WINDOWEVENT_SIZE_CHANGED;
		int32_t data1 = w, data2 = h;
		memcpy(event + 0, &type, 4);
		memcpy(event + 8, &window_id, 4);
		memcpy(event + 12, &kind, 1);
		memcpy(event + 16, &data1, 4);
		memcpy(event + 20, &data2, 4);
	}
}

/* --- inspection hooks for the test (not SDL) --- */
int fake_sdl_pump_count(void) { return g_pumps; }
int fake_sdl_hint_before_init(void) { return g_hint_before_init; }
const char *fake_sdl_hint_app_id(void) { return g_hint_app_id; }
int fake_sdl_set_fullscreen_calls(void) { return g_set_fullscreen_calls; }
uint32_t fake_sdl_set_fullscreen_last(void) { return g_set_fullscreen_last; }
int fake_sdl_queue_len(void) { return g_queue_len; }

/* --- SDL2 ABI subset --- */
int SDL_InitSubSystem(uint32_t flags) {
	const char *driver = getenv("FAKE_SDL_DRIVER");
	(void)flags;
	snprintf(g_driver, sizeof g_driver, "%s", driver && driver[0] ? driver : "wayland");
	g_display_w = env_int("FAKE_SDL_DISPLAY_W", 1920);
	g_display_h = env_int("FAKE_SDL_DISPLAY_H", 1080);
	g_initial_w = env_int("FAKE_SDL_INITIAL_W", g_display_w);
	g_initial_h = env_int("FAKE_SDL_INITIAL_H", g_display_h);
	g_configure_after = env_int("FAKE_SDL_CONFIGURE_AFTER_PUMPS", 0);
	g_late_resize_at = env_int("FAKE_SDL_LATE_RESIZE_AT_PUMP", -1);
	g_late_w = env_int("FAKE_SDL_LATE_W", 0);
	g_late_h = env_int("FAKE_SDL_LATE_H", 0);
	g_initial_fullscreen = env_int("FAKE_SDL_INITIAL_FULLSCREEN", 0);
	g_no_display_bounds = env_int("FAKE_SDL_NO_DISPLAY_BOUNDS", 0);
	g_pumps = 0;
	g_queue_len = 0;
	g_window_alive = 0;
	g_set_fullscreen_calls = 0;
	g_set_fullscreen_last = 0;
	if (g_hint_before_init < 0)
		g_hint_before_init = 0; /* no hint arrived before init */
	g_video_inited = 1;
	return 0;
}

void SDL_QuitSubSystem(uint32_t flags) {
	(void)flags;
	g_video_inited = 0;
	g_hint_before_init = -1;
	g_hint_calls = 0;
	g_hint_app_id[0] = 0;
}

const char *SDL_GetError(void) { return "fake sdl2"; }

const char *SDL_GetCurrentVideoDriver(void) { return g_video_inited ? g_driver : NULL; }

void *SDL_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags) {
	(void)title;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	g_window.w = g_initial_w;
	g_window.h = g_initial_h;
	/* A compositor decides fullscreen; the request flag alone does not. */
	g_window.flags = (flags & ~(uint32_t)FAKE_WINDOW_FULLSCREEN_DESKTOP);
	if (g_initial_fullscreen)
		g_window.flags |= FAKE_WINDOW_FULLSCREEN_DESKTOP;
	g_window_alive = 1;
	return &g_window;
}

void SDL_DestroyWindow(void *window) {
	(void)window;
	g_window_alive = 0;
}

void SDL_PumpEvents(void) {
	g_pumps++;
	if (g_window_alive && g_configure_after >= 0 && g_pumps == g_configure_after + 1 &&
			(g_window.w != g_display_w || g_window.h != g_display_h)) {
		g_window.w = g_display_w;
		g_window.h = g_display_h;
		queue_size_changed(g_window.w, g_window.h);
	}
	if (g_window_alive && g_late_resize_at >= 0 && g_pumps == g_late_resize_at &&
			g_late_w > 0 && g_late_h > 0) {
		g_window.w = g_late_w;
		g_window.h = g_late_h;
		queue_size_changed(g_window.w, g_window.h);
	}
}

void SDL_GL_GetDrawableSize(void *window, int *w, int *h) {
	(void)window;
	if (w) *w = g_window_alive ? g_window.w : 0;
	if (h) *h = g_window_alive ? g_window.h : 0;
}

void SDL_GetWindowSize(void *window, int *w, int *h) {
	SDL_GL_GetDrawableSize(window, w, h);
}

#ifndef FAKE_SDL_MINIMAL
int SDL_SetHint(const char *name, const char *value) {
	g_hint_calls++;
	if (name && strcmp(name, "SDL_APP_ID") == 0) {
		snprintf(g_hint_app_id, sizeof g_hint_app_id, "%s", value ? value : "");
		if (g_hint_before_init < 0)
			g_hint_before_init = !g_video_inited;
	}
	return 1;
}

int SDL_GetDisplayBounds(int index, int *rect) {
	if (g_no_display_bounds || index != 0)
		return -1;
	rect[0] = 0;
	rect[1] = 0;
	rect[2] = g_display_w;
	rect[3] = g_display_h;
	return 0;
}

int SDL_GetDisplayUsableBounds(int index, int *rect) {
	return SDL_GetDisplayBounds(index, rect);
}

int SDL_GetCurrentDisplayMode(int index, void *mode) {
	int *fields = (int *)mode;
	if (g_no_display_bounds || index != 0)
		return -1;
	fields[0] = 0; /* format */
	fields[1] = g_display_w;
	fields[2] = g_display_h;
	fields[3] = 60;
	return 0;
}

int SDL_GetWindowDisplayIndex(void *window) {
	(void)window;
	return 0;
}

int SDL_PeepEvents(void *events, int numevents, int action, uint32_t min_type, uint32_t max_type) {
	int got = 0, i = 0;
	unsigned char *out = (unsigned char *)events;
	if (action != 2 /* SDL_GETEVENT */ || numevents <= 0)
		return 0;
	while (i < g_queue_len && got < numevents) {
		uint32_t type;
		memcpy(&type, g_queue[i], 4);
		if (type >= min_type && type <= max_type) {
			memcpy(out + (size_t)got * FAKE_EVENT_SIZE, g_queue[i], FAKE_EVENT_SIZE);
			got++;
			memmove(g_queue[i], g_queue[i + 1], (size_t)(g_queue_len - i - 1) * FAKE_EVENT_SIZE);
			g_queue_len--;
		} else {
			i++;
		}
	}
	return got;
}

void SDL_FlushEvents(uint32_t min_type, uint32_t max_type) {
	int i = 0;
	while (i < g_queue_len) {
		uint32_t type;
		memcpy(&type, g_queue[i], 4);
		if (type >= min_type && type <= max_type) {
			memmove(g_queue[i], g_queue[i + 1], (size_t)(g_queue_len - i - 1) * FAKE_EVENT_SIZE);
			g_queue_len--;
		} else {
			i++;
		}
	}
}

uint32_t SDL_GetWindowFlags(void *window) {
	(void)window;
	return g_window_alive ? g_window.flags : 0;
}

int SDL_SetWindowFullscreen(void *window, uint32_t flags) {
	(void)window;
	g_set_fullscreen_calls++;
	g_set_fullscreen_last = flags;
	if (flags)
		g_window.flags |= flags;
	else
		g_window.flags &= ~(uint32_t)FAKE_WINDOW_FULLSCREEN_DESKTOP;
	return 0;
}
#endif /* FAKE_SDL_MINIMAL */
