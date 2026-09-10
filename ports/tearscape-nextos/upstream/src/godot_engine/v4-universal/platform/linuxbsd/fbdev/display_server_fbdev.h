// NextOS universal console display server. One measured provider owns the
// window, context and swap for the entire process: raw fbdev/EGL on legacy
// Mali or the firmware SDL2 video path (KMSDRM or a compositor) on DRM
// systems. With SDL2 the window geometry is SDL's, never /dev/fb0's.
#pragma once

#ifdef FBDEV_ENABLED

#include "servers/display/display_server_headless.h"
#include "nxgl_godot_frame_proof.h"

#ifdef GLES3_ENABLED
#include "egl_manager_fbdev.h"
#endif

#include <atomic>
#include <cstdint>

/* V5: the aspect decision type comes from the framework authority. */
#include "nxcompat_video.h"

class DisplayServerFBDev : public DisplayServerHeadless {
	GDSOFTCLASS(DisplayServerFBDev, DisplayServerHeadless);

	enum Provider {
		PROVIDER_NONE,
		PROVIDER_FBDEV,
		PROVIDER_SDL2,
	};

	// Mali fbdev blob native window: {uint16 width, uint16 height}.
	struct FBDevWindow {
		uint16_t width = 0;
		uint16_t height = 0;
	} fbdev_window;

	// Live logical window size. fbdev provider: the raw panel; SDL2 provider:
	// SDL's configured drawable, refreshed every frame (0.2.16).
	Size2i fb_size;
	// Raw /dev/fb0 geometry, kept for the fbdev provider and the receipt.
	Size2i raw_fb_size;
	Callable window_event_callback;
	Callable rect_changed_callback;
	Provider provider = PROVIDER_NONE;
	bool use_vsync = true;
	std::atomic<bool> frame_presented{ false };
	bool health_published = false;
	bool health_blocked = false;
	char health_receipt_path[1152] = {};
	nxgl_godot_frame_proof frame_proof = NXGL_GODOT_FRAME_PROOF_INIT;

#ifdef GLES3_ENABLED
	GLManagerEGL_FBDev *egl_manager = nullptr;
#endif

	static Vector<String> get_rendering_drivers_func_fbdev();
	static DisplayServer *create_func_fbdev(const String &p_rendering_driver, WindowMode p_mode, VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, Context p_context, int64_t p_parent_window, Error &r_error);
	static bool _drm_capability_present();
	static const char *_provider_name(Provider p_provider);
	Error _initialize_fbdev();
	Error _initialize_sdl2();
	void _poll_sdl2_geometry();
	void _write_geometry_receipt_init();
	void _apply_aspect_policy();
	// FV3: the elected decision, its generation and the one readback after the
	// first present. One display authority per video_config_generation.
	void _readback_aspect_policy();
	nxcompat_video_owner_decision video_decision = {};
	bool video_decision_valid = false;
	bool video_readback_done = false;
	unsigned int video_config_generation = 0;
	void _publish_health_receipt();
	void _revoke_health_receipt();
	void _fail_runtime(const char *p_reason, int p_status);

public:
	String get_name() const override { return "fbdev"; }

	int get_screen_count() const override { return 1; }
	Size2i screen_get_size(int p_screen = SCREEN_OF_MAIN_WINDOW) const override { return fb_size; }
	Rect2i screen_get_usable_rect(int p_screen = SCREEN_OF_MAIN_WINDOW) const override { return Rect2i(Point2i(), fb_size); }

	Vector<WindowID> get_window_list() const override {
		Vector<WindowID> list;
		list.push_back(MAIN_WINDOW_ID);
		return list;
	}
	Size2i window_get_size(WindowID p_window = MAIN_WINDOW_ID) const override { return fb_size; }
	Size2i window_get_size_with_decorations(WindowID p_window = MAIN_WINDOW_ID) const override { return fb_size; }
	WindowMode window_get_mode(WindowID p_window = MAIN_WINDOW_ID) const override { return WINDOW_MODE_FULLSCREEN; }

	bool window_can_draw(WindowID p_window = MAIN_WINDOW_ID) const override { return true; }
	bool can_any_window_draw() const override { return true; }
	void window_set_window_event_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override;
	void window_set_rect_changed_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override;

	void window_set_vsync_mode(VSyncMode p_vsync_mode, WindowID p_window) override;
	VSyncMode window_get_vsync_mode(WindowID p_window) const override;
	void request_close();
	void refresh_controls_before_events();
	void publish_health_after_iteration();

	void release_rendering_thread() override;
	void swap_buffers() override;

	static void register_fbdev_driver();

	DisplayServerFBDev(const String &p_rendering_driver, Error &r_error);
	~DisplayServerFBDev();
};

#endif // FBDEV_ENABLED
