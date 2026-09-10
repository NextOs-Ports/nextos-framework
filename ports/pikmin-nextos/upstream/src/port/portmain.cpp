// Port entry point: bring Aurora up, mount the disc, hand control to the game.
//
// The game keeps its own main loop (System::run -> PlugPikiApp::idle); nothing
// here drives frames.  This function only sets the stage and then calls the
// decomp's main().

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <SDL3/SDL_filesystem.h>

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>

#include <dolphin/os.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

#include "port/os_port.h"

extern "C" int game_main(int argc, char* argv[]);

AuroraInfo gAuroraInfo;

namespace {

bool sRunning       = true;
std::string sUserPath;

void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int len)
{
	(void)len;
	const char* levelStr = "??";
	FILE* out            = stdout;
	switch (level) {
	case LOG_DEBUG: levelStr = "DEBUG"; break;
	case LOG_INFO: levelStr = "INFO"; break;
	case LOG_WARNING: levelStr = "WARN"; break;
	case LOG_ERROR:
		levelStr = "ERROR";
		out      = stderr;
		break;
	case LOG_FATAL:
		levelStr = "FATAL";
		out      = stderr;
		break;
	}
	fprintf(out, "[%s | %s] %s\n", levelStr, module, message);
	fflush(out);
	if (level == LOG_FATAL) {
		abort();
	}
}

// The disc is needed at runtime, not just to build.  Look where the launcher
// puts it, then fall back to anything the user passed on the command line.
std::string resolve_disc_path(int argc, char* argv[])
{
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			return argv[i];
		}
	}

	if (const char* env = std::getenv("PIKMIN_DISC")) {
		return env;
	}

	const char* base = SDL_GetBasePath();
	const std::filesystem::path baseDir = base != nullptr ? base : ".";
	static const char* kNames[] = {
		"pikmin.rvz", "pikmin.iso", "Pikmin.rvz", "Pikmin.iso", "game.rvz", "game.iso",
	};
	for (const char* name : kNames) {
		std::error_code ec;
		const auto candidate = baseDir / name;
		if (std::filesystem::exists(candidate, ec)) {
			return candidate.string();
		}
	}

	// Last resort: the first disc image sitting next to the executable.
	std::error_code ec;
	for (std::filesystem::directory_iterator it(baseDir, ec), end; it != end; it.increment(ec)) {
		if (ec) {
			break;
		}
		const auto ext = it->path().extension().string();
		if (ext == ".rvz" || ext == ".iso" || ext == ".gcm" || ext == ".ciso") {
			return it->path().string();
		}
	}

	return {};
}

} // namespace

