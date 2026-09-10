/* SPDX-License-Identifier: GPL-3.0-only */
// Host gate for the SDL2 window geometry authority (WAYLAND_GEOMETRY_PROOF).
//
// Drives nx_sdl2_geometry.{h,cpp} against the fake libSDL2-2.0.so.0 built
// from fake_sdl2.c (found through LD_LIBRARY_PATH, exactly like the display
// server dlopens the firmware SDL2). No Godot, no device, no real SDL.
//
// argv[1]: "full" (complete fake) or "minimal" (fake without the optional
// symbols). Receipts are written under $NX_GEOMETRY_TEST_OUT for the shell
// runner to validate as JSON and to feed to the gate consumer.
#include "nx_sdl2_geometry.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

struct Fixture {
	void *handle = nullptr;
	int (*InitSubSystem)(uint32_t) = nullptr;
	void (*QuitSubSystem)(uint32_t) = nullptr;
	NxSDL_Window *(*CreateWindow)(const char *, int, int, int, int, uint32_t) = nullptr;
	void (*DestroyWindow)(NxSDL_Window *) = nullptr;
	int (*pump_count)(void) = nullptr;
	int (*hint_before_init)(void) = nullptr;
	const char *(*hint_app_id)(void) = nullptr;
	int (*set_fullscreen_calls)(void) = nullptr;
	uint32_t (*set_fullscreen_last)(void) = nullptr;
	NxSdl2GeometryApi api;
};

std::vector<std::string> g_log;
int g_failures = 0;

void capture_log(void *, const char *line) {
	g_log.push_back(line);
	printf("  log: %s\n", line);
}

bool log_contains(const char *needle) {
	for (const std::string &line : g_log) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

#define CHECK(cond, what)                                                   \
	do {                                                                    \
		if (cond) {                                                         \
			printf("  ok   %s\n", what);                                    \
		} else {                                                            \
			printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);        \
			g_failures++;                                                   \
		}                                                                   \
	} while (0)

void *resolve(void *handle, const char *name) {
	return dlsym(handle, name);
}

bool load_fixture(Fixture *f) {
	static const char *const candidates[] = { "libSDL2-2.0.so.0", "libSDL2.so", nullptr };
	for (int i = 0; candidates[i] && !f->handle; i++) {
		f->handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
	}
	if (!f->handle) {
		printf("cannot dlopen the fake SDL2: %s\n", dlerror());
		return false;
	}
#define BIND(field, symbol) *(void **)(&f->field) = dlsym(f->handle, symbol)
	BIND(InitSubSystem, "SDL_InitSubSystem");
	BIND(QuitSubSystem, "SDL_QuitSubSystem");
	BIND(CreateWindow, "SDL_CreateWindow");
	BIND(DestroyWindow, "SDL_DestroyWindow");
	BIND(pump_count, "fake_sdl_pump_count");
	BIND(hint_before_init, "fake_sdl_hint_before_init");
	BIND(hint_app_id, "fake_sdl_hint_app_id");
	BIND(set_fullscreen_calls, "fake_sdl_set_fullscreen_calls");
	BIND(set_fullscreen_last, "fake_sdl_set_fullscreen_last");
#undef BIND
	if (!f->InitSubSystem || !f->QuitSubSystem || !f->CreateWindow || !f->DestroyWindow ||
			!f->pump_count || !f->hint_before_init || !f->hint_app_id ||
			!f->set_fullscreen_calls || !f->set_fullscreen_last) {
		printf("fake SDL2 lacks a required symbol\n");
		return false;
	}
	nx_sdl2_geometry_bind(&f->api, f->handle, resolve);
	return true;
}

std::string receipt_path(const char *name) {
	const char *dir = getenv("NX_GEOMETRY_TEST_OUT");
	std::string path = dir && dir[0] ? dir : ".";
	path += "/";
	path += name;
	path += ".jsonl";
	unlink(path.c_str());
	return path;
}

int count_lines(const std::string &path, const char *needle) {
	FILE *stream = fopen(path.c_str(), "r");
	if (!stream) {
		return -1;
	}
	char line[2048];
	int count = 0;
	while (fgets(line, sizeof(line), stream)) {
		if (strstr(line, needle)) {
			count++;
		}
	}
	fclose(stream);
	return count;
}

void set_env(const char *name, const char *value) {
	if (value) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
}

// Runs the same call sequence the display server performs.
struct Run {
	NxSdl2Geometry g;
	NxSDL_Window *window = nullptr;
	int attach_rc = -1;
	int request_w = 0;
	int request_h = 0;
};

