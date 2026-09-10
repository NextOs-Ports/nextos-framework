// Synthetic input, for driving the flow without a pad in reach.
//
// Off unless PIKMIN_AUTOSTART is set, and it only ever *adds* to whatever the
// real pad reported - it never replaces it.  This exists so the menus and the
// game can be walked on a headless host build; the real controls are entirely
// native (PADRead -> ControllerMgr::updateController), and nothing here runs in
// a release session.
//
//   PIKMIN_AUTOSTART=6                      press START every 6 s
//   PIKMIN_AUTOSTART=6:start,10:a           a script: input at that many seconds
//   PIKMIN_AUTOSTART=6:start,20:sup:120     ...held for that many frames
//
// A scripted list has to guess when the game will be ready for each press, and
// it is wrong as soon as anything takes longer than it did last time.  For input
// the game waits on indefinitely - a text window that only closes on A - use the
// repeater instead, which keeps pressing until something consumes it:
//
//   PIKMIN_AUTOREPEAT=a:4                   press A every 4 s, from the start
//   PIKMIN_AUTOREPEAT=a:4:90                ...starting 90 s in
//
// Names: start a b x y z l r up down left right (buttons),
//        sup sdown sleft sright (main stick, which is what moves Olimar).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dolphin/pad.h>

#include "port/os_port.h"

#define MAX_STEPS 32
#define BUTTON_FRAMES 5
#define STICK_FRAMES 60
#define STICK_THROW 100 // well past PAD_MIN_STICK_READ

typedef struct Step {
	double at;
	u16 button;
	s8 stickX;
	s8 stickY;
	int frames;
} Step;

static int sConfigured;
static Step sSteps[MAX_STEPS];
static int sStepCount;
static int sNextStep;
static double sStartTime;
static int sHoldFrames;
static Step sHeld;

// Repeating mode: no script, just START every `sPeriod` seconds.
static double sPeriod;

// PIKMIN_AUTOREPEAT: one input, over and over, for the waits whose length the
// script cannot know in advance.  Runs alongside the script, not instead of it.
static Step sRepeat;
static double sRepeatPeriod;
static double sRepeatFrom;
static double sRepeatNext;
static int sRepeatHoldFrames;

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int step_from_name(const char* name, size_t len, Step* out)
{
	struct {
		const char* name;
		u16 button;
		s8 stickX;
		s8 stickY;
	} static const kNames[] = {
		{ "start", PAD_BUTTON_START, 0, 0 },
		{ "a", PAD_BUTTON_A, 0, 0 },
		{ "b", PAD_BUTTON_B, 0, 0 },
		{ "x", PAD_BUTTON_X, 0, 0 },
		{ "y", PAD_BUTTON_Y, 0, 0 },
		{ "z", PAD_TRIGGER_Z, 0, 0 },
		{ "l", PAD_TRIGGER_L, 0, 0 },
		{ "r", PAD_TRIGGER_R, 0, 0 },
		{ "up", PAD_BUTTON_UP, 0, 0 },
		{ "down", PAD_BUTTON_DOWN, 0, 0 },
		{ "left", PAD_BUTTON_LEFT, 0, 0 },
		{ "right", PAD_BUTTON_RIGHT, 0, 0 },
		{ "sup", 0, 0, STICK_THROW },
		{ "sdown", 0, 0, -STICK_THROW },
		{ "sleft", 0, -STICK_THROW, 0 },
		{ "sright", 0, STICK_THROW, 0 },
	};
	size_t i;

	for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
		if (strlen(kNames[i].name) == len && strncmp(kNames[i].name, name, len) == 0) {
			out->button = kNames[i].button;
			out->stickX = kNames[i].stickX;
			out->stickY = kNames[i].stickY;
			out->frames = kNames[i].button != 0 ? BUTTON_FRAMES : STICK_FRAMES;
			return 1;
		}
	}
	return 0;
}