extern "C" {

int PikiPortIsRunning(void) { return sRunning ? 1 : 0; }

void PikiPortRequestExit(void)
{
	// Say so. A session that ends on its own is either the window closing or the
	// game asking for a reset, and the log should not leave you guessing which.
	if (sRunning) {
		printf("[pikmin] exit requested\n");
		fflush(stdout);
	}
	sRunning = false;
}

int port_main(int argc, char* argv[])
{
	// English, always - the game picks up the host locale for nothing, but the
	// C library formatting it uses for debug text should stay predictable.
	setenv("LANG", "C", 1);

	OSPortSchedulerInit();

	const char* prefPath = SDL_GetPrefPath("NextOS", "Pikmin");
	sUserPath            = prefPath != nullptr ? prefPath : ".";

	{
		AuroraConfig config{};
		config.appName        = "Pikmin";
		config.userPath       = sUserPath.c_str();
		// GLES2 on the device.  The host build (see configure-host.sh) overrides
		// this so the boot flow can be exercised without a Mali-450 in the room.
		config.desiredBackend = BACKEND_OPENGLES;
		if (const char* backendEnv = std::getenv("PIKMIN_BACKEND")) {
			if (std::strcmp(backendEnv, "null") == 0) {
				config.desiredBackend = BACKEND_NULL;
			} else if (std::strcmp(backendEnv, "opengl") == 0) {
				config.desiredBackend = BACKEND_OPENGL;
			}
		}
		config.logCallback    = &log_callback;
		config.logLevel       = LOG_INFO;
		config.vsync          = true;
		config.startFullscreen = true;
		config.windowPosX     = -1;
		config.windowPosY     = -1;
		config.windowWidth    = 640;
		config.windowHeight   = 480;
		// The GameCube's 24 MB of MEM1 is what the game's heap layout assumes;
		// give it headroom because the port's pointers are twice as wide.
		config.mem1Size = 64 * 1024 * 1024;
		config.mem2Size = 16 * 1024 * 1024;
		// Anamorphic widescreen: the game widens its projection by the real
		// framebuffer's aspect (PikiPortWidescreenFactor) and the stretched
		// present restores the proportions.  On a 4:3 panel both are no-ops.
		config.presentStretch = true;
		config.allowTextureDumps = false;
		config.pauseOnFocusLost  = false;
		config.allowJoystickBackgroundEvents = true;

		// Seed the framebuffer scale before Aurora builds its render targets.
		// Applying it afterwards forces a live target resize and fights Mali's
		// EGL context for the surface - so the env has to be read here, not
		// after init, or the one knob that trades fill rate for speed would trip
		// the very thing this ordering exists to avoid.
		float internalScale = 1.0f;
		if (const char* scaleEnv = std::getenv("PIKMIN_INTERNAL_SCALE")) {
			const float scale = std::strtof(scaleEnv, nullptr);
			if (scale > 0.0f && scale <= 2.0f) {
				internalScale = scale;
			} else {
				fprintf(stderr, "[pikmin] ignoring PIKMIN_INTERNAL_SCALE='%s' (want 0 < scale <= 2)\n", scaleEnv);
			}
		}
		if (internalScale != 1.0f) {
			printf("[pikmin] internal resolution scale %.2f\n", internalScale);
		}
		VISetFrameBufferScale(internalScale);

		gAuroraInfo = aurora_initialize(argc, argv, &config);
	}

	VISetWindowTitle("Pikmin");

	// FIT keeps the content framebuffer at the game's 4:3 (640x480 x scale) so
	// the EFB stays cheap on the Mali.  Fullscreen comes from the anamorphic
	// pair: the game widens its 3D projection by PikiPortWidescreenFactor()
	// (and squeezes 2D by it), and presentStretch blits the EFB over the whole
	// panel, which cancels the distortion.  Real 16:9, no fat fonts.
	AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);

	const std::string discPath = resolve_disc_path(argc, argv);
	if (discPath.empty()) {
		fprintf(stderr, "[pikmin] No disc image found. Put pikmin.rvz next to the executable "
		                "or pass its path as an argument.\n");
		aurora_shutdown();
		return 1;
	}
	printf("[pikmin] Mounting disc: %s\n", discPath.c_str());
	fflush(stdout);
	if (!aurora_dvd_open(discPath.c_str())) {
		fprintf(stderr, "[pikmin] Failed to open disc image: %s\n", discPath.c_str());
		aurora_shutdown();
		return 1;
	}

	PikiPortWatchdogStart();

	// From here the game owns the flow: gsys->Initialise(), then its own loop.
	OSPortSchedulerAcquire();
	game_main(argc, argv);
	OSPortSchedulerRelease();

	// The game's loop has returned, so nothing can still be waiting on a field.
	// Stop the retrace clock before Aurora goes away - its callbacks touch GX.
	PikiPortViStop();

	fflush(stdout);
	fflush(stderr);
	aurora_dvd_close();
	aurora_shutdown();
	return 0;
}

float PikiPortWidescreenFactor(void)
{
	// (surface aspect) / (content-EFB aspect), read from what Aurora actually
	// opened - never hardcoded (the panel decides).  1.0 on a 4:3 panel, so
	// every seam that uses this is a no-op there.
	static float sFactor = 0.0f;
	if (sFactor == 0.0f) {
		const AuroraWindowSize& ws = gAuroraInfo.windowSize;
		if (ws.native_fb_width == 0 || ws.native_fb_height == 0) {
			return 1.0f; // before aurora_initialize: neutral, and do not cache
		}
		// The game's aspect is fixed 4:3 (the FIT policy keeps the EFB there;
		// the init-time fb snapshot predates the policy, so don't consult it).
		const float surface = (float)ws.native_fb_width / (float)ws.native_fb_height;
		float factor        = surface / (640.0f / 480.0f);
		if (factor < 0.5f) {
			factor = 0.5f;
		} else if (factor > 2.0f) {
			factor = 2.0f;
		}
		sFactor = factor;
		printf("[pikmin] widescreen factor %.3f (surface %ux%u)\n", factor, ws.native_fb_width, ws.native_fb_height);
	}
	return sFactor;
}

} // extern "C"