void begin(Fixture *f, Run *r, const char *provider_receipt, int raw_w, int raw_h, int timeout_ms) {
	g_log.clear();
	nx_sdl2_geometry_init(&r->g, &f->api, capture_log, nullptr);
	nx_sdl2_geometry_set_raw_fb(&r->g, raw_w, raw_h);
	nx_sdl2_geometry_prepare_app_id(&r->g);
	CHECK(f->InitSubSystem(0x20u) == 0, "fake video subsystem up");
	nx_sdl2_geometry_initial_request(&r->g, &r->request_w, &r->request_h);
	r->window = f->CreateWindow("Tearscape", 0, 0, r->request_w, r->request_h, 0x1003u);
	CHECK(r->window != nullptr, "fake window created");
	r->attach_rc = nx_sdl2_geometry_attach_window(&r->g, r->window, timeout_ms);
	nx_sdl2_geometry_receipt_open(&r->g, provider_receipt);
	nx_sdl2_geometry_write_init(&r->g, "sdl2");
}

void end(Fixture *f, Run *r) {
	nx_sdl2_geometry_receipt_close(&r->g);
	if (r->window) {
		f->DestroyWindow(r->window);
	}
	f->QuitSubSystem(0x20u);
}

void scenario_wayland(Fixture *f) {
	printf("scenario: wayland compositor (portrait raw fb, landscape logical output)\n");
	set_env("FAKE_SDL_DRIVER", "wayland");
	set_env("FAKE_SDL_DISPLAY_W", "1920");
	set_env("FAKE_SDL_DISPLAY_H", "1080");
	set_env("FAKE_SDL_INITIAL_W", "1080");
	set_env("FAKE_SDL_INITIAL_H", "1920");
	set_env("FAKE_SDL_CONFIGURE_AFTER_PUMPS", "5");
	set_env("FAKE_SDL_LATE_RESIZE_AT_PUMP", nullptr);
	set_env("FAKE_SDL_INITIAL_FULLSCREEN", "0");
	set_env("FAKE_SDL_NO_DISPLAY_BOUNDS", nullptr);
	set_env("SDL_APP_ID", nullptr);
	set_env("SDL_VIDEO_WAYLAND_WMCLASS", nullptr);

	Run r;
	std::string receipt = receipt_path("wayland");
	begin(f, &r, receipt.c_str(), 1080, 1920, 1500);

	// (3) app id hint set before init, derived from the executable.
	CHECK(strcmp(r.g.app_id_source, "exe") == 0 && r.g.app_id[0] != 0, "app id derived from /proc/self/exe");
	CHECK(f->hint_before_init() == 1, "SDL_APP_ID hint arrived before SDL_InitSubSystem");
	CHECK(strcmp(f->hint_app_id(), r.g.app_id) == 0, "hint carries the resolved app id");
	const char *wmclass = getenv("SDL_VIDEO_WAYLAND_WMCLASS");
	CHECK(wmclass && strcmp(wmclass, r.g.app_id) == 0, "SDL_VIDEO_WAYLAND_WMCLASS exported");

	// (1) logical size is the compositor's, not the raw panel.
	CHECK(r.request_w == 1920 && r.request_h == 1080, "request came from SDL display bounds");
	CHECK(r.attach_rc == 0, "attach succeeded");
	CHECK(r.g.logical_w == 1920 && r.g.logical_h == 1080, "final logical size 1920x1080");
	CHECK(!(r.g.logical_w == 1080 && r.g.logical_h == 1920), "raw portrait size not used");
	CHECK(r.g.raw_fb_w == 1080 && r.g.raw_fb_h == 1920, "raw fb recorded informationally");
	// (2) the wait ended by configure, not by timeout.
	CHECK(!r.g.timed_out && r.g.configured, "configure wait ended by event/stable drawable");
	CHECK(r.g.configure_events >= 1, "SIZE_CHANGED counted");
	CHECK(f->pump_count() >= 6, "events were pumped during the wait");
	CHECK(r.g.wait_ms < 1500, "wait shorter than the timeout");
	CHECK(log_contains("nx/sdl2: driver 'wayland' display 1920x1080 requested 1920x1080 configured 1920x1080 drawable 1920x1080 configure_events="),
			"summary log line from SDL data");
	// (4) fullscreen ensured from inside the runtime.
	CHECK(r.g.fullscreen && r.g.fullscreen_forced, "fullscreen ensured by the runtime");
	CHECK(f->set_fullscreen_calls() == 1 && f->set_fullscreen_last() == 0x1001u,
			"SDL_SetWindowFullscreen(FULLSCREEN_DESKTOP) called once");

	end(f, &r);

	// (5) per-frame poll: a second pass with one late compositor resize armed
	// (the fake reads its configuration at init) -> exactly one change.
	Run r2;
	std::string receipt2 = receipt_path("wayland");
	set_env("FAKE_SDL_LATE_RESIZE_AT_PUMP", "12");
	set_env("FAKE_SDL_LATE_W", "1280");
	set_env("FAKE_SDL_LATE_H", "720");
	begin(f, &r2, receipt2.c_str(), 1080, 1920, 1500);
	CHECK(r2.attach_rc == 0 && r2.g.logical_w == 1920 && r2.g.logical_h == 1080, "second pass configured 1920x1080");
	int changes = 0;
	for (int frame = 0; frame < 20; frame++) {
		if (nx_sdl2_geometry_poll(&r2.g)) {
			changes++;
			nx_sdl2_geometry_write_resize(&r2.g, true);
		}
	}
	CHECK(changes == 1, "resize reported exactly once");
	CHECK(r2.g.logical_w == 1280 && r2.g.logical_h == 720, "resize carries the new drawable");
	CHECK(r2.g.resize_count == 1, "resize counter");
	// (6) receipt lines exist (JSON validity is asserted by the runner).
	CHECK(count_lines(receipt2, "\"kind\":\"init\"") == 1, "one init receipt line");
	CHECK(count_lines(receipt2, "\"kind\":\"resize\"") == 1, "one resize receipt line");
	CHECK(count_lines(receipt2, "\"schema\":\"nx-geometry-proof/1\"") == 2, "schema on every line");
	CHECK(count_lines(receipt2, "\"timed_out\":false") == 1, "init records timed_out=false");
	end(f, &r2);
}