// PIKMIN_AUTOREPEAT=<name>:<period seconds>[:<start seconds>]
static void configure_repeat(void)
{
	const char* env = getenv("PIKMIN_AUTOREPEAT");
	const char* p;
	size_t len;

	if (env == NULL || *env == '\0') {
		return;
	}

	len = strcspn(env, ":");
	if (!step_from_name(env, len, &sRepeat)) {
		fprintf(stderr, "[pikmin] PIKMIN_AUTOREPEAT: unknown input '%.*s'\n", (int)len, env);
		return;
	}

	p = env + len;
	if (*p != ':') {
		fprintf(stderr, "[pikmin] PIKMIN_AUTOREPEAT: no period given\n");
		return;
	}
	sRepeatPeriod = strtod(p + 1, (char**)&p);
	if (sRepeatPeriod <= 0.0) {
		fprintf(stderr, "[pikmin] PIKMIN_AUTOREPEAT: period must be positive\n");
		sRepeatPeriod = 0.0;
		return;
	}
	if (*p == ':') {
		sRepeatFrom = strtod(p + 1, NULL);
	}

	sRepeatNext = sRepeatFrom;
	printf("[pikmin] synthetic '%.*s' every %.1f s from %.1f s (debug aid)\n", (int)len, env, sRepeatPeriod, sRepeatFrom);
	fflush(stdout);
}

static void configure(void)
{
	const char* env = getenv("PIKMIN_AUTOSTART");
	const char* p;

	sConfigured = 1;
	sStartTime  = now_seconds();
	configure_repeat();
	if (env == NULL || *env == '\0') {
		return;
	}

	// Bare number: repeat START at that period.
	if (strchr(env, ':') == NULL) {
		sPeriod = strtod(env, NULL);
		if (sPeriod > 0.0) {
			printf("[pikmin] synthetic START every %.1f s (debug aid)\n", sPeriod);
			fflush(stdout);
		}
		return;
	}

	for (p = env; *p != '\0' && sStepCount < MAX_STEPS;) {
		char* end;
		const double at = strtod(p, &end);
		const char* name;
		size_t len;
		Step step = { 0 };

		if (end == p || *end != ':') {
			break;
		}
		name = end + 1;
		len  = strcspn(name, ",:");

		if (!step_from_name(name, len, &step)) {
			fprintf(stderr, "[pikmin] PIKMIN_AUTOSTART: unknown input '%.*s'\n", (int)len, name);
		} else {
			step.at = at;
			p       = name + len;
			if (*p == ':') {
				step.frames = (int)strtol(p + 1, &end, 10);
				if (step.frames <= 0) {
					step.frames = BUTTON_FRAMES;
				}
				p = end;
			}
			sSteps[sStepCount++] = step;
		}

		p = strchr(p, ',');
		if (p == NULL) {
			break;
		}
		p++;
	}

	printf("[pikmin] synthetic input: %d scripted steps (debug aid)\n", sStepCount);
	fflush(stdout);
}

static void apply(PADStatus* pads, const Step* step)
{
	pads[0].button |= step->button;
	if (step->stickX != 0) {
		pads[0].stickX = step->stickX;
	}
	if (step->stickY != 0) {
		pads[0].stickY = step->stickY;
	}
	pads[0].err = PAD_ERR_NONE;
}

void PikiPortDebugInput(PADStatus* pads)
{
	double elapsed;

	if (!sConfigured) {
		configure();
	}
	if (pads == NULL || (sStepCount == 0 && sPeriod <= 0.0 && sRepeatPeriod <= 0.0)) {
		return;
	}

	// The repeater runs on its own clock, on top of whatever the script is
	// doing, so a scripted stick hold and a repeating A can overlap.
	if (sRepeatPeriod > 0.0) {
		if (sRepeatHoldFrames > 0) {
			sRepeatHoldFrames--;
			apply(pads, &sRepeat);
		} else {
			const double since = now_seconds() - sStartTime;
			if (since >= sRepeatNext) {
				sRepeatHoldFrames = sRepeat.frames;
				sRepeatNext       = since + sRepeatPeriod;
				apply(pads, &sRepeat);
			}
		}
	}

	if (sHoldFrames > 0) {
		sHoldFrames--;
		apply(pads, &sHeld);
		return;
	}

	elapsed = now_seconds() - sStartTime;

	if (sStepCount > 0) {
		if (sNextStep < sStepCount && elapsed >= sSteps[sNextStep].at) {
			sHeld       = sSteps[sNextStep];
			sHoldFrames = sHeld.frames;
			sNextStep++;
			apply(pads, &sHeld);
		}
		return;
	}

	if (elapsed >= sPeriod) {
		// Hold for a few frames so an edge-triggered menu sees a press and a
		// release, then arm again.
		memset(&sHeld, 0, sizeof(sHeld));
		sHeld.button = PAD_BUTTON_START;
		sHeld.frames = BUTTON_FRAMES;
		sHoldFrames  = sHeld.frames;
		sStartTime   = now_seconds();
		apply(pads, &sHeld);
	}
}
