// Frame pump.
//
// Pikmin's frame is bounded by DGXGraphics: beginRender() opens it, waitRetrace()
// closes it after GXCopyDisp.  Those are the two places Aurora's frame has to
// line up with, so the port hooks exactly there and leaves the game's cadence -
// including its own retrace throttling - untouched.

#include <cstdio>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_stdinc.h>

#include <aurora/aurora.h>
#include <aurora/event.h>

#include <dolphin/gx.h>
#include <dolphin/os.h>

#include "port/os_port.h"

namespace {
bool sFrameOpen = false;
unsigned sFrameCount = 0;
unsigned sSkipped = 0;

// SELECT+START held on any pad quits the port cleanly, the standard NextOS
// combo. Checked on the raw SDL state so it works regardless of how (or
// whether) those buttons are mapped into the GC pad.
void checkQuitCombo()
{
	static unsigned heldFrames = 0;
	bool held = false;
	int count = 0;
	if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
		for (int i = 0; i < count && !held; ++i) {
			SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
			held = pad != nullptr && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK)
			    && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
		}
		SDL_free(ids);
	}
	heldFrames = held ? heldFrames + 1 : 0;
	if (heldFrames == 20) { // ~0.7 s at 30 fps; a beat, so a stray tap can't quit
		std::printf("[pikmin] SELECT+START held - exiting\n");
		std::fflush(stdout);
		PikiPortRequestExit();
	}
}
}

extern "C" {

void PikiPortPumpEvents(void)
{
	// Called with the scheduler lock held, and it stays held: Aurora records
	// the frame into a packet that its own worker replays, so the only thing
	// that must not happen concurrently is another *game* thread issuing GX.
	const AuroraEvent* event = aurora_update();
	while (event != nullptr && event->type != AURORA_NONE) {
		if (event->type == AURORA_EXIT) {
			PikiPortRequestExit();
			break;
		}
		event++;
	}
	checkQuitCombo();
}

void PikiPortBeginFrame(void)
{
	if (sFrameOpen) {
		return;
	}
	sFrameOpen = aurora_begin_frame();
	if (!sFrameOpen) {
		++sSkipped;
	}
}

void PikiPortEndFrame(void)
{
	if (!sFrameOpen) {
		return;
	}
	sFrameOpen = false;
	aurora_end_frame();
	PikiPortWatchdogFrame();
	if (++sFrameCount <= 5 || (sFrameCount % 120) == 0) {
		std::printf("[pikmin] presented frame %u (skipped begins: %u)\n", sFrameCount, sSkipped);
		std::fflush(stdout);
	}
}

} // extern "C"