void scenario_kmsdrm(Fixture *f) {
	printf("scenario: kmsdrm (first query authoritative)\n");
	set_env("FAKE_SDL_DRIVER", "kmsdrm");
	set_env("FAKE_SDL_DISPLAY_W", "1280");
	set_env("FAKE_SDL_DISPLAY_H", "720");
	set_env("FAKE_SDL_INITIAL_W", "1280");
	set_env("FAKE_SDL_INITIAL_H", "720");
	set_env("FAKE_SDL_CONFIGURE_AFTER_PUMPS", "-1");
	set_env("FAKE_SDL_LATE_RESIZE_AT_PUMP", nullptr);
	set_env("FAKE_SDL_INITIAL_FULLSCREEN", "1");
	set_env("SDL_APP_ID", nullptr);
	set_env("SDL_VIDEO_WAYLAND_WMCLASS", "inherited.wmclass");

	Run r;
	std::string receipt = receipt_path("kmsdrm");
	begin(f, &r, receipt.c_str(), 1280, 720, 1500);
	CHECK(strcmp(r.g.app_id_source, "SDL_VIDEO_WAYLAND_WMCLASS") == 0 &&
					strcmp(r.g.app_id, "inherited.wmclass") == 0,
			"inherited SDL_VIDEO_WAYLAND_WMCLASS wins as app id");
	CHECK(strcmp(getenv("SDL_VIDEO_WAYLAND_WMCLASS"), "inherited.wmclass") == 0, "inherited env not overwritten");
	// (7) immediate exit, no events, no forced fullscreen, no resize.
	CHECK(r.attach_rc == 0 && r.g.logical_w == 1280 && r.g.logical_h == 720, "kmsdrm logical size");
	CHECK(r.g.configure_events == 0, "configure_events=0");
	CHECK(!r.g.timed_out && r.g.configured, "no timeout");
	CHECK(f->pump_count() == 1, "wait exited after the first query");
	CHECK(r.g.wait_ms < 200, "wait_ms small");
	CHECK(r.g.fullscreen && !r.g.fullscreen_forced && f->set_fullscreen_calls() == 0,
			"already fullscreen: SetWindowFullscreen not called");
	int changes = 0;
	for (int frame = 0; frame < 10; frame++) {
		if (nx_sdl2_geometry_poll(&r.g)) {
			changes++;
		}
	}
	CHECK(changes == 0, "no resize on a stable driver");
	CHECK(count_lines(receipt, "\"kind\":\"resize\"") == 0, "no resize receipt line");
	end(f, &r);
}

void scenario_timeout(Fixture *f) {
	printf("scenario: compositor that never configures\n");
	set_env("FAKE_SDL_DRIVER", "wayland");
	set_env("FAKE_SDL_DISPLAY_W", "1920");
	set_env("FAKE_SDL_DISPLAY_H", "1080");
	set_env("FAKE_SDL_INITIAL_W", "1080");
	set_env("FAKE_SDL_INITIAL_H", "1920");
	set_env("FAKE_SDL_CONFIGURE_AFTER_PUMPS", "-1");
	set_env("FAKE_SDL_LATE_RESIZE_AT_PUMP", nullptr);
	set_env("FAKE_SDL_INITIAL_FULLSCREEN", "0");
	set_env("SDL_APP_ID", "explicit.app.id");
	set_env("SDL_VIDEO_WAYLAND_WMCLASS", nullptr);

	Run r;
	std::string receipt = receipt_path("timeout");
	begin(f, &r, receipt.c_str(), 1080, 1920, 80);
	CHECK(strcmp(r.g.app_id_source, "SDL_APP_ID") == 0 && strcmp(r.g.app_id, "explicit.app.id") == 0,
			"SDL_APP_ID env wins as app id");
	// (8) timeout is a recorded, logged status; the last drawable is kept.
	CHECK(r.attach_rc == 0, "attach still yields a usable size");
	CHECK(r.g.timed_out && !r.g.configured, "timed out");
	CHECK(r.g.wait_ms >= 80, "wait_ms reached the timeout");
	CHECK(r.g.logical_w == 1080 && r.g.logical_h == 1920, "unconfigured drawable kept (gate must fail it)");
	CHECK(log_contains("configure wait TIMED OUT"), "timeout logged");
	CHECK(count_lines(receipt, "\"timed_out\":true") == 1, "receipt records timed_out=true");
	end(f, &r);
}

void scenario_minimal(Fixture *f) {
	printf("scenario: firmware SDL2 without the optional symbols\n");
	set_env("FAKE_SDL_DRIVER", "kmsdrm");
	set_env("FAKE_SDL_DISPLAY_W", "800");
	set_env("FAKE_SDL_DISPLAY_H", "600");
	set_env("FAKE_SDL_INITIAL_W", "800");
	set_env("FAKE_SDL_INITIAL_H", "600");
	set_env("FAKE_SDL_CONFIGURE_AFTER_PUMPS", "-1");
	set_env("SDL_APP_ID", nullptr);
	set_env("SDL_VIDEO_WAYLAND_WMCLASS", nullptr);
	CHECK(f->api.SetHint == nullptr && f->api.PeepEvents == nullptr && f->api.GetWindowFlags == nullptr,
			"optional symbols absent in this fake");

	Run r;
	std::string receipt = receipt_path("minimal");
	begin(f, &r, receipt.c_str(), 800, 600, 200);
	CHECK(log_contains("SDL_SetHint unavailable"), "missing SetHint logged");
	CHECK(r.request_w == NX_SDL2_GEOMETRY_FALLBACK_W && r.request_h == NX_SDL2_GEOMETRY_FALLBACK_H,
			"no display authority: fallback request");
	CHECK(log_contains("no display authority available"), "missing display authority logged");
	CHECK(log_contains("SDL_GetWindowFlags unavailable"), "missing fullscreen query logged");
	// No display bounds and no events: a stable non-zero drawable is accepted.
	CHECK(r.attach_rc == 0 && r.g.logical_w == 800 && r.g.logical_h == 600 && !r.g.timed_out,
			"stable drawable accepted without display authority");
	CHECK(!r.g.fullscreen, "fullscreen unverified reported as false");
	end(f, &r);
}

} // namespace

int main(int argc, char **argv) {
	const char *mode = argc > 1 ? argv[1] : "full";
	Fixture f;
	if (!load_fixture(&f)) {
		return 2;
	}
	if (strcmp(mode, "minimal") == 0) {
		scenario_minimal(&f);
	} else {
		scenario_wayland(&f);
		scenario_kmsdrm(&f);
		scenario_timeout(&f);
		std::string fbdev = receipt_path("fbdev");
		CHECK(nx_sdl2_geometry_write_fbdev_init(fbdev.c_str(), 640, 480), "fbdev provider init receipt");
		CHECK(count_lines(fbdev, "\"provider\":\"fbdev\"") == 1, "fbdev receipt names its provider");
		char escaped[64];
		nx_sdl2_geometry_json_escape("a\"b\\c\n", escaped, sizeof(escaped));
		CHECK(strcmp(escaped, "a\\\"b\\\\c\\u000a") == 0, "json escaping");
	}
	printf("SDL2 GEOMETRY HOST (%s): %s failures=%d\n", mode, g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
